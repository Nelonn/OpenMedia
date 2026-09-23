#pragma once

#include <audio/ac3/ac3_imdct.hpp>
#include <audio/ac3/ac3_tables.hpp>
#include <cstdint>
#include <optional>
#include <span>

namespace openmedia {
class BitReader;
}

namespace openmedia::ac3 {

// Speaker locations. Bit positions follow the WAVEFORMATEXTENSIBLE channel
// mask, extended the same way as FFmpeg's AV_CH_* constants, so ascending bit
// order is the conventional interleaving order.
enum ChannelMask : uint64_t {
  CH_FRONT_LEFT = 1ull << 0,
  CH_FRONT_RIGHT = 1ull << 1,
  CH_FRONT_CENTER = 1ull << 2,
  CH_LOW_FREQUENCY = 1ull << 3,
  CH_BACK_LEFT = 1ull << 4,
  CH_BACK_RIGHT = 1ull << 5,
  CH_FRONT_LEFT_OF_CENTER = 1ull << 6,
  CH_FRONT_RIGHT_OF_CENTER = 1ull << 7,
  CH_BACK_CENTER = 1ull << 8,
  CH_SIDE_LEFT = 1ull << 9,
  CH_SIDE_RIGHT = 1ull << 10,
  CH_TOP_CENTER = 1ull << 11,
  CH_TOP_FRONT_LEFT = 1ull << 12,
  CH_TOP_FRONT_CENTER = 1ull << 13,
  CH_TOP_FRONT_RIGHT = 1ull << 14,
  CH_TOP_BACK_LEFT = 1ull << 15,
  CH_TOP_BACK_RIGHT = 1ull << 17,
  CH_WIDE_LEFT = 1ull << 31,
  CH_WIDE_RIGHT = 1ull << 32,
  CH_SURROUND_DIRECT_LEFT = 1ull << 33,
  CH_SURROUND_DIRECT_RIGHT = 1ull << 34,
  CH_LOW_FREQUENCY_2 = 1ull << 35,
};

enum class FrameType : uint8_t {
  INDEPENDENT = 0,
  DEPENDENT = 1,
  AC3_CONVERT = 2, // also used for plain AC-3 syncframes
};

// Fields of syncinfo() and the start of bsi() needed to delimit a syncframe.
struct FrameHeader {
  bool eac3 = false;
  uint8_t bsid = 0;
  FrameType type = FrameType::AC3_CONVERT;
  uint8_t substream_id = 0;
  uint32_t frame_size = 0; // bytes
  uint32_t sample_rate = 0;
  uint32_t bit_rate = 0;
  uint8_t fscod = 0;
  uint8_t sr_shift = 0; // 1 for half sample rate (bsid 9, fscod2), 2 for bsid 10
  uint8_t num_blocks = 6;
  uint8_t acmod = 0;
  bool lfe = false;
  bool chanmap_present = false; // dependent substreams only
  uint16_t chanmap = 0;

  auto channels() const -> int { return FBW_CHANNELS[acmod] + (lfe ? 1 : 0); }
};

// Parses the syncframe header at the start of `data`, including the channel
// map of a dependent substream. Needs at least 8 bytes.
auto parseFrameHeader(std::span<const uint8_t> data) -> std::optional<FrameHeader>;

// Writes the speaker location of every coded channel of the syncframe to
// `locations` (FrameHeader::channels() entries).
void channelLocations(const FrameHeader& header, uint64_t* locations);

// Checks the CRC covering the whole syncframe (crc2 for AC-3, crc for E-AC-3).
auto checkFrameCrc(std::span<const uint8_t> frame) -> bool;

// Destination of decoded PCM: an interleaved buffer with `stride` floats per
// sample frame. slots[ch] is the position of coded channel ch within a sample
// frame, or -1 to drop the channel.
struct PcmTarget {
  float* data = nullptr;
  int stride = 0;
  const int* slots = nullptr;
};

// Decoder for a single AC-3 bitstream or one E-AC-3 substream.
//
// Channels are numbered in coded order: the full bandwidth channels as given
// by acmod (for example L C R Ls Rs) followed by the LFE channel.
class SubstreamDecoder {
public:
  SubstreamDecoder();

  // Decodes one complete syncframe into `target`. On failure the internal
  // state is reset and the written part of the target is undefined.
  auto decode(std::span<const uint8_t> frame, const PcmTarget& target) -> bool;

  void reset();

  // Scale applied to the dynamic range control words: 0 disables DRC,
  // 1 applies it as transmitted.
  void setDrcScale(float scale) { drc_scale_ = scale; }

private:
  struct MantissaGroups {
    float b1[2] = {};
    float b2[2] = {};
    float b4 = 0.0f;
    int n1 = 0;
    int n2 = 0;
    int n4 = 0;
  };

  auto parseAc3Bsi(BitReader& br) -> bool;
  auto parseEac3Bsi(BitReader& br) -> bool;
  auto parseAudioFrame(BitReader& br) -> bool;
  void setAc3FrameDefaults();

  auto decodeBlock(BitReader& br, int blk, const PcmTarget& target) -> bool;
  void parseTransformFlags(BitReader& br, int blk);
  auto parseSpectralExtension(BitReader& br, int blk) -> bool;
  auto parseSpxStrategy(BitReader& br, int blk) -> bool;
  void parseSpxCoordinates(BitReader& br);
  auto parseCoupling(BitReader& br, int blk) -> bool;
  auto parseCouplingStrategy(BitReader& br, int blk) -> bool;
  auto parseCouplingCoordinates(BitReader& br, int blk) -> bool;
  void parseRematrixing(BitReader& br, int blk);
  auto parseExponents(BitReader& br, int blk) -> bool;
  auto decodeExponents(BitReader& br, int ch, int strategy) -> bool;
  auto parseBitAllocation(BitReader& br, int blk) -> bool;
  auto parseSnrOffsets(BitReader& br, int blk) -> bool;
  auto parseDeltaBitAllocation(BitReader& br, int blk) -> bool;

  void allocateBits(int ch);
  void decodeCoefficients(BitReader& br, int blk);
  void decodeMantissas(BitReader& br, int ch, int blk, MantissaGroups& groups);
  void decodeAhtMantissas(BitReader& br, int ch);
  void uncouple();
  void rematrix();
  void applyDynamicRange();
  void applySpectralExtension();
  void synthesize(int blk, const PcmTarget& target);

  auto dither() -> float;
  auto noise() -> float;

  static auto bandStructure(BitReader& br, bool eac3, int blk, int start, int end, const uint8_t* defaults,
                            uint8_t* band_struct, uint8_t* band_sizes) -> int;

  FrameHeader header_;
  Imdct imdct_;
  float drc_scale_ = 1.0f;
  uint32_t rng_ = 1;

  // bsi
  int acmod_ = -1;
  bool lfeon_ = false;
  int nfchans_ = 0;
  int nchans_ = 0; // full bandwidth + LFE
  int lfe_ch_ = 0;

  // audfrm (E-AC-3), or the fixed AC-3 equivalents
  int snroffststr_ = 0;
  bool blkswe_ = true;
  bool dithflage_ = true;
  bool bamode_ = true;
  bool frmfgaincode_ = false;
  bool dbaflde_ = true;
  bool skipflde_ = true;
  bool cplstre_[MAX_BLOCKS] = {};
  bool cplinu_[MAX_BLOCKS] = {};
  uint8_t expstr_[MAX_BLOCKS][MAX_CHANNELS] = {};
  bool chahtinu_[MAX_CHANNELS] = {};
  int spx_atten_[MAX_CHANNELS] = {};

  // Channel indices: 0 = coupling, 1..nfchans = full bandwidth, nfchans + 1 = LFE.
  bool blksw_[MAX_CHANNELS] = {};
  bool dithflag_[MAX_CHANNELS] = {};
  float dynrng_[2] = {1.0f, 1.0f};

  // spectral extension
  bool spxinu_ = false;
  bool chinspx_[MAX_CHANNELS] = {};
  int spx_dst_start_ = 0;
  int spx_src_start_ = 0;
  int spx_end_ = 0;
  int nspxbnds_ = 0;
  uint8_t spx_band_struct_[17] = {};
  uint8_t spx_band_sizes_[17] = {};
  float spx_noise_blend_[MAX_CHANNELS][17] = {};
  float spx_signal_blend_[MAX_CHANNELS][17] = {};
  bool first_spx_coords_[MAX_CHANNELS] = {};

  // coupling
  bool cplinu_blk_ = false;
  bool chincpl_[MAX_CHANNELS] = {};
  bool phsflginu_ = false;
  int ncplbnds_ = 0;
  uint8_t cpl_band_struct_[18] = {};
  uint8_t cpl_band_sizes_[18] = {};
  float cplco_[MAX_CHANNELS][18] = {};
  bool phsflg_[18] = {};
  bool first_cpl_coords_[MAX_CHANNELS] = {};
  bool first_cpl_leak_ = false;

  // rematrixing
  int nrematbnds_ = 0;
  bool rematflg_[4] = {};

  // exponents and bit allocation
  int start_[MAX_CHANNELS] = {};
  int end_[MAX_CHANNELS] = {};
  int8_t exps_[MAX_CHANNELS][MAX_COEFS] = {};
  uint8_t bap_[MAX_CHANNELS][MAX_COEFS] = {};
  int ba_fscod_ = 0; // hearing threshold column
  int ba_shift_ = 0; // decay and band shift for reduced sample rates
  bool zero_baps_ = false;
  int sdecay_ = 0;
  int fdecay_ = 0;
  int sgain_ = 0;
  int dbknee_ = 0;
  int floor_ = 0;
  int csnroffst_ = 0;
  int fsnroffst_[MAX_CHANNELS] = {};
  int fgain_[MAX_CHANNELS] = {};
  int cplfleak_ = 0;
  int cplsleak_ = 0;
  uint8_t dba_mode_[MAX_CHANNELS] = {};
  uint8_t dba_nseg_[MAX_CHANNELS] = {};
  uint8_t dba_offset_[MAX_CHANNELS][8] = {};
  uint8_t dba_length_[MAX_CHANNELS][8] = {};
  uint8_t dba_value_[MAX_CHANNELS][8] = {};

  // coefficients and output
  float coefs_[MAX_CHANNELS][MAX_COEFS] = {};
  float aht_mant_[MAX_CHANNELS][MAX_COEFS][MAX_BLOCKS] = {};
  float delay_[MAX_FBW_CHANNELS + 1][BLOCK_SIZE] = {};
  float discard_[BLOCK_SIZE] = {}; // output of dropped channels
};

} // namespace openmedia::ac3
