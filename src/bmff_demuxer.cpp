#include <algorithm>
#include <annexb.hpp>
#include <cstdint>
#include <cstring>
#include <future>
#include <numeric>
#include <openmedia/audio.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <span>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

static constexpr uint32_t AAC_SAMPLE_RATES[] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350};

static constexpr uint8_t AAC_CHANNELS[] = {0, 1, 2, 3, 4, 5, 6, 8};

static consteval auto ATOM(char a, char b, char c, char d) -> uint32_t {
  return magic_u32(a, b, c, d);
}

template<std::size_t N>
static constexpr auto containsAtom(const uint32_t (&table)[N], uint32_t type) -> bool {
  for (const uint32_t t : table) {
    if (t == type) return true;
  }
  return false;
}

static auto isContainerBox(uint32_t type) -> bool {
  static constexpr uint32_t kTable[] = {
      ATOM('m', 'o', 'o', 'v'),
      ATOM('t', 'r', 'a', 'k'),
      ATOM('e', 'd', 't', 's'),
      ATOM('m', 'd', 'i', 'a'),
      ATOM('m', 'i', 'n', 'f'),
      ATOM('d', 'i', 'n', 'f'),
      ATOM('s', 't', 'b', 'l'),
      ATOM('u', 'd', 't', 'a'),
      ATOM('m', 'e', 't', 'a'),
      ATOM('i', 'l', 's', 't'),
  };
  return containsAtom(kTable, type);
}

static auto isIgnoredBox(uint32_t type) -> bool {
  static constexpr uint32_t kTable[] = {
      ATOM('f', 't', 'y', 'p'),
      ATOM('f', 'r', 'e', 'e'),
      ATOM('s', 'k', 'i', 'p'),
      ATOM('w', 'i', 'd', 'e'),
      ATOM('p', 'n', 'o', 't'),
      ATOM('j', 'P', '2', ' '),
  };
  return containsAtom(kTable, type);
}

static auto isAvcVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t kTable[] = {
      ATOM('a', 'v', 'c', '1'),
      ATOM('a', 'v', 'c', '2'),
      ATOM('a', 'v', 'c', '3'),
      ATOM('a', 'v', 'c', '4'),
      ATOM('H', '2', '6', '4'),
      ATOM('X', '2', '6', '4'),
  };
  return containsAtom(kTable, fmt);
}

static auto isHevcVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t kTable[] = {
      ATOM('h', 'v', 'c', '1'),
      ATOM('h', 'e', 'v', '1'),
      ATOM('H', 'E', 'V', 'C'),
  };
  return containsAtom(kTable, fmt);
}

static auto isVvcVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t k_table[] = {
      ATOM('v', 'v', 'c', '1'),
      ATOM('v', 'v', 'i', '1'),
  };
  return containsAtom(k_table, fmt);
}

static auto isMp4aVariant(uint32_t fmt) -> bool {
  return fmt == ATOM('m', 'p', '4', 'a') || fmt == ATOM('M', 'P', '4', 'A');
}

static auto isPcmVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t kTable[] = {
      ATOM('r', 'a', 'w', ' '),
      ATOM('t', 'w', 'o', 's'),
      ATOM('s', 'o', 'w', 't'),
      ATOM('i', 'n', '2', '4'),
      ATOM('i', 'n', '3', '2'),
      ATOM('f', 'l', '3', '2'),
      ATOM('f', 'l', '6', '4'),
  };
  return containsAtom(kTable, fmt);
}

// ---------------------------------------------------------------------------
// ISO 14496-12 colour/transfer mappings - switch instead of cascaded ifs
// ---------------------------------------------------------------------------

static auto colorSpaceFromPrimaries(uint16_t p) -> OMColorSpace {
  switch (p) {
    case 1: return OM_COLOR_SPACE_BT709;
    case 5: // fall-through: both map to BT601
    case 6: return OM_COLOR_SPACE_BT601;
    case 9: return OM_COLOR_SPACE_BT2020;
    default: return OM_COLOR_SPACE_UNKNOWN;
  }
}

static auto transferCharFromCode(uint16_t t) -> OMTransferCharacteristic {
  switch (t) {
    case 1: return OM_TRANSFER_BT709;
    case 6: return OM_TRANSFER_BT601;
    case 13: return OM_TRANSFER_IEC61966_2_1;
    case 16: return OM_TRANSFER_SMPTE2084;
    case 18: return OM_TRANSFER_ARIB_STD_B67;
    default: return OM_TRANSFER_UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Parse AudioSpecificConfig (ISO 14496-3)
// Returns true and fills out_sample_rate / out_channels / out_profile.
// ---------------------------------------------------------------------------

static auto parseAudioSpecificConfig(std::span<const uint8_t> asc,
                                     uint32_t& out_sample_rate,
                                     uint32_t& out_channels,
                                     OMProfile& out_profile) -> bool {
  if (asc.empty()) return false;

  uint32_t bit_pos = 0;
  const uint32_t total_bits = static_cast<uint32_t>(asc.size()) * 8u;

  auto readBits = [&](uint32_t n) -> uint32_t {
    if (n == 0 || bit_pos + n > total_bits) {
      bit_pos = total_bits;
      return 0;
    }
    uint32_t val = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t bi = bit_pos / 8u;
      const uint32_t bk = 7u - (bit_pos % 8u);
      val = (val << 1u) | ((asc[bi] >> bk) & 1u);
      ++bit_pos;
    }
    return val;
  };
  auto bitsLeft = [&]() -> uint32_t {
    return (bit_pos < total_bits) ? (total_bits - bit_pos) : 0u;
  };
  auto saveBitPos    = [&]() -> uint32_t { return bit_pos; };
  auto restoreBitPos = [&](uint32_t saved) { bit_pos = saved; };

  uint32_t aot = readBits(5);
  if (aot == 31) {
    aot = 32u + readBits(6);
  }

  const uint32_t sr_idx = readBits(4);
  uint32_t sample_rate = 0;
  if (sr_idx == 0xF) {
    sample_rate = readBits(24);
  } else if (sr_idx < std::size(AAC_SAMPLE_RATES)) {
    sample_rate = AAC_SAMPLE_RATES[sr_idx];
  }

  const uint32_t ch_cfg = readBits(4);
  uint32_t channels = (ch_cfg < std::size(AAC_CHANNELS)) ? AAC_CHANNELS[ch_cfg] : 0u;

  uint32_t ext_sr    = 0;
  bool sbr_found     = false;
  bool ps_found      = false;

  if ((aot == 5 || aot == 29) && bitsLeft() >= 4) {
    if (aot == 29) ps_found = true;

    const uint32_t ext_sr_idx = readBits(4);
    uint32_t candidate_ext_sr = 0;
    if (ext_sr_idx == 0xF && bitsLeft() >= 24) {
      candidate_ext_sr = readBits(24);
    } else if (ext_sr_idx < std::size(AAC_SAMPLE_RATES)) {
      candidate_ext_sr = AAC_SAMPLE_RATES[ext_sr_idx];
    }

    uint32_t core_aot = readBits(5);
    if (core_aot == 31) core_aot = 32u + readBits(6);

    sbr_found = true;
    ext_sr    = candidate_ext_sr;
    aot       = core_aot;
  }

  // --- GASpecificConfig — must be consumed before the 0x2B7 extension scan ---
  // Applies to AOT 1 (AAC-Main), 2 (AAC-LC), 3 (AAC-SSR), 4 (AAC-LTP),
  //            6 (AAC-Scalable), 7 (TwinVQ).
  //frameLengthFlag (1) + dependsOnCoreCoder (1) [+ coreCoderDelay (14)] + extensionFlag (1)

  if (aot >= 1 && aot <= 7) {
    readBits(1); // frameLengthFlag
    if (readBits(1)) readBits(14); // dependsOnCoreCoder → coreCoderDelay
    readBits(1); // extensionFlag
  }

  // --- Implicit backwards-compatible SBR/PS signaling via 0x2B7 sync word ---

  if (!sbr_found && bitsLeft() >= 11) {
    const uint32_t saved = saveBitPos();
    if (readBits(11) == 0x2B7u) {
      uint32_t ext_aot = readBits(5);
      if (ext_aot == 31) ext_aot = 32u + readBits(6);

      if (ext_aot == 5 && bitsLeft() >= 1 && readBits(1)) {
        sbr_found = true;

        const uint32_t ext_sr_idx = readBits(4);
        uint32_t candidate_ext_sr = 0;
        if (ext_sr_idx == 0xF && bitsLeft() >= 24) {
          candidate_ext_sr = readBits(24);
        } else if (ext_sr_idx < std::size(AAC_SAMPLE_RATES)) {
          candidate_ext_sr = AAC_SAMPLE_RATES[ext_sr_idx];
        }
        ext_sr = candidate_ext_sr;

        // Check further for PS inside the same extension block
        if (bitsLeft() >= 12) {
          const uint32_t saved2 = saveBitPos();
          if (readBits(11) == 0x2B7u) {
            uint32_t ext_aot2 = readBits(5);
            if (ext_aot2 == 31) ext_aot2 = 32u + readBits(6);
            if (ext_aot2 == 29 && bitsLeft() >= 1 && readBits(1)) {
              ps_found = true;
            } else {
              restoreBitPos(saved2);
            }
          } else {
            restoreBitPos(saved2);
          }
        }
      } else {
        restoreBitPos(saved);
      }
    } else {
      restoreBitPos(saved);
    }
  }

  // --- Write outputs ---

  if (sample_rate)            out_sample_rate = sample_rate;
  if (sbr_found && ext_sr)   out_sample_rate = ext_sr;
  if (channels)               out_channels    = channels;
  if (ps_found && out_channels == 1) out_channels = 2; // PS always upmixes mono → stereo

  uint32_t final_aot = aot;
  if (ps_found)       final_aot = 29;
  else if (sbr_found) final_aot = 5;
  if (final_aot > 0)  out_profile = static_cast<OMProfile>(final_aot);

  return true;
}

struct STSCEntry {
  uint32_t first_chunk;
  uint32_t samples_per_chunk;
  uint32_t sample_description_index;
};

struct STTSEntry {
  uint32_t sample_count;
  uint32_t sample_delta;
};

struct CTTSEntry {
  uint32_t sample_count;
  int32_t sample_offset;
};

struct ELSTEntry {
  int64_t segment_duration = 0;
  int64_t media_time = 0;
  int32_t media_rate = 1;
};

struct Sample {
  int64_t offset = 0;
  uint32_t size = 0;
  int64_t pts = 0;
  int64_t dts = 0;
  uint32_t duration = 0;
  int32_t stream_index = 0;
  bool is_keyframe = false;
};

struct BMFFTrack {
  uint32_t timescale = 0;
  uint32_t handler = 0;
  int32_t index = -1;
  int64_t start_dts = 0;
  int64_t track_duration = 0;
  Track track = {};
  std::unique_ptr<AnnexBFilter> annexb_bsf;

  std::vector<uint32_t> sample_sizes;
  std::vector<int64_t> chunk_offsets;
  std::vector<uint32_t> sync_samples;
  std::vector<STSCEntry> stsc_entries;
  std::vector<STTSEntry> stts_entries;
  std::vector<CTTSEntry> ctts_entries;
  std::vector<ELSTEntry> elst_entries;
  std::vector<Sample> samples;
};

// ---------------------------------------------------------------------------
// Leaf-box parsers - accept a pre-read buffer
// ---------------------------------------------------------------------------

inline void parseMvhd(std::span<const uint8_t> body, uint32_t& out_movie_timescale) {
  BufReader r(body.data(), body.size());
  const uint8_t version = r.read_u8();
  r.skip(3);
  r.skip(version == 1 ? 16 : 8);
  out_movie_timescale = r.read_u32_be();
}

inline void parseTkhd(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  const uint8_t version = r.read_u8();
  r.skip(3);
  r.skip(version == 1 ? 16 : 8);
  track.track.id = static_cast<int32_t>(r.read_u32_be());
}

inline void parseMdhd(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  const uint8_t version = r.read_u8();
  r.skip(3);
  if (version == 1) {
    r.skip(16);
    track.timescale = static_cast<uint32_t>(r.read_u64_be());
    track.track_duration = r.read_i64_be();
  } else {
    r.skip(8);
    track.timescale = r.read_u32_be();
    track.track_duration = static_cast<int64_t>(r.read_u32_be());
  }
}

inline void parseHdlr(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(8); // version + flags + pre_defined
  const uint32_t h = r.read_u32_le();
  track.handler = h;
  switch (h) {
    case ATOM('s', 'o', 'u', 'n'): track.track.format.type = OM_MEDIA_AUDIO; break;
    case ATOM('v', 'i', 'd', 'e'): track.track.format.type = OM_MEDIA_VIDEO; break;
    default: track.track.format.type = OM_MEDIA_NONE; break;
  }
}

inline void parseElst(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  const uint8_t version = r.read_u8();
  r.skip(3);
  const uint32_t count = r.read_u32_be();

  track.elst_entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    ELSTEntry e;
    if (version == 1) {
      e.segment_duration = r.read_i64_be();
      e.media_time = r.read_i64_be();
    } else {
      e.segment_duration = static_cast<int64_t>(r.read_u32_be());
      e.media_time = static_cast<int64_t>(r.read_i32_be());
    }
    e.media_rate = r.read_i32_be() >> 16;
    track.elst_entries.push_back(e);
  }
}

// Apply the first valid edit-list entry (media_rate == 1, media_time >= 0).
inline void applyEditList(BMFFTrack& track) {
  const auto it = std::find_if(
      track.elst_entries.begin(), track.elst_entries.end(),
      [](const ELSTEntry& e) { return e.media_time >= 0 && e.media_rate == 1; });
  if (it != track.elst_entries.end())
    track.start_dts = it->media_time;
}

inline void parseColr(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 4) return;
  BufReader r(body.data(), body.size());
  const uint32_t colour_type = r.read_u32_le();

  uint16_t primaries = 0, transfer = 0;
  switch (colour_type) {
    case ATOM('n', 'c', 'l', 'x'):
      if (body.size() < 11) return;
      primaries = r.read_u16_be();
      transfer = r.read_u16_be();
      break;
    case ATOM('n', 'c', 'l', 'c'):
    case ATOM('r', 'I', 'C', 'C'):
    case ATOM('p', 'r', 'o', 'f'):
      if (body.size() < 10) return;
      primaries = r.read_u16_be();
      transfer = r.read_u16_be();
      break;
    default:
      return;
  }

  track.track.format.video.color_space = colorSpaceFromPrimaries(primaries);
  track.track.format.video.transfer_char = transferCharFromCode(transfer);
}

inline void parseBtrt(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 12) return;
  BufReader r(body.data(), body.size());
  r.skip(8);
  if (const uint32_t avg = r.read_u32_be(); avg) {
    track.track.bitrate = avg;
  }
}

/*inline auto parseAvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  if (body.size() < 4) return false;
  BufReader r(body.data(), body.size());
  r.skip(1); // configurationVersion
  const uint8_t profile = r.read_u8();
  const uint8_t constraints = r.read_u8();
  if (!r.ok()) return false;

  track.track.format.profile =
      (profile == 66 && (constraints & 0x40))
          ? OM_PROFILE_H264_CONSTRAINED_BASELINE
          : static_cast<OMProfile>(profile);
  track.track.extradata.assign(body.begin(), body.end());
  return true;
}*/

inline auto parseAvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  if (body.size() < 7) return false;
  BufReader r(body.data(), body.size());

  r.skip(4); // configurationVersion, profile, constraints, level

  const uint8_t nalu_len_sz = (r.read_u8() & 0x03u) + 1u;

  std::vector<uint8_t> annexb_extra;
  auto extract_nals = [&](uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
      uint16_t nal_size = r.read_u16_be();
      if (r.remaining() < nal_size) break;
      annexb_extra.insert(annexb_extra.end(),
                          AnnexBFilter::START_CODE_LONG,
                          AnnexBFilter::START_CODE_LONG + 4);
      const uint8_t* ptr = r.cur();
      annexb_extra.insert(annexb_extra.end(), ptr, ptr + nal_size);
      r.skip(nal_size);
    }
  };

  uint8_t num_sps = r.read_u8() & 0x1Fu;
  extract_nals(num_sps);
  if (r.remaining() > 0) {
    uint8_t num_pps = r.read_u8();
    extract_nals(num_pps);
  }

  track.track.extradata = annexb_extra; // keep for callers that inspect it
  track.annexb_bsf = std::make_unique<AnnexBFilter>(
      nalu_len_sz, std::move(annexb_extra));
  return true;
}

inline auto parseHvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  if (body.size() < 23) return false;

  BufReader r(body.data(), body.size());

  r.skip(21); // skip header up to lengthSizeMinusOne

  const uint8_t nalu_len_sz = (r.read_u8() & 0x03u) + 1u;
  const uint8_t num_arrays = r.read_u8();

  std::vector<uint8_t> annexb_extra;

  for (uint8_t i = 0; i < num_arrays; ++i) {
    if (r.remaining() < 3) break;

    uint8_t nal_type = r.read_u8() & 0x3Fu;
    (void) nal_type;

    uint16_t num_nalus = r.read_u16_be();

    for (uint16_t j = 0; j < num_nalus; ++j) {
      if (r.remaining() < 2) break;

      uint16_t nal_size = r.read_u16_be();
      if (r.remaining() < nal_size) break;

      annexb_extra.insert(annexb_extra.end(),
                          AnnexBFilter::START_CODE_LONG,
                          AnnexBFilter::START_CODE_LONG + 4);

      const uint8_t* ptr = r.cur();
      annexb_extra.insert(annexb_extra.end(), ptr, ptr + nal_size);

      r.skip(nal_size);
    }
  }

  track.track.extradata = annexb_extra;
  track.annexb_bsf = std::make_unique<AnnexBFilter>(
      nalu_len_sz, std::move(annexb_extra));

  return true;
}

inline auto parseVvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  if (body.size() < 8) return false;

  BufReader r(body.data(), body.size());

  r.skip(1); // configurationVersion

  const uint8_t flags = r.read_u8();
  const uint8_t nalu_len_sz = ((flags >> 1) & 0x03u) + 1u;
  const bool ptl_present = (flags & 0x01u) != 0;

  if (ptl_present) {
    if (r.remaining() < 2) return false;

    uint8_t ci_byte = r.read_u8();
    uint8_t num_bytes_constraint_info = ci_byte >> 2;
    uint8_t num_sub_layers = r.read_u8() & 0x07u;

    if (r.remaining() < 2) return false;
    r.skip(2);

    if (r.remaining() < num_bytes_constraint_info) return false;
    r.skip(num_bytes_constraint_info);

    for (uint8_t i = 0; i < num_sub_layers; ++i) {
      if (r.remaining() < 1) return false;
      uint8_t sublayer_flags = r.read_u8();

      if (sublayer_flags & 0x80u) { // profile_present_flag
        if (r.remaining() < 2) return false;
        r.skip(2);

        if (r.remaining() < num_bytes_constraint_info) return false;
        r.skip(num_bytes_constraint_info);
      }

      if (sublayer_flags & 0x40u) { // level_present_flag
        if (r.remaining() < 1) return false;
        r.skip(1);
      }
    }
  }

  if (r.remaining() < 1) {
    return false;
  }
  uint8_t num_arrays = r.read_u8();

  std::vector<uint8_t> annexb_extra;

  for (uint8_t i = 0; i < num_arrays; ++i) {
    if (r.remaining() < 3) break;

    uint8_t nal_unit_type = r.read_u8() & 0x3Fu;
    uint16_t num_nalus = r.read_u16_be();

    for (uint16_t j = 0; j < num_nalus; ++j) {
      if (r.remaining() < 2) break;

      uint16_t nal_size = r.read_u16_be();
      if (r.remaining() < nal_size) break;

      annexb_extra.insert(annexb_extra.end(),
                          AnnexBFilter::START_CODE_LONG,
                          AnnexBFilter::START_CODE_LONG + 4);

      const uint8_t* ptr = r.cur();
      annexb_extra.insert(annexb_extra.end(), ptr, ptr + nal_size);

      r.skip(nal_size);
    }
  }

  fprintf(stderr, "num_arrays=%u, annexb_extra.size()=%zu, remaining=%zu\n",
       num_arrays, annexb_extra.size(), r.remaining());

  if (annexb_extra.empty()) {
    return false;
  }

  track.track.extradata = annexb_extra;
  track.annexb_bsf = std::make_unique<AnnexBFilter>(
      nalu_len_sz, std::move(annexb_extra));

  return true;
}

inline void parseEvcc(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 12) return;

  const uint8_t profile_idc = body[1];

  const uint8_t bit_depth_luma = ((body[5] >> 3) & 0x07u) + 8u;

  track.track.format.audio.bit_depth = 0;
  (void) bit_depth_luma;

  track.track.format.profile = static_cast<OMProfile>(profile_idc);

  track.track.extradata.assign(body.begin(), body.end());
}

inline void parseVpcc(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 5) return;
  track.track.format.profile = static_cast<OMProfile>(body[4]);
  track.track.extradata.assign(body.begin() + 4, body.end());
}

inline void parseAv1c(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 2) return;
  track.track.format.profile = static_cast<OMProfile>((body[1] >> 5) & 0x07);
  track.track.extradata.assign(body.begin(), body.end());
}

inline void parseDops(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 11) return;
  track.track.format.audio.channels = body[1];
  const uint32_t input_rate = load_u32_be(body.data() + 3);
  track.track.format.audio.sample_rate = input_rate ? input_rate : 48000u;
  track.track.extradata.assign(body.begin(), body.end());
}

inline void parseEsds(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4); // version + flags

  auto readDescLen = [&]() -> uint32_t {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t b = r.read_u8();
      len = (len << 7) | (b & 0x7Fu);
      if (!(b & 0x80u)) break;
    }
    return len;
  };

  if (r.read_u8() != 0x03) return;
  const uint32_t es_len = readDescLen();
  const size_t es_end = r.tell() + es_len;
  if (es_end > r.size()) return;

  r.skip(2); // ES_ID
  const uint8_t es_flags = r.read_u8();
  if (es_flags & 0x80u) { r.skip(2); }           // streamDependenceFlag → dependsOn_ES_ID
  if (es_flags & 0x40u) { r.skip(r.read_u8()); } // URL_Flag → URL_length + URL_string
  if (es_flags & 0x20u) { r.skip(2); }           // OCRstreamFlag → OCR_ES_Id

  while (r.tell() < es_end) {
    const uint8_t tag = r.read_u8();
    const uint32_t len = readDescLen();
    const size_t next = r.tell() + len;
    if (next > r.size()) break;

    if (tag == 0x04 && len >= 13) {
      // DecoderConfigDescriptor
      r.skip(1); // objectTypeIndication
      r.skip(1); // streamType (6 bits) + upStream (1 bit) + reserved (1 bit)
      r.skip(3); // bufferSizeDB
      r.skip(4); // maxBitrate
      if (const uint32_t avg = r.read_u32_be(); avg) {
        track.track.bitrate = avg;
      }

      while (r.tell() + 2 <= next) {
        const uint8_t sub_tag = r.read_u8();
        const uint32_t sub_len = readDescLen();
        if (sub_tag == 0x05 && sub_len > 0) {
          // DecoderSpecificInfo — read raw then strip trailing zeros down to 2-byte minimum
          auto raw = r.read_bytes(sub_len);
          while (raw.size() > 2 && raw.back() == 0x00) {
            raw.pop_back();
          }
          track.track.extradata = raw;

          uint32_t sr = track.track.format.audio.sample_rate;
          uint32_t channels = track.track.format.audio.channels;
          OMProfile prof = track.track.format.profile;
          if (parseAudioSpecificConfig(raw, sr, channels, prof)) {
            track.track.format.audio.sample_rate = sr;
            track.track.format.audio.channels = channels;
            track.track.format.profile = prof;
          }
        } else {
          r.skip(sub_len);
        }
      }
    }
    r.seek(next);
  }
}

inline void parseAlacSpecific(std::span<const uint8_t> body, BMFFTrack& track) {
  // ALACSpecificBox: 4-byte version/flags + 24-byte ALACSpecificConfig
  if (body.size() < 28) return;
  BufReader r(body.data(), body.size());
  r.skip(4); // version + flags
  const uint8_t* c = r.cur();
  track.track.format.audio.bit_depth = c[5];
  track.track.format.audio.channels = c[9];
  track.track.format.audio.sample_rate = load_u32_be(c + 20);
  track.track.extradata = r.read_bytes(24);
}

inline void parseStsz(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  const uint32_t global_size = r.read_u32_be();
  const uint32_t count = r.read_u32_be();
  track.sample_sizes.resize(count);
  if (global_size == 0) {
    for (uint32_t i = 0; i < count; ++i) {
      track.sample_sizes[i] = r.read_u32_be();
    }
  } else {
    std::fill(track.sample_sizes.begin(), track.sample_sizes.end(), global_size);
  }
}

inline void parseStz2(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  r.skip(3); // reserved
  const uint8_t field_size = r.read_u8();
  const uint32_t count = r.read_u32_be();
  track.sample_sizes.resize(count);

  switch (field_size) {
    case 4:
      for (uint32_t i = 0; i < count; i += 2) {
        const uint8_t b = r.read_u8();
        track.sample_sizes[i] = (b >> 4) & 0xFu;
        if (i + 1 < count) track.sample_sizes[i + 1] = b & 0xFu;
      }
      break;
    case 8:
      for (uint32_t i = 0; i < count; ++i) {
        track.sample_sizes[i] = r.read_u8();
      }
      break;
    default: // 16
      for (uint32_t i = 0; i < count; ++i) {
        track.sample_sizes[i] = r.read_u16_be();
      }
      break;
  }
}

inline void parseStco(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  const uint32_t count = r.read_u32_be();
  track.chunk_offsets.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    track.chunk_offsets[i] = static_cast<int64_t>(r.read_u32_be());
  }
}

inline void parseCo64(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  const uint32_t count = r.read_u32_be();
  track.chunk_offsets.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    track.chunk_offsets[i] = static_cast<int64_t>(r.read_u64_be());
  }
}

inline void parseStsc(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  const uint32_t count = r.read_u32_be();
  track.stsc_entries.resize(count);
  for (auto& e : track.stsc_entries) {
    e.first_chunk = r.read_u32_be();
    e.samples_per_chunk = r.read_u32_be();
    e.sample_description_index = r.read_u32_be();
  }
}

inline void parseStts(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  const uint32_t count = r.read_u32_be();
  track.stts_entries.resize(count);
  for (auto& e : track.stts_entries) {
    e.sample_count = r.read_u32_be();
    e.sample_delta = r.read_u32_be();
  }
}

inline void parseCtts(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4); // version + flags
  const uint32_t count = r.read_u32_be();
  track.ctts_entries.resize(count);
  for (auto& e : track.ctts_entries) {
    e.sample_count = r.read_u32_be();
    e.sample_offset = r.read_i32_be();
  }
}

inline void parseStss(std::span<const uint8_t> body, BMFFTrack& track) {
  BufReader r(body.data(), body.size());
  r.skip(4);
  const uint32_t count = r.read_u32_be();
  track.sync_samples.resize(count);
  for (auto& s : track.sync_samples) {
    s = r.read_u32_be();
  }
  std::sort(track.sync_samples.begin(), track.sync_samples.end());
}

// ---------------------------------------------------------------------------
// Sample-table construction - functional style
//
// Iterator state for STTS / CTTS tables is managed with a small helper
// that advances through run-length encoded entries.
// ---------------------------------------------------------------------------

namespace detail {

// Advances through a run-length encoded table (STTS / CTTS).
// Each call to next() returns the current entry value and steps forward.
template<typename Entry, typename ValueFn>
struct RleIterator {
  const std::vector<Entry>& table;
  ValueFn value_fn; // Entry -> value
  size_t idx = 0;
  uint32_t remaining = 0;

  explicit RleIterator(const std::vector<Entry>& t, ValueFn fn)
      : table(t), value_fn(fn) {
    if (!table.empty()) remaining = table[0].sample_count;
  }

  auto next() -> decltype(value_fn(table[0])) {
    using T = decltype(value_fn(table[0]));
    if (idx >= table.size()) return T {};
    const auto val = value_fn(table[idx]);
    if (--remaining == 0 && ++idx < table.size()) {
      remaining = table[idx].sample_count;
    }
    return val;
  }

  auto valid() const -> bool { return idx < table.size(); }
};

} // namespace detail

inline void buildSampleTable(BMFFTrack& track) {
  if (track.sample_sizes.empty() ||
      track.chunk_offsets.empty() ||
      track.stsc_entries.empty() ||
      track.stts_entries.empty()) return;

  track.samples.clear();
  track.samples.reserve(track.sample_sizes.size());

  // Build a lookup from 1-based chunk index → samples_per_chunk.
  // The stsc table is sparse; the last entry whose first_chunk <= ci applies.
  auto samplesPerChunk = [&](uint32_t chunk_id) -> uint32_t {
    // Search from the end for the last applicable entry.
    for (auto it = track.stsc_entries.rbegin(); it != track.stsc_entries.rend(); ++it) {
      if (chunk_id >= it->first_chunk) return it->samples_per_chunk;
    }
    return 0;
  };

  // RLE iterators for timing tables.
  auto stts_iter = detail::RleIterator(track.stts_entries,
                                       [](const STTSEntry& e) { return e.sample_delta; });
  auto ctts_iter = detail::RleIterator(track.ctts_entries,
                                       [](const CTTSEntry& e) { return e.sample_offset; });

  const bool has_ctts = !track.ctts_entries.empty();

  size_t sample_idx = 0;
  int64_t current_dts = 0;

  for (size_t ci = 0; ci < track.chunk_offsets.size(); ++ci) {
    const uint32_t chunk_id = static_cast<uint32_t>(ci + 1u);
    const uint32_t samples_in_chunk = samplesPerChunk(chunk_id);
    if (samples_in_chunk == 0) continue;

    int64_t offset = track.chunk_offsets[ci];

    for (uint32_t s = 0;
         s < samples_in_chunk && sample_idx < track.sample_sizes.size();
         ++s) {
      const uint32_t duration = stts_iter.next();
      const int32_t cts_offset = has_ctts ? ctts_iter.next() : 0;

      const int64_t dts = current_dts - track.start_dts;
      const int64_t pts = dts + static_cast<int64_t>(cts_offset);

      const uint32_t sample_number = static_cast<uint32_t>(sample_idx + 1u);
      const bool is_key = track.sync_samples.empty() ||
                          std::binary_search(track.sync_samples.begin(),
                                             track.sync_samples.end(),
                                             sample_number);
      const uint32_t sz = track.sample_sizes[sample_idx];
      track.samples.push_back({offset, sz, pts, dts, duration, 0, is_key});

      offset += sz;
      current_dts += duration;
      ++sample_idx;
    }
  }
}

class BMFFDemuxer final : public BaseDemuxer {
  int64_t mdat_pos_ = 0;
  int64_t mdat_size_ = 0;
  bool found_mdat_ = false;
  bool inside_alac_entry_ = false;
  bool inside_mp4a_entry_ = false;
  uint32_t movie_timescale_ = 0;

  bool fatal_ = false;

  std::vector<BMFFTrack> bmff_tracks_;
  BMFFTrack* current_track_ = nullptr;
  std::vector<Sample> samples_;
  size_t current_sample_index_ = 0;

public:
  BMFFDemuxer() = default;

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) return OM_IO_INVALID_STREAM;
    if (!input_->canSeek()) return OM_IO_SEEK_REQUIRED;

    parseBoxes(0, input_->size());

    if (fatal_) return OM_FORMAT_END_OF_FILE;

    for (auto& t : bmff_tracks_) {
      applyEditList(t);
      buildSampleTable(t);
    }

    for (auto& t : bmff_tracks_) {
      if (t.track.format.type == OM_MEDIA_NONE) continue;
      if (t.samples.empty()) continue;

      t.track.index = static_cast<int32_t>(tracks_.size());
      t.index = t.track.index;

      int64_t min_pts = INT64_MAX;
      int64_t max_pts_end = INT64_MIN;

      for (auto& s : t.samples) {
        s.stream_index = t.index;
        samples_.push_back(s);
        min_pts = std::min(min_pts, s.pts);
        max_pts_end = std::max(max_pts_end,
                               s.pts + static_cast<int64_t>(s.duration));
      }

      t.track.start_time = min_pts;
      t.track.duration = (max_pts_end > min_pts)
                             ? (max_pts_end - min_pts)
                             : t.track_duration;
      t.track.nb_frames = static_cast<int64_t>(t.samples.size());
      tracks_.push_back(t.track);
    }

    if (samples_.empty() || tracks_.empty()) return OM_FORMAT_PARSE_FAILED;

    std::sort(samples_.begin(), samples_.end(),
              [](const Sample& a, const Sample& b) {
                return a.offset < b.offset;
              });

    current_sample_index_ = 0;
    parsing_state_ = ParsingState::READY;
    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (parsing_state_ != ParsingState::READY)
      return Err(OM_COMMON_NOT_INITIALIZED);
    if (current_sample_index_ >= samples_.size())
      return Err(OM_FORMAT_END_OF_FILE);

    const auto& sample = samples_[current_sample_index_];
    const auto& bmff_track = bmff_tracks_[sample.stream_index];

    std::vector<uint8_t> raw(sample.size);
    input_->seek(sample.offset, Whence::BEG);
    const size_t n = input_->read(raw);
    if (n < sample.size) return Err(OM_FORMAT_END_OF_FILE);

    std::vector<uint8_t> converted;
    std::span<const uint8_t> payload(raw);

    if (bmff_track.annexb_bsf) {
      converted = bmff_track.annexb_bsf->convert(payload, sample.is_keyframe);
      payload = converted;
    }

    Packet pkt;
    pkt.allocate(payload.size());
    memcpy(pkt.bytes.data(), payload.data(), payload.size());

    pkt.stream_index = sample.stream_index;
    pkt.pts = sample.pts;
    pkt.dts = sample.dts;
    pkt.pos = sample.offset;
    pkt.duration = sample.duration;
    pkt.is_keyframe = sample.is_keyframe;

    ++current_sample_index_;
    return Ok(std::move(pkt));
  }

  auto seek(int64_t timestamp_ns, int32_t stream_index) -> OMError override {
    if (stream_index < 0 ||
        stream_index >= static_cast<int32_t>(tracks_.size()))
      return OM_FORMAT_PARSE_FAILED;

    const auto& st = tracks_[stream_index];
    const int64_t den = st.time_base.den ? st.time_base.den : 1;
    const int64_t num = st.time_base.num ? st.time_base.num : 1;
    const int64_t ts_ticks = (timestamp_ns * den) / (num * INT64_C(1'000'000'000));

    // Find the last keyframe whose dts <= ts_ticks on the requested stream.
    size_t best = samples_.size();
    for (size_t i = 0; i < samples_.size(); ++i) {
      const auto& s = samples_[i];
      if (s.stream_index != stream_index) continue;
      if (s.dts > ts_ticks) break;
      if (s.is_keyframe) best = i;
    }

    if (best == samples_.size()) return OM_FORMAT_PARSE_FAILED;
    current_sample_index_ = best;
    return OM_SUCCESS;
  }

private:
  enum class ParsingState {
    READING_HEADER,
    READY,
    ERROR,
  };
  ParsingState parsing_state_ = ParsingState::READING_HEADER;

  // Read exactly `n` bytes from the stream at position `pos`.
  // Sets fatal_ on a short-read and resizes the buffer to actual bytes read.
  auto readBoxData(int64_t pos, uint64_t n) -> std::vector<uint8_t> {
    std::vector<uint8_t> buf(n);
    input_->seek(pos, Whence::BEG);
    const size_t got = input_->read(buf);
    if (got < n) {
      fatal_ = true;
      buf.resize(got);
    }
    return buf;
  }

  void parseBoxes(int64_t start, int64_t end) {
    if (fatal_) return;

    input_->seek(start, Whence::BEG);

    while (!fatal_ && input_->tell() + 8 <= end) {
      uint8_t hdr[8];
      if (input_->read(hdr) != 8) {
        fatal_ = true;
        return;
      }

      const uint32_t raw_size = load_u32_be(hdr);
      const uint32_t type = load_u32(hdr + 4);
      int64_t body_pos = input_->tell();
      uint64_t body_size = 0;

      if (raw_size == 1) {
        // 64-bit extended size follows immediately after the 4-byte type.
        uint8_t large[8];
        if (input_->read(large) != 8) {
          fatal_ = true;
          return;
        }
        const uint64_t full_size = load_u64_be(large);
        body_pos = input_->tell();
        body_size = (full_size >= 16) ? full_size - 16u : 0u;
      } else if (raw_size == 0) {
        // Box extends to the end of the enclosing container.
        body_size = static_cast<uint64_t>(end - body_pos);
      } else if (raw_size < 8) {
        // Malformed: header claims size < minimum. Stop parsing this level.
        fatal_ = true;
        return;
      } else {
        body_size = raw_size - 8u;
      }

      // Guard against body_pos + body_size overflowing int64_t.
      const int64_t box_end =
          (body_size <= static_cast<uint64_t>(end - body_pos))
              ? (body_pos + static_cast<int64_t>(body_size))
              : end;

      handleBox(type, body_pos, static_cast<uint64_t>(box_end - body_pos));
      input_->seek(box_end, Whence::BEG);
    }
  }

  void handleBox(uint32_t type, int64_t pos, uint64_t size) {
    if (fatal_) return;

    switch (type) {
      case ATOM('t', 'r', 'a', 'k'):
        bmff_tracks_.emplace_back();
        current_track_ = &bmff_tracks_.back();
        parseBoxes(pos, pos + size);
        current_track_ = nullptr;
        return;
      case ATOM('m', 'e', 't', 'a'):
        // FullBox: skip 4-byte version+flags header before recursing.
        parseBoxes(pos + 4, pos + size);
        return;
      default:
        break;
    }

    if (isContainerBox(type)) {
      parseBoxes(pos, pos + size);
      return;
    }
    if (isIgnoredBox(type)) return;

    if (type == ATOM('m', 'd', 'a', 't')) {
      mdat_pos_ = pos;
      mdat_size_ = static_cast<int64_t>(size);
      found_mdat_ = true;
      return;
    }

    // --- Leaf boxes: read payload into a buffer then dispatch ---
    auto body = readBoxData(pos, size);
    if (fatal_) return;

    if (type == ATOM('m', 'v', 'h', 'd')) {
      parseMvhd(body, movie_timescale_);
      return;
    }

    if (!current_track_) return;
    auto& track = *current_track_;

    switch (type) {
      // Track structure
      case ATOM('t', 'k', 'h', 'd'): parseTkhd(body, track); break;
      case ATOM('m', 'd', 'h', 'd'): parseMdhd(body, track); break;
      case ATOM('h', 'd', 'l', 'r'): parseHdlr(body, track); break;
      case ATOM('e', 'l', 's', 't'): parseElst(body, track); break;

      // Sample table
      case ATOM('s', 't', 's', 'z'): parseStsz(body, track); break;
      case ATOM('s', 't', 'z', '2'): parseStz2(body, track); break;
      case ATOM('s', 't', 'c', 'o'): parseStco(body, track); break;
      case ATOM('c', 'o', '6', '4'): parseCo64(body, track); break;
      case ATOM('s', 't', 's', 'c'): parseStsc(body, track); break;
      case ATOM('s', 't', 't', 's'): parseStts(body, track); break;
      case ATOM('c', 't', 't', 's'): parseCtts(body, track); break;
      case ATOM('s', 't', 's', 's'): parseStss(body, track); break;

      // Video metadata
      case ATOM('b', 't', 'r', 't'): parseBtrt(body, track); break;
      case ATOM('c', 'o', 'l', 'r'): parseColr(body, track); break;
      case ATOM('m', 'd', 'c', 'v'): /* reserved for future HDR */ break;
      case ATOM('C', 'L', 'L', 'I'):
      case ATOM('c', 'l', 'l', 'i'): /* reserved for future HDR */ break;

      // Codec configuration boxes
      case ATOM('a', 'v', 'c', 'C'): parseAvcc(body, track); break;
      case ATOM('h', 'v', 'c', 'C'): parseHvcc(body, track); break;
      case ATOM('V', 'v', 'c', 'C'):
      case ATOM('v', 'v', 'c', 'C'): parseVvcc(body, track); break;
      case ATOM('e', 'v', 'c', 'C'): parseEvcc(body, track); break;
      case ATOM('v', 'p', 'c', 'C'): parseVpcc(body, track); break;
      case ATOM('a', 'v', '1', 'C'): parseAv1c(body, track); break;
      case ATOM('d', 'O', 'p', 's'): parseDops(body, track); break;

      case ATOM('e', 's', 'd', 's'):
        if (inside_mp4a_entry_) {
          parseEsds(body, track);
        }
        break;
      case ATOM('a', 'l', 'a', 'c'):
        if (inside_alac_entry_) {
          parseAlacSpecific(body, track);
        }
        break;

      case ATOM('d', 'f', 'L', 'a'):
        if (current_track_) {
          if (body.size() > 4) {
            current_track_->track.extradata.assign(
                body.begin() + 4, body.end());
          }
        }
        break;

      case ATOM('s', 't', 's', 'd'):
        parseStsd(pos, size);
        break;

      default:
        break; // Unknown leaf box - silently skip.
    }
  }

  void parseStsd(int64_t pos, uint64_t /*size*/) {
    if (fatal_) return;

    auto& track = *current_track_;
    auto& st = track.track;

    st.time_base = {1, static_cast<int>(
                           track.timescale ? track.timescale : movie_timescale_)};

    // Skip FullBox header (version + flags = 4 bytes), then read entry count.
    input_->seek(pos + 4, Whence::BEG);
    uint8_t count_buf[4];
    if (input_->read(count_buf) != 4) {
      fatal_ = true;
      return;
    }
    const uint32_t count = load_u32_be(count_buf);

    for (uint32_t i = 0; i < count; ++i) {
      if (fatal_) return;

      const int64_t entry_start = input_->tell();
      uint8_t entry_hdr[8];
      if (input_->read(entry_hdr) != 8) {
        fatal_ = true;
        return;
      }

      const uint32_t entry_size = load_u32_be(entry_hdr);
      const uint32_t fmt = load_u32(entry_hdr + 4);
      const int64_t entry_end = entry_start + entry_size;

      if (fmt == ATOM('a', 'l', 'a', 'c')) {
        st.format.type = OM_MEDIA_AUDIO;
        st.format.codec_id = OM_CODEC_ALAC;
        if (!seekAndParseAudioEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        inside_alac_entry_ = true;
        input_->seek(entry_start + 36, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);
        inside_alac_entry_ = false;

      } else if (isMp4aVariant(fmt)) {
        st.format.type = OM_MEDIA_AUDIO;
        st.format.codec_id = OM_CODEC_AAC;
        input_->seek(entry_start + 16, Whence::BEG);
        uint8_t v_buf[2];
        if (input_->read(v_buf) != 2) {
          fatal_ = true;
          return;
        }
        const uint16_t mp4a_version = static_cast<uint16_t>(
            (v_buf[0] << 8u) | v_buf[1]);
        if (!seekAndParseAudioEntry(entry_start + 8, st)) {
          input_->seek(entry_end, Whence::BEG);
          continue;
        }
        const int64_t header_size = 36 + (mp4a_version == 1 ? 16 : 0) + (mp4a_version == 2 ? 36 : 0);
        inside_mp4a_entry_ = true;
        input_->seek(entry_start + header_size, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);
        inside_mp4a_entry_ = false;

      } else if (fmt == ATOM('a', 'c', '-', '3') || fmt == ATOM('e', 'c', '-', '3')) {
        st.format.type = OM_MEDIA_AUDIO;
        st.format.codec_id = (fmt == ATOM('a', 'c', '-', '3')) ? OM_CODEC_AC3 : OM_CODEC_EAC3;
        if (!seekAndParseAudioEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }

      } else if (fmt == ATOM('f', 'l', 'a', 'c') || fmt == ATOM('f', 'L', 'a', 'C')) {
        st.format.type = OM_MEDIA_AUDIO;
        st.format.codec_id = OM_CODEC_FLAC;
        if (!seekAndParseAudioEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 36, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);
      } else if (fmt == ATOM('o', 'p', 'u', 's') || fmt == ATOM('O', 'p', 'u', 's')) {
        st.format.type = OM_MEDIA_AUDIO;
        st.format.codec_id = OM_CODEC_OPUS;
        if (!seekAndParseAudioEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 36, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (isPcmVariant(fmt)) {
        st.format.type = OM_MEDIA_AUDIO;
        st.format.codec_id = OM_CODEC_PCM_U8;
        if (!seekAndParseAudioEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }

      } else if (isAvcVariant(fmt)) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_H264;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          input_->seek(entry_end, Whence::BEG);
          continue;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (isHevcVariant(fmt)) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_HEVC;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (isVvcVariant(fmt)) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_VVC;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (fmt == ATOM('e', 'v', 'c', '1')) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_EVC;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (fmt == ATOM('v', 'p', '0', '8')) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_VP8;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }

      } else if (fmt == ATOM('v', 'p', '0', '9')) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_VP9;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (fmt == ATOM('a', 'v', '0', '1')) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_AV1;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);

      } else if (fmt == ATOM('m', 'p', '4', 'v') || fmt == ATOM('M', 'P', '4', 'V')) {
        st.format.type = OM_MEDIA_VIDEO;
        st.format.codec_id = OM_CODEC_MPEG4;
        if (!seekAndParseVideoEntry(entry_start + 8, st)) {
          fatal_ = true;
          return;
        }
        input_->seek(entry_start + 86, Whence::BEG);
        parseBoxes(input_->tell(), entry_end);
      }

      input_->seek(entry_end, Whence::BEG);
    }
  }

  auto seekAndParseAudioEntry(int64_t body_start, Track& track) const -> bool {
    // AudioSampleEntry layout (from body_start):
    //   6 bytes reserved + 2 bytes data_ref_index  → skip 8
    //   4 bytes reserved + 4 bytes reserved          → skip 8
    //   After those 16 bytes: channel_count (2), sample_size (2),
    //                          compression_id (2), packet_size (2)
    //                          sample_rate (4, 16.16 fixed-point)
    input_->seek(body_start + 16, Whence::BEG);
    uint8_t buf[8];
    if (input_->read(buf) != 8) return false;

    const uint16_t ch = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    const uint16_t bits = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];

    uint8_t sr_buf[4];
    if (input_->read(sr_buf) != 4) return false;

    const uint32_t sr_fp = load_u32_be(sr_buf);
    if (ch) {
      track.format.audio.channels = ch;
    }
    if (bits) {
      track.format.audio.bit_depth = bits;
    }
    if (sr_fp) {
      track.format.audio.sample_rate = sr_fp >> 16;
    }
    return true;
  }

  auto seekAndParseVideoEntry(int64_t body_start, Track& track) const -> bool {
    // VisualSampleEntry layout from body_start:
    //   6 reserved + 2 data_ref_index + 16 pre_defined/reserved = 24 bytes
    //   Then: width (2), height (2)
    input_->seek(body_start + 24, Whence::BEG);
    uint8_t vis[4];
    if (input_->read(vis) != 4) return false;

    track.format.video.width = (static_cast<uint16_t>(vis[0]) << 8) | vis[1];
    track.format.video.height = (static_cast<uint16_t>(vis[2]) << 8) | vis[3];
    track.format.video.color_space = OM_COLOR_SPACE_UNKNOWN;
    track.format.video.transfer_char = OM_TRANSFER_UNKNOWN;
    return true;
  }
};

const FormatDescriptor FORMAT_BMFF = {
    .container_id = OM_CONTAINER_MP4,
    .name = "bmff",
    .long_name = "ISO Base Media File Format",
    .demuxer_factory = [] { return std::make_unique<BMFFDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
