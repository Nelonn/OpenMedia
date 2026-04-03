#pragma once

#include <functional>
#include <openmedia/audio.hpp>
#include <openmedia/codec_defs.h>
#include <openmedia/error.h>
#include <openmedia/frame.hpp>
#include <openmedia/log.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <openmedia/video.hpp>
#include <variant>

namespace openmedia {

struct CodecMeta {
  std::string_view name;
  std::string_view long_name;
  OMMediaType media_type;
};

OPENMEDIA_ABI
auto getCodecMeta(OMCodecId codec_id) -> CodecMeta;

OPENMEDIA_ABI
auto profileToString(OMCodecId codec, OMProfile profile) -> std::string_view;

struct OPENMEDIA_ABI DecoderOptions {
  MediaFormat format;
  Rational time_base = {};
  std::span<const uint8_t> extradata;
  LoggerRef logger;
  Dictionary extra;
};

struct OPENMEDIA_ABI DecodingInfo {
  OMMediaType media_type = OM_MEDIA_NONE;
  union {
    char dummy = 0;
    AudioFormat audio_format;
    VideoFormat video_format;
  };
};

class OPENMEDIA_ABI Decoder {
public:
  virtual ~Decoder() = default;

  virtual auto configure(const DecoderOptions& options) -> OMError = 0;

  virtual auto getInfo() -> std::optional<DecodingInfo> = 0;

  virtual auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> = 0;

  virtual void flush() = 0;
};

enum class RateControlMode {
  CRF,     // Constant Rate Factor - quality-based, codec-internal (x264/x265/SVT)
  CBR,     // Constant Bitrate - strict buffer/bitrate conformance
  VBR,     // Variable Bitrate - target + peak, VBV-constrained
  CQP,     // Constant Quantizer - fixed QP per frame type, no bitrate target
  ABR,     // Average Bitrate - best-effort target, no hard buffer constraints
  HQCBR,   // High-Quality CBR - CBR with lookahead/B-frame optimizations (NVENC)
  HQVBR,   // High-Quality VBR - VBR with lookahead/multi-pass (NVENC)
  QVBR,    // Quality-defined VBR - quality target drives bitrate (NVENC/AMF)
  VBR_LAT, // Low-Latency VBR - VBR without lookahead, minimizes encode delay
  ICQ,     // Intelligent CQP - quality-driven, Intel Quick Sync variant of CRF
};

struct OPENMEDIA_ABI VbvParams {
  int64_t buffer_size;                            // bits
  std::optional<int64_t> buffer_initial_fullness; // bits
};

struct OPENMEDIA_ABI BitrateParams {
  int64_t target_bitrate;             // bps
  std::optional<int64_t> min_bitrate; // bps
  std::optional<int64_t> max_bitrate; // bps
  std::optional<VbvParams> vbv;
};

struct OPENMEDIA_ABI LookaheadParams {
  std::optional<int32_t> lookahead_depth; // frames, 0 = disabled
  std::optional<bool> enable_b_adapt;
};

struct OPENMEDIA_ABI CrfParams { // CRF, ICQ
  float quality;                 // codec quality scale
};

struct OPENMEDIA_ABI CqpParams { // CQP
  int32_t qp_i;
  int32_t qp_p;
  std::optional<int32_t> qp_b;
};

struct OPENMEDIA_ABI CbrParams { // CBR
  BitrateParams bitrate;
};

struct OPENMEDIA_ABI VbrParams { // VBR
  BitrateParams bitrate;
};

struct OPENMEDIA_ABI AbrParams { // ABR
  int64_t target_bitrate;        // bps, no hard buffer constraints
};

struct OPENMEDIA_ABI HqcbrParams { // HQCBR
  BitrateParams bitrate;
  LookaheadParams lookahead;
};

struct OPENMEDIA_ABI HqvbrParams { // HQVBR
  BitrateParams bitrate;
  LookaheadParams lookahead;
  std::optional<bool> enable_peak_constrained;
};

struct OPENMEDIA_ABI QvbrParams { // QVBR
  float quality;
  BitrateParams bitrate;
  LookaheadParams lookahead;
  std::optional<bool> enable_peak_constrained;
};

struct OPENMEDIA_ABI VbrLatParams { // VBR_LAT
  BitrateParams bitrate;            // lookahead forced off
};

using RateControlVariant = std::variant<
    CrfParams,    // CRF
    CbrParams,    // CBR
    VbrParams,    // VBR
    CqpParams,    // CQP
    AbrParams,    // ABR
    HqcbrParams,  // HQCBR
    HqvbrParams,  // HQVBR
    QvbrParams,   // QVBR
    VbrLatParams // VBR_LAT
    //CrfParams     // ICQ - same shape as CRF, disambiguated by index (or wrap if desired)
    >;

struct OPENMEDIA_ABI RateControlParams {
  RateControlVariant params;

  // QP bounds - safety rails valid in all modes
  std::optional<int32_t> min_qp;
  std::optional<int32_t> max_qp;

  auto getMode() const -> RateControlMode {
    return static_cast<RateControlMode>(params.index());
  }
};

struct OPENMEDIA_ABI EncoderOptions {
  MediaFormat format;
  LoggerRef logger;
  Dictionary extra;
  RateControlParams rate_control;
};

struct OPENMEDIA_ABI EncodingInfo {
  std::vector<uint8_t> extradata;
};

class OPENMEDIA_ABI Encoder {
public:
  virtual ~Encoder() = default;

  virtual auto configure(const EncoderOptions& options) -> OMError = 0;

  virtual auto getInfo() -> EncodingInfo = 0;

  virtual auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> = 0;

  virtual auto updateBitrate(const RateControlParams& rc) -> OMError = 0;
};

enum CodecFlags : uint8_t {
  NONE = 0,
  HARDWARE = 1 << 0,
};

struct OPENMEDIA_ABI AudioCodecCaps {
  bool fmt_u8 : 1 = false;
  bool fmt_s16 : 1 = false;
  bool fmt_s32 : 1 = false;
  bool fmt_f32 : 1 = false;
  bool fmt_f64 : 1 = false;
  std::vector<uint32_t> sample_rates = {};
};

struct OPENMEDIA_ABI VideoCodecCaps {
  std::vector<OMPixelFormat> pix_fmts = {};
};

struct OPENMEDIA_ABI CodecCaps {
  std::vector<OMProfile> profiles = {};
  std::vector<int32_t> levels = {};
  bool threading = false;
  std::variant<AudioCodecCaps, VideoCodecCaps> media = {};
};

struct OPENMEDIA_ABI CodecDescriptor {
  OMCodecId codec_id = OM_CODEC_NONE;
  OMMediaType type = OM_MEDIA_NONE;
  std::string_view name;
  std::string_view long_name;
  std::string_view vendor;
  CodecFlags flags = NONE;
  std::optional<CodecCaps> caps = {};
  std::vector<Key> options = {};
  std::function<std::optional<CodecCaps>()> fetch_caps = {};
  std::function<std::unique_ptr<Decoder>()> decoder_factory = {};
  std::function<std::unique_ptr<Encoder>()> encoder_factory = {};

  auto isDecoding() const noexcept -> bool {
    return decoder_factory != nullptr;
  }

  auto isEncoding() const noexcept -> bool {
    return encoder_factory != nullptr;
  }
};

} // namespace openmedia
