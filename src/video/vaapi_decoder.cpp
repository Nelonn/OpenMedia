#include "vaapi_codecs.hpp"

#include "decode_report.hpp"
#include "dx_h264.hpp"
#include "dx_h265.hpp"
#include "h264_dpb.hpp"
#include "hdr_sei.hpp"
#include "reorder_queue.hpp"
#include "vaapi_common.hpp"
#include "vp9_quant.hpp"

#include <util/color_codes.hpp>
#include <video/parser/av1_parser.hpp>
#include <video/parser/h264_types.hpp>
#include <video/parser/h265_parser.hpp>
#include <video/parser/vp9_parser.hpp>

#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace openmedia {

namespace {

// Surfaces beyond what the stream itself needs: the picture being decoded is
// counted separately, these are for the caller. A caller that takes surfaces
// rather than copies holds on to them for as long as it is showing them.
constexpr size_t kHostOutputSlack = 2;
constexpr size_t kHardwareOutputSlack = 8;

// Frame (not field) zig-zag scans, 8.5.6: the H.264 parser keeps scaling lists
// in coded order, VA-API wants them in raster order.
constexpr uint8_t kZigzag4x4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr uint8_t kZigzag8x8[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

auto profileName(VAProfile profile) -> std::string_view {
  switch (profile) {
    case VAProfileH264ConstrainedBaseline: return "H.264 Constrained Baseline";
    case VAProfileH264Main: return "H.264 Main";
    case VAProfileH264High: return "H.264 High";
    case VAProfileH264High10: return "H.264 High 10";
    case VAProfileHEVCMain: return "HEVC Main";
    case VAProfileHEVCMain10: return "HEVC Main 10";
    case VAProfileVP9Profile0: return "VP9 Profile 0";
    case VAProfileVP9Profile2: return "VP9 Profile 2";
    case VAProfileAV1Profile0: return "AV1 Main";
    default: return "unknown profile";
  }
}

auto codecName(OMCodecId codec) -> std::string_view {
  switch (codec) {
    case OM_CODEC_H264: return "H.264";
    case OM_CODEC_H265: return "H.265";
    case OM_CODEC_VP9: return "VP9";
    case OM_CODEC_AV1: return "AV1";
    default: return "?";
  }
}

// What a picture needs from the VA session it is decoded in.
struct SessionParams {
  VAProfile profile = VAProfileNone;
  uint32_t rt_format = VA_RT_FORMAT_YUV420;
  uint32_t width = 0;
  uint32_t height = 0;
  size_t surfaces = 0;
};

struct H264Slice {
  std::span<const uint8_t> nal; // from the NAL header byte, no start code
  h264::NALHeader header;
  h264::SliceHeader slice;
};

// 7.4.1.2.4: the first VCL NAL unit of a new primary coded picture. Container
// packets normally hold exactly one picture, but nothing guarantees it.
auto startsNewPicture(const H264Slice& prev, const H264Slice& cur, const h264::SPS& sps) -> bool {
  const auto& a = prev.slice;
  const auto& b = cur.slice;
  if (b.first_mb_in_slice == 0) return true;
  if (a.frame_num != b.frame_num || a.pic_parameter_set_id != b.pic_parameter_set_id) return true;
  if (a.field_pic_flag != b.field_pic_flag || a.bottom_field_flag != b.bottom_field_flag) return true;
  if ((prev.header.idc == 0) != (cur.header.idc == 0)) return true;
  const bool idr_a = prev.header.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
  const bool idr_b = cur.header.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
  if (idr_a != idr_b || (idr_a && a.idr_pic_id != b.idr_pic_id)) return true;
  if (sps.pic_order_cnt_type == 0 &&
      (a.pic_order_cnt_lsb != b.pic_order_cnt_lsb || a.delta_pic_order_cnt_bottom != b.delta_pic_order_cnt_bottom)) {
    return true;
  }
  if (sps.pic_order_cnt_type == 1 &&
      (a.delta_pic_order_cnt[0] != b.delta_pic_order_cnt[0] || a.delta_pic_order_cnt[1] != b.delta_pic_order_cnt[1])) {
    return true;
  }
  return false;
}

auto isIntraSlice(const h264::SliceHeader& slice) -> bool {
  const int type = slice.slice_type % 5;
  return type == 2 || type == 4;
}

// The part of the coded picture the stream wants shown, 7.4.2.1.1.
auto h264DisplaySize(const h264::SPS& sps) -> std::pair<uint32_t, uint32_t> {
  const uint32_t width = static_cast<uint32_t>(sps.pic_width_in_mbs_minus1 + 1) * 16u;
  const uint32_t height = static_cast<uint32_t>(sps.pic_height_in_map_units_minus1 + 1) * 16u *
                          (sps.frame_mbs_only_flag ? 1u : 2u);
  if (!sps.frame_cropping_flag) return {width, height};
  const bool has_chroma = sps.chroma_format_idc != 0 && !sps.separate_colour_plane_flag;
  const uint32_t unit_x = has_chroma && sps.chroma_format_idc != 3 ? 2u : 1u;
  const uint32_t unit_y = (has_chroma && sps.chroma_format_idc == 1 ? 2u : 1u) * (sps.frame_mbs_only_flag ? 1u : 2u);
  const uint32_t crop_x = unit_x * static_cast<uint32_t>(sps.frame_crop_left_offset + sps.frame_crop_right_offset);
  const uint32_t crop_y = unit_y * static_cast<uint32_t>(sps.frame_crop_top_offset + sps.frame_crop_bottom_offset);
  if (crop_x >= width || crop_y >= height) return {width, height};
  return {width - crop_x, height - crop_y};
}

auto h265DisplaySize(const dx_h265::Sps& sps) -> std::pair<uint32_t, uint32_t> {
  const auto width = static_cast<uint32_t>(sps.pic_width_in_luma_samples);
  const auto height = static_cast<uint32_t>(sps.pic_height_in_luma_samples);
  if (!sps.conformance_window_flag) return {width, height};
  const uint32_t sub_x = (sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2) ? 2u : 1u;
  const uint32_t sub_y = sps.chroma_format_idc == 1 ? 2u : 1u;
  const uint32_t crop_x = sub_x * static_cast<uint32_t>(sps.conf_win_left_offset + sps.conf_win_right_offset);
  const uint32_t crop_y = sub_y * static_cast<uint32_t>(sps.conf_win_top_offset + sps.conf_win_bottom_offset);
  if (crop_x >= width || crop_y >= height) return {width, height};
  return {width - crop_x, height - crop_y};
}

auto vp9ColorSpace(uint8_t cs) -> std::pair<OMColorPrimaries, OMColorSpace> {
  switch (cs) {
    case video_parser::VP9_CS_BT_601:
    case video_parser::VP9_CS_SMPTE_170: return {OM_PRIMARIES_BT601, OM_COLOR_SPACE_BT601};
    case video_parser::VP9_CS_BT_709: return {OM_PRIMARIES_BT709, OM_COLOR_SPACE_BT709};
    case video_parser::VP9_CS_SMPTE_240: return {OM_PRIMARIES_SMPTE240M, OM_COLOR_SPACE_SMPTE240M};
    case video_parser::VP9_CS_BT_2020: return {OM_PRIMARIES_BT2020, OM_COLOR_SPACE_BT2020};
    case video_parser::VP9_CS_RGB: return {OM_PRIMARIES_BT709, OM_COLOR_SPACE_RGB};
    default: return {OM_PRIMARIES_UNKNOWN, OM_COLOR_SPACE_UNKNOWN};
  }
}

// One HEVC picture the decoder still holds as a reference.
struct HevcRef {
  int surface = -1;
  int32_t poc = 0;
  bool long_term = false;
};

// A reference slot of VP9 or AV1. AV1 with film grain produces two surfaces
// per frame: the clean one later frames predict from and the grained one that
// is shown, possibly again later through show_existing_frame.
struct SlotRef {
  int surface = -1;
  int display = -1;
};

class VAAPIDecoder final : public Decoder, private DecodeReport {
public:
  VAAPIDecoder() : DecodeReport("vaapi") {}
  ~VAAPIDecoder() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    release();
    codec_id_ = options.format.codec_id;
    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 && codec_id_ != OM_CODEC_VP9 &&
        codec_id_ != OM_CODEC_AV1) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    auto& libva = LibVA::getInstance();
    if (!libva.load()) return OM_CODEC_HWACCEL_FAILED;

    if (options.hw_device && options.hw_device->type == HWDeviceType::VAAPI && options.hw_device->context) {
      display_ = static_cast<OMVAAPIContext*>(options.hw_device->context)->display;
    } else {
      owned_display_ = vaapi::openDisplay();
      if (!owned_display_) return OM_CODEC_HWACCEL_FAILED;
      display_ = owned_display_->display;
    }
    if (!display_) return OM_CODEC_HWACCEL_FAILED;

    profiles_ = vaapi::queryProfiles(display_);
    if (!anyDecodeProfile()) {
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] the driver cannot decode {}", codecName(codec_id_));
      release();
      return OM_CODEC_NOT_SUPPORTED;
    }

    hardware_output_ = options.hardware_output;
    const auto& video = options.format.video;
    output_format_ = {};
    output_format_.width = video.width;
    output_format_.height = video.height;
    output_format_.format = video.format == OM_FORMAT_P010 ? OM_FORMAT_P010 : OM_FORMAT_NV12;
    output_format_.color_space = video.color_space;
    output_format_.transfer_char = video.transfer_char;
    output_format_.color_primaries = video.color_primaries;
    output_format_.color_range = video.color_range;
    output_format_.mastering_display = video.mastering_display;
    output_format_.content_light_level = video.content_light_level;

    // Wherever the out-of-band configuration says what the stream needs, check
    // it now: a decoder that fails here can be replaced by a software one,
    // whereas one that fails on the first picture just shows nothing.
    std::optional<SessionParams> expected;
    if (codec_id_ == OM_CODEC_H264) {
      h264_.parseExtradata(options.extradata);
      if (const h264::SPS* sps = firstH264Sps()) {
        updateH264Format(*sps);
        const auto params = h264Session(*sps);
        if (!params) return unsupported("H.264 stream (profile_idc {}, {}-bit, chroma_format_idc {})", sps->profile_idc,
                                        sps->bit_depth_luma_minus8 + 8, sps->chroma_format_idc);
        expected = params;
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      h265_ = std::make_unique<video_parser::H265AccessUnitParser>();
      h265_->parseExtradata(options.extradata);
      if (const dx_h265::Sps* sps = firstH265Sps()) {
        updateH265Format(*sps);
        const auto params = h265Session(*sps);
        if (!params) return unsupported("H.265 stream ({}-bit, chroma_format_idc {})", sps->bit_depth_luma_minus8 + 8,
                                        sps->chroma_format_idc);
        expected = params;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      vp9_ = std::make_unique<video_parser::VP9FrameParser>();
      // VP9 has no out-of-band configuration; the profile is all there is.
      const bool high_bit_depth = options.format.profile == OM_PROFILE_VP9_2 || video.format == OM_FORMAT_P010;
      if (!pickProfile(high_bit_depth ? VAProfileVP9Profile2 : VAProfileVP9Profile0)) {
        return unsupported("VP9 profile {}", high_bit_depth ? 2 : 0);
      }
    } else {
      av1_ = std::make_unique<video_parser::AV1ObuParser>();
      // The AV1CodecConfigurationRecord prefixes the configOBUs with four bytes
      // of its own.
      if (options.extradata.size() > 4 && (options.extradata[0] & 0x80) != 0) {
        av1_->parse(options.extradata.subspan(4));
      }
      const auto& seq = av1_->sequenceHeader();
      if (seq.valid) {
        updateAV1Format(seq);
        const auto params = av1Session(seq);
        if (!params) return unsupported("AV1 stream (profile {}, {}-bit)", seq.seq_profile, seq.color_config.bit_depth);
        expected = params;
      }
    }

    if (expected && !vaapi::supportsEntrypoint(display_, expected->profile, VAEntrypointVLD)) {
      return unsupported("{} decoding", profileName(expected->profile));
    }

    if (libva.vaQueryVendorString) {
      if (const char* vendor = libva.vaQueryVendorString(display_)) {
        log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] {} decoder on {}", codecName(codec_id_), vendor);
      }
    }

    waiting_for_key_ = true;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;
    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);

    std::vector<Frame> output;
    if (packet.bytes.empty()) {
      output = reorder_.drain();
      finishOutput(output);
      return Ok(std::move(output));
    }

    OMError error = OM_SUCCESS;
    switch (codec_id_) {
      case OM_CODEC_H264: error = decodeH264(packet, output); break;
      case OM_CODEC_H265: error = decodeH265(packet, output); break;
      case OM_CODEC_VP9: error = decodeVP9(packet, output); break;
      case OM_CODEC_AV1: error = decodeAV1(packet, output); break;
      default: error = OM_CODEC_NOT_SUPPORTED; break;
    }
    finishOutput(output);
    // Pictures that did come out are worth more than the error: the stream
    // goes on, and the error has been logged already.
    if (error != OM_SUCCESS && output.empty()) return Err(error);
    return Ok(std::move(output));
  }

  void flush() override {
    resetReceiveState();
    reorder_.clear();
    resetReferences();
    h264_poc_.reset();
    if (h265_) h265_->restart();
    if (vp9_) vp9_->reset();
    if (av1_) av1_->restart();
    waiting_for_key_ = true;
    hevc_skip_rasl_ = true;
  }

private:
  template<typename... Args>
  auto unsupported(std::format_string<Args...> fmt, Args&&... args) -> OMError {
    log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] the driver cannot decode this {}",
        std::format(fmt, std::forward<Args>(args)...));
    release();
    return OM_CODEC_NOT_SUPPORTED;
  }

  // ---------------------------------------------------------------------------
  // Session: VA config, context and surfaces
  // ---------------------------------------------------------------------------

  auto hasProfile(VAProfile profile) const -> bool {
    return std::find(profiles_.begin(), profiles_.end(), profile) != profiles_.end();
  }

  auto pickProfile(VAProfile profile) const -> bool {
    return hasProfile(profile) && vaapi::supportsEntrypoint(display_, profile, VAEntrypointVLD);
  }

  auto anyDecodeProfile() const -> bool {
    const auto any = [this](std::initializer_list<VAProfile> candidates) {
      return std::any_of(candidates.begin(), candidates.end(), [this](VAProfile p) { return pickProfile(p); });
    };
    switch (codec_id_) {
      case OM_CODEC_H264: return any({VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline});
      case OM_CODEC_H265: return any({VAProfileHEVCMain, VAProfileHEVCMain10});
      case OM_CODEC_VP9: return any({VAProfileVP9Profile0, VAProfileVP9Profile2});
      case OM_CODEC_AV1: return any({VAProfileAV1Profile0});
      default: return false;
    }
  }

  auto slack() const -> size_t { return hardware_output_ ? kHardwareOutputSlack : kHostOutputSlack; }

  // Makes sure a session exists that can decode pictures described by `want`.
  // A session that is large enough is kept; anything else starts a new one,
  // and with it every reference the old one held is gone.
  auto ensureSession(const SessionParams& want) -> OMError {
    auto& libva = LibVA::getInstance();
    if (pool_ && context_ != VA_INVALID_ID && session_.profile == want.profile &&
        session_.rt_format == want.rt_format && want.width <= session_.width && want.height <= session_.height) {
      if (pool_->size() < want.surfaces) pool_->grow(want.surfaces);
      return OM_SUCCESS;
    }

    destroySession();
    resetReferences();

    if (!pickProfile(want.profile)) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "{} is not supported by the driver", profileName(want.profile));
    }

    VAConfigAttrib attribs[3] = {};
    attribs[0].type = VAConfigAttribRTFormat;
    attribs[1].type = VAConfigAttribMaxPictureWidth;
    attribs[2].type = VAConfigAttribMaxPictureHeight;
    if (libva.vaGetConfigAttributes(display_, want.profile, VAEntrypointVLD, attribs, 3) == VA_STATUS_SUCCESS) {
      if (attribs[0].value != VA_ATTRIB_NOT_SUPPORTED && (attribs[0].value & want.rt_format) == 0) {
        return rejectFrame(OM_CODEC_NOT_SUPPORTED, "{} decoding does not support the needed surface format",
                           profileName(want.profile));
      }
      const bool too_wide = attribs[1].value != VA_ATTRIB_NOT_SUPPORTED && want.width > attribs[1].value;
      const bool too_tall = attribs[2].value != VA_ATTRIB_NOT_SUPPORTED && want.height > attribs[2].value;
      if (too_wide || too_tall) {
        return rejectFrame(OM_CODEC_NOT_SUPPORTED, "{}x{} exceeds the {}x{} the driver decodes", want.width,
                           want.height, attribs[1].value, attribs[2].value);
      }
    }

    VAConfigAttrib rt = {};
    rt.type = VAConfigAttribRTFormat;
    rt.value = want.rt_format;
    VAStatus status = libva.vaCreateConfig(display_, want.profile, VAEntrypointVLD, &rt, 1, &config_);
    if (status != VA_STATUS_SUCCESS) {
      config_ = VA_INVALID_ID;
      return rejectFrame(OM_CODEC_HWACCEL_FAILED, "vaCreateConfig for {} failed: {}", profileName(want.profile),
                         vaapi::describe(status));
    }

    pool_ = vaapi::SurfacePool::create(owned_display_, display_, want.rt_format, want.width, want.height, want.surfaces);
    if (!pool_) {
      destroySession();
      return rejectFrame(OM_CODEC_HWACCEL_FAILED, "could not allocate {} surfaces of {}x{}", want.surfaces,
                         want.width, want.height);
    }

    auto surfaces = pool_->surfaces();
    status = libva.vaCreateContext(display_, config_, static_cast<int>(want.width), static_cast<int>(want.height),
                                   VA_PROGRESSIVE, surfaces.data(), static_cast<int>(surfaces.size()), &context_);
    if (status != VA_STATUS_SUCCESS) {
      context_ = VA_INVALID_ID;
      destroySession();
      return rejectFrame(OM_CODEC_HWACCEL_FAILED, "vaCreateContext failed: {}", vaapi::describe(status));
    }

    session_ = want;
    output_format_.format = vaapi::pixelFormatForFourcc(pool_->fourcc());
    log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] decoding {} into {} surfaces of {}x{} ({})",
        profileName(want.profile), surfaces.size(), want.width, want.height,
        pool_->fourcc() == VA_FOURCC_P010 ? "P010" : "NV12");
    return OM_SUCCESS;
  }

  void destroySession() {
    auto& libva = LibVA::getInstance();
    if (libva.isLoaded() && display_) {
      if (context_ != VA_INVALID_ID) libva.vaDestroyContext(display_, context_);
      if (config_ != VA_INVALID_ID) libva.vaDestroyConfig(display_, config_);
    }
    context_ = VA_INVALID_ID;
    config_ = VA_INVALID_ID;
    // Pictures the caller still holds keep the pool, and the surfaces in it,
    // alive through their own references.
    pool_.reset();
    session_ = {};
  }

  void resetReferences() {
    h264_dpb_.reset();
    hevc_dpb_.clear();
    std::fill(std::begin(vp9_refs_), std::end(vp9_refs_), -1);
    std::fill(std::begin(av1_refs_), std::end(av1_refs_), SlotRef {});
  }

  void release() {
    destroySession();
    resetReferences();
    reorder_.clear();
    // State is the better part of a megabyte; clearing it in place keeps a
    // temporary of that size off the stack.
    std::fill(std::begin(h264_.sps_valid), std::end(h264_.sps_valid), false);
    std::fill(std::begin(h264_.pps_valid), std::end(h264_.pps_valid), false);
    h264_.has_sps = false;
    h264_.has_pps = false;
    h264_poc_.reset();
    h265_.reset();
    vp9_.reset();
    av1_.reset();
    owned_display_.reset();
    display_ = nullptr;
    profiles_.clear();
    initialized_ = false;
  }

  // Whether the codec itself still needs a surface.
  auto heldByCodec(int index) const -> bool {
    switch (codec_id_) {
      case OM_CODEC_H264: return h264_dpb_.holds(index);
      case OM_CODEC_H265:
        return std::any_of(hevc_dpb_.begin(), hevc_dpb_.end(), [index](const HevcRef& r) { return r.surface == index; });
      case OM_CODEC_VP9: return std::find(std::begin(vp9_refs_), std::end(vp9_refs_), index) != std::end(vp9_refs_);
      case OM_CODEC_AV1:
        return std::any_of(std::begin(av1_refs_), std::end(av1_refs_),
                           [index](const SlotRef& r) { return r.surface == index || r.display == index; });
      default: return false;
    }
  }

  auto acquireSurface(int exclude = -1) -> int {
    if (!pool_) return -1;
    return pool_->acquire([this](int index) { return heldByCodec(index); }, exclude);
  }

  auto vaSurface(int index) const -> VASurfaceID { return pool_ ? pool_->surface(index) : VA_INVALID_SURFACE; }

  // ---------------------------------------------------------------------------
  // Output
  // ---------------------------------------------------------------------------

  auto makeFrame(int index, uint32_t width, uint32_t height, int64_t pts, int64_t dts, bool keyframe)
      -> std::optional<Frame> {
    auto hardware = pool_ ? pool_->wrap(index) : nullptr;
    if (!hardware) return std::nullopt;
    Picture pic;
    pic.format = vaapi::pixelFormatForFourcc(pool_->fourcc());
    pic.width = width;
    pic.height = height;
    pic.is_keyframe = keyframe;
    pic.color_space = output_format_.color_space;
    pic.transfer_char = output_format_.transfer_char;
    pic.color_primaries = output_format_.color_primaries;
    pic.color_range = output_format_.color_range;
    pic.mastering_display = output_format_.mastering_display;
    pic.content_light_level = output_format_.content_light_level;
    pic.buffer = std::move(hardware);
    Frame frame;
    frame.pts = pts;
    frame.dts = dts;
    frame.data = std::move(pic);
    return frame;
  }

  // Pictures are carried as surfaces until they leave the decoder; a caller
  // that wants system memory gets the copy made only then. By that time the GPU
  // is long done with the picture, so the read-back does not wait on it.
  void finishOutput(std::vector<Frame>& frames) {
    if (hardware_output_) return;
    std::vector<Frame> ready;
    ready.reserve(frames.size());
    for (auto& frame : frames) {
      auto* pic = std::get_if<Picture>(&frame.data);
      if (!pic) continue;
      auto* hardware = std::get_if<std::shared_ptr<HardwarePicture>>(&pic->buffer);
      if (!hardware || !*hardware) {
        ready.push_back(std::move(frame));
        continue;
      }
      const auto pooled = std::dynamic_pointer_cast<vaapi::PooledPicture>(*hardware);
      if (!pooled || !pooled->pool()->download(pooled->index(), pic->width, pic->height, *pic)) {
        rejectFrame(OM_CODEC_DECODE_FAILED, "could not read a decoded surface back");
        continue;
      }
      ready.push_back(std::move(frame));
    }
    frames = std::move(ready);
  }

  void emitReordered(std::optional<Frame> frame, int32_t poc, size_t depth, std::vector<Frame>& output) {
    if (!frame) return;
    for (auto& ready : reorder_.push(std::move(*frame), poc, depth)) output.push_back(std::move(ready));
  }

  void drainReorder(std::vector<Frame>& output) {
    for (auto& held : reorder_.drain()) output.push_back(std::move(held));
  }

  // ---------------------------------------------------------------------------
  // H.264
  // ---------------------------------------------------------------------------

  auto firstH264Sps() const -> const h264::SPS* {
    for (uint32_t i = 0; i < 32; ++i) {
      if (h264_.sps_valid[i]) return &h264_.sps[i];
    }
    return nullptr;
  }

  void updateH264Format(const h264::SPS& sps) {
    const auto [width, height] = h264DisplaySize(sps);
    output_format_.width = width;
    output_format_.height = height;
    if (sps.vui_parameters_present_flag && sps.vui.colour_description_present_flag) {
      output_format_.color_primaries = color_codes::primariesFromCode(sps.vui.colour_primaries);
      output_format_.transfer_char = color_codes::transferFromCode(sps.vui.transfer_characteristics);
      output_format_.color_space = color_codes::colorSpaceFromMatrix(sps.vui.matrix_coefficients);
    }
    if (sps.vui_parameters_present_flag && sps.vui.video_signal_type_present_flag) {
      output_format_.color_range = sps.vui.video_full_range_flag ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
    }
  }

  auto h264Session(const h264::SPS& sps) const -> std::optional<SessionParams> {
    if (sps.chroma_format_idc > 1 || sps.separate_colour_plane_flag) return std::nullopt;
    if (sps.bit_depth_luma_minus8 != sps.bit_depth_chroma_minus8) return std::nullopt;

    SessionParams params;
    if (sps.bit_depth_luma_minus8 == 0) {
      // High decodes everything Main and Constrained Baseline do, so it is the
      // fallback when the driver does not list the exact profile.
      std::vector<VAProfile> candidates;
      if (sps.profile_idc == 66 && sps.constraint_set1_flag) candidates.push_back(VAProfileH264ConstrainedBaseline);
      if (sps.profile_idc == 66 || sps.profile_idc == 77) candidates.push_back(VAProfileH264Main);
      candidates.push_back(VAProfileH264High);
      for (VAProfile p : candidates) {
        if (pickProfile(p)) {
          params.profile = p;
          break;
        }
      }
      params.rt_format = VA_RT_FORMAT_YUV420;
    } else if (sps.bit_depth_luma_minus8 == 2 && pickProfile(VAProfileH264High10)) {
      params.profile = VAProfileH264High10;
      params.rt_format = VA_RT_FORMAT_YUV420_10;
    }
    if (params.profile == VAProfileNone) return std::nullopt;

    params.width = static_cast<uint32_t>(sps.pic_width_in_mbs_minus1 + 1) * 16u;
    params.height = static_cast<uint32_t>(sps.pic_height_in_map_units_minus1 + 1) * 16u *
                    (sps.frame_mbs_only_flag ? 1u : 2u);
    params.surfaces = static_cast<size_t>(std::max(sps.num_ref_frames, 1)) + dx_h264::reorderDepth(sps) + 1 + slack();
    return params;
  }

  auto decodeH264(const Packet& packet, std::vector<Frame>& output) -> OMError {
    hdr_sei::parseAnnexB(packet.bytes, false, output_format_.mastering_display, output_format_.content_light_level);

    const std::span<const uint8_t> data(packet.bytes.data(), packet.bytes.size());
    std::vector<size_t> headers;
    {
      h264::Bitstream scan;
      scan.init(data.data(), data.size());
      while (h264::find_next_nal(scan)) headers.push_back(scan.byte_offset());
    }

    OMError result = OM_SUCCESS;
    std::vector<H264Slice> slices;
    const auto flushPicture = [&] {
      if (slices.empty()) return;
      const OMError error = decodeH264Picture(slices, packet, output);
      if (error != OM_SUCCESS) result = error;
      slices.clear();
    };

    for (size_t i = 0; i < headers.size(); ++i) {
      const size_t begin = headers[i];
      size_t end = i + 1 < headers.size() ? headers[i + 1] - 3 : data.size();
      // A four byte start code leaves its leading zero on the NAL before it.
      while (end > begin && data[end - 1] == 0) --end;
      if (end <= begin) continue;
      const auto nal = data.subspan(begin, end - begin);

      h264::Bitstream bs;
      bs.init(nal.data(), nal.size());
      h264::NALHeader header;
      if (!h264::read_nal_header(header, bs)) continue;

      switch (header.type) {
        case h264::NAL_UNIT_TYPE_SPS:
        case h264::NAL_UNIT_TYPE_PPS: {
          flushPicture();
          h264_.storeNal(nal);
          if (header.type == h264::NAL_UNIT_TYPE_SPS) {
            if (const h264::SPS* sps = firstH264Sps()) updateH264Format(*sps);
          }
          break;
        }
        case h264::NAL_UNIT_TYPE_CODED_SLICE_IDR:
        case h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR: {
          if (!h264_.has_sps || !h264_.has_pps) break;
          H264Slice slice;
          slice.nal = nal;
          slice.header = header;
          if (!h264::read_slice_header(slice.slice, header, h264_.pps, h264_.sps, bs, h264_.rbsp_scratch)) {
            rejectFrame(OM_CODEC_DECODE_FAILED, "H.264 slice header of {} bytes did not parse", nal.size());
            break;
          }
          // Redundant pictures only matter to a decoder that lost the primary.
          if (slice.slice.redundant_pic_cnt > 0) break;
          if (!slices.empty()) {
            const auto& sps = h264_.sps[h264_.pps[slice.slice.pic_parameter_set_id].seq_parameter_set_id];
            if (startsNewPicture(slices.back(), slice, sps)) flushPicture();
          }
          slices.push_back(std::move(slice));
          break;
        }
        case h264::NAL_UNIT_TYPE_AUD:
        case h264::NAL_UNIT_TYPE_END_OF_SEQUENCE:
        case h264::NAL_UNIT_TYPE_END_OF_STREAM:
          flushPicture();
          break;
        default:
          break;
      }
    }
    flushPicture();
    return result;
  }

  auto vaH264Picture(const h264_dpb::RefPicture& ref) const -> VAPictureH264 {
    VAPictureH264 pic = {};
    if (ref.nonExisting()) {
      pic.picture_id = VA_INVALID_SURFACE;
      pic.flags = VA_PICTURE_H264_INVALID;
      return pic;
    }
    pic.picture_id = vaSurface(ref.surface);
    pic.frame_idx = static_cast<uint32_t>(ref.long_term ? ref.long_term_frame_idx : ref.frame_num);
    pic.flags = ref.long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    pic.TopFieldOrderCnt = ref.top_poc;
    pic.BottomFieldOrderCnt = ref.bottom_poc;
    return pic;
  }

  static void invalidate(VAPictureH264& pic) {
    pic = {};
    pic.picture_id = VA_INVALID_SURFACE;
    pic.flags = VA_PICTURE_H264_INVALID;
  }

  auto decodeH264Picture(const std::vector<H264Slice>& slices, const Packet& packet, std::vector<Frame>& output)
      -> OMError {
    const H264Slice& first = slices.front();
    const h264::SliceHeader& head = first.slice;
    const h264::PPS& pps = h264_.pps[head.pic_parameter_set_id];
    const h264::SPS& sps = h264_.sps[pps.seq_parameter_set_id];

    if (head.field_pic_flag) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "H.264 field pictures (PAFF) are not supported; MBAFF is");
    }
    if (pps.num_slice_groups_minus1 > 0) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "H.264 slice groups (FMO) are not supported by VA-API");
    }
    const auto params = h264Session(sps);
    if (!params) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "H.264 profile_idc {} ({}-bit, chroma_format_idc {}) is not supported",
                         sps.profile_idc, sps.bit_depth_luma_minus8 + 8, sps.chroma_format_idc);
    }

    const bool is_idr = first.header.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
    const bool is_reference = first.header.idc != h264::NAL_REF_IDC_PRIORITY_DISPOSABLE;

    // After a seek the stream is entered at a random access point: an IDR, or
    // an all-intra picture (open GOP streams mark those with a recovery point
    // SEI). Anything before it predicts from pictures that were never decoded.
    if (waiting_for_key_) {
      const bool intra = std::all_of(slices.begin(), slices.end(), [](const H264Slice& s) { return isIntraSlice(s.slice); });
      if (!is_idr && !intra) return OM_SUCCESS;
      waiting_for_key_ = false;
    }

    if (const OMError error = ensureSession(*params); error != OM_SUCCESS) return error;

    const int max_frame_num = 1 << (sps.log2_max_frame_num_minus4 + 4);
    if (is_idr) {
      drainReorder(output);
      h264_dpb_.reset();
      h264_poc_.reset();
    } else {
      h264_dpb_.fillFrameNumGap(sps, head.frame_num, [&](int frame_num) {
        // Type 0 counts cannot be inferred for a frame that was not sent, and
        // must not disturb the state the next real picture derives from.
        if (sps.pic_order_cnt_type == 0) return h264_dpb::PictureOrder {};
        h264::SliceHeader inferred = {};
        inferred.frame_num = frame_num;
        auto order = h264_poc_.compute(sps, inferred, false, true);
        h264_poc_.commit(sps, inferred, true, order);
        return order;
      });
    }

    h264_dpb::PictureOrder order = h264_poc_.compute(sps, head, is_idr, is_reference);
    h264_dpb_.updateFrameNumWrap(head.frame_num, max_frame_num);

    const int surface = acquireSurface();
    if (surface < 0) return rejectFrame(OM_CODEC_DECODE_FAILED, "H.264: no free surface to decode into");

    VAPictureParameterBufferH264 pic = {};
    pic.CurrPic.picture_id = vaSurface(surface);
    pic.CurrPic.frame_idx = static_cast<uint32_t>(head.frame_num);
    pic.CurrPic.flags = 0;
    if (is_reference) {
      pic.CurrPic.flags = (is_idr && head.long_term_reference_flag) ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                                                      : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    }
    pic.CurrPic.TopFieldOrderCnt = order.top;
    pic.CurrPic.BottomFieldOrderCnt = order.bottom;

    for (auto& ref : pic.ReferenceFrames) invalidate(ref);
    size_t ref_count = 0;
    for (const auto& ref : h264_dpb_.pictures()) {
      if (ref.nonExisting() || ref_count >= std::size(pic.ReferenceFrames)) continue;
      pic.ReferenceFrames[ref_count++] = vaH264Picture(ref);
    }

    pic.picture_width_in_mbs_minus1 = static_cast<uint16_t>(sps.pic_width_in_mbs_minus1);
    pic.picture_height_in_mbs_minus1 = static_cast<uint16_t>(
        (sps.pic_height_in_map_units_minus1 + 1) * (sps.frame_mbs_only_flag ? 1 : 2) - 1);
    pic.bit_depth_luma_minus8 = static_cast<uint8_t>(sps.bit_depth_luma_minus8);
    pic.bit_depth_chroma_minus8 = static_cast<uint8_t>(sps.bit_depth_chroma_minus8);
    pic.num_ref_frames = static_cast<uint8_t>(sps.num_ref_frames);
    pic.seq_fields.bits.chroma_format_idc = static_cast<uint32_t>(sps.chroma_format_idc);
    pic.seq_fields.bits.residual_colour_transform_flag = static_cast<uint32_t>(sps.separate_colour_plane_flag);
    pic.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = static_cast<uint32_t>(sps.gaps_in_frame_num_value_allowed_flag);
    pic.seq_fields.bits.frame_mbs_only_flag = static_cast<uint32_t>(sps.frame_mbs_only_flag);
    pic.seq_fields.bits.mb_adaptive_frame_field_flag = static_cast<uint32_t>(sps.mb_adaptive_frame_field_flag);
    pic.seq_fields.bits.direct_8x8_inference_flag = static_cast<uint32_t>(sps.direct_8x8_inference_flag);
    pic.seq_fields.bits.MinLumaBiPredSize8x8 = sps.level_idc >= 31 ? 1u : 0u; // A.3.3.2
    pic.seq_fields.bits.log2_max_frame_num_minus4 = static_cast<uint32_t>(sps.log2_max_frame_num_minus4);
    pic.seq_fields.bits.pic_order_cnt_type = static_cast<uint32_t>(sps.pic_order_cnt_type);
    pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4);
    pic.seq_fields.bits.delta_pic_order_always_zero_flag = static_cast<uint32_t>(sps.delta_pic_order_always_zero_flag);
    pic.pic_init_qp_minus26 = static_cast<int8_t>(pps.pic_init_qp_minus26);
    pic.pic_init_qs_minus26 = static_cast<int8_t>(pps.pic_init_qs_minus26);
    pic.chroma_qp_index_offset = static_cast<int8_t>(pps.chroma_qp_index_offset);
    pic.second_chroma_qp_index_offset = static_cast<int8_t>(pps.second_chroma_qp_index_offset);
    pic.pic_fields.bits.entropy_coding_mode_flag = static_cast<uint32_t>(pps.entropy_coding_mode_flag);
    pic.pic_fields.bits.weighted_pred_flag = static_cast<uint32_t>(pps.weighted_pred_flag);
    pic.pic_fields.bits.weighted_bipred_idc = static_cast<uint32_t>(pps.weighted_bipred_idc);
    pic.pic_fields.bits.transform_8x8_mode_flag = static_cast<uint32_t>(pps.transform_8x8_mode_flag);
    pic.pic_fields.bits.field_pic_flag = 0;
    pic.pic_fields.bits.constrained_intra_pred_flag = static_cast<uint32_t>(pps.constrained_intra_pred_flag);
    pic.pic_fields.bits.pic_order_present_flag = static_cast<uint32_t>(pps.pic_order_present_flag);
    pic.pic_fields.bits.deblocking_filter_control_present_flag = static_cast<uint32_t>(pps.deblocking_filter_control_present_flag);
    pic.pic_fields.bits.redundant_pic_cnt_present_flag = static_cast<uint32_t>(pps.redundant_pic_cnt_present_flag);
    pic.pic_fields.bits.reference_pic_flag = is_reference ? 1u : 0u;
    pic.frame_num = static_cast<uint16_t>(head.frame_num);

    // The PPS lists already went through the Table 7-2 fall-back rules, which
    // is where a PPS without lists of its own inherits the SPS ones.
    VAIQMatrixBufferH264 iq = {};
    {
      const int (*lists4)[16] = nullptr;
      const int (*lists8)[64] = nullptr;
      if (pps.pic_scaling_matrix_present_flag) {
        lists4 = pps.ScalingList4x4;
        lists8 = pps.ScalingList8x8;
      } else if (sps.seq_scaling_matrix_present_flag) {
        lists4 = sps.ScalingList4x4;
        lists8 = sps.ScalingList8x8;
      }
      if (lists4) {
        for (int i = 0; i < 6; ++i)
          for (int k = 0; k < 16; ++k) iq.ScalingList4x4[i][kZigzag4x4[k]] = static_cast<uint8_t>(lists4[i][k]);
        // VA-API carries the two luma 8x8 lists: intra Y and inter Y.
        for (int i = 0; i < 2; ++i)
          for (int k = 0; k < 64; ++k) iq.ScalingList8x8[i][kZigzag8x8[k]] = static_cast<uint8_t>(lists8[i][k]);
      } else {
        std::memset(iq.ScalingList4x4, 16, sizeof(iq.ScalingList4x4));
        std::memset(iq.ScalingList8x8, 16, sizeof(iq.ScalingList8x8));
      }
    }

    vaapi::Submission submission(display_, context_);
    submission.addParam(VAPictureParameterBufferType, &pic, sizeof(pic));
    submission.addParam(VAIQMatrixBufferType, &iq, sizeof(iq));

    size_t missing = 0;
    for (const auto& s : slices) {
      VASliceParameterBufferH264 slice = {};
      fillH264Slice(s, pps, max_frame_num, order.poc(), slice, missing);
      submission.addSlice(&slice, sizeof(slice), 1, s.nal.data(), s.nal.size());
    }
    reportMissingReferences(missing, order.poc());

    const VAStatus status = submission.execute(vaSurface(surface));

    // The picture is marked even when the driver rejected it: the stream goes
    // on referring to it by frame_num, and a buffer that lost track of it
    // would mis-address every reference after it.
    h264_poc_.commit(sps, head, is_reference, order);
    h264_dpb_.markCurrent(sps, head, is_idr, is_reference, surface, order);

    if (status != VA_STATUS_SUCCESS) {
      return rejectFrame(OM_CODEC_DECODE_FAILED, "H.264 picture {} failed to decode: {}", order.poc(),
                         vaapi::describe(status));
    }

    // C.4.4: memory_management_control_operation 5 starts the counts over, so
    // everything before it goes out first.
    if (head.mmco5) drainReorder(output);
    const auto [width, height] = h264DisplaySize(sps);
    emitReordered(makeFrame(surface, width, height, packet.pts, packet.dts, is_idr), order.poc(),
                  dx_h264::reorderDepth(sps), output);
    return OM_SUCCESS;
  }

  void fillH264Slice(const H264Slice& s, const h264::PPS& pps, int max_frame_num, int32_t curr_poc,
                     VASliceParameterBufferH264& out, size_t& missing) const {
    const h264::SliceHeader& sh = s.slice;
    const int type = sh.slice_type % 5;
    const bool p_slice = type == 0 || type == 3;
    const bool b_slice = type == 1;

    out.slice_data_size = static_cast<uint32_t>(s.nal.size());
    out.slice_data_offset = 0;
    out.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    // In RBSP bits and counting the NAL header byte, as ffmpeg and Chromium
    // pass it; drivers skip the emulation prevention bytes themselves.
    out.slice_data_bit_offset = static_cast<uint16_t>(8 + sh.header_bit_size);
    out.first_mb_in_slice = static_cast<uint16_t>(sh.first_mb_in_slice);
    out.slice_type = static_cast<uint8_t>(type);
    out.direct_spatial_mv_pred_flag = b_slice ? static_cast<uint8_t>(sh.direct_spatial_mv_pred_flag) : 0;
    out.num_ref_idx_l0_active_minus1 = (p_slice || b_slice) ? static_cast<uint8_t>(sh.num_ref_idx_l0_active_minus1) : 0;
    out.num_ref_idx_l1_active_minus1 = b_slice ? static_cast<uint8_t>(sh.num_ref_idx_l1_active_minus1) : 0;
    out.cabac_init_idc = static_cast<uint8_t>(sh.cabac_init_idc);
    out.slice_qp_delta = static_cast<int8_t>(sh.slice_qp_delta);
    out.disable_deblocking_filter_idc = static_cast<uint8_t>(sh.disable_deblocking_filter_idc);
    out.slice_alpha_c0_offset_div2 = static_cast<int8_t>(sh.slice_alpha_c0_offset_div2);
    out.slice_beta_offset_div2 = static_cast<int8_t>(sh.slice_beta_offset_div2);

    for (auto& ref : out.RefPicList0) invalidate(ref);
    for (auto& ref : out.RefPicList1) invalidate(ref);

    // Entries past the pictures the buffer holds are "no reference picture"
    // and legitimately so -- num_ref_idx_active may well exceed what exists
    // early in a stream. Only a modification naming a picture that is gone
    // means a reference was lost.
    const auto& refs = h264_dpb_.pictures();
    const auto fill = [&](const h264_dpb::RefList& list, VAPictureH264 (&into)[32]) {
      for (size_t i = 0; i < list.size() && i < 32; ++i) {
        if (list[i] >= 0) into[i] = vaH264Picture(refs[static_cast<size_t>(list[i])]);
      }
    };
    if (p_slice) {
      h264_dpb::RefList list0 = h264_dpb_.initialListP();
      if (!h264_dpb_.finishList(list0, sh, 0, max_frame_num)) ++missing;
      fill(list0, out.RefPicList0);
    } else if (b_slice) {
      h264_dpb::RefList list0;
      h264_dpb::RefList list1;
      h264_dpb_.initialListsB(curr_poc, list0, list1);
      if (!h264_dpb_.finishList(list0, sh, 0, max_frame_num)) ++missing;
      if (!h264_dpb_.finishList(list1, sh, 1, max_frame_num)) ++missing;
      fill(list0, out.RefPicList0);
      fill(list1, out.RefPicList1);
    }

    const bool explicit_weights = (pps.weighted_pred_flag && p_slice) || (pps.weighted_bipred_idc == 1 && b_slice);
    if (!explicit_weights) return;
    const auto& pwt = sh.pwt;
    out.luma_log2_weight_denom = static_cast<uint8_t>(pwt.luma_log2_weight_denom);
    out.chroma_log2_weight_denom = static_cast<uint8_t>(pwt.chroma_log2_weight_denom);

    // The tables carry the inferred defaults for the entries whose flag is
    // off, which is what VA-API wants (it infers nothing).
    const auto copy = [](int count, const int* luma_flag, const int* luma_weight, const int* luma_offset,
                         const int* chroma_flag, const int (*chroma_weight)[2], const int (*chroma_offset)[2],
                         uint8_t& out_luma_flag, int16_t* out_luma_weight, int16_t* out_luma_offset,
                         uint8_t& out_chroma_flag, int16_t (*out_chroma_weight)[2], int16_t (*out_chroma_offset)[2]) {
      for (int i = 0; i < count && i < 32; ++i) {
        if (luma_flag[i]) out_luma_flag = 1;
        if (chroma_flag[i]) out_chroma_flag = 1;
        out_luma_weight[i] = static_cast<int16_t>(luma_weight[i]);
        out_luma_offset[i] = static_cast<int16_t>(luma_offset[i]);
        for (int j = 0; j < 2; ++j) {
          out_chroma_weight[i][j] = static_cast<int16_t>(chroma_weight[i][j]);
          out_chroma_offset[i][j] = static_cast<int16_t>(chroma_offset[i][j]);
        }
      }
    };
    copy(sh.num_ref_idx_l0_active_minus1 + 1, pwt.luma_weight_l0_flag, pwt.luma_weight_l0, pwt.luma_offset_l0,
         pwt.chroma_weight_l0_flag, pwt.chroma_weight_l0, pwt.chroma_offset_l0, out.luma_weight_l0_flag,
         out.luma_weight_l0, out.luma_offset_l0, out.chroma_weight_l0_flag, out.chroma_weight_l0, out.chroma_offset_l0);
    if (b_slice) {
      copy(sh.num_ref_idx_l1_active_minus1 + 1, pwt.luma_weight_l1_flag, pwt.luma_weight_l1, pwt.luma_offset_l1,
           pwt.chroma_weight_l1_flag, pwt.chroma_weight_l1, pwt.chroma_offset_l1, out.luma_weight_l1_flag,
           out.luma_weight_l1, out.luma_offset_l1, out.chroma_weight_l1_flag, out.chroma_weight_l1,
           out.chroma_offset_l1);
    }
  }

  // ---------------------------------------------------------------------------
  // H.265
  // ---------------------------------------------------------------------------

  auto firstH265Sps() const -> const dx_h265::Sps* {
    if (!h265_ || !h265_->hasSps()) return nullptr;
    for (int i = 0; i < 16; ++i) {
      if (h265_->sps(i).valid) return &h265_->sps(i);
    }
    return nullptr;
  }

  void updateH265Format(const dx_h265::Sps& sps) {
    const auto [width, height] = h265DisplaySize(sps);
    output_format_.width = width;
    output_format_.height = height;
    if (sps.vui_parameters_present_flag && sps.vui.colour_description_present_flag) {
      output_format_.color_primaries = color_codes::primariesFromCode(sps.vui.colour_primaries);
      output_format_.transfer_char = color_codes::transferFromCode(sps.vui.transfer_characteristics);
      output_format_.color_space = color_codes::colorSpaceFromMatrix(sps.vui.matrix_coeffs);
    }
    if (sps.vui_parameters_present_flag && sps.vui.video_signal_type_present_flag) {
      output_format_.color_range = sps.vui.video_full_range_flag ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
    }
  }

  // Main and Main 10. Range extensions (4:2:2, 4:4:4, 12-bit) would need the
  // RExt parameter buffers, which in turn need the range extension syntax the
  // parser does not read.
  auto h265Session(const dx_h265::Sps& sps) const -> std::optional<SessionParams> {
    if (sps.chroma_format_idc != 1 || sps.separate_colour_plane_flag) return std::nullopt;
    if (sps.bit_depth_luma_minus8 != sps.bit_depth_chroma_minus8) return std::nullopt;
    SessionParams params;
    if (sps.bit_depth_luma_minus8 == 0 && pickProfile(VAProfileHEVCMain)) {
      params.profile = VAProfileHEVCMain;
      params.rt_format = VA_RT_FORMAT_YUV420;
    } else if (sps.bit_depth_luma_minus8 == 2 && pickProfile(VAProfileHEVCMain10)) {
      params.profile = VAProfileHEVCMain10;
      params.rt_format = VA_RT_FORMAT_YUV420_10;
    } else {
      return std::nullopt;
    }
    params.width = vaapi::alignUp(static_cast<uint32_t>(sps.pic_width_in_luma_samples), 16u);
    params.height = vaapi::alignUp(static_cast<uint32_t>(sps.pic_height_in_luma_samples), 16u);
    const int layer = std::clamp(sps.max_sub_layers_minus1, 0, 7);
    const size_t dpb = static_cast<size_t>(std::clamp(sps.sps_max_dec_pic_buffering_minus1[layer] + 1, 1, 16));
    params.surfaces = dpb + dx_h265::reorderDepth(sps) + 1 + slack();
    return params;
  }

  auto decodeH265(const Packet& packet, std::vector<Frame>& output) -> OMError {
    hdr_sei::parseAnnexB(packet.bytes, true, output_format_.mastering_display, output_format_.content_light_level);
    if (!h265_) return OM_CODEC_DECODE_FAILED;

    OMError result = OM_SUCCESS;
    for (auto& parsed : h265_->parse(packet.bytes, true)) {
      if (parsed.parameter_sets_changed) {
        if (const dx_h265::Sps* sps = firstH265Sps()) updateH265Format(*sps);
      }
      if (parsed.slice_headers.empty() || parsed.slice_offsets.size() != parsed.slice_headers.size()) continue;
      const OMError error = decodeH265Picture(parsed, packet, output);
      if (error != OM_SUCCESS) result = error;
    }
    return result;
  }

  // The NAL unit a slice offset points at, without its start code, which is the
  // form ffmpeg hands VA-API slice data in and the one every driver takes.
  static auto h265SliceNal(const video_parser::H265ParsedFrame& parsed, size_t index) -> std::span<const uint8_t> {
    const auto& bs = parsed.bitstream;
    size_t begin = parsed.slice_offsets[index];
    if (begin + 3 > bs.size()) return {};
    begin += 3; // the parser emits every NAL behind a three byte start code
    size_t end = bs.size();
    for (size_t i = begin; i + 3 <= bs.size(); ++i) {
      if (bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 1) {
        end = i;
        break;
      }
    }
    while (end > begin && bs[end - 1] == 0) --end;
    return {bs.data() + begin, end - begin};
  }

  auto decodeH265Picture(const video_parser::H265ParsedFrame& parsed, const Packet& packet, std::vector<Frame>& output)
      -> OMError {
    const auto& first = parsed.slice_headers.front();
    const auto& pps = h265_->pps(first.pps_id);
    if (!pps.valid) return rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 slice refers to PPS {}, not seen", first.pps_id);
    const auto& sps = h265_->sps(pps.sps_id);
    if (!sps.valid) return rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 PPS refers to SPS {}, not seen", pps.sps_id);

    const auto params = h265Session(sps);
    if (!params) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "H.265 {}-bit chroma_format_idc {} is not supported",
                         sps.bit_depth_luma_minus8 + 8, sps.chroma_format_idc);
    }

    const int nal_type = parsed.nal_unit_type;
    const bool irap = dx_h265::isIrap(nal_type);
    const bool rasl = nal_type == video_parser::NAL_RASL_N || nal_type == video_parser::NAL_RASL_R;

    if (irap) {
      waiting_for_key_ = false;
      hevc_skip_rasl_ = parsed.no_rasl_output_flag;
    } else if (waiting_for_key_) {
      return OM_SUCCESS;
    }
    // RASL pictures of an IRAP that starts the stream over predict from
    // pictures before it that were never decoded (8.1.3).
    if (rasl && hevc_skip_rasl_) return OM_SUCCESS;

    if (const OMError error = ensureSession(*params); error != OM_SUCCESS) return error;

    const int32_t poc = parsed.poc;
    if (irap && parsed.no_rasl_output_flag) {
      drainReorder(output);
      hevc_dpb_.clear();
    }

    // 8.3.2: the reference picture set says which pictures stay references.
    const auto set = dx_h265::referenceSet(sps, first, poc, nal_type);
    const auto wantedFor = [&](int32_t ref_poc) -> const dx_h265::WantedPicture* {
      for (size_t i = 0; i < set.count; ++i) {
        if (set.matches(set.pictures[i], ref_poc)) return &set.pictures[i];
      }
      return nullptr;
    };
    std::erase_if(hevc_dpb_, [&](const HevcRef& ref) { return wantedFor(ref.poc) == nullptr; });
    for (auto& ref : hevc_dpb_) ref.long_term = wantedFor(ref.poc)->long_term;

    const int surface = acquireSurface();
    if (surface < 0) return rejectFrame(OM_CODEC_DECODE_FAILED, "H.265: no free surface to decode into");

    VAPictureParameterBufferHEVC pic = {};
    pic.CurrPic.picture_id = vaSurface(surface);
    pic.CurrPic.pic_order_cnt = poc;
    pic.CurrPic.flags = 0;
    for (auto& ref : pic.ReferenceFrames) {
      ref.picture_id = VA_INVALID_SURFACE;
      ref.pic_order_cnt = 0;
      ref.flags = VA_PICTURE_HEVC_INVALID;
    }

    // ReferenceFrames holds every picture the buffer keeps, not only the ones
    // this picture predicts from: the driver reads surface lifetime off it.
    uint8_t va_index_of_ref[32];
    std::memset(va_index_of_ref, 0xff, sizeof(va_index_of_ref));
    uint8_t frames = 0;
    for (size_t i = 0; i < hevc_dpb_.size() && frames < std::size(pic.ReferenceFrames); ++i) {
      const auto& ref = hevc_dpb_[i];
      const auto* wanted = wantedFor(ref.poc);
      auto& va = pic.ReferenceFrames[frames];
      va.picture_id = vaSurface(ref.surface);
      va.pic_order_cnt = ref.poc;
      va.flags = ref.long_term ? VA_PICTURE_HEVC_LONG_TERM_REFERENCE : 0;
      if (wanted) {
        if (wanted->list == dx_h265::RefList::st_curr_before) va.flags |= VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        if (wanted->list == dx_h265::RefList::st_curr_after) va.flags |= VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
        if (wanted->list == dx_h265::RefList::lt_curr) va.flags |= VA_PICTURE_HEVC_RPS_LT_CURR;
      }
      if (i < std::size(va_index_of_ref)) va_index_of_ref[i] = frames;
      ++frames;
    }

    // RefPicSetStCurrBefore/StCurrAfter/LtCurr as indices into
    // ReferenceFrames, in the order the set lists them (8.3.2).
    std::vector<uint8_t> curr[3];
    size_t missing = 0;
    for (size_t i = 0; i < set.count; ++i) {
      const auto& wanted = set.pictures[i];
      if (wanted.list == dx_h265::RefList::foll) continue;
      uint8_t index = 0xff;
      for (size_t k = 0; k < hevc_dpb_.size(); ++k) {
        if (set.matches(wanted, hevc_dpb_[k].poc)) {
          index = k < std::size(va_index_of_ref) ? va_index_of_ref[k] : 0xff;
          break;
        }
      }
      if (index == 0xff) ++missing;
      curr[static_cast<size_t>(wanted.list)].push_back(index);
    }
    reportMissingReferences(missing, poc);

    fillH265PicParams(sps, pps, first, parsed, pic);

    vaapi::Submission submission(display_, context_);
    submission.addParam(VAPictureParameterBufferType, &pic, sizeof(pic));

    if (sps.scaling_list_enabled_flag) {
      // The SPS carries the Table 7-5/7-6 defaults when it enables the lists
      // without sending any.
      const auto& sl = pps.pps_scaling_list_data_present_flag ? pps.scaling_list_data : sps.scaling_list_data;
      VAIQMatrixBufferHEVC iq = {};
      for (int m = 0; m < 6; ++m) {
        for (int k = 0; k < 16; ++k) iq.ScalingList4x4[m][video_parser::kH265DiagonalScan4x4.raster[k]] = sl.scaling_list_4x4[m][k];
        for (int k = 0; k < 64; ++k) {
          const uint8_t raster = video_parser::kH265DiagonalScan8x8.raster[k];
          iq.ScalingList8x8[m][raster] = sl.scaling_list_8x8[m][k];
          iq.ScalingList16x16[m][raster] = sl.scaling_list_16x16[m][k];
          if (m < 2) iq.ScalingList32x32[m][raster] = sl.scaling_list_32x32[m][k];
        }
        iq.ScalingListDC16x16[m] = sl.scaling_list_dc_coef_16x16[m];
        if (m < 2) iq.ScalingListDC32x32[m] = sl.scaling_list_dc_coef_32x32[m];
      }
      submission.addParam(VAIQMatrixBufferType, &iq, sizeof(iq));
    }

    // Dependent slice segments carry only their address; everything else is
    // the preceding independent segment's.
    const video_parser::H265SliceHeader* independent = &first;
    for (size_t i = 0; i < parsed.slice_headers.size(); ++i) {
      const auto& segment = parsed.slice_headers[i];
      if (!segment.dependent_slice_segment_flag) independent = &segment;
      const auto nal = h265SliceNal(parsed, i);
      if (nal.empty() || segment.slice_data_byte_offset < 3) continue;

      VASliceParameterBufferHEVC slice = {};
      fillH265Slice(sps, pps, *independent, segment, curr, slice);
      slice.slice_data_size = static_cast<uint32_t>(nal.size());
      slice.slice_data_offset = 0;
      slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
      slice.slice_data_byte_offset = segment.slice_data_byte_offset - 3;
      // Emulation prevention bytes inside the header: the RBSP header ends a
      // byte past header_bit_size (byte_alignment), everything beyond that in
      // the escaped offset is escapes. Chromium passes it; some drivers use it
      // to find the slice data without re-parsing the header.
      {
        const uint32_t rbsp_bytes = 2u + (segment.header_bit_size + 8u) / 8u;
        const uint32_t escaped = slice.slice_data_byte_offset;
        slice.slice_data_num_emu_prevn_bytes =
            static_cast<uint16_t>(escaped > rbsp_bytes ? escaped - rbsp_bytes : 0);
      }
      slice.LongSliceFlags.fields.LastSliceOfPic = i + 1 == parsed.slice_headers.size() ? 1u : 0u;
      submission.addSlice(&slice, sizeof(slice), 1, nal.data(), nal.size());
    }

    const VAStatus status = submission.execute(vaSurface(surface));

    // 8.3.2: every picture is a short-term reference once decoded; the next
    // picture's set decides whether it stays one.
    hevc_dpb_.push_back({surface, poc, false});

    if (status != VA_STATUS_SUCCESS) {
      return rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 picture {} failed to decode: {}", poc, vaapi::describe(status));
    }

    // pic_output_flag 0: a picture only other pictures predict from.
    if (!first.pic_output_flag) return OM_SUCCESS;
    const auto [width, height] = h265DisplaySize(sps);
    emitReordered(makeFrame(surface, width, height, packet.pts, packet.dts, irap), poc, dx_h265::reorderDepth(sps),
                  output);
    return OM_SUCCESS;
  }

  static void fillH265PicParams(const dx_h265::Sps& sps, const dx_h265::Pps& pps,
                                const video_parser::H265SliceHeader& sh, const video_parser::H265ParsedFrame& parsed,
                                VAPictureParameterBufferHEVC& pic) {
    pic.pic_width_in_luma_samples = static_cast<uint16_t>(sps.pic_width_in_luma_samples);
    pic.pic_height_in_luma_samples = static_cast<uint16_t>(sps.pic_height_in_luma_samples);

    auto& pf = pic.pic_fields.bits;
    pf.chroma_format_idc = static_cast<uint32_t>(sps.chroma_format_idc);
    pf.separate_colour_plane_flag = sps.separate_colour_plane_flag;
    pf.pcm_enabled_flag = sps.pcm_enabled_flag;
    pf.scaling_list_enabled_flag = sps.scaling_list_enabled_flag;
    pf.transform_skip_enabled_flag = pps.transform_skip_enabled_flag;
    pf.amp_enabled_flag = sps.amp_enabled_flag;
    pf.strong_intra_smoothing_enabled_flag = sps.strong_intra_smoothing_enabled_flag;
    pf.sign_data_hiding_enabled_flag = pps.sign_data_hiding_enabled_flag;
    pf.constrained_intra_pred_flag = pps.constrained_intra_pred_flag;
    pf.cu_qp_delta_enabled_flag = pps.cu_qp_delta_enabled_flag;
    pf.weighted_pred_flag = pps.weighted_pred_flag;
    pf.weighted_bipred_flag = pps.weighted_bipred_flag;
    pf.transquant_bypass_enabled_flag = pps.transquant_bypass_enabled_flag;
    pf.tiles_enabled_flag = pps.tiles_enabled_flag;
    pf.entropy_coding_sync_enabled_flag = pps.entropy_coding_sync_enabled_flag;
    pf.pps_loop_filter_across_slices_enabled_flag = pps.pps_loop_filter_across_slices_enabled_flag;
    pf.loop_filter_across_tiles_enabled_flag = pps.tiles_enabled_flag && pps.loop_filter_across_tiles_enabled_flag;
    pf.pcm_loop_filter_disabled_flag = sps.pcm_loop_filter_disabled_flag;
    const int layer = std::clamp(sps.max_sub_layers_minus1, 0, 7);
    // Lets the driver skip keeping pictures around for reordering.
    pf.NoPicReorderingFlag = sps.sps_max_num_reorder_pics[layer] == 0 ? 1u : 0u;
    pf.NoBiPredFlag = 0;

    pic.sps_max_dec_pic_buffering_minus1 = static_cast<uint8_t>(sps.sps_max_dec_pic_buffering_minus1[layer]);
    pic.bit_depth_luma_minus8 = static_cast<uint8_t>(sps.bit_depth_luma_minus8);
    pic.bit_depth_chroma_minus8 = static_cast<uint8_t>(sps.bit_depth_chroma_minus8);
    pic.pcm_sample_bit_depth_luma_minus1 = static_cast<uint8_t>(sps.pcm_sample_bit_depth_luma_minus1);
    pic.pcm_sample_bit_depth_chroma_minus1 = static_cast<uint8_t>(sps.pcm_sample_bit_depth_chroma_minus1);
    pic.log2_min_luma_coding_block_size_minus3 = static_cast<uint8_t>(sps.log2_min_luma_coding_block_size_minus3);
    pic.log2_diff_max_min_luma_coding_block_size = static_cast<uint8_t>(sps.log2_diff_max_min_luma_coding_block_size);
    pic.log2_min_transform_block_size_minus2 = static_cast<uint8_t>(sps.log2_min_luma_transform_block_size_minus2);
    pic.log2_diff_max_min_transform_block_size = static_cast<uint8_t>(sps.log2_diff_max_min_luma_transform_block_size);
    pic.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<uint8_t>(sps.log2_min_pcm_luma_coding_block_size_minus3);
    pic.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<uint8_t>(sps.log2_diff_max_min_pcm_luma_coding_block_size);
    pic.max_transform_hierarchy_depth_intra = static_cast<uint8_t>(sps.max_transform_hierarchy_depth_intra);
    pic.max_transform_hierarchy_depth_inter = static_cast<uint8_t>(sps.max_transform_hierarchy_depth_inter);
    pic.init_qp_minus26 = static_cast<int8_t>(pps.init_qp_minus26);
    pic.diff_cu_qp_delta_depth = static_cast<uint8_t>(pps.diff_cu_qp_delta_depth);
    pic.pps_cb_qp_offset = static_cast<int8_t>(pps.pps_cb_qp_offset);
    pic.pps_cr_qp_offset = static_cast<int8_t>(pps.pps_cr_qp_offset);
    pic.log2_parallel_merge_level_minus2 = static_cast<uint8_t>(pps.log2_parallel_merge_level_minus2);

    if (pps.tiles_enabled_flag) {
      pic.num_tile_columns_minus1 = static_cast<uint8_t>(pps.num_tile_columns_minus1);
      pic.num_tile_rows_minus1 = static_cast<uint8_t>(pps.num_tile_rows_minus1);
      const int columns = std::min(pps.num_tile_columns_minus1 + 1, static_cast<int>(std::size(pic.column_width_minus1)));
      const int rows = std::min(pps.num_tile_rows_minus1 + 1, static_cast<int>(std::size(pic.row_height_minus1)));
      if (pps.uniform_spacing_flag) {
        // 6.5.1: the sizes a uniform layout implies, which drivers expect
        // spelled out either way.
        const int ctb_size = 1 << (sps.log2_min_luma_coding_block_size_minus3 + 3 +
                                   sps.log2_diff_max_min_luma_coding_block_size);
        const int width_ctbs = (sps.pic_width_in_luma_samples + ctb_size - 1) / ctb_size;
        const int height_ctbs = (sps.pic_height_in_luma_samples + ctb_size - 1) / ctb_size;
        const int n_cols = pps.num_tile_columns_minus1 + 1;
        const int n_rows = pps.num_tile_rows_minus1 + 1;
        for (int i = 0; i < columns; ++i) {
          pic.column_width_minus1[i] =
              static_cast<uint16_t>(((i + 1) * width_ctbs) / n_cols - (i * width_ctbs) / n_cols - 1);
        }
        for (int i = 0; i < rows; ++i) {
          pic.row_height_minus1[i] =
              static_cast<uint16_t>(((i + 1) * height_ctbs) / n_rows - (i * height_ctbs) / n_rows - 1);
        }
      } else {
        for (int i = 0; i < columns; ++i) pic.column_width_minus1[i] = static_cast<uint16_t>(pps.column_width_minus1[i]);
        for (int i = 0; i < rows; ++i) pic.row_height_minus1[i] = static_cast<uint16_t>(pps.row_height_minus1[i]);
      }
    }

    auto& sf = pic.slice_parsing_fields.bits;
    sf.lists_modification_present_flag = pps.lists_modification_present_flag;
    sf.long_term_ref_pics_present_flag = sps.long_term_ref_pics_present_flag;
    sf.sps_temporal_mvp_enabled_flag = sps.sps_temporal_mvp_enabled_flag;
    sf.cabac_init_present_flag = pps.cabac_init_present_flag;
    sf.output_flag_present_flag = pps.output_flag_present_flag;
    sf.dependent_slice_segments_enabled_flag = pps.dependent_slice_segments_enabled_flag;
    sf.pps_slice_chroma_qp_offsets_present_flag = pps.pps_slice_chroma_qp_offsets_present_flag;
    sf.sample_adaptive_offset_enabled_flag = sps.sample_adaptive_offset_enabled_flag;
    sf.deblocking_filter_override_enabled_flag = pps.deblocking_filter_override_enabled_flag;
    sf.pps_disable_deblocking_filter_flag = pps.pps_deblocking_filter_disabled_flag;
    sf.slice_segment_header_extension_present_flag = pps.slice_segment_header_extension_present_flag;
    sf.RapPicFlag = dx_h265::isIrap(parsed.nal_unit_type) ? 1u : 0u;
    sf.IdrPicFlag = dx_h265::isIdr(parsed.nal_unit_type) ? 1u : 0u;
    sf.IntraPicFlag = dx_h265::isIrap(parsed.nal_unit_type) ? 1u : 0u;

    pic.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(sps.log2_max_pic_order_cnt_lsb_minus4);
    pic.num_short_term_ref_pic_sets = static_cast<uint8_t>(sps.num_short_term_ref_pic_sets);
    pic.num_long_term_ref_pic_sps = static_cast<uint8_t>(sps.num_long_term_ref_pics_sps);
    pic.num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>(pps.num_ref_idx_l0_default_active_minus1);
    pic.num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>(pps.num_ref_idx_l1_default_active_minus1);
    pic.pps_beta_offset_div2 = static_cast<int8_t>(pps.pps_beta_offset_div2);
    pic.pps_tc_offset_div2 = static_cast<int8_t>(pps.pps_tc_offset_div2);
    pic.num_extra_slice_header_bits = static_cast<uint8_t>(pps.num_extra_slice_header_bits);
    // How far the hardware has to skip to get past a set coded in the slice
    // header; zero when the slice picked one of the SPS sets.
    pic.st_rps_bits = sh.short_term_ref_pic_set_sps_flag ? 0u : static_cast<uint32_t>(sh.st_rps_bits);
  }

  // 8.3.4 for one list: the temporary list cycles through the current sets
  // until it is long enough, then list_entry_lX picks from it.
  static auto h265RefList(const video_parser::H265SliceHeader& sh, int which, const std::vector<uint8_t> (&curr)[3])
      -> std::array<uint8_t, 15> {
    std::array<uint8_t, 15> list;
    list.fill(0xff);
    const size_t total = curr[0].size() + curr[1].size() + curr[2].size();
    if (total == 0) return list;

    const int active = (which == 0 ? sh.num_ref_idx_l0_active_minus1 : sh.num_ref_idx_l1_active_minus1) + 1;
    const size_t temp_size = std::max(static_cast<size_t>(active), total);
    const std::vector<uint8_t>* order[3] = {&curr[0], &curr[1], &curr[2]};
    if (which == 1) std::swap(order[0], order[1]);

    std::vector<uint8_t> temp;
    temp.reserve(temp_size);
    while (temp.size() < temp_size) {
      for (const auto* set : order) {
        for (size_t i = 0; i < set->size() && temp.size() < temp_size; ++i) temp.push_back((*set)[i]);
      }
    }

    for (int i = 0; i < active && i < 15; ++i) {
      const size_t index = sh.ref_pic_list_modification_flag[which] ? sh.list_entry[which][i] : static_cast<size_t>(i);
      list[static_cast<size_t>(i)] = index < temp.size() ? temp[index] : 0xff;
    }
    return list;
  }

  static void fillH265Slice(const dx_h265::Sps& sps, const dx_h265::Pps& pps,
                            const video_parser::H265SliceHeader& sh, const video_parser::H265SliceHeader& segment,
                            const std::vector<uint8_t> (&curr)[3], VASliceParameterBufferHEVC& out) {
    out.slice_segment_address = static_cast<uint32_t>(segment.slice_segment_address);

    auto& f = out.LongSliceFlags.fields;
    f.dependent_slice_segment_flag = segment.dependent_slice_segment_flag;
    f.slice_type = static_cast<uint32_t>(sh.slice_type);
    f.color_plane_id = static_cast<uint32_t>(sh.colour_plane_id);
    f.slice_sao_luma_flag = sh.slice_sao_luma_flag;
    f.slice_sao_chroma_flag = sh.slice_sao_chroma_flag;
    f.mvd_l1_zero_flag = sh.mvd_l1_zero_flag;
    f.cabac_init_flag = sh.cabac_init_flag;
    f.slice_temporal_mvp_enabled_flag = sh.slice_temporal_mvp_enabled_flag;
    f.slice_deblocking_filter_disabled_flag = sh.slice_deblocking_filter_disabled_flag;
    f.collocated_from_l0_flag = sh.collocated_from_l0_flag;
    f.slice_loop_filter_across_slices_enabled_flag = sh.slice_loop_filter_across_slices_enabled_flag;

    const bool intra = sh.slice_type == 2;
    const bool bipred = sh.slice_type == 0;
    out.collocated_ref_idx = sh.slice_temporal_mvp_enabled_flag ? static_cast<uint8_t>(sh.collocated_ref_idx) : 0xff;
    out.num_ref_idx_l0_active_minus1 = intra ? 0 : static_cast<uint8_t>(sh.num_ref_idx_l0_active_minus1);
    out.num_ref_idx_l1_active_minus1 = bipred ? static_cast<uint8_t>(sh.num_ref_idx_l1_active_minus1) : 0;
    out.slice_qp_delta = static_cast<int8_t>(sh.slice_qp_delta);
    out.slice_cb_qp_offset = static_cast<int8_t>(sh.slice_cb_qp_offset);
    out.slice_cr_qp_offset = static_cast<int8_t>(sh.slice_cr_qp_offset);
    out.slice_beta_offset_div2 = static_cast<int8_t>(sh.slice_beta_offset_div2);
    out.slice_tc_offset_div2 = static_cast<int8_t>(sh.slice_tc_offset_div2);
    out.five_minus_max_num_merge_cand = intra ? 0 : static_cast<uint8_t>(sh.five_minus_max_num_merge_cand);

    std::memset(out.RefPicList, 0xff, sizeof(out.RefPicList));
    if (!intra) {
      const auto list0 = h265RefList(sh, 0, curr);
      std::copy(list0.begin(), list0.end(), out.RefPicList[0]);
      if (bipred) {
        const auto list1 = h265RefList(sh, 1, curr);
        std::copy(list1.begin(), list1.end(), out.RefPicList[1]);
      }
    }

    const bool weighted = (sh.slice_type == 1 && pps.weighted_pred_flag) || (bipred && pps.weighted_bipred_flag);
    if (!weighted) return;
    const auto& pwt = sh.pred_weight_table;
    out.luma_log2_weight_denom = static_cast<uint8_t>(pwt.luma_log2_weight_denom);
    const bool chroma = sps.chroma_format_idc != 0;
    if (chroma) out.delta_chroma_log2_weight_denom = static_cast<int8_t>(pwt.delta_chroma_log2_weight_denom);
    const int chroma_denom = pwt.luma_log2_weight_denom + pwt.delta_chroma_log2_weight_denom;
    // 7-56 without high_precision_offsets_enabled_flag, which is a range
    // extension: the offsets are in 8-bit units whatever the bit depth.
    constexpr int kHalfRange = 128;
    const auto chromaOffset = [&](int delta_offset, int delta_weight) -> int8_t {
      const int weight = (1 << chroma_denom) + delta_weight;
      const int offset = kHalfRange + delta_offset - ((kHalfRange * weight) >> chroma_denom);
      return static_cast<int8_t>(std::clamp(offset, -kHalfRange, kHalfRange - 1));
    };

    for (int i = 0; i <= sh.num_ref_idx_l0_active_minus1 && i < 15; ++i) {
      out.delta_luma_weight_l0[i] = static_cast<int8_t>(pwt.delta_luma_weight_l0[i]);
      out.luma_offset_l0[i] = static_cast<int8_t>(pwt.luma_offset_l0[i]);
      for (int j = 0; j < 2 && chroma; ++j) {
        out.delta_chroma_weight_l0[i][j] = static_cast<int8_t>(pwt.delta_chroma_weight_l0[i][j]);
        out.ChromaOffsetL0[i][j] = chromaOffset(pwt.delta_chroma_offset_l0[i][j], pwt.delta_chroma_weight_l0[i][j]);
      }
    }
    if (!bipred) return;
    for (int i = 0; i <= sh.num_ref_idx_l1_active_minus1 && i < 15; ++i) {
      out.delta_luma_weight_l1[i] = static_cast<int8_t>(pwt.delta_luma_weight_l1[i]);
      out.luma_offset_l1[i] = static_cast<int8_t>(pwt.luma_offset_l1[i]);
      for (int j = 0; j < 2 && chroma; ++j) {
        out.delta_chroma_weight_l1[i][j] = static_cast<int8_t>(pwt.delta_chroma_weight_l1[i][j]);
        out.ChromaOffsetL1[i][j] = chromaOffset(pwt.delta_chroma_offset_l1[i][j], pwt.delta_chroma_weight_l1[i][j]);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // VP9
  // ---------------------------------------------------------------------------

  auto decodeVP9(const Packet& packet, std::vector<Frame>& output) -> OMError {
    if (!vp9_) return OM_CODEC_DECODE_FAILED;
    OMError result = OM_SUCCESS;
    for (const auto& parsed : vp9_->parse(packet.bytes)) {
      const OMError error = decodeVP9Frame(parsed, packet, output);
      if (error != OM_SUCCESS) result = error;
    }
    return result;
  }

  auto decodeVP9Frame(const video_parser::VP9ParsedFrame& parsed, const Packet& packet, std::vector<Frame>& output)
      -> OMError {
    const auto& h = parsed.header;
    if (!h.valid) return rejectFrame(OM_CODEC_DECODE_FAILED, "VP9 frame header of {} bytes did not parse",
                                     parsed.bitstream.size());

    if (h.show_existing_frame) {
      const int surface = vp9_refs_[h.frame_to_show_map_idx & 7];
      if (surface < 0 || waiting_for_key_) return OM_SUCCESS;
      if (auto frame = makeFrame(surface, h.frame_width, h.frame_height, packet.pts, packet.dts, false))
        output.push_back(std::move(*frame));
      return OM_SUCCESS;
    }

    if (h.subsampling_x != 1 || h.subsampling_y != 1 || (h.bit_depth != 8 && h.bit_depth != 10)) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "VP9 profile {} at {}-bit {} is not supported", h.profile, h.bit_depth,
                         h.subsampling_x ? "4:2:2" : "4:4:4");
    }

    if (waiting_for_key_) {
      if (h.frame_type != video_parser::VP9_KEY_FRAME) return OM_SUCCESS;
      waiting_for_key_ = false;
    }

    if (h.frame_type == video_parser::VP9_KEY_FRAME || h.intra_only) {
      const auto [primaries, matrix] = vp9ColorSpace(h.color_space);
      if (primaries != OM_PRIMARIES_UNKNOWN) output_format_.color_primaries = primaries;
      if (matrix != OM_COLOR_SPACE_UNKNOWN) output_format_.color_space = matrix;
      output_format_.color_range = h.color_range ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
      output_format_.width = h.frame_width;
      output_format_.height = h.frame_height;
    }

    SessionParams params;
    params.profile = h.bit_depth == 10 ? VAProfileVP9Profile2 : VAProfileVP9Profile0;
    params.rt_format = h.bit_depth == 10 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    // Inter frames may be smaller than their references (reference scaling);
    // keeping the surfaces at the largest size seen avoids tearing the
    // references down for those.
    params.width = vaapi::alignUp(std::max(h.frame_width, session_.profile == params.profile ? session_.width : 0u), 16u);
    params.height = vaapi::alignUp(std::max(h.frame_height, session_.profile == params.profile ? session_.height : 0u), 16u);
    params.surfaces = video_parser::VP9_NUM_REF_FRAMES + 1 + slack();
    // A new session starts without references, so an inter frame that needs
    // one (it grew past them, say) has nothing left to predict from.
    if (const OMError error = ensureSession(params); error != OM_SUCCESS) return error;

    if (!h.frame_is_intra) {
      for (uint32_t i = 0; i < video_parser::VP9_REFS_PER_FRAME; ++i) {
        if (vp9_refs_[h.ref_frame_idx[i] & 7] < 0) {
          reportMissingReferences(1, 0);
          return OM_SUCCESS;
        }
      }
    }

    const int surface = acquireSurface();
    if (surface < 0) return rejectFrame(OM_CODEC_DECODE_FAILED, "VP9: no free surface to decode into");

    VADecPictureParameterBufferVP9 pic = {};
    pic.frame_width = static_cast<uint16_t>(h.frame_width);
    pic.frame_height = static_cast<uint16_t>(h.frame_height);
    for (uint32_t i = 0; i < video_parser::VP9_NUM_REF_FRAMES; ++i) {
      pic.reference_frames[i] = vp9_refs_[i] >= 0 ? vaSurface(vp9_refs_[i]) : VA_INVALID_SURFACE;
    }
    auto& pf = pic.pic_fields.bits;
    pf.subsampling_x = h.subsampling_x;
    pf.subsampling_y = h.subsampling_y;
    pf.frame_type = h.frame_type;
    pf.show_frame = h.show_frame;
    pf.error_resilient_mode = h.error_resilient_mode;
    pf.intra_only = h.intra_only;
    pf.allow_high_precision_mv = h.frame_is_intra ? 0u : static_cast<uint32_t>(h.allow_high_precision_mv);
    // VA-API numbers the filters as libvpx does (EIGHTTAP = 0, SMOOTH = 1);
    // the bitstream mapping the parser uses has those two the other way round.
    pf.mcomp_filter_type = static_cast<uint32_t>(h.interpolation_filter ^ (h.interpolation_filter <= 1 ? 1 : 0));
    pf.frame_parallel_decoding_mode = h.frame_parallel_decoding_mode;
    pf.reset_frame_context = h.reset_frame_context;
    pf.refresh_frame_context = h.refresh_frame_context;
    pf.frame_context_idx = h.frame_context_idx;
    pf.segmentation_enabled = h.segmentation.enabled;
    pf.segmentation_temporal_update = h.segmentation.temporal_update;
    pf.segmentation_update_map = h.segmentation.update_map;
    pf.last_ref_frame = h.ref_frame_idx[0];
    pf.last_ref_frame_sign_bias = h.ref_frame_sign_bias[video_parser::VP9_LAST_FRAME];
    pf.golden_ref_frame = h.ref_frame_idx[1];
    pf.golden_ref_frame_sign_bias = h.ref_frame_sign_bias[video_parser::VP9_GOLDEN_FRAME];
    pf.alt_ref_frame = h.ref_frame_idx[2];
    pf.alt_ref_frame_sign_bias = h.ref_frame_sign_bias[video_parser::VP9_ALTREF_FRAME];
    pf.lossless_flag = h.quantization.lossless;
    pic.filter_level = h.loop_filter.level;
    pic.sharpness_level = h.loop_filter.sharpness;
    pic.log2_tile_rows = h.tile_rows_log2;
    pic.log2_tile_columns = h.tile_cols_log2;
    pic.frame_header_length_in_bytes = static_cast<uint8_t>(h.uncompressed_header_size);
    pic.first_partition_size = h.header_size_in_bytes;
    std::copy(std::begin(h.segmentation.tree_probs), std::end(h.segmentation.tree_probs), pic.mb_segment_tree_probs);
    if (h.segmentation.temporal_update) {
      std::copy(std::begin(h.segmentation.pred_probs), std::end(h.segmentation.pred_probs), pic.segment_pred_probs);
    } else {
      std::memset(pic.segment_pred_probs, 255, sizeof(pic.segment_pred_probs));
    }
    pic.profile = h.profile;
    pic.bit_depth = h.bit_depth;

    VASliceParameterBufferVP9 slice = {};
    slice.slice_data_size = static_cast<uint32_t>(parsed.bitstream.size());
    slice.slice_data_offset = 0;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    fillVP9Segments(h, slice);

    vaapi::Submission submission(display_, context_);
    submission.addParam(VAPictureParameterBufferType, &pic, sizeof(pic));
    // The whole frame goes in, uncompressed header included: the driver
    // skips frame_header_length_in_bytes itself.
    submission.addSlice(&slice, sizeof(slice), 1, parsed.bitstream.data(), parsed.bitstream.size());
    const VAStatus status = submission.execute(vaSurface(surface));
    if (status != VA_STATUS_SUCCESS) {
      return rejectFrame(OM_CODEC_DECODE_FAILED, "VP9 frame failed to decode: {}", vaapi::describe(status));
    }

    for (uint32_t i = 0; i < video_parser::VP9_NUM_REF_FRAMES; ++i) {
      if (h.refresh_frame_flags & (1u << i)) vp9_refs_[i] = surface;
    }
    if (h.show_frame) {
      if (auto frame = makeFrame(surface, h.frame_width, h.frame_height, packet.pts, packet.dts,
                                 h.frame_type == video_parser::VP9_KEY_FRAME)) {
        output.push_back(std::move(*frame));
      }
    }
    return OM_SUCCESS;
  }

  // VA-API wants the per-segment dequantisers and loop filter levels worked
  // out rather than the feature data (libvpx vp9_loop_filter_frame_init and
  // vp9_get_qindex).
  static void fillVP9Segments(const video_parser::VP9FrameHeader& h, VASliceParameterBufferVP9& slice) {
    const auto& seg = h.segmentation;
    const auto& lf = h.loop_filter;
    const auto& q = h.quantization;
    const int shift = lf.level >> 5;
    for (uint32_t i = 0; i < video_parser::VP9_MAX_SEGMENTS; ++i) {
      auto& out = slice.seg_param[i];
      const bool enabled = seg.enabled;

      int qindex = q.base_q_idx;
      if (enabled && seg.feature_enabled[i][0]) {
        qindex = seg.abs_or_delta_update ? seg.feature_data[i][0] : q.base_q_idx + seg.feature_data[i][0];
        qindex = std::clamp(qindex, 0, 255);
      }
      out.luma_dc_quant_scale = vp9_quant::dcQuant(qindex, q.delta_q_y_dc, h.bit_depth);
      out.luma_ac_quant_scale = vp9_quant::acQuant(qindex, 0, h.bit_depth);
      out.chroma_dc_quant_scale = vp9_quant::dcQuant(qindex, q.delta_q_uv_dc, h.bit_depth);
      out.chroma_ac_quant_scale = vp9_quant::acQuant(qindex, q.delta_q_uv_ac, h.bit_depth);

      int level = lf.level;
      if (enabled && seg.feature_enabled[i][1]) {
        level = seg.abs_or_delta_update ? seg.feature_data[i][1] : lf.level + seg.feature_data[i][1];
        level = std::clamp(level, 0, 63);
      }
      if (!lf.delta_enabled) {
        std::memset(out.filter_level, level, sizeof(out.filter_level));
      } else {
        const auto clip = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 63)); };
        out.filter_level[0][0] = clip(level + lf.ref_deltas[0] * (1 << shift));
        out.filter_level[0][1] = out.filter_level[0][0];
        for (int ref = 1; ref < 4; ++ref) {
          for (int mode = 0; mode < 2; ++mode) {
            out.filter_level[ref][mode] = clip(level + (lf.ref_deltas[ref] + lf.mode_deltas[mode]) * (1 << shift));
          }
        }
      }

      out.segment_flags.fields.segment_reference_enabled = enabled && seg.feature_enabled[i][2];
      out.segment_flags.fields.segment_reference = static_cast<uint16_t>(seg.feature_data[i][2] & 3);
      out.segment_flags.fields.segment_reference_skipped = enabled && seg.feature_enabled[i][3];
    }
  }

  // ---------------------------------------------------------------------------
  // AV1
  // ---------------------------------------------------------------------------

  void updateAV1Format(const video_parser::AV1SequenceHeader& seq) {
    const auto& cc = seq.color_config;
    if (cc.color_description_present_flag) {
      output_format_.color_primaries = color_codes::primariesFromCode(cc.color_primaries);
      output_format_.transfer_char = color_codes::transferFromCode(cc.transfer_characteristics);
      output_format_.color_space = color_codes::colorSpaceFromMatrix(cc.matrix_coefficients);
    }
    output_format_.color_range = cc.color_range ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
    if (output_format_.width == 0 || output_format_.height == 0) {
      output_format_.width = seq.max_frame_width;
      output_format_.height = seq.max_frame_height;
    }
  }

  void updateAV1HdrMetadata() {
    const auto& hdr = av1_->hdrMetadata();
    if (hdr.has_mdcv) {
      // OMMasteringDisplayMetadata follows the H.26x SEI conventions: 0.00002
      // chromaticity units, 0.0001 cd/m2 luminance, green-blue-red order. AV1
      // codes 0.16 fixed point chromaticity, 24.8 / 18.14 luminance and
      // red-green-blue order.
      auto chroma = [](uint16_t v) {
        return static_cast<uint16_t>((static_cast<uint32_t>(v) * 50000u + 32768u) / 65536u);
      };
      constexpr uint32_t kRgbForSei[3] = {1, 2, 0};
      auto& md = output_format_.mastering_display;
      for (uint32_t i = 0; i < 3; ++i) {
        md.display_primaries[i][0] = chroma(hdr.primary_chromaticity_x[kRgbForSei[i]]);
        md.display_primaries[i][1] = chroma(hdr.primary_chromaticity_y[kRgbForSei[i]]);
      }
      md.white_point[0] = chroma(hdr.white_point_chromaticity_x);
      md.white_point[1] = chroma(hdr.white_point_chromaticity_y);
      md.max_display_mastering_luminance =
          static_cast<uint32_t>((static_cast<uint64_t>(hdr.luminance_max) * 10000u + 128u) / 256u);
      md.min_display_mastering_luminance =
          static_cast<uint32_t>((static_cast<uint64_t>(hdr.luminance_min) * 10000u + 8192u) / 16384u);
      md.has_value = true;
    }
    if (hdr.has_cll) {
      auto& cll = output_format_.content_light_level;
      cll.max_content_light_level = hdr.max_cll;
      cll.max_pic_average_light_level = hdr.max_fall;
      cll.has_value = true;
    }
  }

  // Profile 0 (Main): 8 and 10-bit 4:2:0 or monochrome.
  auto av1Session(const video_parser::AV1SequenceHeader& seq) const -> std::optional<SessionParams> {
    const auto& cc = seq.color_config;
    if (seq.seq_profile != 0 || (cc.bit_depth != 8 && cc.bit_depth != 10)) return std::nullopt;
    if (!cc.mono_chrome && (cc.subsampling_x != 1 || cc.subsampling_y != 1)) return std::nullopt;
    if (!pickProfile(VAProfileAV1Profile0)) return std::nullopt;
    SessionParams params;
    params.profile = VAProfileAV1Profile0;
    params.rt_format = cc.bit_depth == 10 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    params.width = vaapi::alignUp(std::max(seq.max_frame_width, 1u), 16u);
    params.height = vaapi::alignUp(std::max(seq.max_frame_height, 1u), 16u);
    // Every slot may hold a clean and a grained surface when the stream has
    // film grain.
    const size_t per_slot = seq.film_grain_params_present ? 2 : 1;
    params.surfaces = video_parser::AV1_NUM_REF_FRAMES * per_slot + per_slot + slack();
    return params;
  }

  struct AV1Tile {
    size_t offset = 0; // within parsed.bitstream
    uint32_t size = 0;
    uint16_t row = 0;
    uint16_t column = 0;
    uint16_t tg_start = 0;
    uint16_t tg_end = 0;
  };

  // The tiles of every tile group of the frame (spec 5.11.1).
  static auto collectAV1Tiles(const video_parser::AV1ParsedFrame& parsed, std::vector<AV1Tile>& tiles) -> bool {
    const auto& ti = parsed.header.tile_info;
    const uint32_t num_tiles = ti.tile_cols * ti.tile_rows;
    if (num_tiles == 0 || ti.tile_cols == 0) return false;
    const uint32_t tile_size_bytes = std::max<uint32_t>(ti.tile_size_bytes, 1);

    for (const auto& obu : parsed.obus) {
      if (obu.type != video_parser::AV1_OBU_FRAME && obu.type != video_parser::AV1_OBU_TILE_GROUP) continue;
      const bool frame_obu = obu.type == video_parser::AV1_OBU_FRAME;
      const size_t tg_offset = frame_obu ? parsed.tile_group_offset : obu.payload_offset;
      const size_t tg_size = frame_obu ? parsed.tile_group_size : obu.payload_size;
      if (tg_size == 0 || tg_offset + tg_size > parsed.bitstream.size()) continue;

      BitReader reader(std::span<const uint8_t>(parsed.bitstream.data() + tg_offset, tg_size));
      uint32_t tg_start = 0;
      uint32_t tg_end = num_tiles - 1;
      if (num_tiles > 1) {
        const bool present = reader.readFlag();
        if (present && !frame_obu) {
          const uint32_t bits = ti.tile_cols_log2 + ti.tile_rows_log2;
          tg_start = reader.readBits(bits);
          tg_end = reader.readBits(bits);
        }
      }
      reader.alignToByte();
      if (!reader.ok() || tg_end < tg_start || tg_end >= num_tiles) return false;

      size_t pos = tg_offset + reader.bytePosition();
      const size_t end = tg_offset + tg_size;
      for (uint32_t t = tg_start; t <= tg_end; ++t) {
        uint32_t size = 0;
        if (t == tg_end) {
          if (pos > end) return false;
          size = static_cast<uint32_t>(end - pos);
        } else {
          if (pos + tile_size_bytes > end) return false;
          uint32_t coded = 0;
          for (uint32_t b = 0; b < tile_size_bytes; ++b)
            coded |= static_cast<uint32_t>(parsed.bitstream[pos + b]) << (8 * b);
          pos += tile_size_bytes;
          size = coded + 1;
          if (pos + size > end) return false;
        }
        AV1Tile tile;
        tile.offset = pos;
        tile.size = size;
        tile.row = static_cast<uint16_t>(t / ti.tile_cols);
        tile.column = static_cast<uint16_t>(t % ti.tile_cols);
        tile.tg_start = static_cast<uint16_t>(tg_start);
        tile.tg_end = static_cast<uint16_t>(tg_end);
        tiles.push_back(tile);
        pos += size;
      }
    }
    return tiles.size() == num_tiles;
  }

  auto decodeAV1(const Packet& packet, std::vector<Frame>& output) -> OMError {
    if (!av1_) return OM_CODEC_DECODE_FAILED;
    auto frames = av1_->parse(packet.bytes);
    const auto& seq = av1_->sequenceHeader();
    if (seq.valid) {
      updateAV1Format(seq);
      updateAV1HdrMetadata();
    }

    OMError result = OM_SUCCESS;
    for (const auto& parsed : frames) {
      if (!parsed.has_frame_header) continue;
      const OMError error = decodeAV1Frame(parsed, packet, output);
      if (error != OM_SUCCESS) result = error;
    }
    return result;
  }

  auto decodeAV1Frame(const video_parser::AV1ParsedFrame& parsed, const Packet& packet, std::vector<Frame>& output)
      -> OMError {
    const auto& h = parsed.header;
    const auto& seq = av1_->sequenceHeader();
    if (!h.valid || !seq.valid) return rejectFrame(OM_CODEC_DECODE_FAILED, "AV1 frame header did not parse");

    if (h.show_existing_frame) {
      const SlotRef slot = av1_refs_[h.frame_to_show_map_idx & 7];
      if (slot.surface < 0) return OM_SUCCESS;
      if (auto frame = makeFrame(slot.display >= 0 ? slot.display : slot.surface, h.upscaled_width, h.frame_height,
                                 packet.pts, packet.dts, h.frame_type == video_parser::AV1_KEY_FRAME)) {
        output.push_back(std::move(*frame));
      }
      // Showing a key frame this way refreshes every slot with it (7.21).
      if (h.frame_type == video_parser::AV1_KEY_FRAME) {
        std::fill(std::begin(av1_refs_), std::end(av1_refs_), slot);
        waiting_for_key_ = false;
      }
      return OM_SUCCESS;
    }

    const auto params = av1Session(seq);
    if (!params) {
      return rejectFrame(OM_CODEC_NOT_SUPPORTED, "AV1 profile {} at {}-bit is not supported", seq.seq_profile,
                         seq.color_config.bit_depth);
    }

    const bool key = h.frame_type == video_parser::AV1_KEY_FRAME;
    if (waiting_for_key_) {
      if (!key) return OM_SUCCESS;
      waiting_for_key_ = false;
    }

    if (const OMError error = ensureSession(*params); error != OM_SUCCESS) return error;

    if (!h.frame_is_intra) {
      for (uint32_t i = 0; i < video_parser::AV1_REFS_PER_FRAME; ++i) {
        if (av1_refs_[h.ref_frame_idx[i] & 7].surface < 0) {
          reportMissingReferences(1, static_cast<int32_t>(h.order_hint));
          return OM_SUCCESS;
        }
      }
    }

    std::vector<AV1Tile> tiles;
    if (!collectAV1Tiles(parsed, tiles)) {
      return rejectFrame(OM_CODEC_DECODE_FAILED, "AV1 tile data of the frame is incomplete or malformed");
    }

    const bool grain = seq.film_grain_params_present && h.film_grain.apply_grain;
    const int surface = acquireSurface();
    const int display = grain ? acquireSurface(surface) : -1;
    if (surface < 0 || (grain && display < 0)) {
      return rejectFrame(OM_CODEC_DECODE_FAILED, "AV1: no free surface to decode into");
    }

    VADecPictureParameterBufferAV1 pic = {};
    fillAV1PicParams(seq, h, surface, display, pic);

    std::vector<uint8_t> data;
    std::vector<VASliceParameterBufferAV1> slices(tiles.size());
    size_t total = 0;
    for (const auto& tile : tiles) total += tile.size;
    data.reserve(total);
    for (size_t i = 0; i < tiles.size(); ++i) {
      auto& slice = slices[i];
      slice.slice_data_size = tiles[i].size;
      slice.slice_data_offset = static_cast<uint32_t>(data.size());
      slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
      slice.tile_row = tiles[i].row;
      slice.tile_column = tiles[i].column;
      // Deprecated in libva, but still read by drivers that predate that.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
      slice.tg_start = tiles[i].tg_start;
      slice.tg_end = tiles[i].tg_end;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
      slice.anchor_frame_idx = 0;
      slice.tile_idx_in_tile_list = 0;
      const uint8_t* src = parsed.bitstream.data() + tiles[i].offset;
      data.insert(data.end(), src, src + tiles[i].size);
    }

    vaapi::Submission submission(display_, context_);
    submission.addParam(VAPictureParameterBufferType, &pic, sizeof(pic));
    submission.addSlice(slices.data(), sizeof(VASliceParameterBufferAV1), static_cast<uint32_t>(slices.size()),
                        data.data(), data.size());
    const VAStatus status = submission.execute(vaSurface(surface));
    if (status != VA_STATUS_SUCCESS) {
      return rejectFrame(OM_CODEC_DECODE_FAILED, "AV1 frame failed to decode: {}", vaapi::describe(status));
    }

    for (uint32_t i = 0; i < video_parser::AV1_NUM_REF_FRAMES; ++i) {
      if (h.refresh_frame_flags & (1u << i)) av1_refs_[i] = {surface, display};
    }
    if (h.show_frame) {
      if (auto frame = makeFrame(grain ? display : surface, h.upscaled_width, h.frame_height, packet.pts, packet.dts,
                                 key)) {
        output.push_back(std::move(*frame));
      }
    }
    return OM_SUCCESS;
  }

  void fillAV1PicParams(const video_parser::AV1SequenceHeader& seq, const video_parser::AV1FrameHeader& h,
                        int surface, int display, VADecPictureParameterBufferAV1& pic) const {
    const auto& cc = seq.color_config;
    pic.profile = seq.seq_profile;
    pic.order_hint_bits_minus_1 = static_cast<uint8_t>(seq.order_hint_bits > 0 ? seq.order_hint_bits - 1 : 0);
    pic.bit_depth_idx = cc.bit_depth == 12 ? 2 : (cc.bit_depth == 10 ? 1 : 0);
    pic.matrix_coefficients = cc.matrix_coefficients;

    auto& sf = pic.seq_info_fields.fields;
    sf.still_picture = seq.still_picture;
    sf.use_128x128_superblock = seq.use_128x128_superblock;
    sf.enable_filter_intra = seq.enable_filter_intra;
    sf.enable_intra_edge_filter = seq.enable_intra_edge_filter;
    sf.enable_interintra_compound = seq.enable_interintra_compound;
    sf.enable_masked_compound = seq.enable_masked_compound;
    sf.enable_dual_filter = seq.enable_dual_filter;
    sf.enable_order_hint = seq.enable_order_hint;
    sf.enable_jnt_comp = seq.enable_jnt_comp;
    sf.enable_cdef = seq.enable_cdef;
    sf.mono_chrome = cc.mono_chrome;
    sf.color_range = cc.color_range;
    sf.subsampling_x = cc.subsampling_x;
    sf.subsampling_y = cc.subsampling_y;
    sf.film_grain_params_present = seq.film_grain_params_present;

    // The reference is decoded into current_frame; with film grain the shown
    // copy with the grain applied lands in current_display_picture.
    pic.current_frame = vaSurface(surface);
    pic.current_display_picture = display >= 0 ? vaSurface(display) : vaSurface(surface);
    pic.anchor_frames_num = 0;
    pic.anchor_frames_list = nullptr;
    // The full (upscaled) width; superres_scale_denominator says how much
    // narrower the coded frame is.
    pic.frame_width_minus1 = static_cast<uint16_t>(h.upscaled_width - 1);
    pic.frame_height_minus1 = static_cast<uint16_t>(h.frame_height - 1);

    const bool resets_all = h.frame_type == video_parser::AV1_KEY_FRAME && h.show_frame;
    for (uint32_t i = 0; i < video_parser::AV1_NUM_REF_FRAMES; ++i) {
      pic.ref_frame_map[i] =
          (!resets_all && av1_refs_[i].surface >= 0) ? vaSurface(av1_refs_[i].surface) : VA_INVALID_SURFACE;
    }
    for (uint32_t i = 0; i < video_parser::AV1_REFS_PER_FRAME; ++i) pic.ref_frame_idx[i] = h.ref_frame_idx[i];
    pic.primary_ref_frame = h.primary_ref_frame;
    pic.order_hint = static_cast<uint8_t>(h.order_hint);

    // Segmentation, clipped to the ranges 5.9.14 allows, as ffmpeg does.
    const auto& seg = h.segmentation;
    auto& sb = pic.seg_info.segment_info_fields.bits;
    sb.enabled = seg.enabled;
    sb.update_map = seg.update_map;
    sb.temporal_update = seg.temporal_update;
    sb.update_data = seg.update_data;
    constexpr bool kSigned[8] = {true, true, true, true, true, false, false, false};
    constexpr int kMax[8] = {255, 63, 63, 63, 63, 7, 0, 0};
    for (uint32_t i = 0; i < video_parser::AV1_MAX_SEGMENTS; ++i) {
      for (uint32_t j = 0; j < video_parser::AV1_SEG_LVL_MAX; ++j) {
        if (seg.feature_enabled[i][j]) pic.seg_info.feature_mask[i] |= static_cast<uint8_t>(1u << j);
        const int lo = kSigned[j] ? -kMax[j] : 0;
        pic.seg_info.feature_data[i][j] = static_cast<int16_t>(std::clamp<int>(seg.feature_data[i][j], lo, kMax[j]));
      }
    }

    const auto& fg = h.film_grain;
    if (display >= 0) {
      auto& ff = pic.film_grain_info.film_grain_info_fields.bits;
      ff.apply_grain = 1;
      ff.chroma_scaling_from_luma = fg.chroma_scaling_from_luma;
      ff.grain_scaling_minus_8 = static_cast<uint32_t>(std::max(fg.grain_scaling, uint8_t {8}) - 8);
      ff.ar_coeff_lag = fg.ar_coeff_lag;
      ff.ar_coeff_shift_minus_6 = static_cast<uint32_t>(std::max(fg.ar_coeff_shift, uint8_t {6}) - 6);
      ff.grain_scale_shift = fg.grain_scale_shift;
      ff.overlap_flag = fg.overlap_flag;
      ff.clip_to_restricted_range = fg.clip_to_restricted_range;
      auto& g = pic.film_grain_info;
      g.grain_seed = fg.grain_seed;
      g.num_y_points = fg.num_y_points;
      g.num_cb_points = fg.num_cb_points;
      g.num_cr_points = fg.num_cr_points;
      for (uint32_t i = 0; i < 14; ++i) {
        g.point_y_value[i] = fg.point_y_value[i];
        g.point_y_scaling[i] = fg.point_y_scaling[i];
      }
      for (uint32_t i = 0; i < 10; ++i) {
        g.point_cb_value[i] = fg.point_cb_value[i];
        g.point_cb_scaling[i] = fg.point_cb_scaling[i];
        g.point_cr_value[i] = fg.point_cr_value[i];
        g.point_cr_scaling[i] = fg.point_cr_scaling[i];
      }
      // Coded with an offset of 128, passed without it.
      for (uint32_t i = 0; i < 24; ++i) g.ar_coeffs_y[i] = static_cast<int8_t>(static_cast<int>(fg.ar_coeffs_y[i]) - 128);
      for (uint32_t i = 0; i < 25; ++i) {
        g.ar_coeffs_cb[i] = static_cast<int8_t>(static_cast<int>(fg.ar_coeffs_cb[i]) - 128);
        g.ar_coeffs_cr[i] = static_cast<int8_t>(static_cast<int>(fg.ar_coeffs_cr[i]) - 128);
      }
      g.cb_mult = fg.cb_mult;
      g.cb_luma_mult = fg.cb_luma_mult;
      g.cb_offset = fg.cb_offset;
      g.cr_mult = fg.cr_mult;
      g.cr_luma_mult = fg.cr_luma_mult;
      g.cr_offset = fg.cr_offset;
    }

    const auto& ti = h.tile_info;
    pic.tile_cols = static_cast<uint8_t>(ti.tile_cols);
    pic.tile_rows = static_cast<uint8_t>(ti.tile_rows);
    const uint32_t sb_shift = seq.use_128x128_superblock ? 5u : 4u;
    const uint32_t sb_round = (1u << sb_shift) - 1;
    for (uint32_t i = 0; i < ti.tile_cols && i < std::size(pic.width_in_sbs_minus_1); ++i) {
      const uint32_t mi = ti.mi_col_starts[i + 1] - ti.mi_col_starts[i];
      pic.width_in_sbs_minus_1[i] = static_cast<uint16_t>(((mi + sb_round) >> sb_shift) - 1);
    }
    for (uint32_t i = 0; i < ti.tile_rows && i < std::size(pic.height_in_sbs_minus_1); ++i) {
      const uint32_t mi = ti.mi_row_starts[i + 1] - ti.mi_row_starts[i];
      pic.height_in_sbs_minus_1[i] = static_cast<uint16_t>(((mi + sb_round) >> sb_shift) - 1);
    }
    pic.tile_count_minus_1 = 0; // large scale tile only
    pic.context_update_tile_id = static_cast<uint16_t>(ti.context_update_tile_id);

    auto& pf = pic.pic_info_fields.bits;
    pf.frame_type = h.frame_type;
    pf.show_frame = h.show_frame;
    pf.showable_frame = h.showable_frame;
    pf.error_resilient_mode = h.error_resilient_mode;
    pf.disable_cdf_update = h.disable_cdf_update;
    pf.allow_screen_content_tools = h.allow_screen_content_tools;
    pf.force_integer_mv = h.force_integer_mv;
    pf.allow_intrabc = h.allow_intrabc;
    pf.use_superres = h.use_superres;
    pf.allow_high_precision_mv = h.allow_high_precision_mv;
    pf.is_motion_mode_switchable = h.is_motion_mode_switchable;
    pf.use_ref_frame_mvs = h.use_ref_frame_mvs;
    pf.disable_frame_end_update_cdf = h.disable_frame_end_update_cdf;
    pf.uniform_tile_spacing_flag = ti.uniform_tile_spacing_flag;
    pf.allow_warped_motion = h.allow_warped_motion;
    pf.large_scale_tile = 0;

    pic.superres_scale_denominator =
        h.use_superres ? h.superres_denom : static_cast<uint8_t>(video_parser::AV1_SUPERRES_NUM);
    pic.interp_filter = h.interpolation_filter;

    const auto& lf = h.loop_filter;
    pic.filter_level[0] = lf.level[0];
    pic.filter_level[1] = lf.level[1];
    pic.filter_level_u = lf.level[2];
    pic.filter_level_v = lf.level[3];
    pic.loop_filter_info_fields.bits.sharpness_level = lf.sharpness & 7;
    pic.loop_filter_info_fields.bits.mode_ref_delta_enabled = lf.delta_enabled;
    pic.loop_filter_info_fields.bits.mode_ref_delta_update = lf.delta_update;
    for (uint32_t i = 0; i < video_parser::AV1_TOTAL_REFS_PER_FRAME; ++i) pic.ref_deltas[i] = lf.ref_deltas[i];
    pic.mode_deltas[0] = lf.mode_deltas[0];
    pic.mode_deltas[1] = lf.mode_deltas[1];

    const auto& q = h.quantization;
    pic.base_qindex = q.base_q_idx;
    pic.y_dc_delta_q = static_cast<int8_t>(q.delta_q_y_dc);
    pic.u_dc_delta_q = static_cast<int8_t>(q.delta_q_u_dc);
    pic.u_ac_delta_q = static_cast<int8_t>(q.delta_q_u_ac);
    pic.v_dc_delta_q = static_cast<int8_t>(q.delta_q_v_dc);
    pic.v_ac_delta_q = static_cast<int8_t>(q.delta_q_v_ac);
    pic.qmatrix_fields.bits.using_qmatrix = q.using_qmatrix;
    pic.qmatrix_fields.bits.qm_y = q.qm_y & 15;
    pic.qmatrix_fields.bits.qm_u = q.qm_u & 15;
    pic.qmatrix_fields.bits.qm_v = q.qm_v & 15;

    auto& mc = pic.mode_control_fields.bits;
    mc.delta_q_present_flag = h.delta_q_present;
    mc.log2_delta_q_res = h.delta_q_res & 3;
    mc.delta_lf_present_flag = lf.delta_lf_present;
    mc.log2_delta_lf_res = lf.delta_lf_res & 3;
    mc.delta_lf_multi = lf.delta_lf_multi;
    mc.tx_mode = h.tx_mode & 3;
    mc.reference_select = h.reference_select;
    mc.reduced_tx_set_used = h.reduced_tx_set;
    mc.skip_mode_present = h.skip_mode_present;

    const auto& cdef = h.cdef;
    pic.cdef_damping_minus_3 = static_cast<uint8_t>(cdef.damping >= 3 ? cdef.damping - 3 : 0);
    pic.cdef_bits = cdef.bits;
    for (uint32_t i = 0; i < (1u << cdef.bits) && i < 8; ++i) {
      // Primary strength in the upper bits, the coded secondary (0..3) below.
      pic.cdef_y_strengths[i] = static_cast<uint8_t>((cdef.y_pri_strength[i] << 2) | (cdef.y_sec_strength[i] & 3));
      pic.cdef_uv_strengths[i] = static_cast<uint8_t>((cdef.uv_pri_strength[i] << 2) | (cdef.uv_sec_strength[i] & 3));
    }

    const auto& lr = h.lr;
    auto& lrf = pic.loop_restoration_fields.bits;
    lrf.yframe_restoration_type = lr.frame_restoration_type[0] & 3;
    lrf.cbframe_restoration_type = lr.frame_restoration_type[1] & 3;
    lrf.crframe_restoration_type = lr.frame_restoration_type[2] & 3;
    if (lr.uses_lr) {
      lrf.lr_unit_shift = static_cast<uint16_t>((lr.loop_restoration_size_log2[0] - 6) & 3);
      lrf.lr_uv_shift = static_cast<uint16_t>((lr.loop_restoration_size_log2[0] - lr.loop_restoration_size_log2[1]) & 1);
    }

    const auto& gm = h.global_motion;
    for (uint32_t i = 0; i < 7; ++i) {
      const uint32_t ref = video_parser::AV1_LAST_FRAME + i;
      pic.wm[i].wmtype = static_cast<VAAV1TransformationType>(gm.type[ref]);
      for (uint32_t j = 0; j < 6; ++j) pic.wm[i].wmmat[j] = gm.params[ref][j];
      pic.wm[i].invalid = gm.invalid[ref] ? 1 : 0;
    }
  }

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------

  bool initialized_ = false;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  bool hardware_output_ = false;
  VideoFormat output_format_ = {};

  vaapi::OwnedDisplay owned_display_;
  VADisplay display_ = nullptr;
  std::vector<VAProfile> profiles_;

  SessionParams session_;
  VAConfigID config_ = VA_INVALID_ID;
  VAContextID context_ = VA_INVALID_ID;
  std::shared_ptr<vaapi::SurfacePool> pool_;

  // Until a random access point has been decoded -- at the start, and after
  // every flush -- pictures that predict from earlier ones are dropped.
  bool waiting_for_key_ = true;
  ReorderQueue reorder_;

  dx_h264::State h264_;
  h264_dpb::Dpb h264_dpb_;
  h264_dpb::PocState h264_poc_;

  std::unique_ptr<video_parser::H265AccessUnitParser> h265_;
  std::vector<HevcRef> hevc_dpb_;
  bool hevc_skip_rasl_ = true;

  std::unique_ptr<video_parser::VP9FrameParser> vp9_;
  int vp9_refs_[video_parser::VP9_NUM_REF_FRAMES] = {-1, -1, -1, -1, -1, -1, -1, -1};

  std::unique_ptr<video_parser::AV1ObuParser> av1_;
  SlotRef av1_refs_[video_parser::AV1_NUM_REF_FRAMES] = {};
};

} // namespace

auto createVAAPIDecoder() -> std::unique_ptr<Decoder> {
  return std::make_unique<VAAPIDecoder>();
}

} // namespace openmedia
