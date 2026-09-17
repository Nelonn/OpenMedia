#include <openmedia/hw_dx11.h>
#include <openmedia/video.hpp>

#include <d3d11_3.h>
#include <dxva.h>
#include <wrl/client.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#include <video/parser/av1_parser.hpp>
#include <video/parser/vp9_parser.hpp>
#include <video/parser/h265_parser.hpp>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>

#include <util/wmf.hpp>
#include "dx_h264.hpp"
#include "dx_h265.hpp"
#include "hw_common.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "msvcrt.lib")

// EB533D05-D234-4530-9162-801691238C93
static constexpr GUID OM_MF_VIDEO_DEVICE_Manager = {0xeb533d05, 0xd234, 0x4530, {0x91, 0x62, 0x80, 0x16, 0x91, 0x23, 0x8c, 0x93}};

static constexpr GUID DXVA_NO_ENCRYPT = {0x1b81bed0, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};

using Microsoft::WRL::ComPtr;

namespace openmedia {

// Hardware video encoding on Windows does not go through D3D11 — the D3D11
// video API only decodes and post-processes. What "dx11 encode" means here is
// a Media Foundation encoder transform bound to our D3D11 device, so encoded
// frames can be fed straight from a D3D11 texture without a round trip through
// system memory.
//
// Vendor encoder MFTs are asynchronous: they must be unlocked, then driven by
// METransformNeedInput / METransformHaveOutput events rather than by calling
// ProcessInput and ProcessOutput in a loop. Software MFTs stay synchronous, so
// both models are supported here.
// MFShutdown() tears the platform down for the whole process, dropping
// references Media Foundation holds internally — including ones to our D3D11
// device. Tying that to an encoder's lifetime meant destroying an encoder left
// the shared device over-released and the next teardown crashed. Start the
// platform once and leave it up; Windows reclaims it at process exit.
static void ensureMediaFoundation() {
  static const bool started = [] { return SUCCEEDED(MFStartup(MF_VERSION)); }();
  (void) started;
}

class DX11Encoder final : public Encoder {
  OMDX11Context* hw_context_ = nullptr;
  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFMediaEventGenerator> event_generator_;
  ComPtr<ICodecAPI> codec_api_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT device_reset_token_ = 0;

  VideoFormat input_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t timescale_ = 90000;
  bool initialized_ = false;
  bool is_async_ = false;
  bool provides_samples_ = false;
  std::vector<uint8_t> extradata_;
  bool parameter_sets_captured_ = false;
  OMMasteringDisplayMetadata hdr_mastering_display_ = {};
  OMContentLightLevel hdr_content_light_level_ = {};
  std::vector<uint8_t> hdr_sei_;

  auto setup_device_manager() -> bool {
    ID3D11Device* device = HWD3D11Context_getDevice(hw_context_);
    if (!device) return false;

    if (FAILED(MFCreateDXGIDeviceManager(&device_reset_token_, &device_manager_))) return false;
    if (FAILED(device_manager_->ResetDevice(device, device_reset_token_))) return false;

    return true;
  }

  auto setup_types(const EncoderOptions& options) -> bool {
    ComPtr<IMFMediaType> input_type;
    ComPtr<IMFMediaType> output_type;

    const bool ten_bit = options.video_format.format == OM_FORMAT_P010;

    // The output type has to be set first: an encoder MFT cannot validate an
    // input type until it knows what it is producing.
    if (FAILED(MFCreateMediaType(&output_type))) return false;
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    GUID mf_codec = codecIdToMFVideoFormat(options.format.codec_id);
    if (mf_codec == MFVideoFormat_Base) return false;
    output_type->SetGUID(MF_MT_SUBTYPE, mf_codec);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, input_format_.width, input_format_.height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, options.format.video.framerate.num, options.format.video.framerate.den);
    MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, map_color_primaries(options.format.video.color_primaries));
    output_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, map_transfer_characteristics(options.format.video.transfer_char));
    output_type->SetUINT32(MF_MT_YUV_MATRIX, map_color_space(options.format.video.color_space, ten_bit ? 10 : 8));
    output_type->SetUINT32(MF_MT_AVG_BITRATE, target_bitrate(options.rate_control));
    output_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, map_nominal_range(options.format.video.color_range));
    apply_hdr_metadata(output_type.Get(), options.format.video.mastering_display,
                       options.format.video.content_light_level);
    {
      const uint32_t profile = map_profile(options.format.codec_id, options.format.profile,
                                           ten_bit ? 10 : 8);
      if (profile != 0) output_type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    }
    if (FAILED(encoder_->SetOutputType(0, output_type.Get(), 0))) return false;

    if (FAILED(MFCreateMediaType(&input_type))) return false;
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, ten_bit ? MFVideoFormat_P010 : MFVideoFormat_NV12);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, input_format_.width, input_format_.height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, options.format.video.framerate.num, options.format.video.framerate.den);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, map_color_primaries(options.format.video.color_primaries));
    input_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, map_transfer_characteristics(options.format.video.transfer_char));
    input_type->SetUINT32(MF_MT_YUV_MATRIX, map_color_space(options.format.video.color_space, ten_bit ? 10 : 8));
    input_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, map_nominal_range(options.format.video.color_range));
    apply_hdr_metadata(input_type.Get(), options.format.video.mastering_display,
                       options.format.video.content_light_level);
    if (FAILED(encoder_->SetInputType(0, input_type.Get(), 0))) return false;

    captureExtradata();
    return true;
  }

  // Collects the parameter sets out of the first keyframe. The MFT's
  // MF_MT_MPEG_SEQUENCE_HEADER blob is not trustworthy on its own — the HEVC
  // encoder reports SPS and PPS but omits the VPS, which makes the result
  // unusable as MP4 extradata. What the encoder actually puts in the bitstream
  // always is.
  void captureExtradataFromKeyframe(std::span<const uint8_t> data) {
    std::vector<uint8_t> sets;

    if (codec_id_ == OM_CODEC_AV1) {
      // Take the sequence header OBU verbatim.
      size_t pos = 0;
      while (pos < data.size()) {
        const uint8_t header = data[pos];
        const uint8_t type = static_cast<uint8_t>((header >> 3) & 0x0F);
        const bool extension = (header >> 2) & 1;
        const bool has_size = (header >> 1) & 1;
        size_t cursor = pos + 1 + (extension ? 1 : 0);
        if (!has_size || cursor >= data.size()) break;
        uint64_t payload = 0;
        size_t leb = 0;
        for (; leb < 8 && cursor + leb < data.size(); ++leb) {
          const uint8_t byte = data[cursor + leb];
          payload |= static_cast<uint64_t>(byte & 0x7F) << (leb * 7);
          if ((byte & 0x80) == 0) { ++leb; break; }
        }
        cursor += leb;
        if (cursor + payload > data.size()) break;
        if (type == 1) { // OBU_SEQUENCE_HEADER
          sets.assign(data.begin() + pos, data.begin() + cursor + payload);
          break;
        }
        pos = cursor + static_cast<size_t>(payload);
      }
    } else {
      const bool hevc = (codec_id_ == OM_CODEC_H265);
      size_t pos = 0;
      auto next_start = [&](size_t from) -> size_t {
        for (size_t i = from; i + 3 <= data.size(); ++i)
          if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
        return data.size();
      };
      pos = next_start(0);
      while (pos < data.size()) {
        const size_t nal_start = pos + 3;
        const size_t next = next_start(nal_start);
        if (nal_start >= data.size()) break;
        const uint8_t header = data[nal_start];
        const uint32_t type = hevc ? ((header >> 1) & 0x3F) : (header & 0x1F);
        const bool is_param = hevc ? (type == 32 || type == 33 || type == 34)
                                   : (type == 7 || type == 8);
        if (is_param) {
          size_t end = next;
          // Trim the zero byte belonging to a 4-byte start code that follows.
          while (end > nal_start && data[end - 1] == 0) --end;
          const uint8_t start_code[4] = {0, 0, 0, 1};
          sets.insert(sets.end(), start_code, start_code + 4);
          sets.insert(sets.end(), data.begin() + nal_start, data.begin() + end);
        }
        pos = next;
      }
    }

    if (!sets.empty()) extradata_ = std::move(sets);
  }

  // Fallback: whatever the MFT advertises on its output type.
  void captureExtradata() {
    ComPtr<IMFMediaType> current;
    if (FAILED(encoder_->GetOutputCurrentType(0, &current)) || !current) return;

    UINT32 size = 0;
    if (FAILED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return;
    std::vector<uint8_t> blob(size);
    if (SUCCEEDED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size, &size)))
      extradata_ = std::move(blob);
  }

  static auto target_bitrate(const RateControlParams& rc) -> uint32_t {
    switch (rc.getMode()) {
      case RateControlMode::CBR:
        return static_cast<uint32_t>(std::get<CbrParams>(rc.params).bitrate.target_bitrate);
      case RateControlMode::VBR:
        return static_cast<uint32_t>(std::get<VbrParams>(rc.params).bitrate.target_bitrate);
      case RateControlMode::ABR:
        return static_cast<uint32_t>(std::get<AbrParams>(rc.params).target_bitrate);
      default:
        return 5000000;
    }
  }

  // Bitrate alone is not enough to get sensible output; the rate control mode
  // and GOP length have to be pushed through ICodecAPI or the MFT falls back to
  // whatever the driver defaults to.
  void apply_rate_control(const EncoderOptions& options) {
    if (!codec_api_) return;

    auto set_u32 = [&](const GUID& prop, uint32_t value) {
      VARIANT v = {};
      v.vt = VT_UI4;
      v.ulVal = value;
      codec_api_->SetValue(&prop, &v);
    };

    uint32_t mode = eAVEncCommonRateControlMode_UnconstrainedVBR;
    switch (options.rate_control.getMode()) {
      case RateControlMode::CBR:   mode = eAVEncCommonRateControlMode_CBR; break;
      case RateControlMode::CQP:   mode = eAVEncCommonRateControlMode_Quality; break;
      case RateControlMode::CRF:
      case RateControlMode::ICQ:   mode = eAVEncCommonRateControlMode_Quality; break;
      case RateControlMode::VBR:   mode = eAVEncCommonRateControlMode_PeakConstrainedVBR; break;
      default:                     mode = eAVEncCommonRateControlMode_UnconstrainedVBR; break;
    }
    set_u32(CODECAPI_AVEncCommonRateControlMode, mode);
    set_u32(CODECAPI_AVEncCommonMeanBitRate, target_bitrate(options.rate_control));

    if (options.rate_control.getMode() == RateControlMode::VBR) {
      const auto& vbr = std::get<VbrParams>(options.rate_control.params);
      if (vbr.bitrate.max_bitrate)
        set_u32(CODECAPI_AVEncCommonMaxBitRate, static_cast<uint32_t>(*vbr.bitrate.max_bitrate));
    }
    if (options.rate_control.getMode() == RateControlMode::CQP) {
      const auto& cqp = std::get<CqpParams>(options.rate_control.params);
      set_u32(CODECAPI_AVEncVideoEncodeQP, static_cast<uint32_t>(cqp.qp_i));
    }

    // Keyframe interval, expressed in frames.
    uint32_t gop = 0;
    if (const auto* value = options.extra.get("gop_size")) {
      if (const auto n = value->getInt64()) gop = static_cast<uint32_t>(*n);
    }
    if (gop == 0 && options.format.video.framerate.den > 0)
      gop = static_cast<uint32_t>(options.format.video.framerate.num /
                                  std::max(1, options.format.video.framerate.den)) * 2;
    if (gop > 0) set_u32(CODECAPI_AVEncMPVGOPSize, gop);
  }

  static auto map_profile(OMCodecId codec, OMProfile profile, uint8_t bit_depth) -> uint32_t {
    if (codec == OM_CODEC_H264) {
      switch (profile) {
        case OM_PROFILE_H264_BASELINE: return eAVEncH264VProfile_Base;
        case OM_PROFILE_H264_MAIN:     return eAVEncH264VProfile_Main;
        case OM_PROFILE_H264_HIGH:     return eAVEncH264VProfile_High;
        default: return 0;
      }
    }
    if (codec == OM_CODEC_H265) {
      // 10-bit HEVC must be signalled as Main10 or the encoder falls back to
      // Main and silently truncates the input.
      return (bit_depth > 8) ? static_cast<uint32_t>(eAVEncH265VProfile_Main_420_10)
                             : static_cast<uint32_t>(eAVEncH265VProfile_Main_420_8);
    }
    return 0;
  }

  static auto map_color_primaries(OMColorPrimaries p) -> uint32_t {
    switch (p) {
      case OM_PRIMARIES_BT709:     return MFVideoPrimaries_BT709;
      case OM_PRIMARIES_BT470M:    return MFVideoPrimaries_BT470_2_SysM;
      case OM_PRIMARIES_BT470BG:   return MFVideoPrimaries_BT470_2_SysBG;
      case OM_PRIMARIES_BT601:     return MFVideoPrimaries_SMPTE170M;
      case OM_PRIMARIES_SMPTE240M: return MFVideoPrimaries_SMPTE240M;
      case OM_PRIMARIES_BT2020:    return MFVideoPrimaries_BT2020;
      case OM_PRIMARIES_SMPTE428:  return MFVideoPrimaries_XYZ;
      case OM_PRIMARIES_SMPTE431:  return MFVideoPrimaries_DCI_P3;
      case OM_PRIMARIES_SMPTE432:  return MFVideoPrimaries_Display_P3;
      case OM_PRIMARIES_EBU3213:   return MFVideoPrimaries_EBU3213;
      // OM_PRIMARIES_FILM has no Media Foundation equivalent.
      default:                     return MFVideoPrimaries_Unknown;
    }
  }

  static auto map_transfer_characteristics(OMTransferCharacteristic t) -> uint32_t {
    switch (t) {
      case OM_TRANSFER_BT709:      return MFVideoTransFunc_709;
      case OM_TRANSFER_BT601:      return MFVideoTransFunc_709; // shares the 709 curve
      case OM_TRANSFER_SMPTE240M:  return MFVideoTransFunc_240M;
      case OM_TRANSFER_LINEAR:     return MFVideoTransFunc_10;
      case OM_TRANSFER_SRGB:       return MFVideoTransFunc_sRGB;
      case OM_TRANSFER_GAMMA22:    return MFVideoTransFunc_22;
      case OM_TRANSFER_GAMMA28:    return MFVideoTransFunc_28;
      case OM_TRANSFER_BT2020_10:
      case OM_TRANSFER_BT2020_12:  return MFVideoTransFunc_2020;
      case OM_TRANSFER_PQ:         return MFVideoTransFunc_2084;
      case OM_TRANSFER_HLG:        return MFVideoTransFunc_HLG;
      default:                     return MFVideoTransFunc_Unknown;
    }
  }

  static auto map_nominal_range(OMColorRange range) -> uint32_t {
    switch (range) {
      case OM_COLOR_RANGE_FULL:    return MFNominalRange_0_255;
      case OM_COLOR_RANGE_LIMITED: return MFNominalRange_16_235;
      default:                     return MFNominalRange_Unknown;
    }
  }

  // HDR10 static metadata. Media Foundation carries ST.2086 mastering display
  // and CEA-861.3 light levels as media-type attributes; without them the
  // encoder emits a bitstream that is technically PQ but carries no mastering
  // information, which players treat as unmastered HDR.
  static void apply_hdr_metadata(IMFMediaType* type,
                                 const OMMasteringDisplayMetadata& mastering,
                                 const OMContentLightLevel& light) {
    if (mastering.has_value) {
      const auto& md = mastering;
      // Ours is in 0.0001 nit units; MF wants nits for the maximum and
      // 0.0001 nits for the minimum.
      type->SetUINT32(MF_MT_MAX_MASTERING_LUMINANCE,
                      md.max_display_mastering_luminance / 10000u);
      type->SetUINT32(MF_MT_MIN_MASTERING_LUMINANCE,
                      md.min_display_mastering_luminance);
    }
    if (light.has_value) {
      const auto& cll = light;
      type->SetUINT32(MF_MT_MAX_LUMINANCE_LEVEL, cll.max_content_light_level);
      type->SetUINT32(MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL,
                      cll.max_pic_average_light_level);
    }
  }

  static auto map_color_space(OMColorSpace c, uint8_t bit_depth) -> uint32_t {
    switch (c) {
      case OM_COLOR_SPACE_BT709:      return MFVideoTransferMatrix_BT709;
      case OM_COLOR_SPACE_BT601:      return MFVideoTransferMatrix_BT601;
      case OM_COLOR_SPACE_SMPTE240M:  return MFVideoTransferMatrix_SMPTE240M;
      case OM_COLOR_SPACE_FCC:        return MFVideoTransferMatrix_FCC47;
      case OM_COLOR_SPACE_YCGCO:      return MFVideoTransferMatrix_YCgCo;
      case OM_COLOR_SPACE_RGB:        return MFVideoTransferMatrix_Identity;
      case OM_COLOR_SPACE_CHROMA_DERIVED_NCL: return MFVideoTransferMatrix_Chroma;
      case OM_COLOR_SPACE_CHROMA_DERIVED_CL:  return MFVideoTransferMatrix_Chroma_const;
      case OM_COLOR_SPACE_BT2020:
        // BT.2020 has separate 10- and 12-bit matrices; always claiming the
        // 10-bit one mis-signals 12-bit content.
        return (bit_depth >= 12) ? MFVideoTransferMatrix_BT2020_12
                                 : MFVideoTransferMatrix_BT2020_10;
      // BT.2020 constant luminance and ICtCp have no Media Foundation value.
      default:                        return MFVideoTransferMatrix_Unknown;
    }
  }


  // The encoder MFTs accept the HDR media-type attributes but do not write the
  // corresponding ST.2086 / CEA-861.3 messages into the elementary stream, so a
  // standalone .h265 or .obu carries PQ signalling with no mastering data. Build
  // the messages here and put them in front of every keyframe, which is what
  // software encoders do and what players expect when there is no container to
  // carry the metadata.
  static void appendBE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }

  static void appendBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }

  // Annex-B payloads must not contain 00 00 00/01/02/03, so re-insert the
  // emulation prevention byte.
  static void appendEscaped(std::vector<uint8_t>& out, const std::vector<uint8_t>& raw) {
    uint32_t zeros = 0;
    for (uint8_t byte : raw) {
      if (zeros >= 2 && byte <= 3) {
        out.push_back(0x03);
        zeros = 0;
      }
      out.push_back(byte);
      zeros = (byte == 0) ? zeros + 1 : 0;
    }
  }

  auto buildHdrSei() const -> std::vector<uint8_t> {
    std::vector<uint8_t> out;
    const bool have_md = hdr_mastering_display_.has_value;
    const bool have_cll = hdr_content_light_level_.has_value;
    if (!have_md && !have_cll) return out;

    if (codec_id_ == OM_CODEC_AV1) {
      // AV1 carries these as metadata OBUs rather than SEI.
      auto emit_obu = [&](uint8_t metadata_type, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> payload;
        payload.push_back(metadata_type); // leb128, single byte for 1 and 2
        payload.insert(payload.end(), body.begin(), body.end());
        payload.push_back(0x80); // trailing bits
        out.push_back(0x2A);     // OBU_METADATA, has_size_field
        size_t size = payload.size();
        do {                     // leb128 size
          uint8_t byte = static_cast<uint8_t>(size & 0x7F);
          size >>= 7;
          if (size) byte |= 0x80;
          out.push_back(byte);
        } while (size);
        out.insert(out.end(), payload.begin(), payload.end());
      };

      if (have_cll) {
        std::vector<uint8_t> body;
        appendBE16(body, hdr_content_light_level_.max_content_light_level);
        appendBE16(body, hdr_content_light_level_.max_pic_average_light_level);
        emit_obu(1, body); // METADATA_TYPE_HDR_CLL
      }
      if (have_md) {
        // AV1 uses a different fixed point to the H.26x SEI, and orders the
        // primaries red-green-blue where the SEI orders them green-blue-red.
        // The stored values follow the SEI convention, so convert both.
        const auto& md = hdr_mastering_display_;
        auto chroma = [](uint16_t v) -> uint16_t {          // 1/50000 -> 0.16
          return static_cast<uint16_t>((static_cast<uint32_t>(v) * 65536u + 25000u) / 50000u);
        };
        const uint32_t sei_index_for_rgb[3] = {2, 0, 1};    // R, G, B

        std::vector<uint8_t> body;
        for (uint32_t i = 0; i < 3; ++i) {
          const uint32_t c = sei_index_for_rgb[i];
          appendBE16(body, chroma(md.display_primaries[c][0]));
          appendBE16(body, chroma(md.display_primaries[c][1]));
        }
        appendBE16(body, chroma(md.white_point[0]));
        appendBE16(body, chroma(md.white_point[1]));
        // luminance_max is 24.8 and luminance_min is 18.14, both from 1/10000.
        appendBE32(body, static_cast<uint32_t>(
                             (static_cast<uint64_t>(md.max_display_mastering_luminance) * 256u + 5000u) / 10000u));
        appendBE32(body, static_cast<uint32_t>(
                             (static_cast<uint64_t>(md.min_display_mastering_luminance) * 16384u + 5000u) / 10000u));
        emit_obu(2, body); // METADATA_TYPE_HDR_MDCV
      }
      return out;
    }

    const bool hevc = (codec_id_ == OM_CODEC_H265);
    auto emit_sei = [&](uint8_t payload_type, const std::vector<uint8_t>& body) {
      std::vector<uint8_t> rbsp;
      rbsp.push_back(payload_type);
      rbsp.push_back(static_cast<uint8_t>(body.size())); // both payloads are < 255
      rbsp.insert(rbsp.end(), body.begin(), body.end());
      rbsp.push_back(0x80); // rbsp_trailing_bits

      out.insert(out.end(), {0, 0, 0, 1});
      if (hevc) {
        out.push_back(0x4E); // nal_unit_type 39 (PREFIX_SEI), layer 0
        out.push_back(0x01); // temporal_id_plus1
      } else {
        out.push_back(0x06); // nal_unit_type 6 (SEI)
      }
      appendEscaped(out, rbsp);
    };

    if (have_md) {
      std::vector<uint8_t> body;
      // Primaries are stored in the order the SEI uses (green, blue, red).
      for (uint32_t i = 0; i < 3; ++i) {
        appendBE16(body, hdr_mastering_display_.display_primaries[i][0]);
        appendBE16(body, hdr_mastering_display_.display_primaries[i][1]);
      }
      appendBE16(body, hdr_mastering_display_.white_point[0]);
      appendBE16(body, hdr_mastering_display_.white_point[1]);
      appendBE32(body, hdr_mastering_display_.max_display_mastering_luminance);
      appendBE32(body, hdr_mastering_display_.min_display_mastering_luminance);
      emit_sei(137, body); // mastering_display_colour_volume
    }
    if (have_cll) {
      std::vector<uint8_t> body;
      appendBE16(body, hdr_content_light_level_.max_content_light_level);
      appendBE16(body, hdr_content_light_level_.max_pic_average_light_level);
      emit_sei(144, body); // content_light_level_info
    }
    return out;
  }

  // Returns the byte offset at which extra OBUs may be inserted: past the
  // leading temporal delimiter and sequence header, before the frame itself.
  static auto av1InsertPoint(std::span<const uint8_t> data) -> size_t {
    size_t pos = 0;
    size_t insert_at = 0;
    while (pos < data.size()) {
      const uint8_t header = data[pos];
      const uint8_t type = static_cast<uint8_t>((header >> 3) & 0x0F);
      const bool extension = (header >> 2) & 1;
      const bool has_size = (header >> 1) & 1;
      size_t cursor = pos + 1 + (extension ? 1 : 0);
      if (!has_size || cursor >= data.size()) break;
      uint64_t payload = 0;
      size_t leb = 0;
      for (; leb < 8 && cursor + leb < data.size(); ++leb) {
        const uint8_t byte = data[cursor + leb];
        payload |= static_cast<uint64_t>(byte & 0x7F) << (leb * 7);
        if ((byte & 0x80) == 0) { ++leb; break; }
      }
      cursor += leb;
      if (cursor + payload > data.size()) break;
      const size_t next = cursor + static_cast<size_t>(payload);
      // 2 = OBU_TEMPORAL_DELIMITER, 1 = OBU_SEQUENCE_HEADER
      if (type == 2 || type == 1) insert_at = next;
      else break;
      pos = next;
    }
    return insert_at;
  }

  auto wrap_frame(const Frame& frame) -> ComPtr<IMFSample> {
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return {};
    if (!std::holds_alternative<Picture>(frame.data)) return {};
    const auto& picture = std::get<Picture>(frame.data);

    if (std::holds_alternative<std::shared_ptr<HardwarePicture>>(picture.buffer)) {
      auto hw_pic = std::get<std::shared_ptr<HardwarePicture>>(picture.buffer);
      if (hw_pic->getType() != HWDeviceType::DX11) return {};
      auto dx_pic = std::static_pointer_cast<DX11HardwarePicture>(hw_pic);
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), dx_pic->pic->texture, 0, FALSE, &buffer))) return {};
      sample->AddBuffer(buffer.Get());
    } else {
      // System memory input: pack the planes tightly, honouring the sample size
      // so 10-bit P010 is not truncated to 8 bits per component.
      const uint32_t bpp = (input_format_.format == OM_FORMAT_P010) ? 2u : 1u;
      const uint32_t luma_rows = input_format_.height;
      const uint32_t chroma_rows = (input_format_.height + 1) / 2;
      const size_t row_bytes = static_cast<size_t>(input_format_.width) * bpp;
      const size_t total = row_bytes * (luma_rows + chroma_rows);

      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(total), &buffer))) return {};
      BYTE* data = nullptr;
      if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return {};
      for (int plane = 0; plane < 2; ++plane) {
        const uint8_t* src = picture.planes.getData(plane);
        const size_t src_stride = picture.planes.getLinesize(plane);
        const uint32_t rows = (plane == 0) ? luma_rows : chroma_rows;
        if (!src) {
          std::memset(data, plane == 0 ? 0 : 0x80, row_bytes * rows);
          data += row_bytes * rows;
          continue;
        }
        for (uint32_t y = 0; y < rows; ++y) {
          std::memcpy(data, src + y * src_stride, row_bytes);
          data += row_bytes;
        }
      }
      buffer->Unlock();
      buffer->SetCurrentLength(static_cast<DWORD>(total));
      sample->AddBuffer(buffer.Get());
    }

    sample->SetSampleTime(static_cast<LONGLONG>(frame.pts) * 10000000LL / timescale_);
    return sample;
  }

  auto collect_output(std::vector<Packet>& packets) -> HRESULT {
    MFT_OUTPUT_STREAM_INFO stream_info = {};
    encoder_->GetOutputStreamInfo(0, &stream_info);

    MFT_OUTPUT_DATA_BUFFER output = {};
    output.dwStreamID = 0;

    ComPtr<IMFSample> out_sample;
    if (!provides_samples_) {
      if (FAILED(MFCreateSample(&out_sample))) return E_FAIL;
      ComPtr<IMFMediaBuffer> out_buffer;
      if (FAILED(MFCreateMemoryBuffer(std::max<DWORD>(stream_info.cbSize, 1), &out_buffer))) return E_FAIL;
      out_sample->AddBuffer(out_buffer.Get());
      output.pSample = out_sample.Get();
    }

    DWORD status = 0;
    HRESULT hr = encoder_->ProcessOutput(0, 1, &output, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // The encoder renegotiated its output type; pick up the new sequence
      // header so the muxer gets the right one.
      ComPtr<IMFMediaType> new_type;
      if (SUCCEEDED(encoder_->GetOutputAvailableType(0, 0, &new_type)))
        encoder_->SetOutputType(0, new_type.Get(), 0);
      captureExtradata();
      if (output.pEvents) output.pEvents->Release();
      return hr;
    }
    if (FAILED(hr)) {
      if (output.pEvents) output.pEvents->Release();
      return hr;
    }

    if (output.pSample) {
      ComPtr<IMFMediaBuffer> buf;
      if (SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&buf))) {
        DWORD len = 0;
        BYTE* data = nullptr;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
          Packet pkt;
          pkt.allocate(len);
          std::memcpy(pkt.bytes.data(), data, len);
          buf->Unlock();

          LONGLONG time = 0;
          if (SUCCEEDED(output.pSample->GetSampleTime(&time)))
            pkt.pts = time * timescale_ / 10000000LL;
          LONGLONG duration = 0;
          if (SUCCEEDED(output.pSample->GetSampleDuration(&duration)))
            pkt.duration = duration * timescale_ / 10000000LL;
          // Encoders here emit in display order, so DTS tracks PTS.
          pkt.dts = pkt.pts;
          UINT32 clean_point = 0;
          if (SUCCEEDED(output.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)))
            pkt.is_keyframe = clean_point != 0;
          if (pkt.is_keyframe && !parameter_sets_captured_) {
            captureExtradataFromKeyframe(std::span<const uint8_t>(pkt.bytes.data(), pkt.bytes.size()));
            parameter_sets_captured_ = true;
          }
          if (pkt.is_keyframe && !hdr_sei_.empty()) {
            // Annex-B SEI goes in front; AV1 metadata OBUs have to land after
            // the temporal delimiter and sequence header.
            const size_t at = (codec_id_ == OM_CODEC_AV1)
                                  ? av1InsertPoint(std::span<const uint8_t>(pkt.bytes.data(), pkt.bytes.size()))
                                  : 0;
            Packet with_hdr;
            with_hdr.allocate(hdr_sei_.size() + pkt.bytes.size());
            std::memcpy(with_hdr.bytes.data(), pkt.bytes.data(), at);
            std::memcpy(with_hdr.bytes.data() + at, hdr_sei_.data(), hdr_sei_.size());
            std::memcpy(with_hdr.bytes.data() + at + hdr_sei_.size(),
                        pkt.bytes.data() + at, pkt.bytes.size() - at);
            with_hdr.pts = pkt.pts;
            with_hdr.dts = pkt.dts;
            with_hdr.duration = pkt.duration;
            with_hdr.is_keyframe = true;
            packets.push_back(std::move(with_hdr));
          } else {
            packets.push_back(std::move(pkt));
          }
        }
      }
      if (provides_samples_) output.pSample->Release();
    }
    if (output.pEvents) output.pEvents->Release();
    return S_OK;
  }

  // Async MFTs hand out METransformNeedInput / METransformHaveOutput events.
  // `sample` may be null when draining.
  auto pump_async(IMFSample* sample, std::vector<Packet>& packets, bool drain) -> OMError {
    bool input_pending = sample != nullptr;

    while (true) {
      ComPtr<IMFMediaEvent> event;
      // Do not block once the input is placed and nothing is being drained;
      // the encoder is free to buffer frames before producing anything.
      const DWORD flags = (input_pending || drain) ? 0 : MF_EVENT_FLAG_NO_WAIT;
      HRESULT hr = event_generator_->GetEvent(flags, &event);
      if (hr == MF_E_NO_EVENTS_AVAILABLE) return OM_SUCCESS;
      if (FAILED(hr)) return OM_CODEC_ENCODE_FAILED;

      MediaEventType type = MEUnknown;
      if (FAILED(event->GetType(&type))) return OM_CODEC_ENCODE_FAILED;

      if (type == METransformNeedInput) {
        if (input_pending) {
          if (FAILED(encoder_->ProcessInput(0, sample, 0))) return OM_CODEC_ENCODE_FAILED;
          input_pending = false;
          if (!drain) return OM_SUCCESS;
        } else if (drain) {
          // Nothing left to feed; the drain message already went in and the
          // encoder will follow up with its remaining output.
          continue;
        }
      } else if (type == METransformHaveOutput) {
        const HRESULT out_hr = collect_output(packets);
        if (out_hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
        if (FAILED(out_hr)) return OM_CODEC_ENCODE_FAILED;
      } else if (type == METransformDrainComplete) {
        return OM_SUCCESS;
      }
    }
  }

  auto pump_sync(IMFSample* sample, std::vector<Packet>& packets) -> OMError {
    if (sample && FAILED(encoder_->ProcessInput(0, sample, 0))) return OM_CODEC_ENCODE_FAILED;
    while (true) {
      const HRESULT hr = collect_output(packets);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return OM_SUCCESS;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
      if (FAILED(hr)) return OM_CODEC_ENCODE_FAILED;
    }
  }

public:
  DX11Encoder() {
    ensureMediaFoundation();
  }

  ~DX11Encoder() override {
    release();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (!options.hw_device || options.hw_device->type != HWDeviceType::DX11) return OM_CODEC_HWACCEL_FAILED;
    hw_context_ = static_cast<OMDX11Context*>(options.hw_device->context);
    codec_id_ = options.format.codec_id;

    if (!setup_device_manager()) return OM_CODEC_HWACCEL_FAILED;

    MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, codecIdToMFVideoFormat(options.format.codec_id)};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &output_info, &activates, &count)) || count == 0) {
      return OM_CODEC_NOT_FOUND;
    }
    HRESULT activate_hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(activate_hr) || !encoder_) return OM_CODEC_OPEN_FAILED;

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(encoder_->GetAttributes(&attributes)) && attributes) {
      UINT32 async = 0;
      if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
        // An async MFT refuses every call until it is unlocked, and this has to
        // happen before any type is set.
        if (FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) return OM_CODEC_OPEN_FAILED;
        is_async_ = true;
      }
      attributes->SetUINT32(MF_LOW_LATENCY, FALSE);
    }

    // Hand the encoder our D3D11 device so texture input stays on the GPU.
    encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                             reinterpret_cast<ULONG_PTR>(device_manager_.Get()));

    encoder_.As(&codec_api_);

    input_format_ = options.video_format;
    hdr_mastering_display_ = options.format.video.mastering_display;
    hdr_content_light_level_ = options.format.video.content_light_level;
    hdr_sei_ = buildHdrSei();
    apply_rate_control(options);
    if (!setup_types(options)) return OM_CODEC_OPEN_FAILED;

    MFT_OUTPUT_STREAM_INFO stream_info = {};
    encoder_->GetOutputStreamInfo(0, &stream_info);
    provides_samples_ =
        (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    if (is_async_ && FAILED(encoder_.As(&event_generator_))) return OM_CODEC_OPEN_FAILED;

    if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return OM_CODEC_OPEN_FAILED;
    if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return OM_CODEC_OPEN_FAILED;

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);

    std::vector<Packet> packets;

    // An empty frame means flush: drain whatever the encoder still holds.
    if (!std::holds_alternative<Picture>(frame.data) ||
        std::get<Picture>(frame.data).width == 0) {
      encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
      const OMError err = is_async_ ? pump_async(nullptr, packets, true)
                                    : pump_sync(nullptr, packets);
      if (err != OM_SUCCESS) return Err(err);
      return Ok(std::move(packets));
    }

    ComPtr<IMFSample> sample = wrap_frame(frame);
    if (!sample) return Err(OM_CODEC_INVALID_PARAMS);

    const OMError err = is_async_ ? pump_async(sample.Get(), packets, false)
                                  : pump_sync(sample.Get(), packets);
    if (err != OM_SUCCESS) return Err(err);

    if (extradata_.empty()) captureExtradata();
    return Ok(std::move(packets));
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    // The encoder does not invent HDR metadata; it signals what the caller
    // supplied, and the muxer needs the same values for the container boxes.
    info.mastering_display = hdr_mastering_display_;
    info.content_light_level = hdr_content_light_level_;
    return info;
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!encoder_) return OM_CODEC_OPEN_FAILED;
    if (!codec_api_) return OM_COMMON_NOT_SUPPORTED;

    VARIANT v = {};
    v.vt = VT_UI4;
    v.ulVal = target_bitrate(rc);
    const GUID prop = CODECAPI_AVEncCommonMeanBitRate;
    return SUCCEEDED(codec_api_->SetValue(&prop, &v)) ? OM_SUCCESS : OM_CODEC_OPEN_FAILED;
  }

  void release() {
    initialized_ = false;
    if (encoder_) {
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      encoder_.Reset();
    }
    event_generator_.Reset();
    codec_api_.Reset();
    device_manager_.Reset();
    extradata_.clear();
    hdr_sei_.clear();
    parameter_sets_captured_ = false;
    is_async_ = false;
    provides_samples_ = false;
  }
};

class DX11Decoder final : public Decoder {
  struct Slot {
    dx_h264::DpbEntry dpb;
    ComPtr<ID3D11VideoDecoderOutputView> view;
  };

  struct ReorderEntry {
    int32_t poc = 0;
    Frame frame = {};
  };

  OMDX11Context* hw_context_ = nullptr;
  bool owns_hw_context_ = false;
  bool initialized_ = false;
  VideoFormat output_format_ = {};

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  ID3D11VideoDevice* video_device_ = nullptr;
  ID3D11VideoContext* video_context_ = nullptr;

  ComPtr<ID3D11VideoDecoder> decoder_;
  D3D11_VIDEO_DECODER_CONFIG decoder_config_ = {};
  ComPtr<ID3D11Texture2D> dpb_texture_;
  std::vector<Slot> slots_;

  dx_h264::State h264_;
  std::unique_ptr<video_parser::H265AccessUnitParser> h265_;
  std::unique_ptr<video_parser::AV1ObuParser> av1_;
  std::unique_ptr<video_parser::VP9FrameParser> vp9_;
  // Maps each VP9 reference slot (0..7) to the DPB texture index holding it.
  int32_t vp9_ref_slot_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  // libvpx only lets the decoder reuse the previous frame's motion vectors
  // when the geometry is unchanged, so track what the last frame looked like.
  uint32_t vp9_last_width_ = 0;
  uint32_t vp9_last_height_ = 0;
  bool vp9_last_show_frame_ = false;
  // Maps each AV1 reference slot (0..7) to the DPB texture index holding it.
  int32_t av1_ref_slot_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  dx_h265::PocState h265_poc_;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t padded_width_ = 0;
  uint32_t padded_height_ = 0;
  uint32_t dpb_slot_count_ = 17;
  // H.264 reference picture buffer and its marking process. The other codecs
  // still use next_slot_/reference_usage_ below.
  dx_h264::Dpb h264_dpb_;
  uint32_t next_slot_ = 0;
  uint32_t next_ref_ = 0;
  uint32_t feedback_ = 1;
  std::vector<uint8_t> reference_usage_;
  std::vector<ReorderEntry> h264_reorder_queue_;

public:
  ~DX11Decoder() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    release();
    codec_id_ = options.format.codec_id;
    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 &&
        codec_id_ != OM_CODEC_AV1 && codec_id_ != OM_CODEC_VP9) return OM_CODEC_NOT_SUPPORTED;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;

    uint8_t bit_depth = 8;
    if (codec_id_ == OM_CODEC_H264) {
      h264_.parseExtradata(options.extradata);
      padded_width_ = dx_h264::alignUp(width_, 16u);
      padded_height_ = dx_h264::alignUp(height_, 16u);
      if (h264_.has_sps) {
        for (uint32_t i = 0; i < 32; ++i) {
          if (!h264_.sps_valid[i]) continue;
          padded_width_ = static_cast<uint32_t>((h264_.sps[i].pic_width_in_mbs_minus1 + 1) * 16);
          padded_height_ = static_cast<uint32_t>((h264_.sps[i].pic_height_in_map_units_minus1 + 1) * 16);
          // max_num_ref_frames slots for the reference pictures the DPB holds,
          // one for the picture being decoded, and one more so a non-reference
          // picture always has somewhere to go that no reference occupies.
          dpb_slot_count_ =
              std::clamp<uint32_t>(h264_.sps[i].num_ref_frames + 2, 3, 17);
          h264_dpb_.configure(dpb_slot_count_,
                              static_cast<uint32_t>(h264_.sps[i].num_ref_frames),
                              1u << (h264_.sps[i].log2_max_frame_num_minus4 + 4));
          bit_depth = static_cast<uint8_t>(h264_.sps[i].bit_depth_luma_minus8 + 8);
          // The DXVA profiles used below are 4:2:0 only; accepting a 4:2:2 or
          // 4:4:4 stream here would decode into an NV12/P010 surface and hand
          // back a wrong picture instead of failing over to software.
          if (h264_.sps[i].chroma_format_idc != 1) return OM_CODEC_NOT_SUPPORTED;
          break;
        }
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      vp9_ = std::make_unique<video_parser::VP9FrameParser>();
      // VP9 carries no out-of-band configuration; everything of interest is in
      // the first frame's uncompressed header.
      bit_depth = static_cast<uint8_t>(options.format.video.format == OM_FORMAT_P010 ? 10 : 8);
      padded_width_ = dx_h264::alignUp(width_, 64u);
      padded_height_ = dx_h264::alignUp(height_, 64u);
      dpb_slot_count_ = 9; // 8 reference slots plus the frame being decoded
    } else if (codec_id_ == OM_CODEC_AV1) {
      av1_ = std::make_unique<video_parser::AV1ObuParser>();
      // The AV1CodecConfigurationRecord prefixes the config OBUs with four
      // bytes of its own; feeding those to the OBU parser yields nothing.
      if (options.extradata.size() > 4 && (options.extradata[0] & 0x80) != 0)
        av1_->parse(options.extradata.subspan(4));
      const auto& seq = av1_->sequenceHeader();
      if (seq.valid) {
        width_ = seq.max_frame_width;
        height_ = seq.max_frame_height;
        bit_depth = seq.color_config.bit_depth;
      }
      // AV1 superblocks are up to 128x128, so give the surface room for a
      // whole one; the download crops back to the coded size.
      padded_width_ = dx_h264::alignUp(width_, 128u);
      padded_height_ = dx_h264::alignUp(height_, 128u);
      dpb_slot_count_ = 9; // 8 reference slots plus the frame being decoded
    } else {
      h265_ = std::make_unique<video_parser::H265AccessUnitParser>();
      h265_->parseExtradata(options.extradata);
      padded_width_ = dx_h264::alignUp(width_, 32u);
      padded_height_ = dx_h264::alignUp(height_, 32u);
      if (h265_->hasSps()) {
        for (int i = 0; i < 16; ++i) {
          const auto& s = h265_->sps(i);
          if (!s.valid) continue;
          padded_width_ = dx_h264::alignUp(static_cast<uint32_t>(s.pic_width_in_luma_samples), 32u);
          padded_height_ = dx_h264::alignUp(static_cast<uint32_t>(s.pic_height_in_luma_samples), 32u);
          dpb_slot_count_ = 16; // HEVC usually needs up to 16
          bit_depth = static_cast<uint8_t>(s.bit_depth_luma_minus8 + 8);
          if (s.chroma_format_idc != 1) return OM_CODEC_NOT_SUPPORTED;
          break;
        }
      }
    }

    if (options.hw_device && options.hw_device->type == HWDeviceType::DX11 && options.hw_device->context) {
      hw_context_ = static_cast<OMDX11Context*>(options.hw_device->context);
      owns_hw_context_ = false;
    } else {
      OMDX11Init init = {};
      init.adapter_index = -1;
      hw_context_ = HWD3D11Context_create(init);
      owns_hw_context_ = true;
    }
    if (!hw_context_) return OM_CODEC_HWACCEL_FAILED;

    device_ = HWD3D11Context_getDevice(hw_context_);
    video_device_ = HWD3D11Context_getVideoDevice(hw_context_);
    video_context_ = HWD3D11Context_getVideoContext(hw_context_);
    if (!device_ || !video_device_ || !video_context_) return OM_CODEC_HWACCEL_FAILED;
    device_->GetImmediateContext(&context_);
    if (!context_) return OM_CODEC_HWACCEL_FAILED;

    if (!createDecoderResources(bit_depth)) return OM_CODEC_HWACCEL_FAILED;

    output_format_ = {};
    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = bit_depth > 8 ? OM_FORMAT_P010 : OM_FORMAT_NV12;
    output_format_.color_space = options.format.video.color_space;
    output_format_.transfer_char = options.format.video.transfer_char;
    output_format_.color_primaries = options.format.video.color_primaries;
    output_format_.color_range = OM_COLOR_RANGE_UNSPECIFIED;
    if (codec_id_ == OM_CODEC_H264 && h264_.has_sps) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_.sps_valid[i]) continue;
        const auto& s = h264_.sps[i];
        if (s.vui_parameters_present_flag && s.vui.colour_description_present_flag) {
          output_format_.color_primaries = (OMColorPrimaries) s.vui.colour_primaries;
          output_format_.transfer_char = (OMTransferCharacteristic) s.vui.transfer_characteristics;
          output_format_.color_space = (OMColorSpace) s.vui.matrix_coefficients;
        }
        if (s.vui_parameters_present_flag && s.vui.video_signal_type_present_flag) {
          output_format_.color_range = s.vui.video_full_range_flag ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
        }
        break;
      }
    } else if (codec_id_ == OM_CODEC_H265 && h265_ && h265_->hasSps()) {
      for (int i = 0; i < 16; ++i) {
        const auto& s = h265_->sps(i);
        if (!s.valid) continue;
        if (s.vui_parameters_present_flag && s.vui.colour_description_present_flag) {
          output_format_.color_primaries = (OMColorPrimaries) s.vui.colour_primaries;
          output_format_.transfer_char = (OMTransferCharacteristic) s.vui.transfer_characteristics;
          output_format_.color_space = (OMColorSpace) s.vui.matrix_coeffs;
        }
        if (s.vui_parameters_present_flag && s.vui.video_signal_type_present_flag) {
          output_format_.color_range = s.vui.video_full_range_flag ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
        }
        break;
      }
    }

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
    if (packet.bytes.empty()) {
      if (codec_id_ == OM_CODEC_H264) return Ok(drainH264Reordered());
      return Ok(std::vector<Frame> {});
    }

    if (codec_id_ == OM_CODEC_H264) return decodeH264(packet);
    if (codec_id_ == OM_CODEC_H265) return decodeH265(packet);
    if (codec_id_ == OM_CODEC_AV1) return decodeAV1(packet);
    if (codec_id_ == OM_CODEC_VP9) return decodeVP9(packet);
    return Err(OM_CODEC_NOT_SUPPORTED);
  }

  void flush() override {
    resetReceiveState();
    for (auto& slot : slots_) slot.dpb = {};
    h264_dpb_.reset();
    reference_usage_.clear();
    h264_reorder_queue_.clear();
    next_slot_ = 0;
    next_ref_ = 0;
    h264_.resetPoc();
    h265_poc_.reset();
    if (av1_) av1_->reset();
    for (auto& r : av1_ref_slot_) r = -1;
    if (vp9_) vp9_->reset();
    for (auto& r : vp9_ref_slot_) r = -1;
    vp9_last_width_ = 0;
    vp9_last_height_ = 0;
    vp9_last_show_frame_ = false;
  }

private:
  auto pushH264Reordered(Frame frame, int32_t poc, size_t reorder_depth) -> std::vector<Frame> {
    if (reorder_depth == 0) return {std::move(frame)};

    h264_reorder_queue_.push_back({poc, std::move(frame)});
    if (h264_reorder_queue_.size() <= reorder_depth) return {};

    auto it = std::min_element(h264_reorder_queue_.begin(), h264_reorder_queue_.end(), [](const auto& a, const auto& b) {
      return a.poc < b.poc;
    });
    std::vector<Frame> output;
    output.push_back(std::move(it->frame));
    h264_reorder_queue_.erase(it);
    return output;
  }

  auto drainH264Reordered() -> std::vector<Frame> {
    std::sort(h264_reorder_queue_.begin(), h264_reorder_queue_.end(), [](const auto& a, const auto& b) {
      return a.poc < b.poc;
    });

    std::vector<Frame> output;
    output.reserve(h264_reorder_queue_.size());
    for (auto& entry : h264_reorder_queue_) output.push_back(std::move(entry.frame));
    h264_reorder_queue_.clear();
    return output;
  }

  auto decodeH264(const Packet& packet) -> Result<std::vector<Frame>, OMError> {

    auto parsed = h264_.parseFrame(packet.bytes);
    if (parsed.slice_offsets.empty()) return Ok(std::vector<Frame> {});
    if (parsed.slice.pic_parameter_set_id < 0 || parsed.slice.pic_parameter_set_id >= 256 || !h264_.pps_valid[parsed.slice.pic_parameter_set_id]) {
      return Err(OM_CODEC_DECODE_FAILED);
    }
    const auto& pps = h264_.pps[parsed.slice.pic_parameter_set_id];
    if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32 || !h264_.sps_valid[pps.seq_parameter_set_id]) {
      return Err(OM_CODEC_DECODE_FAILED);
    }
    const auto& sps = h264_.sps[pps.seq_parameter_set_id];
    if (sps.bit_depth_luma_minus8 != 0 || sps.bit_depth_chroma_minus8 != 0 || pps.num_slice_groups_minus1 != 0) {
      return Err(OM_CODEC_NOT_SUPPORTED);
    }

    std::vector<Frame> pre_output;
    if (parsed.is_intra) {
      pre_output = drainH264Reordered();
      h264_dpb_.reset();
      h264_.resetPoc();
      parsed.poc = h264_.computePoc(parsed.slice, parsed.is_reference);
    }

    // Resolve the short-term pictures against this frame_num before anything
    // reads or marks them, then take a slot the DPB is not holding a reference
    // in — including for non-reference pictures, which must not be decoded over
    // a surface the stream still refers to.
    h264_dpb_.updatePicNums(static_cast<uint32_t>(parsed.slice.frame_num));
    const uint32_t current_slot = h264_dpb_.acquireSlot();

    DXVA_PicParams_H264 pic_params = {};
    dx_h264::fillPicParams(sps, pps, parsed.slice, parsed, current_slot, h264_dpb_, feedback_++, pic_params);

    DXVA_Qmatrix_H264 qmatrix = {};
    dx_h264::fillQMatrix(sps, pps, qmatrix);

    std::vector<DXVA_Slice_H264_Short> slices(parsed.slice_offsets.size());
    for (size_t i = 0; i < parsed.slice_offsets.size(); ++i) {
      const uint32_t begin = parsed.slice_offsets[i];
      const uint32_t end = (i + 1 < parsed.slice_offsets.size()) ? parsed.slice_offsets[i + 1] : static_cast<uint32_t>(parsed.bitstream.size());
      slices[i].BSNALunitDataLocation = begin;
      slices[i].SliceBytesInBuffer = end - begin;
      slices[i].wBadSliceChopping = 0;
    }

    HRESULT hr = video_context_->DecoderBeginFrame(decoder_.Get(), slots_[current_slot].view.Get(), 0, nullptr);
    if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

    // DXVA wants the bitstream buffer a multiple of 128 bytes, zero-padded, with
    // the padding counted into the last slice. Handing over an unpadded buffer
    // leaves it up to the driver whether the trailing partial block is read at
    // all, which is why only some frames used to come out decoded.
    const size_t bitstream_size = parsed.bitstream.size();
    const size_t padded_size = (bitstream_size + 127u) & ~size_t(127u);
    if (!slices.empty()) {
      slices.back().SliceBytesInBuffer += static_cast<UINT>(padded_size - bitstream_size);
    }

    UINT buffer_size = 0;
    void* buffer = nullptr;
    hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &buffer_size, &buffer);
    if (FAILED(hr) || padded_size > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
    std::memcpy(buffer, parsed.bitstream.data(), bitstream_size);
    std::memset(static_cast<uint8_t*>(buffer) + bitstream_size, 0, padded_size - bitstream_size);
    video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);

    hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &buffer_size, &buffer);
    if (FAILED(hr) || sizeof(pic_params) > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
    std::memcpy(buffer, &pic_params, sizeof(pic_params));
    video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);

    hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &buffer_size, &buffer);
    if (FAILED(hr) || sizeof(qmatrix) > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
    std::memcpy(buffer, &qmatrix, sizeof(qmatrix));
    video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX);

    hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &buffer_size, &buffer);
    if (FAILED(hr) || slices.size() * sizeof(DXVA_Slice_H264_Short) > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
    std::memcpy(buffer, slices.data(), slices.size() * sizeof(DXVA_Slice_H264_Short));
    video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);

    D3D11_VIDEO_DECODER_BUFFER_DESC descs[4] = {};
    descs[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
    descs[0].DataSize = static_cast<UINT>(padded_size);
    descs[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
    descs[1].DataSize = sizeof(pic_params);
    descs[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
    descs[2].DataSize = sizeof(qmatrix);
    descs[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
    descs[3].DataSize = static_cast<UINT>(slices.size() * sizeof(DXVA_Slice_H264_Short));
    hr = video_context_->SubmitDecoderBuffers(decoder_.Get(), 4, descs);
    if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);
    hr = video_context_->DecoderEndFrame(decoder_.Get());
    if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

    auto picture = download(current_slot);
    if (!picture.has_value()) return Err(OM_CODEC_DECODE_FAILED);

    h264_dpb_.store(current_slot, parsed.poc, parsed.slice, parsed.is_intra, parsed.is_reference);

    Frame frame = {};
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(*picture);
    auto output = pushH264Reordered(std::move(frame), parsed.poc, dx_h264::reorderDepth(sps));
    if (!pre_output.empty()) {
      pre_output.insert(pre_output.end(), std::make_move_iterator(output.begin()), std::make_move_iterator(output.end()));
      return Ok(std::move(pre_output));
    }
    return Ok(std::move(output));
  }

  auto createDecoderResources(uint8_t bit_depth) -> bool {
    decoder_.Reset();
    dpb_texture_.Reset();
    slots_.clear();

    GUID target_profile = {};
    if (codec_id_ == OM_CODEC_H264) {
      target_profile = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    } else if (codec_id_ == OM_CODEC_AV1) {
      // Only Profile0 (4:2:0, 8/10-bit) is exposed by current DXVA drivers.
      target_profile = D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0;
    } else if (codec_id_ == OM_CODEC_VP9) {
      target_profile = (bit_depth > 8) ? D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2
                                       : D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0;
    } else {
      target_profile = (bit_depth > 8) ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10
                                       : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
    }
    bool profile_supported = false;
    const UINT profile_count = video_device_->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < profile_count; ++i) {
      GUID profile = {};
      if (SUCCEEDED(video_device_->GetVideoDecoderProfile(i, &profile)) && profile == target_profile) {
        profile_supported = true;
        break;
      }
    }
    if (!profile_supported) return false;

    D3D11_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.Guid = target_profile;
    decoder_desc.SampleWidth = padded_width_;
    decoder_desc.SampleHeight = padded_height_;
    decoder_desc.OutputFormat = (bit_depth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    UINT config_count = 0;
    if (FAILED(video_device_->GetVideoDecoderConfigCount(&decoder_desc, &config_count)) || config_count == 0) return false;
    bool found_config = false;
    for (UINT i = 0; i < config_count; ++i) {
      D3D11_VIDEO_DECODER_CONFIG config = {};
      if (FAILED(video_device_->GetVideoDecoderConfig(&decoder_desc, i, &config))) continue;
      const UINT expected_raw = (codec_id_ == OM_CODEC_H264) ? 2u : 1u;
      if (config.guidConfigBitstreamEncryption == DXVA_NO_ENCRYPT &&
          config.guidConfigMBcontrolEncryption == DXVA_NO_ENCRYPT &&
          config.guidConfigResidDiffEncryption == DXVA_NO_ENCRYPT &&
          (codec_id_ == OM_CODEC_AV1 || codec_id_ == OM_CODEC_VP9 ||
           config.ConfigBitstreamRaw == expected_raw)) {
        decoder_config_ = config;
        found_config = true;
        break;
      }
    }
    if (!found_config) return false;
    if (FAILED(video_device_->CreateVideoDecoder(&decoder_desc, &decoder_config_, &decoder_))) return false;

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = padded_width_;
    texture_desc.Height = padded_height_;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = dpb_slot_count_;
    texture_desc.Format = decoder_desc.OutputFormat;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&texture_desc, nullptr, &dpb_texture_))) return false;

    slots_.resize(dpb_slot_count_);
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view_desc = {};
      view_desc.DecodeProfile = target_profile;
      view_desc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      view_desc.Texture2D.ArraySlice = i;
      if (FAILED(video_device_->CreateVideoDecoderOutputView(dpb_texture_.Get(), &view_desc, &slots_[i].view))) return false;
    }

    output_format_.format = static_cast<OMPixelFormat>(texture_desc.Format == DXGI_FORMAT_P010 ? OM_FORMAT_P010 : OM_FORMAT_NV12);
    return true;
  }

  auto decodeH265(const Packet& packet) -> Result<std::vector<Frame>, OMError> {
    if (!h265_) return Err(OM_CODEC_DECODE_FAILED);
    auto frames = h265_->parse(packet.bytes, true);
    if (frames.empty()) return Ok(std::vector<Frame> {});

    std::vector<Frame> output;
    output.reserve(frames.size());
    for (auto& parsed : frames) {
      if (parsed.slice_offsets.empty() || parsed.slice_headers.empty()) continue;
      const auto& sh = parsed.slice_headers.front();
      if (sh.pps_id < 0 || sh.pps_id >= 64 || !h265_->pps(sh.pps_id).valid) return Err(OM_CODEC_DECODE_FAILED);
      const auto& pps = h265_->pps(sh.pps_id);
      if (pps.sps_id < 0 || pps.sps_id >= 16 || !h265_->sps(pps.sps_id).valid) return Err(OM_CODEC_DECODE_FAILED);
      const auto& sps = h265_->sps(pps.sps_id);
      if (sps.chroma_format_idc != 1 || sps.bit_depth_luma_minus8 != sps.bit_depth_chroma_minus8) return Err(OM_CODEC_NOT_SUPPORTED);

      const uint8_t bit_depth = static_cast<uint8_t>(sps.bit_depth_luma_minus8 + 8);
      const OMPixelFormat expected_format = bit_depth > 8 ? OM_FORMAT_P010 : OM_FORMAT_NV12;
      if (output_format_.format != expected_format) {
        padded_width_ = dx_h264::alignUp(static_cast<uint32_t>(sps.pic_width_in_luma_samples), 32u);
        padded_height_ = dx_h264::alignUp(static_cast<uint32_t>(sps.pic_height_in_luma_samples), 32u);
        dpb_slot_count_ = 16;
        reference_usage_.clear();
        next_ref_ = 0;
        next_slot_ = 0;
        h265_poc_.reset();
        if (!createDecoderResources(bit_depth)) return Err(OM_CODEC_HWACCEL_FAILED);
      }

      const int32_t poc = h265_poc_.compute(sps, parsed, sh);
      if (dx_h265::isIrap(parsed.nal_unit_type)) {
        for (auto& slot : slots_) slot.dpb.is_reference = false;
        reference_usage_.clear();
        next_ref_ = 0;
        next_slot_ = 0;
      }

      const uint32_t current_slot = next_slot_;
      std::vector<dx_h264::DpbEntry> dpb;
      dpb.reserve(slots_.size());
      for (const auto& slot : slots_) dpb.push_back(slot.dpb);

      DXVA_PicParams_HEVC pic_params = {};
      dx_h265::fillPicParams(sps, pps, sh, parsed, poc, current_slot, reference_usage_, dpb, feedback_++, pic_params);
      const auto slice_data = dx_h265::appendBitstreamAndSliceDataWithStartCode(parsed);
      if (slice_data.bitstream.empty() || slice_data.slices.empty()) return Err(OM_CODEC_DECODE_FAILED);
      DXVA_Qmatrix_HEVC qmatrix = {};
      const bool submit_qmatrix = sps.scaling_list_enabled_flag;
      if (submit_qmatrix) dx_h265::fillQMatrix(sps, pps, qmatrix);

      HRESULT hr = video_context_->DecoderBeginFrame(decoder_.Get(), slots_[current_slot].view.Get(), 0, nullptr);
      if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

      UINT buffer_size = 0;
      void* buffer = nullptr;
      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &buffer_size, &buffer);
      if (FAILED(hr) || slice_data.bitstream.size() > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
      std::memcpy(buffer, slice_data.bitstream.data(), slice_data.bitstream.size());
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);

      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &buffer_size, &buffer);
      if (FAILED(hr) || sizeof(pic_params) > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
      std::memcpy(buffer, &pic_params, sizeof(pic_params));
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);

      if (submit_qmatrix) {
        hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &buffer_size, &buffer);
        if (FAILED(hr) || sizeof(qmatrix) > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
        std::memcpy(buffer, &qmatrix, sizeof(qmatrix));
        video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX);
      }

      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &buffer_size, &buffer);
      if (FAILED(hr) || slice_data.slices.size() * sizeof(DXVA_Slice_HEVC_Short) > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
      std::memcpy(buffer, slice_data.slices.data(), slice_data.slices.size() * sizeof(DXVA_Slice_HEVC_Short));
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);

      D3D11_VIDEO_DECODER_BUFFER_DESC descs[4] = {};
      UINT desc_count = 0;
      descs[desc_count].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
      descs[desc_count++].DataSize = static_cast<UINT>(slice_data.bitstream.size());
      descs[desc_count].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
      descs[desc_count++].DataSize = sizeof(pic_params);
      if (submit_qmatrix) {
        descs[desc_count].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
        descs[desc_count++].DataSize = sizeof(qmatrix);
      }
      descs[desc_count].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
      descs[desc_count++].DataSize = static_cast<UINT>(slice_data.slices.size() * sizeof(DXVA_Slice_HEVC_Short));
      hr = video_context_->SubmitDecoderBuffers(decoder_.Get(), desc_count, descs);
      if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);
      hr = video_context_->DecoderEndFrame(decoder_.Get());
      if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

      auto picture = download(current_slot);
      if (!picture.has_value()) return Err(OM_CODEC_DECODE_FAILED);

      slots_[current_slot].dpb.poc = poc;
      slots_[current_slot].dpb.frame_num = static_cast<uint32_t>(poc);
      slots_[current_slot].dpb.is_reference = parsed.is_reference;
      if (parsed.is_reference && dpb_slot_count_ > 1) {
        if (next_ref_ >= reference_usage_.size()) reference_usage_.resize(next_ref_ + 1);
        reference_usage_[next_ref_] = static_cast<uint8_t>(current_slot);
        next_ref_ = (next_ref_ + 1) % (dpb_slot_count_ - 1);
      }
      next_slot_ = (next_slot_ + 1) % dpb_slot_count_;

      Frame frame = {};
      frame.pts = packet.pts;
      frame.dts = packet.dts;
      frame.data = std::move(*picture);
      output.push_back(std::move(frame));
    }
    return Ok(std::move(output));
  }


  // Collects the tile payloads of one frame. DXVA wants the tile data packed
  // contiguously with offsets relative to the start of that packed buffer,
  // unlike Vulkan which indexes into the original packet.
  struct AV1TileRef {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
  };

  auto collectAV1Tiles(const video_parser::AV1ParsedFrame& parsed,
                       std::vector<AV1TileRef>& tiles) -> bool {
    const auto& ti = parsed.header.tile_info;
    const uint32_t num_tiles = ti.tile_cols * ti.tile_rows;
    if (num_tiles == 0) return false;
    const uint32_t tile_size_bytes = ti.tile_size_bytes > 0 ? ti.tile_size_bytes : 1;

    for (const auto& obu : parsed.obus) {
      if (obu.type != video_parser::AV1_OBU_FRAME &&
          obu.type != video_parser::AV1_OBU_TILE_GROUP) continue;

      size_t tg_offset = (obu.type == video_parser::AV1_OBU_FRAME) ? parsed.tile_group_offset
                                                                   : obu.payload_offset;
      size_t tg_size = (obu.type == video_parser::AV1_OBU_FRAME) ? parsed.tile_group_size
                                                                 : obu.payload_size;
      if (tg_size == 0 || tg_offset + tg_size > parsed.bitstream.size()) continue;

      BitReader reader(std::span<const uint8_t>(parsed.bitstream.data() + tg_offset, tg_size));
      uint32_t tg_start = 0;
      uint32_t tg_end = num_tiles - 1;
      if (num_tiles > 1) {
        const bool present = reader.readFlag();
        if (present && obu.type != video_parser::AV1_OBU_FRAME) {
          const uint32_t bits = ti.tile_cols_log2 + ti.tile_rows_log2;
          tg_start = reader.readBits(bits);
          tg_end = reader.readBits(bits);
        }
      }
      reader.alignToByte();

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
        tiles.push_back({parsed.bitstream.data() + pos, size});
        pos += size;
      }
    }
    return tiles.size() == num_tiles;
  }

  auto pickAV1Slot() const -> uint32_t {
    bool in_use[32] = {};
    for (int32_t r : av1_ref_slot_)
      if (r >= 0 && r < static_cast<int32_t>(dpb_slot_count_)) in_use[r] = true;
    for (uint32_t i = 0; i < dpb_slot_count_; ++i)
      if (!in_use[i]) return i;
    return 0;
  }

  void fillAV1PicParams(const video_parser::AV1ParsedFrame& parsed, uint32_t slot,
                        DXVA_PicParams_AV1& pp) {
    const auto& seq = av1_->sequenceHeader();
    const auto& h = parsed.header;

    pp.width = h.frame_width;
    pp.height = h.frame_height;
    pp.max_width = seq.max_frame_width;
    pp.max_height = seq.max_frame_height;

    pp.CurrPicTextureIndex = static_cast<UCHAR>(slot);
    // Unlike Vulkan's coded_denom, DXVA wants the reconstructed denominator.
    pp.superres_denom = h.use_superres ? h.superres_denom
                                       : static_cast<UCHAR>(video_parser::AV1_SUPERRES_NUM);
    pp.bitdepth = seq.color_config.bit_depth;
    pp.seq_profile = seq.seq_profile;

    const auto& ti = h.tile_info;
    pp.tiles.cols = static_cast<UCHAR>(ti.tile_cols);
    pp.tiles.rows = static_cast<UCHAR>(ti.tile_rows);
    pp.tiles.context_update_id = static_cast<USHORT>(ti.context_update_tile_id);

    // Tile sizes are expressed in superblocks.
    const uint32_t sb_shift = seq.use_128x128_superblock ? 5u : 4u;
    for (uint32_t i = 0; i < ti.tile_cols && i < 64; ++i) {
      const uint32_t mi = ti.mi_col_starts[i + 1] - ti.mi_col_starts[i];
      pp.tiles.widths[i] = static_cast<USHORT>((mi + (1u << sb_shift) - 1) >> sb_shift);
    }
    for (uint32_t i = 0; i < ti.tile_rows && i < 64; ++i) {
      const uint32_t mi = ti.mi_row_starts[i + 1] - ti.mi_row_starts[i];
      pp.tiles.heights[i] = static_cast<USHORT>((mi + (1u << sb_shift) - 1) >> sb_shift);
    }

    pp.coding.use_128x128_superblock = seq.use_128x128_superblock;
    pp.coding.intra_edge_filter = seq.enable_intra_edge_filter;
    pp.coding.interintra_compound = seq.enable_interintra_compound;
    pp.coding.masked_compound = seq.enable_masked_compound;
    // Per the DXVA spec these three take the *frame* header values even though
    // the sequence header has similarly named fields.
    pp.coding.warped_motion = h.allow_warped_motion;
    pp.coding.dual_filter = seq.enable_dual_filter;
    pp.coding.jnt_comp = seq.enable_jnt_comp;
    pp.coding.screen_content_tools = h.allow_screen_content_tools;
    pp.coding.integer_mv = h.force_integer_mv;
    pp.coding.cdef = seq.enable_cdef;
    pp.coding.restoration = seq.enable_restoration;
    pp.coding.film_grain = seq.film_grain_params_present;
    pp.coding.intrabc = h.allow_intrabc;
    pp.coding.high_precision_mv = h.allow_high_precision_mv;
    pp.coding.switchable_motion_mode = h.is_motion_mode_switchable;
    pp.coding.filter_intra = seq.enable_filter_intra;
    pp.coding.disable_frame_end_update_cdf = h.disable_frame_end_update_cdf;
    pp.coding.disable_cdf_update = h.disable_cdf_update;
    pp.coding.reference_mode = h.reference_select;
    pp.coding.skip_mode = h.skip_mode_present;
    pp.coding.reduced_tx_set = h.reduced_tx_set;
    pp.coding.superres = h.use_superres;
    pp.coding.tx_mode = h.tx_mode;
    pp.coding.use_ref_frame_mvs = h.use_ref_frame_mvs;
    pp.coding.enable_ref_frame_mvs = seq.enable_ref_frame_mvs;
    pp.coding.reference_frame_update =
        !(h.show_existing_frame && h.frame_type == video_parser::AV1_KEY_FRAME);

    pp.format.frame_type = h.frame_type;
    pp.format.show_frame = h.show_frame;
    pp.format.showable_frame = h.showable_frame;
    pp.format.subsampling_x = seq.color_config.subsampling_x;
    pp.format.subsampling_y = seq.color_config.subsampling_y;
    pp.format.mono_chrome = seq.color_config.mono_chrome;

    pp.primary_ref_frame = h.primary_ref_frame;
    pp.order_hint = static_cast<UCHAR>(h.order_hint);
    pp.order_hint_bits = seq.order_hint_bits;

    for (uint32_t i = 0; i < 7; ++i) {
      const uint8_t map_idx = h.ref_frame_idx[i];
      const int32_t ref_slot = (map_idx < 8) ? av1_ref_slot_[map_idx] : -1;
      if (h.frame_is_intra || ref_slot < 0) {
        pp.frame_refs[i].Index = 0xFF;
        continue;
      }
      pp.frame_refs[i].Index = map_idx;
      pp.frame_refs[i].width = h.frame_width;
      pp.frame_refs[i].height = h.frame_height;
      const uint32_t ref_name = video_parser::AV1_LAST_FRAME + i;
      for (uint32_t j = 0; j < 6; ++j)
        pp.frame_refs[i].wmmat[j] = h.global_motion.params[ref_name][j];
      pp.frame_refs[i].wmtype = h.global_motion.type[ref_name];
      pp.frame_refs[i].wminvalid =
          (h.global_motion.type[ref_name] == video_parser::AV1_IDENTITY);
    }
    for (uint32_t i = 0; i < 8; ++i)
      pp.RefFrameMapTextureIndex[i] =
          av1_ref_slot_[i] >= 0 ? static_cast<UCHAR>(av1_ref_slot_[i]) : 0xFF;

    pp.loop_filter.filter_level[0] = h.loop_filter.level[0];
    pp.loop_filter.filter_level[1] = h.loop_filter.level[1];
    pp.loop_filter.filter_level_u = h.loop_filter.level[2];
    pp.loop_filter.filter_level_v = h.loop_filter.level[3];
    pp.loop_filter.sharpness_level = h.loop_filter.sharpness;
    pp.loop_filter.mode_ref_delta_enabled = h.loop_filter.delta_enabled;
    pp.loop_filter.mode_ref_delta_update = h.loop_filter.delta_update;
    pp.loop_filter.delta_lf_multi = h.loop_filter.delta_lf_multi;
    pp.loop_filter.delta_lf_present = h.loop_filter.delta_lf_present;
    for (uint32_t i = 0; i < 8; ++i)
      pp.loop_filter.ref_deltas[i] = h.loop_filter.ref_deltas[i];
    pp.loop_filter.mode_deltas[0] = h.loop_filter.mode_deltas[0];
    pp.loop_filter.mode_deltas[1] = h.loop_filter.mode_deltas[1];
    pp.loop_filter.delta_lf_res = h.loop_filter.delta_lf_res;
    for (uint32_t i = 0; i < 3; ++i) {
      // The AV1 spec numbering already matches what DXVA expects here.
      pp.loop_filter.frame_restoration_type[i] = h.lr.frame_restoration_type[i];
      pp.loop_filter.log2_restoration_unit_size[i] = h.lr.loop_restoration_size_log2[i];
    }

    pp.quantization.delta_q_present = h.delta_q_present;
    pp.quantization.delta_q_res = h.delta_q_res;
    pp.quantization.base_qindex = h.quantization.base_q_idx;
    pp.quantization.y_dc_delta_q = static_cast<CHAR>(h.quantization.delta_q_y_dc);
    pp.quantization.u_dc_delta_q = static_cast<CHAR>(h.quantization.delta_q_u_dc);
    pp.quantization.v_dc_delta_q = static_cast<CHAR>(h.quantization.delta_q_v_dc);
    pp.quantization.u_ac_delta_q = static_cast<CHAR>(h.quantization.delta_q_u_ac);
    pp.quantization.v_ac_delta_q = static_cast<CHAR>(h.quantization.delta_q_v_ac);
    pp.quantization.qm_y = h.quantization.using_qmatrix ? h.quantization.qm_y : 0xFF;
    pp.quantization.qm_u = h.quantization.using_qmatrix ? h.quantization.qm_u : 0xFF;
    pp.quantization.qm_v = h.quantization.using_qmatrix ? h.quantization.qm_v : 0xFF;

    pp.cdef.damping = h.cdef.damping >= 3 ? (h.cdef.damping - 3) : 0;
    pp.cdef.bits = h.cdef.bits;
    for (uint32_t i = 0; i < 8; ++i) {
      pp.cdef.y_strengths[i].primary = h.cdef.y_pri_strength[i];
      pp.cdef.y_strengths[i].secondary = h.cdef.y_sec_strength[i];
      pp.cdef.uv_strengths[i].primary = h.cdef.uv_pri_strength[i];
      pp.cdef.uv_strengths[i].secondary = h.cdef.uv_sec_strength[i];
    }

    pp.interp_filter = h.interpolation_filter;

    pp.segmentation.enabled = h.segmentation.enabled;
    pp.segmentation.update_map = h.segmentation.update_map;
    pp.segmentation.update_data = h.segmentation.update_data;
    pp.segmentation.temporal_update = h.segmentation.temporal_update;
    for (uint32_t i = 0; i < 8; ++i) {
      for (uint32_t j = 0; j < 8; ++j) {
        if (h.segmentation.feature_enabled[i][j])
          pp.segmentation.feature_mask[i].mask |= static_cast<UCHAR>(1u << j);
        pp.segmentation.feature_data[i][j] = h.segmentation.feature_data[i][j];
      }
    }

    if (h.film_grain.apply_grain) {
      const auto& fg = h.film_grain;
      pp.film_grain.apply_grain = 1;
      pp.film_grain.scaling_shift_minus8 = fg.grain_scaling >= 8 ? (fg.grain_scaling - 8) : 0;
      pp.film_grain.chroma_scaling_from_luma = fg.chroma_scaling_from_luma;
      pp.film_grain.ar_coeff_lag = fg.ar_coeff_lag;
      pp.film_grain.ar_coeff_shift_minus6 = fg.ar_coeff_shift >= 6 ? (fg.ar_coeff_shift - 6) : 0;
      pp.film_grain.grain_scale_shift = fg.grain_scale_shift;
      pp.film_grain.overlap_flag = fg.overlap_flag;
      pp.film_grain.clip_to_restricted_range = fg.clip_to_restricted_range;
      pp.film_grain.matrix_coeff_is_identity = (seq.color_config.matrix_coefficients == 0);
      pp.film_grain.grain_seed = fg.grain_seed;
      pp.film_grain.num_y_points = fg.num_y_points;
      for (uint32_t i = 0; i < fg.num_y_points && i < 14; ++i) {
        pp.film_grain.scaling_points_y[i][0] = fg.point_y_value[i];
        pp.film_grain.scaling_points_y[i][1] = fg.point_y_scaling[i];
      }
      pp.film_grain.num_cb_points = fg.num_cb_points;
      for (uint32_t i = 0; i < fg.num_cb_points && i < 10; ++i) {
        pp.film_grain.scaling_points_cb[i][0] = fg.point_cb_value[i];
        pp.film_grain.scaling_points_cb[i][1] = fg.point_cb_scaling[i];
      }
      pp.film_grain.num_cr_points = fg.num_cr_points;
      for (uint32_t i = 0; i < fg.num_cr_points && i < 10; ++i) {
        pp.film_grain.scaling_points_cr[i][0] = fg.point_cr_value[i];
        pp.film_grain.scaling_points_cr[i][1] = fg.point_cr_scaling[i];
      }
      for (uint32_t i = 0; i < 24; ++i) pp.film_grain.ar_coeffs_y[i] = fg.ar_coeffs_y[i];
      for (uint32_t i = 0; i < 25; ++i) pp.film_grain.ar_coeffs_cb[i] = fg.ar_coeffs_cb[i];
      for (uint32_t i = 0; i < 25; ++i) pp.film_grain.ar_coeffs_cr[i] = fg.ar_coeffs_cr[i];
      pp.film_grain.cb_mult = fg.cb_mult;
      pp.film_grain.cb_luma_mult = fg.cb_luma_mult;
      pp.film_grain.cr_mult = fg.cr_mult;
      pp.film_grain.cr_luma_mult = fg.cr_luma_mult;
      pp.film_grain.cb_offset = static_cast<SHORT>(fg.cb_offset);
      pp.film_grain.cr_offset = static_cast<SHORT>(fg.cr_offset);
    }

    pp.StatusReportFeedbackNumber = feedback_++;
  }

  auto decodeAV1(const Packet& packet) -> Result<std::vector<Frame>, OMError> {
    if (!av1_) return Err(OM_CODEC_DECODE_FAILED);
    auto parsed_frames = av1_->parse(packet.bytes);
    if (parsed_frames.empty()) return Ok(std::vector<Frame> {});

    std::vector<Frame> output;
    for (const auto& parsed : parsed_frames) {
      if (!parsed.header.valid) continue;
      const auto& h = parsed.header;

      if (h.show_existing_frame) {
        const int32_t slot = (h.frame_to_show_map_idx < 8)
                                 ? av1_ref_slot_[h.frame_to_show_map_idx] : -1;
        if (slot < 0) continue;
        if (auto pic = download(static_cast<uint32_t>(slot))) {
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          frame.data = std::move(*pic);
          output.push_back(std::move(frame));
        }
        // Re-showing a key frame resets the whole reference pool to it.
        if (h.refresh_frame_flags == 0xFF)
          for (auto& r : av1_ref_slot_) r = slot;
        continue;
      }

      std::vector<AV1TileRef> tiles;
      if (!collectAV1Tiles(parsed, tiles) || tiles.empty()) continue;

      const uint32_t slot = pickAV1Slot();

      DXVA_PicParams_AV1 pic_params = {};
      fillAV1PicParams(parsed, slot, pic_params);

      HRESULT hr = E_FAIL;
      for (int attempt = 0; attempt < 512; ++attempt) {
        hr = video_context_->DecoderBeginFrame(decoder_.Get(), slots_[slot].view.Get(), 0, nullptr);
        if (hr != E_PENDING) break;
      }
      if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

      void* buffer = nullptr;
      UINT buffer_size = 0;

      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &buffer_size, &buffer);
      if (FAILED(hr) || buffer_size < sizeof(pic_params)) { video_context_->DecoderEndFrame(decoder_.Get()); return Err(OM_CODEC_DECODE_FAILED); }
      std::memcpy(buffer, &pic_params, sizeof(pic_params));
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);

      // The tile payloads go in back-to-back; DataOffset indexes that buffer.
      std::vector<DXVA_Tile_AV1> tile_params(tiles.size());
      size_t total = 0;
      for (size_t i = 0; i < tiles.size(); ++i) {
        tile_params[i].DataOffset = static_cast<UINT>(total);
        tile_params[i].DataSize = tiles[i].size;
        tile_params[i].row = static_cast<USHORT>(i / std::max<uint32_t>(pic_params.tiles.cols, 1));
        tile_params[i].column = static_cast<USHORT>(i % std::max<uint32_t>(pic_params.tiles.cols, 1));
        tile_params[i].anchor_frame = 0xFF;
        total += tiles[i].size;
      }

      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &buffer_size, &buffer);
      if (FAILED(hr) || buffer_size < total) { video_context_->DecoderEndFrame(decoder_.Get()); return Err(OM_CODEC_DECODE_FAILED); }
      {
        auto* dst = static_cast<uint8_t*>(buffer);
        size_t off = 0;
        for (const auto& t : tiles) { std::memcpy(dst + off, t.data, t.size); off += t.size; }
      }
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);

      const size_t tile_bytes = tile_params.size() * sizeof(DXVA_Tile_AV1);
      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &buffer_size, &buffer);
      if (FAILED(hr) || buffer_size < tile_bytes) { video_context_->DecoderEndFrame(decoder_.Get()); return Err(OM_CODEC_DECODE_FAILED); }
      std::memcpy(buffer, tile_params.data(), tile_bytes);
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);

      D3D11_VIDEO_DECODER_BUFFER_DESC descs[3] = {};
      descs[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
      descs[0].DataSize = static_cast<UINT>(sizeof(pic_params));
      descs[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
      descs[1].DataSize = static_cast<UINT>(total);
      descs[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
      descs[2].DataSize = static_cast<UINT>(tile_bytes);
      if (FAILED(video_context_->SubmitDecoderBuffers(decoder_.Get(), 3, descs))) {
        video_context_->DecoderEndFrame(decoder_.Get());
        return Err(OM_CODEC_DECODE_FAILED);
      }
      if (FAILED(video_context_->DecoderEndFrame(decoder_.Get()))) return Err(OM_CODEC_DECODE_FAILED);

      uint8_t refresh = h.refresh_frame_flags;
      if (h.frame_type == video_parser::AV1_KEY_FRAME && h.show_frame) refresh = 0xFF;
      for (uint32_t i = 0; i < 8; ++i)
        if (refresh & (1u << i)) av1_ref_slot_[i] = static_cast<int32_t>(slot);

      if (h.show_frame) {
        auto pic = download(slot);
        if (!pic.has_value()) return Err(OM_CODEC_DECODE_FAILED);
        Frame frame = {};
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        frame.data = std::move(*pic);
        output.push_back(std::move(frame));
      }
    }
    return Ok(std::move(output));
  }


  auto pickVP9Slot() const -> uint32_t {
    bool in_use[32] = {};
    for (int32_t r : vp9_ref_slot_)
      if (r >= 0 && r < static_cast<int32_t>(dpb_slot_count_)) in_use[r] = true;
    for (uint32_t i = 0; i < dpb_slot_count_; ++i)
      if (!in_use[i]) return i;
    return 0;
  }

  void fillVP9PicParams(const video_parser::VP9ParsedFrame& parsed, uint32_t slot,
                        DXVA_PicParams_VP9& pp) {
    const auto& h = parsed.header;

    pp.CurrPic.Index7Bits = static_cast<UCHAR>(slot);
    pp.profile = h.profile;

    pp.frame_type = h.frame_type;
    pp.show_frame = h.show_frame;
    pp.error_resilient_mode = h.error_resilient_mode;
    pp.subsampling_x = h.subsampling_x;
    pp.subsampling_y = h.subsampling_y;
    pp.extra_plane = 0;
    pp.refresh_frame_context = h.refresh_frame_context;
    pp.frame_parallel_decoding_mode = h.frame_parallel_decoding_mode;
    pp.intra_only = h.intra_only;
    pp.frame_context_idx = h.frame_context_idx;
    pp.reset_frame_context = h.reset_frame_context;
    pp.allow_high_precision_mv = h.allow_high_precision_mv;

    pp.width = h.frame_width;
    pp.height = h.frame_height;
    pp.BitDepthMinus8Luma = static_cast<UCHAR>(h.bit_depth - 8);
    pp.BitDepthMinus8Chroma = static_cast<UCHAR>(h.bit_depth - 8);
    pp.interp_filter = h.interpolation_filter;

    for (uint32_t i = 0; i < 8; ++i) {
      pp.ref_frame_map[i].bPicEntry =
          vp9_ref_slot_[i] >= 0 ? static_cast<UCHAR>(vp9_ref_slot_[i]) : 0xFF;
      const auto& ref = vp9_->refSlot(i);
      pp.ref_frame_coded_width[i] = ref.width;
      pp.ref_frame_coded_height[i] = ref.height;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint8_t map_idx = h.ref_frame_idx[i];
      pp.frame_refs[i].bPicEntry =
          (map_idx < 8 && vp9_ref_slot_[map_idx] >= 0)
              ? static_cast<UCHAR>(vp9_ref_slot_[map_idx]) : 0xFF;
    }
    for (uint32_t i = 0; i < 4; ++i)
      pp.ref_frame_sign_bias[i] = static_cast<CHAR>(h.ref_frame_sign_bias[i]);

    pp.filter_level = static_cast<CHAR>(h.loop_filter.level);
    pp.sharpness_level = static_cast<CHAR>(h.loop_filter.sharpness);
    pp.mode_ref_delta_enabled = h.loop_filter.delta_enabled;
    pp.mode_ref_delta_update = h.loop_filter.delta_update;
    // Mirrors libvpx: the previous frame's motion vectors may only be reused
    // when the geometry matches and nothing reset the context.
    pp.use_prev_in_find_mv_refs =
        vp9_last_width_ == h.frame_width && vp9_last_height_ == h.frame_height &&
        !h.error_resilient_mode && !h.intra_only && vp9_last_show_frame_;

    for (uint32_t i = 0; i < 4; ++i) pp.ref_deltas[i] = h.loop_filter.ref_deltas[i];
    for (uint32_t i = 0; i < 2; ++i) pp.mode_deltas[i] = h.loop_filter.mode_deltas[i];

    pp.base_qindex = h.quantization.base_q_idx;
    pp.y_dc_delta_q = h.quantization.delta_q_y_dc;
    pp.uv_dc_delta_q = h.quantization.delta_q_uv_dc;
    pp.uv_ac_delta_q = h.quantization.delta_q_uv_ac;

    pp.stVP9Segments.enabled = h.segmentation.enabled;
    pp.stVP9Segments.update_map = h.segmentation.update_map;
    pp.stVP9Segments.temporal_update = h.segmentation.temporal_update;
    pp.stVP9Segments.abs_delta = h.segmentation.abs_or_delta_update;
    for (uint32_t i = 0; i < 7; ++i)
      pp.stVP9Segments.tree_probs[i] = h.segmentation.tree_probs[i];
    for (uint32_t i = 0; i < 3; ++i)
      pp.stVP9Segments.pred_probs[i] = h.segmentation.pred_probs[i];
    for (uint32_t i = 0; i < 8; ++i) {
      pp.stVP9Segments.feature_mask[i] = 0;
      for (uint32_t j = 0; j < 4; ++j) {
        if (h.segmentation.feature_enabled[i][j])
          pp.stVP9Segments.feature_mask[i] |= static_cast<UCHAR>(1u << j);
        pp.stVP9Segments.feature_data[i][j] = h.segmentation.feature_data[i][j];
      }
    }

    pp.log2_tile_cols = h.tile_cols_log2;
    pp.log2_tile_rows = h.tile_rows_log2;
    pp.uncompressed_header_size_byte_aligned =
        static_cast<USHORT>(h.uncompressed_header_size);
    pp.first_partition_size = h.header_size_in_bytes;

    pp.StatusReportFeedbackNumber = feedback_++;
  }

  auto decodeVP9(const Packet& packet) -> Result<std::vector<Frame>, OMError> {
    if (!vp9_) return Err(OM_CODEC_DECODE_FAILED);
    auto parsed_frames = vp9_->parse(packet.bytes);
    if (parsed_frames.empty()) return Ok(std::vector<Frame> {});

    std::vector<Frame> output;
    for (const auto& parsed : parsed_frames) {
      const auto& h = parsed.header;
      if (!h.valid) continue;

      if (h.show_existing_frame) {
        const int32_t slot = (h.frame_to_show_map_idx < 8)
                                 ? vp9_ref_slot_[h.frame_to_show_map_idx] : -1;
        if (slot < 0) continue;
        if (auto pic = download(static_cast<uint32_t>(slot))) {
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          frame.data = std::move(*pic);
          output.push_back(std::move(frame));
        }
        continue;
      }

      const uint32_t slot = pickVP9Slot();

      DXVA_PicParams_VP9 pic_params = {};
      fillVP9PicParams(parsed, slot, pic_params);

      HRESULT hr = E_FAIL;
      for (int attempt = 0; attempt < 512; ++attempt) {
        hr = video_context_->DecoderBeginFrame(decoder_.Get(), slots_[slot].view.Get(), 0, nullptr);
        if (hr != E_PENDING) break;
      }
      if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

      void* buffer = nullptr;
      UINT buffer_size = 0;

      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &buffer_size, &buffer);
      if (FAILED(hr) || buffer_size < sizeof(pic_params)) { video_context_->DecoderEndFrame(decoder_.Get()); return Err(OM_CODEC_DECODE_FAILED); }
      std::memcpy(buffer, &pic_params, sizeof(pic_params));
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);

      // VP9 hands the decoder the whole frame, header included, as one slice.
      const size_t frame_size = parsed.bitstream.size();
      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &buffer_size, &buffer);
      if (FAILED(hr) || buffer_size < frame_size) { video_context_->DecoderEndFrame(decoder_.Get()); return Err(OM_CODEC_DECODE_FAILED); }
      std::memcpy(buffer, parsed.bitstream.data(), frame_size);
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);

      DXVA_Slice_VPx_Short slice = {};
      slice.BSNALunitDataLocation = 0;
      slice.SliceBytesInBuffer = static_cast<UINT>(frame_size);
      slice.wBadSliceChopping = 0;
      hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &buffer_size, &buffer);
      if (FAILED(hr) || buffer_size < sizeof(slice)) { video_context_->DecoderEndFrame(decoder_.Get()); return Err(OM_CODEC_DECODE_FAILED); }
      std::memcpy(buffer, &slice, sizeof(slice));
      video_context_->ReleaseDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);

      D3D11_VIDEO_DECODER_BUFFER_DESC descs[3] = {};
      descs[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
      descs[0].DataSize = static_cast<UINT>(sizeof(pic_params));
      descs[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
      descs[1].DataSize = static_cast<UINT>(frame_size);
      descs[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
      descs[2].DataSize = static_cast<UINT>(sizeof(slice));
      if (FAILED(video_context_->SubmitDecoderBuffers(decoder_.Get(), 3, descs))) {
        video_context_->DecoderEndFrame(decoder_.Get());
        return Err(OM_CODEC_DECODE_FAILED);
      }
      if (FAILED(video_context_->DecoderEndFrame(decoder_.Get()))) return Err(OM_CODEC_DECODE_FAILED);

      for (uint32_t i = 0; i < 8; ++i)
        if (h.refresh_frame_flags & (1u << i)) vp9_ref_slot_[i] = static_cast<int32_t>(slot);

      vp9_last_width_ = h.frame_width;
      vp9_last_height_ = h.frame_height;
      vp9_last_show_frame_ = h.show_frame;

      if (h.show_frame) {
        auto pic = download(slot);
        if (!pic.has_value()) return Err(OM_CODEC_DECODE_FAILED);
        Frame frame = {};
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        frame.data = std::move(*pic);
        output.push_back(std::move(frame));
      }
    }
    return Ok(std::move(output));
  }

  auto download(uint32_t slot) -> std::optional<Picture> {
    D3D11_TEXTURE2D_DESC desc = {};
    dpb_texture_->GetDesc(&desc);
    desc.ArraySize = 1;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return std::nullopt;
    context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, dpb_texture_.Get(), D3D11CalcSubresource(0, slot, 1), nullptr);

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return std::nullopt;

    const OMPixelFormat om_fmt = (desc.Format == DXGI_FORMAT_P010 ? OM_FORMAT_P010 : OM_FORMAT_NV12);
    Picture pic(om_fmt, width_, height_);
    pic.color_space = output_format_.color_space;
    pic.transfer_char = output_format_.transfer_char;
    pic.color_primaries = output_format_.color_primaries;
    pic.color_range = output_format_.color_range;
    pic.mastering_display = output_format_.mastering_display;
    pic.content_light_level = output_format_.content_light_level;
    const auto y_stride = pic.planes.getLinesize(0);
    const auto uv_stride = pic.planes.getLinesize(1);
    auto* y = pic.planes.getData(0);
    auto* uv = pic.planes.getData(1);
    const auto* src_y = static_cast<const uint8_t*>(map.pData);
    const auto* src_uv = src_y + static_cast<size_t>(map.RowPitch) * padded_height_;
    const size_t bpp = getBytesPerPixel(om_fmt, 0);
    for (uint32_t row = 0; row < height_; ++row) std::memcpy(y + static_cast<size_t>(row) * y_stride, src_y + static_cast<size_t>(row) * map.RowPitch, width_ * bpp);
    for (uint32_t row = 0; row < (height_ + 1) / 2; ++row) std::memcpy(uv + static_cast<size_t>(row) * uv_stride, src_uv + static_cast<size_t>(row) * map.RowPitch, width_ * bpp);
    context_->Unmap(staging.Get(), 0);
    return pic;
  }

  void release() {
    initialized_ = false;
    slots_.clear();
    dpb_texture_.Reset();
    decoder_.Reset();
    if (context_) {
      context_->Release();
      context_ = nullptr;
    }
    device_ = nullptr;
    video_device_ = nullptr;
    video_context_ = nullptr;
    if (owns_hw_context_ && hw_context_) HWD3D11Context_delete(hw_context_);
    hw_context_ = nullptr;
    owns_hw_context_ = false;
  }
};

const CodecDescriptor CODEC_DX11_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_h264",
    .long_name = "DirectX11 H.264 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<DX11Decoder>(); },
};

const CodecDescriptor CODEC_DX11_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_h265",
    .long_name = "DirectX11 H.265/HEVC Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<DX11Decoder>(); },
};

const CodecDescriptor CODEC_DX11_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_av1",
    .long_name = "DirectX11 AV1 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_AV1_MAIN},
    },
    .decoder_factory = [] { return std::make_unique<DX11Decoder>(); },
};

const CodecDescriptor CODEC_DX11_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_vp9",
    .long_name = "DirectX11 VP9 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<DX11Decoder>(); },
};

const CodecDescriptor CODEC_DX11_ENC_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_h264_enc",
    .long_name = "DirectX11 H.264 Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<DX11Encoder>(); },
};

const CodecDescriptor CODEC_DX11_ENC_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_h265_enc",
    .long_name = "DirectX11 H.265/HEVC Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<DX11Encoder>(); },
};

const CodecDescriptor CODEC_DX11_ENC_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_av1_enc",
    .long_name = "DirectX11 AV1 Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<DX11Encoder>(); },
};

} // namespace openmedia
