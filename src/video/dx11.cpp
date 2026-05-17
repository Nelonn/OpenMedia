#include <openmedia/hw_dx11.h>
#include <openmedia/video.hpp>

#include <algorithm>
#include <codecs.hpp>
#include <cstdint>
#include <cstring>
#include <d3d11_3.h>
#include <dxva.h>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "dx_h264.hpp"

namespace openmedia {

using Microsoft::WRL::ComPtr;

static constexpr GUID DXVA_NO_ENCRYPT = {0x1b81bed0, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};

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
    if (options.format.codec_id != OM_CODEC_H264) return OM_CODEC_NOT_SUPPORTED;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;

    h264_.parseExtradata(options.extradata);
    padded_width_ = dx_h264::alignUp(width_, 16u);
    padded_height_ = dx_h264::alignUp(height_, 16u);
    if (h264_.has_sps) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_.sps_valid[i]) continue;
        padded_width_ = static_cast<uint32_t>((h264_.sps[i].pic_width_in_mbs_minus1 + 1) * 16);
        padded_height_ = static_cast<uint32_t>((h264_.sps[i].pic_height_in_map_units_minus1 + 1) * 16);
        dpb_slot_count_ = std::clamp<uint32_t>(h264_.sps[i].num_ref_frames + 1, 2, 17);
        break;
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

    bool profile_supported = false;
    const UINT profile_count = video_device_->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < profile_count; ++i) {
      GUID profile = {};
      if (SUCCEEDED(video_device_->GetVideoDecoderProfile(i, &profile)) && profile == D3D11_DECODER_PROFILE_H264_VLD_NOFGT) {
        profile_supported = true;
        break;
      }
    }
    if (!profile_supported) return OM_CODEC_NOT_SUPPORTED;

    D3D11_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.Guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    decoder_desc.SampleWidth = padded_width_;
    decoder_desc.SampleHeight = padded_height_;
    decoder_desc.OutputFormat = DXGI_FORMAT_NV12;

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
    texture_desc.Format = DXGI_FORMAT_NV12;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&texture_desc, nullptr, &dpb_texture_))) return OM_CODEC_HWACCEL_FAILED;

    slots_.resize(dpb_slot_count_);
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view_desc = {};
      view_desc.DecodeProfile = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
      view_desc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      view_desc.Texture2D.ArraySlice = i;
      if (FAILED(video_device_->CreateVideoDecoderOutputView(dpb_texture_.Get(), &view_desc, &slots_[i].view))) return OM_CODEC_HWACCEL_FAILED;
    }

    output_format_ = {OM_FORMAT_NV12, width_, height_};
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
    if (packet.bytes.empty()) return Ok(std::vector<Frame>{});

    auto parsed = h264_.parseFrame(packet.bytes);
    if (parsed.slice_offsets.empty()) return Ok(std::vector<Frame>{});
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
    return Ok(std::vector<Frame>{std::move(frame)});
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

    Picture pic(OM_FORMAT_NV12, width_, height_);
    const auto y_stride = pic.planes.getLinesize(0);
    const auto uv_stride = pic.planes.getLinesize(1);
    auto* y = pic.planes.getData(0);
    auto* uv = pic.planes.getData(1);
    const auto* src_y = static_cast<const uint8_t*>(map.pData);
    const auto* src_uv = src_y + static_cast<size_t>(map.RowPitch) * padded_height_;
    for (uint32_t row = 0; row < height_; ++row) std::memcpy(y + static_cast<size_t>(row) * y_stride, src_y + static_cast<size_t>(row) * map.RowPitch, width_);
    for (uint32_t row = 0; row < (height_ + 1) / 2; ++row) std::memcpy(uv + static_cast<size_t>(row) * uv_stride, src_uv + static_cast<size_t>(row) * map.RowPitch, width_);
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
    .caps = CodecCaps{.profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH}},
    .decoder_factory = [] { return std::make_unique<DX11Decoder>(); },
};

} // namespace openmedia
