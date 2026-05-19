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
#include <vector>
#include <video/parser/h265_parser.hpp>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>

#include <util/wmf.hpp>
#include "dx_h264.hpp"
#include "hw_common.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "msvcrt.lib")

// EB533D05-D234-4530-9162-801691238C93
static constexpr GUID OM_MF_VIDEO_DEVICE_Manager = {0xeb533d05, 0xd234, 0x4530, {0x91, 0x62, 0x80, 0x16, 0x91, 0x23, 0x8c, 0x93}};

static constexpr GUID DXVA_NO_ENCRYPT = {0x1b81bed0, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};

using Microsoft::WRL::ComPtr;

namespace openmedia {

class DX11Encoder final : public Encoder {
  OMDX11Context* hw_context_ = nullptr;
  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT device_reset_token_ = 0;

  VideoFormat input_format_ = {};
  uint32_t timescale_ = 90000;
  bool initialized_ = false;

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

    if (FAILED(MFCreateMediaType(&input_type))) return false;
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, options.video_format.format == OM_FORMAT_P010 ? MFVideoFormat_P010 : MFVideoFormat_NV12);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, input_format_.width, input_format_.height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, options.format.video.framerate.num, options.format.video.framerate.den);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, map_color_primaries(options.format.video.color_primaries));
    input_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, map_transfer_characteristics(options.format.video.transfer_char));
    input_type->SetUINT32(MF_MT_YUV_MATRIX, map_color_space(options.format.video.color_space));

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
    output_type->SetUINT32(MF_MT_YUV_MATRIX, map_color_space(options.format.video.color_space));

    uint32_t bitrate = 5000000;
    if (options.rate_control.getMode() == RateControlMode::CBR) {
      bitrate = (uint32_t) std::get<CbrParams>(options.rate_control.params).bitrate.target_bitrate;
    } else if (options.rate_control.getMode() == RateControlMode::VBR) {
      bitrate = (uint32_t) std::get<VbrParams>(options.rate_control.params).bitrate.target_bitrate;
    }
    output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);

    if (FAILED(encoder_->SetOutputType(0, output_type.Get(), 0))) return false;
    if (FAILED(encoder_->SetInputType(0, input_type.Get(), 0))) return false;

    return true;
  }

  static auto map_color_primaries(OMColorPrimaries p) -> uint32_t {
    switch (p) {
      case OM_PRIMARIES_BT709: return MFVideoPrimaries_BT709;
      case OM_PRIMARIES_BT2020: return MFVideoPrimaries_BT2020;
      case OM_PRIMARIES_BT601: return MFVideoPrimaries_SMPTE170M;
      default: return MFVideoPrimaries_Unknown;
    }
  }

  static auto map_transfer_characteristics(OMTransferCharacteristic t) -> uint32_t {
    switch (t) {
      case OM_TRANSFER_BT709: return MFVideoTransFunc_709;
      case OM_TRANSFER_PQ: return 12;  // MFVideoTransFunc_2084
      case OM_TRANSFER_HLG: return 11; // MFVideoTransFunc_2020
      default: return MFVideoTransFunc_Unknown;
    }
  }

  static auto map_color_space(OMColorSpace c) -> uint32_t {
    switch (c) {
      case OM_COLOR_SPACE_BT709: return MFVideoTransferMatrix_BT709;
      case OM_COLOR_SPACE_BT2020: return MFVideoTransferMatrix_BT2020_10;
      case OM_COLOR_SPACE_BT601: return MFVideoTransferMatrix_BT601;
      default: return MFVideoTransferMatrix_Unknown;
    }
  }


public:
  DX11Encoder() {
    MFStartup(MF_VERSION);
  }

  ~DX11Encoder() override {
    release();
    MFShutdown();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (!options.hw_device || options.hw_device->type != HWDeviceType::DX11) return OM_CODEC_HWACCEL_FAILED;
    hw_context_ = static_cast<OMDX11Context*>(options.hw_device->context);

    if (!setup_device_manager()) return OM_CODEC_HWACCEL_FAILED;

    MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, codecIdToMFVideoFormat(options.format.codec_id)};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &output_info, &activates, &count)) || count == 0) {
      return OM_CODEC_NOT_FOUND;
    }
    activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);

    if (!encoder_) return OM_CODEC_OPEN_FAILED;

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(encoder_->GetAttributes(&attributes))) {
      attributes->SetUnknown(OM_MF_VIDEO_DEVICE_Manager, device_manager_.Get());
    }

    input_format_ = options.video_format;
    if (!setup_types(options)) return OM_CODEC_OPEN_FAILED;

    if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return OM_CODEC_OPEN_FAILED;
    if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return OM_CODEC_OPEN_FAILED;

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return Err(OM_CODEC_ENCODE_FAILED);

    if (!std::holds_alternative<Picture>(frame.data)) return Err(OM_CODEC_INVALID_PARAMS);
    const auto& picture = std::get<Picture>(frame.data);

    if (std::holds_alternative<std::shared_ptr<HardwarePicture>>(picture.buffer)) {
      auto hw_pic = std::get<std::shared_ptr<HardwarePicture>>(picture.buffer);
      if (hw_pic->getType() == HWDeviceType::DX11) {
        auto dx_pic = std::static_pointer_cast<DX11HardwarePicture>(hw_pic);
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), dx_pic->pic->texture, 0, FALSE, &buffer))) return Err(OM_CODEC_ENCODE_FAILED);
        sample->AddBuffer(buffer.Get());
      } else {
        return Err(OM_CODEC_NOT_SUPPORTED);
      }
    } else {
      // Host picture copy
      const auto& host_pic = std::get<HostPicture>(picture.buffer);
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(host_pic.buffer->bytes().size()), &buffer))) return Err(OM_CODEC_ENCODE_FAILED);
      BYTE* data = nullptr;
      if (SUCCEEDED(buffer->Lock(&data, nullptr, nullptr))) {
        // Copy NV12
        for (int i = 0; i < 2; ++i) {
          const uint8_t* src = picture.planes.getData(i);
          size_t src_stride = picture.planes.getLinesize(i);
          size_t height = (i == 0) ? input_format_.height : (input_format_.height + 1) / 2;
          for (size_t y = 0; y < height; ++y) {
            std::memcpy(data, src + y * src_stride, input_format_.width);
            data += input_format_.width;
          }
        }
        buffer->Unlock();
      }
      buffer->SetCurrentLength(static_cast<DWORD>(host_pic.buffer->bytes().size()));
      sample->AddBuffer(buffer.Get());
    }

    sample->SetSampleTime(frame.pts * 10000000LL / timescale_);

    if (FAILED(encoder_->ProcessInput(0, sample.Get(), 0))) return Err(OM_CODEC_ENCODE_FAILED);

    std::vector<Packet> packets;
    while (true) {
      MFT_OUTPUT_DATA_BUFFER output = {};
      output.dwStreamID = 0;
      MFT_OUTPUT_STREAM_INFO stream_info = {};
      encoder_->GetOutputStreamInfo(0, &stream_info);

      ComPtr<IMFSample> out_sample;
      if (!(stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        MFCreateSample(&out_sample);
        ComPtr<IMFMediaBuffer> out_buffer;
        MFCreateMemoryBuffer(stream_info.cbSize, &out_buffer);
        out_sample->AddBuffer(out_buffer.Get());
        output.pSample = out_sample.Get();
      }

      DWORD status = 0;
      HRESULT hr = encoder_->ProcessOutput(0, 1, &output, &status);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (FAILED(hr)) return Err(OM_CODEC_ENCODE_FAILED);

      if (output.pSample) {
        ComPtr<IMFMediaBuffer> buf;
        output.pSample->GetBufferByIndex(0, &buf);
        DWORD len = 0;
        BYTE* data = nullptr;
        buf->Lock(&data, nullptr, &len);
        Packet pkt;
        pkt.allocate(len);
        std::memcpy(pkt.bytes.data(), data, len);
        buf->Unlock();

        LONGLONG time = 0;
        output.pSample->GetSampleTime(&time);
        pkt.pts = time * timescale_ / 10000000LL;
        packets.push_back(std::move(pkt));

        if (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) output.pSample->Release();
      }
      if (output.pEvents) output.pEvents->Release();
    }

    return Ok(std::move(packets));
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    // WMF doesn't always provide extradata easily until first frame or drain
    return info;
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!encoder_) return OM_CODEC_OPEN_FAILED;

    ComPtr<IMFAttributes> attributes;
    if (FAILED(encoder_->GetAttributes(&attributes))) return OM_CODEC_OPEN_FAILED;

    uint32_t bitrate = 5000000;
    if (rc.getMode() == RateControlMode::CBR) {
      bitrate = (uint32_t) std::get<CbrParams>(rc.params).bitrate.target_bitrate;
    } else if (rc.getMode() == RateControlMode::VBR) {
      bitrate = (uint32_t) std::get<VbrParams>(rc.params).bitrate.target_bitrate;
    }

    attributes->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    return OM_SUCCESS;
  }

  void release() {
    initialized_ = false;
    if (encoder_) {
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      encoder_.Reset();
    }
    device_manager_.Reset();
  }
};

class DX11Decoder final : public Decoder {
  struct Slot {
    dx_h264::DpbEntry dpb;
    ComPtr<ID3D11VideoDecoderOutputView> view;
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
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t padded_width_ = 0;
  uint32_t padded_height_ = 0;
  uint32_t dpb_slot_count_ = 17;
  uint32_t next_slot_ = 0;
  uint32_t next_ref_ = 0;
  uint32_t feedback_ = 1;
  std::vector<uint8_t> reference_usage_;

public:
  ~DX11Decoder() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    release();
    codec_id_ = options.format.codec_id;
    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265) return OM_CODEC_NOT_SUPPORTED;
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
          dpb_slot_count_ = std::clamp<uint32_t>(h264_.sps[i].num_ref_frames + 1, 2, 17);
          bit_depth = static_cast<uint8_t>(h264_.sps[i].bit_depth_luma_minus8 + 8);
          break;
        }
      }
    } else {
      h265_ = std::make_unique<video_parser::H265AccessUnitParser>();
      h265_->parseExtradata(options.extradata);
      padded_width_ = dx_h264::alignUp(width_, 32u);
      padded_height_ = dx_h264::alignUp(height_, 32u);
      if (h265_->hasSps()) {
        const auto& s = h265_->sps(0); // Use first SPS
        padded_width_ = dx_h264::alignUp(static_cast<uint32_t>(s.pic_width_in_luma_samples), 32u);
        padded_height_ = dx_h264::alignUp(static_cast<uint32_t>(s.pic_height_in_luma_samples), 32u);
        dpb_slot_count_ = 16; // HEVC usually needs up to 16
        bit_depth = static_cast<uint8_t>(s.bit_depth_luma_minus8 + 8);
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

    GUID target_profile = (codec_id_ == OM_CODEC_H264) ? D3D11_DECODER_PROFILE_H264_VLD_NOFGT : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
    bool profile_supported = false;
    const UINT profile_count = video_device_->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < profile_count; ++i) {
      GUID profile = {};
      if (SUCCEEDED(video_device_->GetVideoDecoderProfile(i, &profile)) && profile == target_profile) {
        profile_supported = true;
        break;
      }
    }
    if (!profile_supported && codec_id_ == OM_CODEC_H265) {
      target_profile = D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10;
      for (UINT i = 0; i < profile_count; ++i) {
        GUID profile = {};
        if (SUCCEEDED(video_device_->GetVideoDecoderProfile(i, &profile)) && profile == target_profile) {
          profile_supported = true;
          break;
        }
      }
    }
    if (!profile_supported) return OM_CODEC_NOT_SUPPORTED;

    D3D11_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.Guid = target_profile;
    decoder_desc.SampleWidth = padded_width_;
    decoder_desc.SampleHeight = padded_height_;
    decoder_desc.OutputFormat = (bit_depth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    UINT config_count = 0;
    if (FAILED(video_device_->GetVideoDecoderConfigCount(&decoder_desc, &config_count)) || config_count == 0) return OM_CODEC_HWACCEL_FAILED;
    bool found_config = false;
    for (UINT i = 0; i < config_count; ++i) {
      D3D11_VIDEO_DECODER_CONFIG config = {};
      if (FAILED(video_device_->GetVideoDecoderConfig(&decoder_desc, i, &config))) continue;
      if (config.guidConfigBitstreamEncryption == DXVA_NO_ENCRYPT &&
          config.guidConfigMBcontrolEncryption == DXVA_NO_ENCRYPT &&
          config.guidConfigResidDiffEncryption == DXVA_NO_ENCRYPT &&
          config.ConfigBitstreamRaw == 2) {
        decoder_config_ = config;
        found_config = true;
        break;
      }
      if (!found_config) {
        decoder_config_ = config;
        found_config = true;
      }
    }
    if (!found_config) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(video_device_->CreateVideoDecoder(&decoder_desc, &decoder_config_, &decoder_))) return OM_CODEC_HWACCEL_FAILED;

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = padded_width_;
    texture_desc.Height = padded_height_;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = dpb_slot_count_;
    texture_desc.Format = decoder_desc.OutputFormat;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&texture_desc, nullptr, &dpb_texture_))) return OM_CODEC_HWACCEL_FAILED;

    slots_.resize(dpb_slot_count_);
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view_desc = {};
      view_desc.DecodeProfile = target_profile;
      view_desc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      view_desc.Texture2D.ArraySlice = i;
      if (FAILED(video_device_->CreateVideoDecoderOutputView(dpb_texture_.Get(), &view_desc, &slots_[i].view))) return OM_CODEC_HWACCEL_FAILED;
    }

    output_format_ = {static_cast<OMPixelFormat>(texture_desc.Format == DXGI_FORMAT_P010 ? OM_FORMAT_P010 : OM_FORMAT_NV12), width_, height_};
    if (h264_.has_sps) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_.sps_valid[i]) continue;
        const auto& s = h264_.sps[i];
        if (s.vui_parameters_present_flag && s.vui.colour_description_present_flag) {
          output_format_.color_primaries = (OMColorPrimaries) s.vui.colour_primaries;
          output_format_.transfer_char = (OMTransferCharacteristic) s.vui.transfer_characteristics;
          output_format_.color_space = (OMColorSpace) s.vui.matrix_coefficients;
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
    if (packet.bytes.empty()) return Ok(std::vector<Frame> {});

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

    if (parsed.is_intra) {
      for (auto& slot : slots_) slot.dpb.is_reference = false;
      reference_usage_.clear();
      next_ref_ = 0;
      next_slot_ = 0;
      h264_.resetPoc();
      parsed.poc = h264_.computePoc(parsed.slice);
    }

    const uint32_t current_slot = next_slot_;
    DXVA_PicParams_H264 pic_params = {};
    std::vector<dx_h264::DpbEntry> dpb;
    dpb.reserve(slots_.size());
    for (const auto& slot : slots_) dpb.push_back(slot.dpb);
    dx_h264::fillPicParams(sps, pps, parsed.slice, parsed, current_slot, reference_usage_, dpb, feedback_++, pic_params);

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

    UINT buffer_size = 0;
    void* buffer = nullptr;
    hr = video_context_->GetDecoderBuffer(decoder_.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &buffer_size, &buffer);
    if (FAILED(hr) || parsed.bitstream.size() > buffer_size) return Err(OM_CODEC_DECODE_FAILED);
    std::memcpy(buffer, parsed.bitstream.data(), parsed.bitstream.size());
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
    descs[0].DataSize = static_cast<UINT>(parsed.bitstream.size());
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

    slots_[current_slot].dpb.poc = parsed.poc;
    slots_[current_slot].dpb.frame_num = static_cast<uint32_t>(parsed.slice.frame_num);
    slots_[current_slot].dpb.is_reference = parsed.is_reference;
    if (parsed.is_reference && dpb_slot_count_ > 1) {
      if (next_ref_ >= reference_usage_.size()) reference_usage_.resize(next_ref_ + 1);
      reference_usage_[next_ref_] = static_cast<uint8_t>(current_slot);
      next_ref_ = (next_ref_ + 1) % (dpb_slot_count_ - 1);
      next_slot_ = (next_slot_ + 1) % dpb_slot_count_;
    }

    Frame frame = {};
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(*picture);
    return Ok(std::vector<Frame> {std::move(frame)});
  }

  void flush() override {
    for (auto& slot : slots_) slot.dpb = {};
    reference_usage_.clear();
    next_slot_ = 0;
    next_ref_ = 0;
    h264_.resetPoc();
  }

private:
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

const CodecDescriptor CODEC_DX11_ENC_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_h264_enc",
    .long_name = "DirectX11 H.264 Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<DX11Encoder>(); },
};

} // namespace openmedia
