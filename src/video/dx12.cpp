#include <openmedia/hw_dx12.h>
#include <openmedia/video.hpp>

#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
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
#include "dx_h265.hpp"
#include "hw_common.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

// EB533D05-D234-4530-9162-801691238C93
static constexpr GUID OM_MF_VIDEO_DEVICE_Manager = {0xeb533d05, 0xd234, 0x4530, {0x91, 0x62, 0x80, 0x16, 0x91, 0x23, 0x8c, 0x93}};

using Microsoft::WRL::ComPtr;

namespace openmedia {

class DX12Decoder final : public Decoder {
  static constexpr uint64_t BITSTREAM_SIZE = 8ull * 1024ull * 1024ull;

  struct ReorderEntry {
    int32_t poc = 0;
    Frame frame = {};
  };

  bool initialized_ = false;
  VideoFormat output_format_ = {};

  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12VideoDevice> video_device_;
  ComPtr<ID3D12CommandQueue> video_queue_;
  ComPtr<ID3D12CommandQueue> copy_queue_;
  ComPtr<ID3D12CommandAllocator> video_allocator_;
  ComPtr<ID3D12VideoDecodeCommandList> video_cmd_;
  ComPtr<ID3D12CommandAllocator> copy_allocator_;
  ComPtr<ID3D12GraphicsCommandList> copy_cmd_;
  ComPtr<ID3D12Fence> video_fence_;
  ComPtr<ID3D12Fence> copy_fence_;
  HANDLE video_event_ = nullptr;
  HANDLE copy_event_ = nullptr;
  uint64_t video_fence_value_ = 0;
  uint64_t copy_fence_value_ = 0;

  ComPtr<ID3D12VideoDecoder> decoder_;
  ComPtr<ID3D12VideoDecoderHeap> decoder_heap_;
  ComPtr<ID3D12Resource> bitstream_buffer_;
  uint8_t* bitstream_ptr_ = nullptr;
  ComPtr<ID3D12Resource> dpb_texture_;
  ComPtr<ID3D12Resource> output_texture_;
  bool reference_only_ = false;

  dx_h264::State h264_;
  std::unique_ptr<video_parser::H265AccessUnitParser> h265_;
  dx_h265::PocState h265_poc_;
  std::vector<dx_h264::DpbEntry> dpb_;
  D3D12_RESOURCE_STATES dpb_states_[17] = {};
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
  std::vector<ReorderEntry> h264_reorder_queue_;

public:
  ~DX12Decoder() override { release(); }

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
        for (int i = 0; i < 16; ++i) {
          const auto& s = h265_->sps(i);
          if (!s.valid) continue;
          padded_width_ = dx_h264::alignUp(static_cast<uint32_t>(s.pic_width_in_luma_samples), 32u);
          padded_height_ = dx_h264::alignUp(static_cast<uint32_t>(s.pic_height_in_luma_samples), 32u);
          dpb_slot_count_ = std::clamp<uint32_t>(static_cast<uint32_t>(s.sps_max_dec_pic_buffering_minus1[s.max_sub_layers_minus1] + 1), 2, 17);
          bit_depth = static_cast<uint8_t>(s.bit_depth_luma_minus8 + 8);
          break;
        }
      }
    }

    if (options.hw_device && options.hw_device->type == HWDeviceType::DX12 && options.hw_device->context) {
      auto* ctx = static_cast<OMDX12Context*>(options.hw_device->context);
      device_ = HWD3D12Context_getDevice(ctx);
      video_device_ = HWD3D12Context_getVideoDevice(ctx);
    } else {
      if (FAILED(createDevice())) return OM_CODEC_HWACCEL_FAILED;
    }
    if (!device_ || !video_device_) return OM_CODEC_HWACCEL_FAILED;

    if (FAILED(createQueuesAndCommands())) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(createDecoder())) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(createResources())) return OM_CODEC_HWACCEL_FAILED;

    output_format_ = {static_cast<OMPixelFormat>(bit_depth > 8 ? OM_FORMAT_P010 : OM_FORMAT_NV12), width_, height_};
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
    if (packet.bytes.empty()) {
      if (codec_id_ == OM_CODEC_H264) return Ok(drainH264Reordered());
      return Ok(std::vector<Frame> {});
    }

    if (codec_id_ == OM_CODEC_H264) return decodeH264(packet);
    if (codec_id_ == OM_CODEC_H265) return decodeH265(packet);
    return Err(OM_CODEC_NOT_SUPPORTED);
  }

  void flush() override {
    resetReceiveState();
    for (auto& entry : dpb_) entry = {};
    reference_usage_.clear();
    h264_reorder_queue_.clear();
    next_slot_ = 0;
    next_ref_ = 0;
    h264_.resetPoc();
    h265_poc_.reset();
  }

private:
  static auto h264ReorderDepth(const h264::SPS& sps) -> size_t {
    if (sps.vui_parameters_present_flag && sps.vui.bitstream_restriction_flag) {
      return static_cast<size_t>(std::clamp(sps.vui.num_reorder_frames, 0, 16));
    }
    return 0;
  }

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
    if (parsed.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_DECODE_FAILED);
    if (parsed.slice.pic_parameter_set_id < 0 || parsed.slice.pic_parameter_set_id >= 256 || !h264_.pps_valid[parsed.slice.pic_parameter_set_id]) return Err(OM_CODEC_DECODE_FAILED);
    const auto& pps = h264_.pps[parsed.slice.pic_parameter_set_id];
    if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32 || !h264_.sps_valid[pps.seq_parameter_set_id]) return Err(OM_CODEC_DECODE_FAILED);
    const auto& sps = h264_.sps[pps.seq_parameter_set_id];
    if (sps.bit_depth_luma_minus8 != 0 || sps.bit_depth_chroma_minus8 != 0 || pps.num_slice_groups_minus1 != 0) return Err(OM_CODEC_NOT_SUPPORTED);

    std::vector<Frame> pre_output;
    if (parsed.is_intra) {
      pre_output = drainH264Reordered();
      for (auto& entry : dpb_) entry.is_reference = false;
      reference_usage_.clear();
      next_ref_ = 0;
      next_slot_ = 0;
      h264_.resetPoc();
      parsed.poc = h264_.computePoc(parsed.slice);
    }

    std::memcpy(bitstream_ptr_, parsed.bitstream.data(), parsed.bitstream.size());
    const uint32_t current_slot = next_slot_;

    HRESULT hr = recordDecode(parsed, sps, pps, current_slot);
    if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

    auto picture = download(current_slot);
    if (!picture.has_value()) return Err(OM_CODEC_DECODE_FAILED);

    dpb_[current_slot].poc = parsed.poc;
    dpb_[current_slot].frame_num = static_cast<uint32_t>(parsed.slice.frame_num);
    dpb_[current_slot].is_reference = parsed.is_reference;
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
    auto output = pushH264Reordered(std::move(frame), parsed.poc, h264ReorderDepth(sps));
    if (!pre_output.empty()) {
      pre_output.insert(pre_output.end(), std::make_move_iterator(output.begin()), std::make_move_iterator(output.end()));
      return Ok(std::move(pre_output));
    }
    return Ok(std::move(output));
  }

  auto decodeH265(const Packet& packet) -> Result<std::vector<Frame>, OMError> {
    if (!h265_) return Err(OM_CODEC_DECODE_FAILED);
    auto frames = h265_->parse(packet.bytes, true);
    if (frames.empty()) return Ok(std::vector<Frame> {});

    std::vector<Frame> output;
    output.reserve(frames.size());
    for (auto& parsed : frames) {
      if (parsed.slice_offsets.empty() || parsed.slice_headers.empty()) continue;
      const auto slice_data = dx_h265::appendBitstreamAndSliceDataWithStartCode(parsed);
      if (slice_data.bitstream.empty() || slice_data.slices.empty() || slice_data.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_DECODE_FAILED);
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
        dpb_slot_count_ = std::clamp<uint32_t>(static_cast<uint32_t>(sps.sps_max_dec_pic_buffering_minus1[sps.max_sub_layers_minus1] + 1), 2, 17);
        for (auto& entry : dpb_) entry = {};
        reference_usage_.clear();
        next_ref_ = 0;
        next_slot_ = 0;
        h265_poc_.reset();
        if (FAILED(createDecoder())) return Err(OM_CODEC_HWACCEL_FAILED);
        if (FAILED(createResources())) return Err(OM_CODEC_HWACCEL_FAILED);
        output_format_.format = expected_format;
      }

      const int32_t poc = h265_poc_.compute(sps, parsed, sh);
      if (dx_h265::isIrap(parsed.nal_unit_type)) {
        for (auto& entry : dpb_) entry.is_reference = false;
        reference_usage_.clear();
        next_ref_ = 0;
        next_slot_ = 0;
      }

      std::memcpy(bitstream_ptr_, slice_data.bitstream.data(), slice_data.bitstream.size());
      const uint32_t current_slot = next_slot_;
      HRESULT hr = recordDecodeH265(parsed, slice_data, sps, pps, sh, poc, current_slot);
      if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

      auto picture = download(current_slot);
      if (!picture.has_value()) return Err(OM_CODEC_DECODE_FAILED);

      dpb_[current_slot].poc = poc;
      dpb_[current_slot].frame_num = static_cast<uint32_t>(poc);
      dpb_[current_slot].is_reference = parsed.is_reference;
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

  auto createDevice() -> HRESULT {
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc = {};
      adapter->GetDesc1(&desc);
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
      if (SUCCEEDED(hr) && SUCCEEDED(device_.As(&video_device_))) return S_OK;
      device_.Reset();
      video_device_.Reset();
    }
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (SUCCEEDED(hr)) hr = device_.As(&video_device_);
    return hr;
  }

  auto createQueuesAndCommands() -> HRESULT {
    D3D12_COMMAND_QUEUE_DESC q = {};
    q.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
    HRESULT hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&video_queue_));
    if (FAILED(hr)) return hr;
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&copy_queue_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, IID_PPV_ARGS(&video_allocator_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, video_allocator_.Get(), nullptr, IID_PPV_ARGS(&video_cmd_));
    if (FAILED(hr)) return hr;
    video_cmd_->Close();
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy_allocator_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_allocator_.Get(), nullptr, IID_PPV_ARGS(&copy_cmd_));
    if (FAILED(hr)) return hr;
    copy_cmd_->Close();
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&video_fence_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_));
    if (FAILED(hr)) return hr;
    video_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    copy_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return (video_event_ && copy_event_) ? S_OK : E_FAIL;
  }

  auto createDecoder() -> HRESULT {
    decoder_.Reset();
    decoder_heap_.Reset();

    D3D12_VIDEO_DECODE_CONFIGURATION config = {};
    config.DecodeProfile = (codec_id_ == OM_CODEC_H264) ? D3D12_VIDEO_DECODE_PROFILE_H264 : D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
    config.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;

    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {};
    uint8_t bit_depth = 8;
    if (codec_id_ == OM_CODEC_H264) {
      if (h264_.has_sps) {
        for (uint32_t i = 0; i < 32; ++i) {
          if (!h264_.sps_valid[i]) continue;
          bit_depth = static_cast<uint8_t>(h264_.sps[i].bit_depth_luma_minus8 + 8);
          break;
        }
      }
    } else {
      if (h265_ && h265_->hasSps()) {
        for (int i = 0; i < 16; ++i) {
          const auto& s = h265_->sps(i);
          if (!s.valid) continue;
          bit_depth = static_cast<uint8_t>(s.bit_depth_luma_minus8 + 8);
          if (bit_depth > 8) config.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10;
          break;
        }
      }
    }
    support.Configuration = config;
    support.DecodeFormat = (bit_depth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    support.Width = padded_width_;
    support.Height = padded_height_;
    support.FrameRate = {0, 1};
    HRESULT hr = video_device_->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof(support));
    if (FAILED(hr) || support.DecodeTier < D3D12_VIDEO_DECODE_TIER_1) return FAILED(hr) ? hr : E_FAIL;
    reference_only_ = (support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_REFERENCE_ONLY_ALLOCATIONS_REQUIRED) != 0;

    D3D12_VIDEO_DECODER_HEAP_DESC heap_desc = {};
    heap_desc.Configuration = config;
    heap_desc.DecodeWidth = padded_width_;
    heap_desc.DecodeHeight = padded_height_;
    heap_desc.Format = support.DecodeFormat;
    heap_desc.FrameRate = {0, 1};
    heap_desc.MaxDecodePictureBufferCount = dpb_slot_count_;
    if (support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED) {
      heap_desc.DecodeWidth = dx_h264::alignUp(padded_width_, 32u);
      heap_desc.DecodeHeight = dx_h264::alignUp(padded_height_, 32u);
    }
    hr = video_device_->CreateVideoDecoderHeap(&heap_desc, IID_PPV_ARGS(&decoder_heap_));
    if (FAILED(hr)) return hr;

    D3D12_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.Configuration = config;
    return video_device_->CreateVideoDecoder(&decoder_desc, IID_PPV_ARGS(&decoder_));
  }

  auto createResources() -> HRESULT {
    if (bitstream_buffer_ && bitstream_ptr_) bitstream_buffer_->Unmap(0, nullptr);
    bitstream_ptr_ = nullptr;
    output_texture_.Reset();
    dpb_texture_.Reset();
    bitstream_buffer_.Reset();

    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Width = BITSTREAM_SIZE;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    HRESULT hr = device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&bitstream_buffer_));
    if (FAILED(hr)) return hr;
    hr = bitstream_buffer_->Map(0, nullptr, reinterpret_cast<void**>(&bitstream_ptr_));
    if (FAILED(hr)) return hr;

    uint8_t bit_depth = 8;
    if (codec_id_ == OM_CODEC_H264) {
      if (h264_.has_sps) {
        for (uint32_t i = 0; i < 32; ++i) {
          if (!h264_.sps_valid[i]) continue;
          bit_depth = static_cast<uint8_t>(h264_.sps[i].bit_depth_luma_minus8 + 8);
          break;
        }
      }
    } else {
      if (h265_ && h265_->hasSps()) {
        for (int i = 0; i < 16; ++i) {
          if (!h265_->sps(i).valid) continue;
          bit_depth = static_cast<uint8_t>(h265_->sps(i).bit_depth_luma_minus8 + 8);
          break;
        }
      }
    }
    const DXGI_FORMAT fmt = (bit_depth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    D3D12_RESOURCE_DESC tex = {};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Format = fmt;
    tex.Width = padded_width_;
    tex.Height = padded_height_;
    tex.DepthOrArraySize = static_cast<UINT16>(dpb_slot_count_);
    tex.MipLevels = 1;
    tex.SampleDesc.Count = 1;
    tex.Flags = reference_only_ ? (D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
                                : D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&dpb_texture_));
    if (FAILED(hr)) return hr;

    if (reference_only_) {
      tex.DepthOrArraySize = 1;
      tex.Flags = D3D12_RESOURCE_FLAG_NONE;
      hr = device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output_texture_));
      if (FAILED(hr)) return hr;
    }

    dpb_.resize(dpb_slot_count_);
    std::fill(std::begin(dpb_states_), std::end(dpb_states_), D3D12_RESOURCE_STATE_COMMON);
    return S_OK;
  }

  auto recordDecode(const dx_h264::ParsedFrame& parsed, const h264::SPS& sps, const h264::PPS& pps, uint32_t current_slot) -> HRESULT {
    HRESULT hr = video_allocator_->Reset();
    if (FAILED(hr)) return hr;
    hr = video_cmd_->Reset(video_allocator_.Get());
    if (FAILED(hr)) return hr;

    if (dpb_states_[current_slot] != D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) {
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dpb_texture_.Get();
        barrier.Transition.StateBefore = dpb_states_[current_slot];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      }
      barriers[0].Transition.Subresource = current_slot;
      barriers[1].Transition.Subresource = dpb_slot_count_ + current_slot;
      video_cmd_->ResourceBarrier(2, barriers);
      dpb_states_[current_slot] = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
    }
    if (reference_only_) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = output_texture_.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      video_cmd_->ResourceBarrier(1, &barrier);
    }

    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS input = {};
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS output = {};
    if (reference_only_) {
      output.pOutputTexture2D = output_texture_.Get();
      output.OutputSubresource = 0;
      output.ConversionArguments.Enable = TRUE;
      output.ConversionArguments.pReferenceTexture2D = dpb_texture_.Get();
      output.ConversionArguments.ReferenceSubresource = current_slot;
    } else {
      output.pOutputTexture2D = dpb_texture_.Get();
      output.OutputSubresource = current_slot;
    }

    ID3D12Resource* reference_frames[17] = {};
    UINT reference_subresources[17] = {};
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      reference_frames[i] = dpb_texture_.Get();
      reference_subresources[i] = i;
    }
    input.ReferenceFrames.NumTexture2Ds = dpb_slot_count_;
    input.ReferenceFrames.ppTexture2Ds = reference_frames;
    input.ReferenceFrames.pSubresources = reference_subresources;
    input.CompressedBitstream.pBuffer = bitstream_buffer_.Get();
    input.CompressedBitstream.Offset = 0;
    input.CompressedBitstream.Size = parsed.bitstream.size();
    input.pHeap = decoder_heap_.Get();

    DXVA_PicParams_H264 pic = {};
    dx_h264::fillPicParams(sps, pps, parsed.slice, parsed, current_slot, reference_usage_, dpb_, feedback_++, pic);
    DXVA_Qmatrix_H264 qmatrix = {};
    dx_h264::fillQMatrix(sps, pps, qmatrix);
    std::vector<DXVA_Slice_H264_Short> slices(parsed.slice_offsets.size());
    for (size_t i = 0; i < parsed.slice_offsets.size(); ++i) {
      const uint32_t begin = parsed.slice_offsets[i];
      const uint32_t end = (i + 1 < parsed.slice_offsets.size()) ? parsed.slice_offsets[i + 1] : static_cast<uint32_t>(parsed.bitstream.size());
      slices[i].BSNALunitDataLocation = begin;
      slices[i].SliceBytesInBuffer = end - begin;
    }
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof(pic), &pic};
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, sizeof(qmatrix), &qmatrix};
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, static_cast<UINT>(slices.size() * sizeof(DXVA_Slice_H264_Short)), slices.data()};

    video_cmd_->DecodeFrame(decoder_.Get(), &output, &input);

    if (reference_only_) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = output_texture_.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      video_cmd_->ResourceBarrier(1, &barrier);
    } else {
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dpb_texture_.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      }
      barriers[0].Transition.Subresource = current_slot;
      barriers[1].Transition.Subresource = dpb_slot_count_ + current_slot;
      video_cmd_->ResourceBarrier(2, barriers);
      dpb_states_[current_slot] = D3D12_RESOURCE_STATE_COMMON;
    }

    hr = video_cmd_->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[] = {video_cmd_.Get()};
    video_queue_->ExecuteCommandLists(1, lists);
    return signal(video_queue_.Get(), video_fence_.Get(), video_fence_value_, video_event_);
  }

  auto recordDecodeH265(const dx_h265::ParsedFrame& parsed,
                        const dx_h265::SliceData& slice_data,
                        const dx_h265::Sps& sps,
                        const dx_h265::Pps& pps,
                        const dx_h265::SliceHeader& sh,
                        int32_t poc,
                        uint32_t current_slot) -> HRESULT {
    HRESULT hr = video_allocator_->Reset();
    if (FAILED(hr)) return hr;
    hr = video_cmd_->Reset(video_allocator_.Get());
    if (FAILED(hr)) return hr;

    if (dpb_states_[current_slot] != D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) {
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dpb_texture_.Get();
        barrier.Transition.StateBefore = dpb_states_[current_slot];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      }
      barriers[0].Transition.Subresource = current_slot;
      barriers[1].Transition.Subresource = dpb_slot_count_ + current_slot;
      video_cmd_->ResourceBarrier(2, barriers);
      dpb_states_[current_slot] = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
    }
    if (reference_only_) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = output_texture_.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      video_cmd_->ResourceBarrier(1, &barrier);
    }

    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS input = {};
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS output = {};
    if (reference_only_) {
      output.pOutputTexture2D = output_texture_.Get();
      output.OutputSubresource = 0;
      output.ConversionArguments.Enable = TRUE;
      output.ConversionArguments.pReferenceTexture2D = dpb_texture_.Get();
      output.ConversionArguments.ReferenceSubresource = current_slot;
    } else {
      output.pOutputTexture2D = dpb_texture_.Get();
      output.OutputSubresource = current_slot;
    }

    ID3D12Resource* reference_frames[17] = {};
    UINT reference_subresources[17] = {};
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      reference_frames[i] = dpb_texture_.Get();
      reference_subresources[i] = i;
    }
    input.ReferenceFrames.NumTexture2Ds = dpb_slot_count_;
    input.ReferenceFrames.ppTexture2Ds = reference_frames;
    input.ReferenceFrames.pSubresources = reference_subresources;
    input.CompressedBitstream.pBuffer = bitstream_buffer_.Get();
    input.CompressedBitstream.Offset = 0;
    input.CompressedBitstream.Size = slice_data.bitstream.size();
    input.pHeap = decoder_heap_.Get();

    DXVA_PicParams_HEVC pic = {};
    dx_h265::fillPicParams(sps, pps, sh, parsed, poc, current_slot, reference_usage_, dpb_, feedback_++, pic);
    DXVA_Qmatrix_HEVC qmatrix = {};
    if (sps.scaling_list_enabled_flag) dx_h265::fillQMatrix(sps, pps, qmatrix);
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof(pic), &pic};
    if (sps.scaling_list_enabled_flag) {
      input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, sizeof(qmatrix), &qmatrix};
    }
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, static_cast<UINT>(slice_data.slices.size() * sizeof(DXVA_Slice_HEVC_Short)), const_cast<DXVA_Slice_HEVC_Short*>(slice_data.slices.data())};

    video_cmd_->DecodeFrame(decoder_.Get(), &output, &input);

    if (reference_only_) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = output_texture_.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      video_cmd_->ResourceBarrier(1, &barrier);
    } else {
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dpb_texture_.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      }
      barriers[0].Transition.Subresource = current_slot;
      barriers[1].Transition.Subresource = dpb_slot_count_ + current_slot;
      video_cmd_->ResourceBarrier(2, barriers);
      dpb_states_[current_slot] = D3D12_RESOURCE_STATE_COMMON;
    }

    hr = video_cmd_->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[] = {video_cmd_.Get()};
    video_queue_->ExecuteCommandLists(1, lists);
    return signal(video_queue_.Get(), video_fence_.Get(), video_fence_value_, video_event_);
  }

  auto download(uint32_t current_slot) -> std::optional<Picture> {
    if (FAILED(wait(video_fence_.Get(), video_fence_value_, video_event_))) return std::nullopt;

    ID3D12Resource* src = reference_only_ ? output_texture_.Get() : dpb_texture_.Get();
    const UINT src_subresource_y = reference_only_ ? 0 : current_slot;
    const UINT src_subresource_uv = reference_only_ ? 1 : dpb_slot_count_ + current_slot;

    D3D12_RESOURCE_DESC single = src->GetDesc();
    single.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT rows[2] = {};
    UINT64 row_sizes[2] = {};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&single, 0, 2, 0, footprints, rows, row_sizes, &total);

    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readback_desc.Width = total;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return std::nullopt;

    if (FAILED(copy_allocator_->Reset())) return std::nullopt;
    if (FAILED(copy_cmd_->Reset(copy_allocator_.Get(), nullptr))) return std::nullopt;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = src;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copy_cmd_->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.pResource = readback.Get();
    D3D12_TEXTURE_COPY_LOCATION loc_src = {};
    loc_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    loc_src.pResource = src;
    dst.PlacedFootprint = footprints[0];
    loc_src.SubresourceIndex = src_subresource_y;
    copy_cmd_->CopyTextureRegion(&dst, 0, 0, 0, &loc_src, nullptr);
    dst.PlacedFootprint = footprints[1];
    loc_src.SubresourceIndex = src_subresource_uv;
    copy_cmd_->CopyTextureRegion(&dst, 0, 0, 0, &loc_src, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    copy_cmd_->ResourceBarrier(1, &barrier);
    if (FAILED(copy_cmd_->Close())) return std::nullopt;
    ID3D12CommandList* lists[] = {copy_cmd_.Get()};
    copy_queue_->ExecuteCommandLists(1, lists);
    if (FAILED(signal(copy_queue_.Get(), copy_fence_.Get(), copy_fence_value_, copy_event_))) return std::nullopt;
    if (FAILED(wait(copy_fence_.Get(), copy_fence_value_, copy_event_))) return std::nullopt;

    uint8_t* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return std::nullopt;
    const OMPixelFormat om_fmt = (src->GetDesc().Format == DXGI_FORMAT_P010 ? OM_FORMAT_P010 : OM_FORMAT_NV12);
    Picture pic(om_fmt, width_, height_);
    const auto y_stride = pic.planes.getLinesize(0);
    const auto uv_stride = pic.planes.getLinesize(1);
    auto* y = pic.planes.getData(0);
    auto* uv = pic.planes.getData(1);
    const uint8_t* src_y = data + footprints[0].Offset;
    const uint8_t* src_uv = data + footprints[1].Offset;
    const size_t bpp = getBytesPerPixel(om_fmt, 0);
    for (uint32_t row = 0; row < height_; ++row) std::memcpy(y + static_cast<size_t>(row) * y_stride, src_y + static_cast<size_t>(row) * footprints[0].Footprint.RowPitch, width_ * bpp);
    for (uint32_t row = 0; row < (height_ + 1) / 2; ++row) std::memcpy(uv + static_cast<size_t>(row) * uv_stride, src_uv + static_cast<size_t>(row) * footprints[1].Footprint.RowPitch, width_ * bpp);
    readback->Unmap(0, nullptr);
    return pic;
  }

  static auto signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t& value, HANDLE) -> HRESULT {
    ++value;
    return queue->Signal(fence, value);
  }

  static auto wait(ID3D12Fence* fence, uint64_t value, HANDLE event) -> HRESULT {
    if (fence->GetCompletedValue() >= value) return S_OK;
    HRESULT hr = fence->SetEventOnCompletion(value, event);
    if (FAILED(hr)) return hr;
    return WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
  }

  void release() {
    initialized_ = false;
    if (bitstream_buffer_ && bitstream_ptr_) bitstream_buffer_->Unmap(0, nullptr);
    bitstream_ptr_ = nullptr;
    output_texture_.Reset();
    dpb_texture_.Reset();
    bitstream_buffer_.Reset();
    decoder_heap_.Reset();
    decoder_.Reset();
    video_cmd_.Reset();
    copy_cmd_.Reset();
    video_allocator_.Reset();
    copy_allocator_.Reset();
    video_queue_.Reset();
    copy_queue_.Reset();
    video_fence_.Reset();
    copy_fence_.Reset();
    video_device_.Reset();
    device_.Reset();
    if (video_event_) CloseHandle(video_event_);
    if (copy_event_) CloseHandle(copy_event_);
    video_event_ = nullptr;
    copy_event_ = nullptr;
  }
};

class DX12Encoder final : public Encoder {
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D11Device> d3d11_device_;
  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT device_reset_token_ = 0;

  VideoFormat input_format_ = {};
  uint32_t timescale_ = 90000;
  bool initialized_ = false;

  auto setup_d3d11_interop() -> bool {
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) return false;

    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, nullptr))) return false;

    if (FAILED(MFCreateDXGIDeviceManager(&device_reset_token_, &device_manager_))) return false;
    if (FAILED(device_manager_->ResetDevice(d3d11_device_.Get(), device_reset_token_))) return false;

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
  DX12Encoder() {
    MFStartup(MF_VERSION);
  }

  ~DX12Encoder() override {
    release();
    MFShutdown();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (!options.hw_device || options.hw_device->type != HWDeviceType::DX12) return OM_CODEC_HWACCEL_FAILED;
    auto* ctx = static_cast<OMDX12Context*>(options.hw_device->context);
    device_ = HWD3D12Context_getDevice(ctx);

    if (!setup_d3d11_interop()) return OM_CODEC_HWACCEL_FAILED;

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
      if (hw_pic->getType() == HWDeviceType::DX12) {
        auto dx_pic = std::static_pointer_cast<DX12HardwarePicture>(hw_pic);

        // Zero-copy DX12 to DX11 interop
        HANDLE shared_handle = nullptr;
        if (FAILED(device_->CreateSharedHandle(dx_pic->pic->texture, nullptr, GENERIC_ALL, nullptr, &shared_handle))) return Err(OM_CODEC_ENCODE_FAILED);

        ComPtr<ID3D11Texture2D> d3d11_tex;
        HRESULT hr = d3d11_device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Texture2D), &d3d11_tex);
        CloseHandle(shared_handle);
        if (FAILED(hr)) return Err(OM_CODEC_ENCODE_FAILED);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), d3d11_tex.Get(), 0, FALSE, &buffer))) return Err(OM_CODEC_ENCODE_FAILED);
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
    d3d11_device_.Reset();
    device_.Reset();
  }
};

const CodecDescriptor CODEC_DX12_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h264",
    .long_name = "DirectX12 H.264 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h265",
    .long_name = "DirectX12 H.265/HEVC Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_vp9",
    .long_name = "DirectX12 VP9 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_av1",
    .long_name = "DirectX12 AV1 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_ENC_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h264_enc",
    .long_name = "DirectX12 H.264 Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<DX12Encoder>(); },
};

const CodecDescriptor CODEC_DX12_ENC_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h265_enc",
    .long_name = "DirectX12 H.265 Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<DX12Encoder>(); },
};

} // namespace openmedia
