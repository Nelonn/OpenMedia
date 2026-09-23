#include "audio/ac3/ac3_decoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <util/bit_reader.hpp>

namespace openmedia::ac3 {

namespace {

enum DbaMode : uint8_t {
  DBA_REUSE = 0,
  DBA_NEW = 1,
  DBA_NONE = 2,
  DBA_RESERVED = 3,
};

constexpr auto symmetric(int code, int levels) -> float {
  return static_cast<float>(2 * code - (levels - 1)) / static_cast<float>(levels);
}

// Dequantization tables for the symmetric quantizers (A/52 Tables 7.19 to
// 7.23). Out-of-range codes decode to zero.
struct MantissaTables {
  float bap1[32][3] = {};
  float bap2[128][3] = {};
  float bap3[8] = {};
  float bap4[128][2] = {};
  float bap5[16] = {};
  float exp_scale[32] = {};

  constexpr MantissaTables() {
    for (int i = 0; i < 27; ++i) {
      bap1[i][0] = symmetric(i / 9, 3);
      bap1[i][1] = symmetric((i % 9) / 3, 3);
      bap1[i][2] = symmetric(i % 3, 3);
    }
    for (int i = 0; i < 125; ++i) {
      bap2[i][0] = symmetric(i / 25, 5);
      bap2[i][1] = symmetric((i % 25) / 5, 5);
      bap2[i][2] = symmetric(i % 5, 5);
    }
    for (int i = 0; i < 7; ++i) bap3[i] = symmetric(i, 7);
    for (int i = 0; i < 121; ++i) {
      bap4[i][0] = symmetric(i / 11, 11);
      bap4[i][1] = symmetric(i % 11, 11);
    }
    for (int i = 0; i < 15; ++i) bap5[i] = symmetric(i, 15);
    float scale = 1.0f;
    for (float& s : exp_scale) {
      s = scale;
      scale *= 0.5f;
    }
  }
};

constexpr MantissaTables MANTISSA;

// CRC-16 with generator x^16 + x^15 + x^2 + 1, MSB first (A/52 Section 7.10.1).
constexpr auto CRC_TABLE = [] {
  std::array<uint16_t, 256> table {};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i << 8;
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1;
    }
    table[i] = static_cast<uint16_t>(crc);
  }
  return table;
}();

// Speaker location of each coded channel for every acmod (A/52 Table 5.8).
// Single surround channels map to back center.
constexpr uint64_t ACMOD_LOCATIONS[8][5] = {
  {CH_FRONT_LEFT, CH_FRONT_RIGHT},
  {CH_FRONT_CENTER},
  {CH_FRONT_LEFT, CH_FRONT_RIGHT},
  {CH_FRONT_LEFT, CH_FRONT_CENTER, CH_FRONT_RIGHT},
  {CH_FRONT_LEFT, CH_FRONT_RIGHT, CH_BACK_CENTER},
  {CH_FRONT_LEFT, CH_FRONT_CENTER, CH_FRONT_RIGHT, CH_BACK_CENTER},
  {CH_FRONT_LEFT, CH_FRONT_RIGHT, CH_SIDE_LEFT, CH_SIDE_RIGHT},
  {CH_FRONT_LEFT, CH_FRONT_CENTER, CH_FRONT_RIGHT, CH_SIDE_LEFT, CH_SIDE_RIGHT},
};

// Locations for each chanmap bit, most significant bit first (TS 102 366 Table E1.4).
constexpr uint64_t CHANMAP_LOCATIONS[16][2] = {
  {CH_FRONT_LEFT, 0},
  {CH_FRONT_CENTER, 0},
  {CH_FRONT_RIGHT, 0},
  {CH_SIDE_LEFT, 0},
  {CH_SIDE_RIGHT, 0},
  {CH_FRONT_LEFT_OF_CENTER, CH_FRONT_RIGHT_OF_CENTER},
  {CH_BACK_LEFT, CH_BACK_RIGHT},
  {CH_BACK_CENTER, 0},
  {CH_TOP_CENTER, 0},
  {CH_SURROUND_DIRECT_LEFT, CH_SURROUND_DIRECT_RIGHT},
  {CH_WIDE_LEFT, CH_WIDE_RIGHT},
  {CH_TOP_FRONT_LEFT, CH_TOP_FRONT_RIGHT},
  {CH_TOP_FRONT_CENTER, 0},
  {CH_TOP_BACK_LEFT, CH_TOP_BACK_RIGHT},
  {CH_LOW_FREQUENCY_2, 0},
  {CH_LOW_FREQUENCY, 0},
};

// Dynamic range gain word to linear gain (A/52 Section 7.7.1.2): a 3-bit
// signed exponent followed by a 5-bit mantissa with an implied leading one.
auto dynamicRangeGain(uint32_t code) -> float {
  const int exponent = static_cast<int>(code >> 5) - ((code & 0x80) ? 8 : 0);
  return std::ldexp(static_cast<float>(32 + (code & 0x1f)) / 32.0f, exponent);
}

// Inverse of the 6-point DCT-II used by adaptive hybrid transform
// (TS 102 366 Section E.3.5): x[m] = X[0] + sqrt(2) * sum_k X[k] cos(k (2m + 1) pi / 12).
void inverseAhtTransform(float* x) {
  static const auto BASIS = [] {
    std::array<std::array<float, 6>, 6> basis {};
    for (int m = 0; m < 6; ++m) {
      basis[m][0] = 1.0f;
      for (int k = 1; k < 6; ++k) {
        basis[m][k] = static_cast<float>(std::numbers::sqrt2 * std::cos(k * (2 * m + 1) * std::numbers::pi / 12.0));
      }
    }
    return basis;
  }();
  float in[6];
  std::copy_n(x, 6, in);
  for (int m = 0; m < 6; ++m) {
    float sum = 0.0f;
    for (int k = 0; k < 6; ++k) sum += BASIS[m][k] * in[k];
    x[m] = sum;
  }
}

auto calcLowComp1(int a, int b0, int b1, int c) -> int {
  if (b0 + 256 == b1) return c;
  if (b0 > b1) return std::max(a - 64, 0);
  return a;
}

auto calcLowComp(int a, int b0, int b1, int band) -> int {
  if (band < 7) return calcLowComp1(a, b0, b1, 384);
  if (band < 20) return calcLowComp1(a, b0, b1, 320);
  return std::max(a - 128, 0);
}

}

auto parseFrameHeader(std::span<const uint8_t> data) -> std::optional<FrameHeader> {
  if (data.size() < 8 || data[0] != 0x0B || data[1] != 0x77) return std::nullopt;

  FrameHeader h;
  h.bsid = data[5] >> 3;
  BitReader br(data);
  br.skipBits(16);

  if (h.bsid <= 10) {
    br.skipBits(16); // crc1
    h.fscod = static_cast<uint8_t>(br.readBits(2));
    const uint32_t frmsizecod = br.readBits(6);
    if (h.fscod == 3 || frmsizecod > 37) return std::nullopt;
    br.skipBits(5 + 3); // bsid, bsmod
    h.acmod = static_cast<uint8_t>(br.readBits(3));
    if ((h.acmod & 1) && h.acmod != 1) br.skipBits(2); // cmixlev
    if (h.acmod & 4) br.skipBits(2);                   // surmixlev
    if (h.acmod == 2) br.skipBits(2);                  // dsurmod
    h.lfe = br.readFlag();

    h.eac3 = false;
    h.type = FrameType::AC3_CONVERT;
    h.sr_shift = static_cast<uint8_t>(std::max<int>(h.bsid, 8) - 8);
    h.sample_rate = SAMPLE_RATES[h.fscod] >> h.sr_shift;
    h.bit_rate = (BIT_RATES[frmsizecod >> 1] * 1000u) >> h.sr_shift;
    h.frame_size = FRAME_SIZE_WORDS[frmsizecod][h.fscod] * 2u;
    h.num_blocks = 6;
  } else if (h.bsid <= 16) {
    h.eac3 = true;
    const uint32_t strmtyp = br.readBits(2);
    if (strmtyp == 3) return std::nullopt;
    h.type = static_cast<FrameType>(strmtyp);
    h.substream_id = static_cast<uint8_t>(br.readBits(3));
    h.frame_size = (br.readBits(11) + 1) * 2;
    h.fscod = static_cast<uint8_t>(br.readBits(2));
    if (h.fscod == 3) {
      const uint32_t fscod2 = br.readBits(2);
      if (fscod2 == 3) return std::nullopt;
      h.sample_rate = SAMPLE_RATES[fscod2] / 2;
      h.sr_shift = 1;
      h.num_blocks = 6;
    } else {
      h.num_blocks = EAC3_BLOCKS[br.readBits(2)];
      h.sample_rate = SAMPLE_RATES[h.fscod];
    }
    h.acmod = static_cast<uint8_t>(br.readBits(3));
    h.lfe = br.readFlag();
    if (h.frame_size < 8) return std::nullopt;
    h.bit_rate = static_cast<uint32_t>(8ull * h.frame_size * h.sample_rate / (h.num_blocks * 256u));

    if (h.type == FrameType::DEPENDENT) {
      br.skipBits(5); // bsid
      for (int i = 0; i < (h.acmod == 0 ? 2 : 1); ++i) {
        br.skipBits(5);                    // dialnorm
        if (br.readFlag()) br.skipBits(8); // compr
      }
      h.chanmap_present = br.readFlag();
      if (h.chanmap_present) h.chanmap = static_cast<uint16_t>(br.readBits(16));
    }
  } else {
    return std::nullopt;
  }

  if (!br.ok()) return std::nullopt;
  return h;
}

void channelLocations(const FrameHeader& header, uint64_t* locations) {
  const int nfchans = FBW_CHANNELS[header.acmod];
  std::copy_n(ACMOD_LOCATIONS[header.acmod], nfchans, locations);
  if (header.lfe) locations[nfchans] = CH_LOW_FREQUENCY;
  if (!header.chanmap_present) return;

  // Channels of a dependent substream take the chanmap locations in order.
  uint64_t mapped[16] = {};
  int count = 0;
  for (int bit = 0; bit < 16; ++bit) {
    if (!(header.chanmap & (0x8000u >> bit))) continue;
    for (const uint64_t location : CHANMAP_LOCATIONS[bit]) {
      if (location && count < 16) mapped[count++] = location;
    }
  }
  if (count == header.channels()) std::copy_n(mapped, count, locations);
}

auto checkFrameCrc(std::span<const uint8_t> frame) -> bool {
  if (frame.size() < 4) return false;
  uint16_t crc = 0;
  for (size_t i = 2; i < frame.size(); ++i) {
    crc = static_cast<uint16_t>((crc << 8) ^ CRC_TABLE[(crc >> 8) ^ frame[i]]);
  }
  return crc == 0;
}

// ---------------------------------------------------------------------------
// SubstreamDecoder

SubstreamDecoder::SubstreamDecoder() {
  reset();
}

void SubstreamDecoder::reset() {
  std::memset(delay_, 0, sizeof(delay_));
  std::memset(coefs_, 0, sizeof(coefs_));
  std::memset(exps_, 0, sizeof(exps_));
  std::memset(bap_, 0, sizeof(bap_));
  std::memset(expstr_, 0, sizeof(expstr_));
  std::fill(std::begin(dba_mode_), std::end(dba_mode_), DBA_NONE);
  std::fill(std::begin(dynrng_), std::end(dynrng_), 1.0f);
  spxinu_ = false;
  cplinu_blk_ = false;
  nrematbnds_ = 0;
  acmod_ = -1;
  rng_ = 1;
}

auto SubstreamDecoder::dither() -> float {
  // Uniform in [-0.707, 0.707) (A/52 Section 7.3.4).
  rng_ = rng_ * 1664525u + 1013904223u;
  return static_cast<float>(static_cast<int32_t>(rng_)) * (0.707107f / 2147483648.0f);
}

auto SubstreamDecoder::noise() -> float {
  rng_ = rng_ * 1664525u + 1013904223u;
  return static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
}

auto SubstreamDecoder::decode(std::span<const uint8_t> frame, const PcmTarget& target) -> bool {
  const auto hdr = parseFrameHeader(frame);
  if (!hdr || hdr->frame_size > frame.size()) {
    reset();
    return false;
  }
  if (hdr->acmod != acmod_ || hdr->lfe != lfeon_) {
    // The overlap of the previous frame belongs to other channels.
    std::memset(delay_, 0, sizeof(delay_));
  }
  header_ = *hdr;
  acmod_ = hdr->acmod;
  lfeon_ = hdr->lfe;
  nfchans_ = FBW_CHANNELS[acmod_];
  nchans_ = nfchans_ + (lfeon_ ? 1 : 0);
  lfe_ch_ = nfchans_ + 1;

  if (!header_.eac3) {
    ba_fscod_ = header_.fscod;
    ba_shift_ = header_.sr_shift;
  } else if (header_.sr_shift) {
    // Reduced rate E-AC-3: fscod2 selects the hearing threshold column.
    ba_fscod_ = header_.sample_rate == 24000 ? 0 : header_.sample_rate == 22050 ? 1 : 2;
    ba_shift_ = 0;
  } else {
    ba_fscod_ = header_.fscod;
    ba_shift_ = 0;
  }

  BitReader br(frame.first(header_.frame_size));
  br.skipBits(16);

  const bool bsi_ok = header_.eac3 ? parseEac3Bsi(br) : parseAc3Bsi(br);
  if (!bsi_ok) {
    reset();
    return false;
  }

  // Per-frame syntax state.
  start_[lfe_ch_] = 0;
  end_[lfe_ch_] = 7;
  chincpl_[lfe_ch_] = false;
  dithflag_[0] = true;
  dithflag_[lfe_ch_] = false;
  blksw_[lfe_ch_] = false;
  for (int ch = 1; ch <= nfchans_; ++ch) {
    first_spx_coords_[ch] = true;
    first_cpl_coords_[ch] = true;
  }

  if (!header_.eac3) {
    setAc3FrameDefaults();
  } else if (!parseAudioFrame(br)) {
    reset();
    return false;
  }

  for (int blk = 0; blk < header_.num_blocks; ++blk) {
    if (!decodeBlock(br, blk, target) || !br.ok()) {
      reset();
      return false;
    }
  }
  return true;
}

auto SubstreamDecoder::parseAc3Bsi(BitReader& br) -> bool {
  br.skipBits(16 + 2 + 6); // crc1, fscod, frmsizecod
  br.skipBits(5 + 3 + 3);  // bsid, bsmod, acmod
  if ((acmod_ & 1) && acmod_ != 1) br.skipBits(2); // cmixlev
  if (acmod_ & 4) br.skipBits(2);                  // surmixlev
  if (acmod_ == 2) br.skipBits(2);                 // dsurmod
  br.skipBits(1);                                  // lfeon

  for (int i = 0; i < (acmod_ == 0 ? 2 : 1); ++i) {
    br.skipBits(5);                    // dialnorm
    if (br.readFlag()) br.skipBits(8); // compr
    if (br.readFlag()) br.skipBits(8); // langcod
    if (br.readFlag()) br.skipBits(7); // mixlevel, roomtyp
  }
  br.skipBits(2); // copyrightb, origbs

  if (header_.bsid == 6) {
    // Alternate bit stream syntax (A/52 Annex D).
    if (br.readFlag()) br.skipBits(14); // dmixmod, ltrt/loro mix levels
    if (br.readFlag()) br.skipBits(14); // dsurexmod, dheadphonmod, adconvtyp, xbsi2, encinfo
  } else {
    if (br.readFlag()) br.skipBits(14); // timecod1
    if (br.readFlag()) br.skipBits(14); // timecod2
  }
  if (br.readFlag()) {
    const uint32_t addbsil = br.readBits(6);
    br.skipBits((addbsil + 1) * 8);
  }
  return br.ok();
}

auto SubstreamDecoder::parseEac3Bsi(BitReader& br) -> bool {
  br.skipBits(2 + 3 + 11 + 2 + 2 + 3 + 1); // strmtyp .. lfeon (fscod2 takes the numblkscod slot)
  br.skipBits(5);                          // bsid

  for (int i = 0; i < (acmod_ == 0 ? 2 : 1); ++i) {
    br.skipBits(5);                    // dialnorm
    if (br.readFlag()) br.skipBits(8); // compr
  }

  if (header_.type == FrameType::DEPENDENT && br.readFlag()) br.skipBits(16); // chanmap, see parseFrameHeader

  // Mixing metadata.
  if (br.readFlag()) {
    if (acmod_ > 2) br.skipBits(2);                 // dmixmod
    if ((acmod_ & 1) && acmod_ > 2) br.skipBits(6); // ltrtcmixlev, lorocmixlev
    if (acmod_ & 4) br.skipBits(6);                 // ltrtsurmixlev, lorosurmixlev
    if (lfeon_ && br.readFlag()) br.skipBits(5);    // lfemixlevcod
    if (header_.type == FrameType::INDEPENDENT) {
      if (br.readFlag()) br.skipBits(6);                // pgmscl
      if (acmod_ == 0 && br.readFlag()) br.skipBits(6); // pgmscl2
      if (br.readFlag()) br.skipBits(6);                // extpgmscl
      switch (br.readBits(2)) {                         // mixdef
        case 1: br.skipBits(5); break;
        case 2: br.skipBits(12); break;
        case 3: br.skipBits((br.readBits(5) + 2) * 8); break;
        default: break;
      }
      if (acmod_ < 2) {
        if (br.readFlag()) br.skipBits(14);                // panmean, paninfo
        if (acmod_ == 0 && br.readFlag()) br.skipBits(14); // panmean2, paninfo2
      }
      if (br.readFlag()) { // frmmixcfginfoe
        if (header_.num_blocks == 1) {
          br.skipBits(5);
        } else {
          for (int blk = 0; blk < header_.num_blocks; ++blk) {
            if (br.readFlag()) br.skipBits(5);
          }
        }
      }
    }
  }

  // Informational metadata.
  if (br.readFlag()) {
    br.skipBits(3 + 1 + 1);          // bsmod, copyrightb, origbs
    if (acmod_ == 2) br.skipBits(4); // dsurmod, dheadphonmod
    if (acmod_ >= 6) br.skipBits(2); // dsurexmod
    for (int i = 0; i < (acmod_ == 0 ? 2 : 1); ++i) {
      if (br.readFlag()) br.skipBits(8); // mixlevel, roomtyp, adconvtyp
    }
    if (header_.fscod < 3) br.skipBits(1); // sourcefscod
  }

  if (header_.type == FrameType::INDEPENDENT && header_.num_blocks != 6) br.skipBits(1); // convsync
  if (header_.type == FrameType::AC3_CONVERT && (header_.num_blocks == 6 || br.readFlag())) {
    br.skipBits(6); // frmsizecod
  }
  if (br.readFlag()) {
    const uint32_t addbsil = br.readBits(6);
    br.skipBits((addbsil + 1) * 8);
  }
  return br.ok();
}

void SubstreamDecoder::setAc3FrameDefaults() {
  snroffststr_ = -1; // AC-3 syntax: snroffste in every block
  blkswe_ = true;
  dithflage_ = true;
  bamode_ = true;
  frmfgaincode_ = false;
  dbaflde_ = true;
  skipflde_ = true;
  first_cpl_leak_ = false;
  std::fill(std::begin(chahtinu_), std::end(chahtinu_), false);
  std::fill(std::begin(spx_atten_), std::end(spx_atten_), -1);
}

auto SubstreamDecoder::parseAudioFrame(BitReader& br) -> bool {
  const int nblocks = header_.num_blocks;
  bool expstre = true;
  bool ahte = false;
  if (nblocks == 6) {
    expstre = br.readFlag();
    ahte = br.readFlag();
  }
  snroffststr_ = static_cast<int>(br.readBits(2));
  const bool transproce = br.readFlag();
  blkswe_ = br.readFlag();
  dithflage_ = br.readFlag();
  bamode_ = br.readFlag();
  frmfgaincode_ = br.readFlag();
  dbaflde_ = br.readFlag();
  skipflde_ = br.readFlag();
  const bool spxattene = br.readFlag();

  if (!blkswe_) std::fill(blksw_ + 1, blksw_ + 1 + nfchans_, false);
  if (!dithflage_) std::fill(dithflag_ + 1, dithflag_ + 1 + nfchans_, true);
  if (!bamode_) {
    sdecay_ = SLOW_DECAY[2];
    fdecay_ = FAST_DECAY[1];
    sgain_ = SLOW_GAIN[1];
    dbknee_ = DB_PER_BIT[2];
    floor_ = FLOOR_TABLE[7];
  }

  // Coupling strategy per block.
  if (acmod_ > 1) {
    for (int blk = 0; blk < nblocks; ++blk) {
      cplstre_[blk] = blk == 0 || br.readFlag();
      cplinu_[blk] = cplstre_[blk] ? br.readFlag() : cplinu_[blk - 1];
    }
  } else {
    std::fill(std::begin(cplstre_), std::end(cplstre_), false);
    std::fill(std::begin(cplinu_), std::end(cplinu_), false);
  }
  const auto ncplblks = std::count(cplinu_, cplinu_ + nblocks, true);

  // Exponent strategies.
  if (expstre) {
    for (int blk = 0; blk < nblocks; ++blk) {
      for (int ch = cplinu_[blk] ? 0 : 1; ch <= nfchans_; ++ch) {
        expstr_[blk][ch] = static_cast<uint8_t>(br.readBits(2));
      }
    }
  } else {
    for (int ch = (acmod_ > 1 && ncplblks > 0) ? 0 : 1; ch <= nfchans_; ++ch) {
      const uint32_t frmchexpstr = br.readBits(5);
      for (int blk = 0; blk < 6; ++blk) expstr_[blk][ch] = FRAME_EXP_STRATEGIES[frmchexpstr][blk];
    }
  }
  if (lfeon_) {
    for (int blk = 0; blk < nblocks; ++blk) expstr_[blk][lfe_ch_] = static_cast<uint8_t>(br.readBits(1));
  }

  if (header_.type == FrameType::INDEPENDENT && (nblocks == 6 || br.readFlag())) {
    br.skipBits(5 * nfchans_); // convexpstr
  }

  // Adaptive hybrid transform. AHT requires the exponents (and, for the
  // coupling channel, the coupling strategy) of blocks 1 to 5 to be reused.
  std::fill(std::begin(chahtinu_), std::end(chahtinu_), false);
  if (ahte) {
    for (int ch = ncplblks == 6 ? 0 : 1; ch <= nchans_; ++ch) {
      bool possible = true;
      for (int blk = 1; blk < 6 && possible; ++blk) {
        possible = expstr_[blk][ch] == EXP_REUSE && !(ch == 0 && cplstre_[blk]);
      }
      chahtinu_[ch] = possible && br.readFlag();
    }
  }

  if (snroffststr_ == 0) {
    csnroffst_ = static_cast<int>(br.readBits(6));
    std::fill(fsnroffst_, fsnroffst_ + nchans_ + 1, static_cast<int>(br.readBits(4)));
  }

  if (transproce) {
    for (int ch = 1; ch <= nfchans_; ++ch) {
      if (br.readFlag()) br.skipBits(10 + 8); // transprocloc, transproclen
    }
  }

  for (int ch = 1; ch <= nfchans_; ++ch) {
    spx_atten_[ch] = (spxattene && br.readFlag()) ? static_cast<int>(br.readBits(5)) : -1;
  }

  if (nblocks > 1 && br.readFlag()) {
    // blkstrtinfo: (nblocks - 1) * (4 + ceil(log2(words per frame))) bits.
    const uint32_t words = header_.frame_size / 2;
    const int bits = std::bit_width(words - 1);
    br.skipBits(static_cast<size_t>(nblocks - 1) * (4 + bits));
  }

  first_cpl_leak_ = true;
  return br.ok();
}

auto SubstreamDecoder::bandStructure(BitReader& br, bool eac3, int blk, int start, int end, const uint8_t* defaults,
                                     uint8_t* band_struct, uint8_t* band_sizes) -> int {
  if (blk == 0) std::copy_n(defaults, end, band_struct);
  if (!eac3 || br.readFlag()) {
    for (int sb = start + 1; sb < end; ++sb) band_struct[sb] = static_cast<uint8_t>(br.readBits(1));
  }
  int bands = 0;
  band_sizes[0] = 12;
  for (int sb = start + 1; sb < end; ++sb) {
    if (band_struct[sb]) {
      band_sizes[bands] += 12;
    } else {
      band_sizes[++bands] = 12;
    }
  }
  return bands + 1;
}

// ---------------------------------------------------------------------------
// Audio block

auto SubstreamDecoder::decodeBlock(BitReader& br, int blk, const PcmTarget& target) -> bool {
  parseTransformFlags(br, blk);
  if (!parseSpectralExtension(br, blk)) return false;
  if (!parseCoupling(br, blk)) return false;
  parseRematrixing(br, blk);
  if (!parseExponents(br, blk)) return false;
  if (!parseBitAllocation(br, blk) || !br.ok()) return false;

  const int first = cplinu_blk_ ? 0 : 1;
  zero_baps_ = csnroffst_ == 0 && std::all_of(fsnroffst_ + first, fsnroffst_ + nchans_ + 1, [](int f) { return f == 0; });
  for (int ch = first; ch <= nchans_; ++ch) allocateBits(ch);

  decodeCoefficients(br, blk);
  if (!br.ok()) return false;

  if (cplinu_blk_) uncouple();
  if (acmod_ == 2) rematrix();
  applyDynamicRange();
  if (spxinu_) applySpectralExtension();
  synthesize(blk, target);
  return true;
}

void SubstreamDecoder::parseTransformFlags(BitReader& br, int blk) {
  if (blkswe_) {
    for (int ch = 1; ch <= nfchans_; ++ch) blksw_[ch] = br.readFlag();
  }
  if (dithflage_) {
    for (int ch = 1; ch <= nfchans_; ++ch) dithflag_[ch] = br.readFlag();
  }
  for (int i = 0; i < (acmod_ == 0 ? 2 : 1); ++i) {
    if (br.readFlag()) {
      dynrng_[i] = dynamicRangeGain(br.readBits(8));
    } else if (blk == 0) {
      dynrng_[i] = 1.0f;
    }
  }
}

auto SubstreamDecoder::parseSpectralExtension(BitReader& br, int blk) -> bool {
  if (header_.eac3 && (blk == 0 || br.readFlag())) {
    spxinu_ = br.readFlag();
    if (spxinu_ && !parseSpxStrategy(br, blk)) return false;
  }
  if (header_.eac3 && spxinu_) {
    parseSpxCoordinates(br);
  } else {
    spxinu_ = false;
    std::fill(chinspx_ + 1, chinspx_ + 1 + nfchans_, false);
    std::fill(first_spx_coords_ + 1, first_spx_coords_ + 1 + nfchans_, true);
  }
  return true;
}

auto SubstreamDecoder::parseSpxStrategy(BitReader& br, int blk) -> bool {
  if (acmod_ == 1) {
    chinspx_[1] = true;
  } else {
    for (int ch = 1; ch <= nfchans_; ++ch) chinspx_[ch] = br.readFlag();
  }
  const int spxstrtf = static_cast<int>(br.readBits(2));
  int begin = static_cast<int>(br.readBits(3)) + 2;
  int end = static_cast<int>(br.readBits(3)) + 5;
  if (begin > 7) begin += begin - 7;
  if (end > 7) end += end - 7;

  spx_dst_start_ = spxstrtf * 12 + 25;
  spx_src_start_ = begin * 12 + 25;
  spx_end_ = end * 12 + 25;
  if (begin >= end || spx_dst_start_ >= spx_src_start_) return false;

  nspxbnds_ = bandStructure(br, true, blk, begin, end, DEFAULT_SPX_BAND_STRUCT, spx_band_struct_, spx_band_sizes_);
  return true;
}

void SubstreamDecoder::parseSpxCoordinates(BitReader& br) {
  for (int ch = 1; ch <= nfchans_; ++ch) {
    if (!chinspx_[ch]) {
      first_spx_coords_[ch] = true;
      continue;
    }
    if (!first_spx_coords_[ch] && !br.readFlag()) continue;
    first_spx_coords_[ch] = false;

    const float blend = static_cast<float>(br.readBits(5)) / 32.0f;
    const int mstrspxco = static_cast<int>(br.readBits(2)) * 3;
    int bin = spx_src_start_;
    for (int bnd = 0; bnd < nspxbnds_; ++bnd) {
      const int size = spx_band_sizes_[bnd];
      // Noise blending grows towards the top of the extension range.
      const float nratio =
        std::clamp(static_cast<float>(bin + size / 2) / static_cast<float>(spx_end_) - blend, 0.0f, 1.0f);
      bin += size;

      const int exp = static_cast<int>(br.readBits(4));
      const int mant = static_cast<int>(br.readBits(2));
      const float coord = std::ldexp(static_cast<float>(exp == 15 ? mant * 2 : mant + 4), 2 - exp - mstrspxco);
      // Uniform noise has a variance of 1/3, hence the factor 3.
      spx_noise_blend_[ch][bnd] = std::sqrt(3.0f * nratio) * coord;
      spx_signal_blend_[ch][bnd] = std::sqrt(1.0f - nratio) * coord;
    }
  }
}

auto SubstreamDecoder::parseCoupling(BitReader& br, int blk) -> bool {
  const bool eac3 = header_.eac3;
  if (eac3 ? cplstre_[blk] : br.readFlag()) {
    if (!eac3) cplinu_[blk] = br.readFlag();
    if (!parseCouplingStrategy(br, blk)) return false;
  } else if (!eac3) {
    if (blk == 0) return false;
    cplinu_[blk] = cplinu_[blk - 1];
  }
  cplinu_blk_ = cplinu_[blk];
  return !cplinu_blk_ || parseCouplingCoordinates(br, blk);
}

auto SubstreamDecoder::parseCouplingStrategy(BitReader& br, int blk) -> bool {
  const bool eac3 = header_.eac3;
  if (!cplinu_[blk]) {
    std::fill(chincpl_ + 1, chincpl_ + 1 + nfchans_, false);
    std::fill(first_cpl_coords_ + 1, first_cpl_coords_ + 1 + nfchans_, true);
    first_cpl_leak_ = eac3;
    phsflginu_ = false;
    return true;
  }

  if (acmod_ < 2) return false;
  if (eac3 && br.readFlag()) {
    // Enhanced coupling (ecplinu) is not supported.
    return false;
  }

  if (eac3 && acmod_ == 2) {
    chincpl_[1] = chincpl_[2] = true;
  } else {
    for (int ch = 1; ch <= nfchans_; ++ch) chincpl_[ch] = br.readFlag();
  }
  phsflginu_ = acmod_ == 2 && br.readFlag();

  const int cplbegf = static_cast<int>(br.readBits(4));
  const int cplendf = spxinu_ ? (spx_src_start_ - 37) / 12 : static_cast<int>(br.readBits(4)) + 3;
  if (cplbegf >= cplendf) return false;
  start_[0] = cplbegf * 12 + 37;
  end_[0] = cplendf * 12 + 37;
  ncplbnds_ = bandStructure(br, eac3, blk, cplbegf, cplendf, DEFAULT_CPL_BAND_STRUCT, cpl_band_struct_, cpl_band_sizes_);
  return true;
}

auto SubstreamDecoder::parseCouplingCoordinates(BitReader& br, int blk) -> bool {
  bool coords_present = false;
  for (int ch = 1; ch <= nfchans_; ++ch) {
    if (!chincpl_[ch]) {
      first_cpl_coords_[ch] = true;
      continue;
    }
    if ((header_.eac3 && first_cpl_coords_[ch]) || br.readFlag()) {
      first_cpl_coords_[ch] = false;
      coords_present = true;
      const int mstrcplco = static_cast<int>(br.readBits(2)) * 3;
      for (int bnd = 0; bnd < ncplbnds_; ++bnd) {
        const int exp = static_cast<int>(br.readBits(4));
        const int mant = static_cast<int>(br.readBits(4));
        const float value = exp == 15 ? static_cast<float>(mant) / 16.0f : static_cast<float>(mant + 16) / 32.0f;
        // The factor 8 is part of the coupling coordinate definition (A/52 Section 7.4.3).
        cplco_[ch][bnd] = std::ldexp(value, 3 - exp - mstrcplco);
      }
    } else if (blk == 0 || first_cpl_coords_[ch]) {
      return false;
    }
  }
  if (acmod_ == 2 && coords_present) {
    for (int bnd = 0; bnd < ncplbnds_; ++bnd) phsflg_[bnd] = phsflginu_ && br.readFlag();
  }
  return true;
}

void SubstreamDecoder::parseRematrixing(BitReader& br, int blk) {
  if (acmod_ != 2) return;
  if ((header_.eac3 && blk == 0) || br.readFlag()) {
    nrematbnds_ = 4;
    if (cplinu_blk_ && start_[0] <= 61) {
      nrematbnds_ -= 1 + (start_[0] == 37 ? 1 : 0);
    } else if (spxinu_ && spx_src_start_ <= 61) {
      nrematbnds_ -= 1;
    }
    for (int bnd = 0; bnd < nrematbnds_; ++bnd) rematflg_[bnd] = br.readFlag();
  } else if (blk == 0) {
    nrematbnds_ = 0;
  }
}

auto SubstreamDecoder::parseExponents(BitReader& br, int blk) -> bool {
  const int first = cplinu_blk_ ? 0 : 1;
  if (!header_.eac3) {
    for (int ch = first; ch <= nfchans_; ++ch) expstr_[blk][ch] = static_cast<uint8_t>(br.readBits(2));
    if (lfeon_) expstr_[blk][lfe_ch_] = static_cast<uint8_t>(br.readBits(1));
  }

  // Channel bandwidth.
  for (int ch = 1; ch <= nfchans_; ++ch) {
    if (expstr_[blk][ch] == EXP_REUSE) continue;
    start_[ch] = 0;
    if (chincpl_[ch]) {
      end_[ch] = start_[0];
    } else if (chinspx_[ch]) {
      end_[ch] = spx_src_start_;
    } else {
      const int chbwcod = static_cast<int>(br.readBits(6));
      if (chbwcod > 60) return false;
      end_[ch] = chbwcod * 3 + 73;
    }
  }

  for (int ch = first; ch <= nchans_; ++ch) {
    const int strategy = expstr_[blk][ch];
    if (strategy == EXP_REUSE) continue;
    if (!decodeExponents(br, ch, strategy)) return false;
    if (ch >= 1 && ch <= nfchans_) br.skipBits(2); // gainrng
  }
  return true;
}

auto SubstreamDecoder::decodeExponents(BitReader& br, int ch, int strategy) -> bool {
  // Grouped differential exponents (A/52 Section 7.1.3).
  const int repeat = 1 << (strategy - 1);
  const int group_bins = 3 * repeat;
  int ngrps;
  int pos;
  int prev = static_cast<int>(br.readBits(4));
  if (ch == 0) {
    ngrps = (end_[0] - start_[0]) / group_bins;
    prev <<= 1;
    pos = start_[0];
  } else {
    ngrps = ch == lfe_ch_ ? 2 : (end_[ch] + group_bins - 4) / group_bins;
    exps_[ch][0] = static_cast<int8_t>(prev);
    pos = 1;
  }
  for (int grp = 0; grp < ngrps; ++grp) {
    const uint32_t code = br.readBits(7);
    if (code >= 125) return false;
    const int deltas[3] = {static_cast<int>(code / 25), static_cast<int>(code % 25 / 5), static_cast<int>(code % 5)};
    for (const int delta : deltas) {
      prev += delta - 2;
      if (prev < 0 || prev > 24) return false;
      for (int r = 0; r < repeat && pos < MAX_COEFS; ++r) exps_[ch][pos++] = static_cast<int8_t>(prev);
    }
  }
  return true;
}

auto SubstreamDecoder::parseBitAllocation(BitReader& br, int blk) -> bool {
  const bool eac3 = header_.eac3;
  if (bamode_) {
    if (br.readFlag()) {
      sdecay_ = SLOW_DECAY[br.readBits(2)] >> ba_shift_;
      fdecay_ = FAST_DECAY[br.readBits(2)] >> ba_shift_;
      sgain_ = SLOW_GAIN[br.readBits(2)];
      dbknee_ = DB_PER_BIT[br.readBits(2)];
      floor_ = FLOOR_TABLE[br.readBits(3)];
    } else if (blk == 0 && !eac3) {
      return false;
    }
  }

  if (!parseSnrOffsets(br, blk)) return false;

  if (cplinu_blk_) {
    if (first_cpl_leak_ || br.readFlag()) {
      cplfleak_ = static_cast<int>(br.readBits(3));
      cplsleak_ = static_cast<int>(br.readBits(3));
    } else if (!eac3 && blk == 0) {
      return false;
    }
    first_cpl_leak_ = false;
  }

  if (!parseDeltaBitAllocation(br, blk)) return false;

  if (skipflde_ && br.readFlag()) {
    const uint32_t skipl = br.readBits(9);
    br.skipBits(skipl * 8);
  }
  return true;
}

auto SubstreamDecoder::parseSnrOffsets(BitReader& br, int blk) -> bool {
  const int first = cplinu_blk_ ? 0 : 1;
  if (!header_.eac3) {
    if (br.readFlag()) {
      csnroffst_ = static_cast<int>(br.readBits(6));
      for (int ch = first; ch <= nchans_; ++ch) {
        fsnroffst_[ch] = static_cast<int>(br.readBits(4));
        fgain_[ch] = FAST_GAIN[br.readBits(3)];
      }
    } else if (blk == 0) {
      return false;
    }
    return true;
  }

  if (snroffststr_ != 0 && br.readFlag()) {
    csnroffst_ = static_cast<int>(br.readBits(6));
    if (snroffststr_ == 1) {
      std::fill(fsnroffst_, fsnroffst_ + nchans_ + 1, static_cast<int>(br.readBits(4)));
    } else {
      for (int ch = first; ch <= nchans_; ++ch) fsnroffst_[ch] = static_cast<int>(br.readBits(4));
    }
  }

  if (frmfgaincode_ && br.readFlag()) {
    for (int ch = first; ch <= nchans_; ++ch) fgain_[ch] = FAST_GAIN[br.readBits(3)];
  } else if (blk == 0) {
    std::fill(fgain_, fgain_ + nchans_ + 1, FAST_GAIN[4]);
  }

  if (header_.type == FrameType::INDEPENDENT && br.readFlag()) br.skipBits(10); // convsnroffst
  return true;
}

auto SubstreamDecoder::parseDeltaBitAllocation(BitReader& br, int blk) -> bool {
  const int first = cplinu_blk_ ? 0 : 1;
  if (dbaflde_ && br.readFlag()) {
    for (int ch = first; ch <= nfchans_; ++ch) {
      dba_mode_[ch] = static_cast<uint8_t>(br.readBits(2));
      if (dba_mode_[ch] == DBA_RESERVED) return false;
    }
    for (int ch = first; ch <= nfchans_; ++ch) {
      if (dba_mode_[ch] != DBA_NEW) continue;
      dba_nseg_[ch] = static_cast<uint8_t>(br.readBits(3) + 1);
      for (int seg = 0; seg < dba_nseg_[ch]; ++seg) {
        dba_offset_[ch][seg] = static_cast<uint8_t>(br.readBits(5));
        dba_length_[ch][seg] = static_cast<uint8_t>(br.readBits(4));
        dba_value_[ch][seg] = static_cast<uint8_t>(br.readBits(3));
      }
    }
  } else if (blk == 0) {
    std::fill(std::begin(dba_mode_), std::end(dba_mode_), DBA_NONE);
  }
  return true;
}

void SubstreamDecoder::allocateBits(int ch) {
  // Parametric bit allocation (A/52 Section 7.2).
  const int start = start_[ch];
  const int end = end_[ch];
  uint8_t* bap = bap_[ch];
  if (end <= start) return;
  if (zero_baps_) {
    std::fill(bap + start, bap + end, uint8_t {0});
    return;
  }

  // Exponent mapping into PSD and PSD integration.
  int psd[MAX_COEFS];
  int bndpsd[CRITICAL_BANDS + 1] = {};
  for (int bin = start; bin < end; ++bin) psd[bin] = 3072 - (exps_[ch][bin] << 7);
  {
    int bin = start;
    int band = BIN_TO_BAND[start];
    do {
      int v = psd[bin++];
      const int band_end = std::min<int>(BAND_START[band + 1], end);
      for (; bin < band_end; ++bin) {
        const int max = std::max(v, psd[bin]);
        const int adr = std::min(max - ((v + psd[bin] + 1) >> 1), 255);
        v = max + LOG_ADD[adr];
      }
      bndpsd[band++] = v;
    } while (end > BAND_START[band]);
  }

  // Excitation function.
  const bool is_lfe = lfeon_ && ch == lfe_ch_;
  const int fgain = fgain_[ch];
  const int bndstrt = BIN_TO_BAND[start];
  const int bndend = BIN_TO_BAND[end - 1] + 1;
  int excite[CRITICAL_BANDS] = {};
  int begin;
  int fastleak;
  int slowleak;
  if (bndstrt == 0) {
    int lowcomp = calcLowComp1(0, bndpsd[0], bndpsd[1], 384);
    excite[0] = bndpsd[0] - fgain - lowcomp;
    lowcomp = calcLowComp1(lowcomp, bndpsd[1], bndpsd[2], 384);
    excite[1] = bndpsd[1] - fgain - lowcomp;
    begin = 7;
    fastleak = 0;
    slowleak = 0;
    for (int band = 2; band < 7; ++band) {
      if (!(is_lfe && band == 6)) lowcomp = calcLowComp1(lowcomp, bndpsd[band], bndpsd[band + 1], 384);
      fastleak = bndpsd[band] - fgain;
      slowleak = bndpsd[band] - sgain_;
      excite[band] = fastleak - lowcomp;
      if (!(is_lfe && band == 6) && bndpsd[band] <= bndpsd[band + 1]) {
        begin = band + 1;
        break;
      }
    }
    const int end1 = std::min(bndend, 22);
    for (int band = begin; band < end1; ++band) {
      if (!(is_lfe && band == 6)) lowcomp = calcLowComp(lowcomp, bndpsd[band], bndpsd[band + 1], band);
      fastleak = std::max(fastleak - fdecay_, bndpsd[band] - fgain);
      slowleak = std::max(slowleak - sdecay_, bndpsd[band] - sgain_);
      excite[band] = std::max(fastleak - lowcomp, slowleak);
    }
    begin = 22;
  } else {
    // Coupling channel.
    begin = bndstrt;
    fastleak = (cplfleak_ << 8) + 768;
    slowleak = (cplsleak_ << 8) + 768;
  }
  for (int band = begin; band < bndend; ++band) {
    fastleak = std::max(fastleak - fdecay_, bndpsd[band] - fgain);
    slowleak = std::max(slowleak - sdecay_, bndpsd[band] - sgain_);
    excite[band] = std::max(fastleak, slowleak);
  }

  // Masking curve.
  int mask[CRITICAL_BANDS] = {};
  for (int band = bndstrt; band < bndend; ++band) {
    if (bndpsd[band] < dbknee_) excite[band] += (dbknee_ - bndpsd[band]) >> 2;
    mask[band] = std::max<int>(HEARING_THRESHOLD[band >> ba_shift_][ba_fscod_], excite[band]);
  }

  // Delta bit allocation (A/52 Section 7.2.2.6). Segment offsets are
  // relative to band 0.
  if (!is_lfe && (dba_mode_[ch] == DBA_REUSE || dba_mode_[ch] == DBA_NEW)) {
    int band = 0;
    for (int seg = 0; seg < dba_nseg_[ch]; ++seg) {
      band += dba_offset_[ch][seg];
      const int value = dba_value_[ch][seg];
      const int delta = (value >= 4 ? value - 3 : value - 4) * 128;
      for (int i = 0; i < dba_length_[ch][seg] && band < CRITICAL_BANDS; ++i) mask[band++] += delta;
    }
  }

  // Bit allocation pointers.
  const int snroffset = (((csnroffst_ - 15) << 4) + fsnroffst_[ch]) << 2;
  const uint8_t* table = chahtinu_[ch] ? HEBAP_TABLE : BAP_TABLE;
  int bin = start;
  int band = bndstrt;
  int band_end;
  do {
    const int m = (std::max(mask[band] - snroffset - floor_, 0) & 0x1fe0) + floor_;
    band_end = std::min<int>(BAND_START[++band], end);
    for (; bin < band_end; ++bin) {
      const int address = std::clamp((psd[bin] - m) >> 5, 0, 63);
      bap[bin] = table[address];
    }
  } while (end > band_end);
}

void SubstreamDecoder::decodeCoefficients(BitReader& br, int blk) {
  // The coupling channel follows the first coupled channel.
  MantissaGroups groups;
  bool got_cpl = false;
  for (int ch = 1; ch <= nchans_; ++ch) {
    decodeMantissas(br, ch, blk, groups);
    if (cplinu_blk_ && chincpl_[ch] && !got_cpl) {
      decodeMantissas(br, 0, blk, groups);
      got_cpl = true;
    }
  }
}

void SubstreamDecoder::decodeAhtMantissas(BitReader& br, int ch) {
  const int start = start_[ch];
  const int end = end_[ch];
  const uint8_t* hebap = bap_[ch];

  // Gain adaptive quantization (TS 102 366 Section E.3.4).
  const int gaqmod = static_cast<int>(br.readBits(2));
  const int endbap = gaqmod < 2 ? 12 : 17;
  uint8_t gains[MAX_COEFS + 2];
  int ngains = 0;
  if (gaqmod == 1 || gaqmod == 2) {
    for (int bin = start; bin < end; ++bin) {
      if (hebap[bin] > 7 && hebap[bin] < endbap) gains[ngains++] = br.readFlag() ? static_cast<uint8_t>(gaqmod) : 0;
    }
  } else if (gaqmod == 3) {
    // Three gains share one 5-bit code.
    for (int bin = start; bin < end; ++bin) {
      if (hebap[bin] <= 7 || hebap[bin] >= endbap) continue;
      if (ngains % 3 == 0) {
        const uint32_t code = std::min<uint32_t>(br.readBits(5), 26);
        gains[ngains] = static_cast<uint8_t>(code / 9);
        gains[ngains + 1] = static_cast<uint8_t>(code % 9 / 3);
        gains[ngains + 2] = static_cast<uint8_t>(code % 3);
      }
      ++ngains;
    }
  }

  int gain_index = 0;
  for (int bin = start; bin < end; ++bin) {
    float* mant = aht_mant_[ch][bin];
    const int b = hebap[bin];
    const int bits = HEBAP_BITS[b];
    if (b == 0) {
      std::fill_n(mant, 6, 0.0f);
      continue;
    }
    if (b < 8) {
      // Vector quantization.
      const int16_t* vector = AHT_VQ_CODEBOOKS[b][br.readBits(bits)];
      for (int blk = 0; blk < 6; ++blk) mant[blk] = static_cast<float>(vector[blk]) * (1.0f / 32768.0f);
    } else {
      // Scalar quantization, optionally with gain adaptive quantization.
      const int log_gain = (gaqmod != 0 && b < endbap) ? gains[gain_index++] : 0;
      const int gbits = bits - log_gain;
      const float small_scale = 1.0f / static_cast<float>(1 << (bits - 1));
      for (int blk = 0; blk < 6; ++blk) {
        const int32_t code = br.readSigned(gbits);
        if (log_gain != 0 && code == -(1 << (gbits - 1))) {
          // Large mantissa, remapped to correct for the dead zone.
          const int mbits = bits - (2 - log_gain);
          const float x = static_cast<float>(br.readSigned(mbits)) / static_cast<float>(1 << (mbits - 1));
          const float a = GAQ_REMAP_A[b - 8][log_gain - 1] / 32768.0f;
          const float offset = x >= 0.0f ? (log_gain == 1 ? 0.5f : 0.25f) : GAQ_REMAP_NEG_B[b - 8][log_gain - 1] / 32768.0f;
          mant[blk] = x + a * x + offset;
        } else {
          float x = static_cast<float>(code) * small_scale;
          if (log_gain == 0) x += GAQ_REMAP_GAIN1[b - 8] / 32768.0f * x;
          mant[blk] = x;
        }
      }
    }
    inverseAhtTransform(mant);
  }
}

void SubstreamDecoder::decodeMantissas(BitReader& br, int ch, int blk, MantissaGroups& groups) {
  const int start = start_[ch];
  const int end = end_[ch];
  const int8_t* exps = exps_[ch];
  const uint8_t* bap = bap_[ch];
  float* coefs = coefs_[ch];
  const bool dither_on = ch == 0 || dithflag_[ch];

  std::fill(coefs, coefs + start, 0.0f);
  std::fill(coefs + std::max(end, start), coefs + MAX_COEFS, 0.0f);

  if (chahtinu_[ch]) {
    if (blk == 0) decodeAhtMantissas(br, ch);
    for (int bin = start; bin < end; ++bin) {
      const float scale = MANTISSA.exp_scale[exps[bin]];
      if (bap[bin] == 0) {
        coefs[bin] = dither_on ? dither() * scale : 0.0f;
      } else {
        coefs[bin] = aht_mant_[ch][bin][blk] * scale;
      }
    }
    return;
  }

  for (int bin = start; bin < end; ++bin) {
    float value;
    switch (bap[bin]) {
      case 0:
        value = dither_on ? dither() : 0.0f;
        break;
      case 1:
        if (groups.n1 > 0) {
          value = groups.b1[--groups.n1];
        } else {
          const auto& g = MANTISSA.bap1[br.readBits(5)];
          value = g[0];
          groups.b1[1] = g[1];
          groups.b1[0] = g[2];
          groups.n1 = 2;
        }
        break;
      case 2:
        if (groups.n2 > 0) {
          value = groups.b2[--groups.n2];
        } else {
          const auto& g = MANTISSA.bap2[br.readBits(7)];
          value = g[0];
          groups.b2[1] = g[1];
          groups.b2[0] = g[2];
          groups.n2 = 2;
        }
        break;
      case 3:
        value = MANTISSA.bap3[br.readBits(3)];
        break;
      case 4:
        if (groups.n4 > 0) {
          value = groups.b4;
          groups.n4 = 0;
        } else {
          const auto& g = MANTISSA.bap4[br.readBits(7)];
          value = g[0];
          groups.b4 = g[1];
          groups.n4 = 1;
        }
        break;
      case 5:
        value = MANTISSA.bap5[br.readBits(4)];
        break;
      default: {
        const int bits = BAP_BITS[std::min<int>(bap[bin], 15)];
        value = static_cast<float>(br.readSigned(bits)) / static_cast<float>(1 << (bits - 1));
        break;
      }
    }
    coefs[bin] = value * MANTISSA.exp_scale[exps[bin]];
  }
}

void SubstreamDecoder::uncouple() {
  int bin = start_[0];
  for (int bnd = 0; bnd < ncplbnds_; ++bnd) {
    const int band_end = bin + cpl_band_sizes_[bnd];
    for (int ch = 1; ch <= nfchans_; ++ch) {
      if (!chincpl_[ch]) continue;
      float co = cplco_[ch][bnd];
      if (acmod_ == 2 && ch == 2 && phsflg_[bnd]) co = -co;
      for (int b = bin; b < band_end; ++b) {
        // Channels without dither keep zero-bit mantissas at zero.
        coefs_[ch][b] = (!dithflag_[ch] && bap_[0][b] == 0) ? 0.0f : coefs_[0][b] * co;
      }
    }
    bin = band_end;
  }
}

void SubstreamDecoder::rematrix() {
  const int end = std::min(end_[1], end_[2]);
  for (int bnd = 0; bnd < nrematbnds_; ++bnd) {
    if (!rematflg_[bnd]) continue;
    const int band_end = std::min<int>(end, REMATRIX_BANDS[bnd + 1]);
    for (int bin = REMATRIX_BANDS[bnd]; bin < band_end; ++bin) {
      const float l = coefs_[1][bin];
      const float r = coefs_[2][bin];
      coefs_[1][bin] = l + r;
      coefs_[2][bin] = l - r;
    }
  }
}

void SubstreamDecoder::applyDynamicRange() {
  for (int ch = 1; ch <= nchans_; ++ch) {
    // Dual mono carries a separate gain word for the second channel.
    float gain = dynrng_[(acmod_ == 0 && ch == 2) ? 1 : 0];
    if (drc_scale_ != 1.0f) gain = std::pow(gain, drc_scale_);
    if (gain == 1.0f) continue;
    std::transform(coefs_[ch], coefs_[ch] + MAX_COEFS, coefs_[ch], [gain](float c) { return c * gain; });
  }
}

void SubstreamDecoder::applySpectralExtension() {
  // Map the extension bands onto copies of the translation region
  // [spx_dst_start_, spx_src_start_), wrapping around when it runs out.
  bool wrap[17] = {true};
  int copy_sizes[40];
  int ncopies = 0;
  int bin = spx_dst_start_;
  for (int bnd = 0; bnd < nspxbnds_; ++bnd) {
    const int size = spx_band_sizes_[bnd];
    if (bin + size > spx_src_start_) {
      copy_sizes[ncopies++] = bin - spx_dst_start_;
      bin = spx_dst_start_;
      wrap[bnd] = true;
    }
    for (int i = 0; i < size;) {
      if (bin == spx_src_start_) {
        copy_sizes[ncopies++] = bin - spx_dst_start_;
        bin = spx_dst_start_;
      }
      const int n = std::min(size - i, spx_src_start_ - bin);
      bin += n;
      i += n;
    }
  }
  copy_sizes[ncopies++] = bin - spx_dst_start_;

  for (int ch = 1; ch <= nfchans_; ++ch) {
    if (!chinspx_[ch]) continue;
    float* coefs = coefs_[ch];

    bin = spx_src_start_;
    for (int i = 0; i < ncopies; ++i) {
      std::copy_n(coefs + spx_dst_start_, copy_sizes[i], coefs + bin);
      bin += copy_sizes[i];
    }

    float rms[17];
    bin = spx_src_start_;
    for (int bnd = 0; bnd < nspxbnds_; ++bnd) {
      const int size = spx_band_sizes_[bnd];
      float energy = 0.0f;
      for (int i = 0; i < size; ++i, ++bin) energy += coefs[bin] * coefs[bin];
      rms[bnd] = std::sqrt(energy / static_cast<float>(size));
    }

    // Notch filter at the band edges where the copy wraps (TS 102 366 Section E.3.3.8).
    if (spx_atten_[ch] >= 0) {
      const int code = spx_atten_[ch];
      float atten[3];
      for (int i = 0; i < 3; ++i) atten[i] = std::exp2(-static_cast<float>((i + 1) * (code + 1)) / 15.0f);
      bin = spx_src_start_ - 2;
      for (int bnd = 0; bnd < nspxbnds_; ++bnd) {
        if (wrap[bnd]) {
          coefs[bin] *= atten[0];
          coefs[bin + 1] *= atten[1];
          coefs[bin + 2] *= atten[2];
          coefs[bin + 3] *= atten[1];
          coefs[bin + 4] *= atten[0];
        }
        bin += spx_band_sizes_[bnd];
      }
    }

    bin = spx_src_start_;
    for (int bnd = 0; bnd < nspxbnds_; ++bnd) {
      const float noise_scale = spx_noise_blend_[ch][bnd] * rms[bnd];
      const float signal_scale = spx_signal_blend_[ch][bnd];
      for (int i = 0; i < spx_band_sizes_[bnd]; ++i, ++bin) {
        coefs[bin] = coefs[bin] * signal_scale + noise() * noise_scale;
      }
    }
  }
}

void SubstreamDecoder::synthesize(int blk, const PcmTarget& target) {
  for (int ch = 1; ch <= nchans_; ++ch) {
    // Dropped channels still run the transform to keep their overlap current.
    const int slot = target.slots[ch - 1];
    float* out = slot >= 0 ? target.data + static_cast<size_t>(blk) * BLOCK_SIZE * target.stride + slot : discard_;
    const int stride = slot >= 0 ? target.stride : 1;
    if (blksw_[ch]) {
      imdct_.shortBlocks(coefs_[ch], delay_[ch - 1], out, stride);
    } else {
      imdct_.longBlock(coefs_[ch], delay_[ch - 1], out, stride);
    }
  }
}

} // namespace openmedia::ac3
