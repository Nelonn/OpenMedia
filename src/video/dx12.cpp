#include <openmedia/hw_dx12.h>
#include <openmedia/video.hpp>

#include <d3d11_1.h>
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
#include <video/hdr_sei.hpp>
#include <video/decode_report.hpp>
#include <video/reorder_queue.hpp>
#include <util/color_codes.hpp>

#include "dx_h264.hpp"
#include "dx_h265.hpp"
#include "dx_surface.hpp"
#include "hw_common.hpp"

using Microsoft::WRL::ComPtr;

namespace openmedia {

class DX12Decoder final : public Decoder, private DecodeReport {
  static constexpr uint64_t BITSTREAM_SIZE = 8ull * 1024ull * 1024ull;

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
  // HEVC's surface follows the stream's chroma format and depth.
  uint8_t bit_depth_ = 8;
  int chroma_format_idc_ = 1;
  DXGI_FORMAT surface_format_ = DXGI_FORMAT_NV12;

  dx_h264::State h264_;
  std::unique_ptr<video_parser::H265AccessUnitParser> h265_;
  dx_h265::Dpb h265_dpb_;
  dx_h264::Dpb h264_dpb_;
  D3D12_RESOURCE_STATES dpb_states_[17] = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t padded_width_ = 0;
  uint32_t padded_height_ = 0;
  uint32_t dpb_slot_count_ = 17;
  uint32_t feedback_ = 1;
  // Pictures waiting for the ones that come before them in presentation order. H.264 and
  // HEVC both hold back; which of them is decoding decides only how the depth is read.
  ReorderQueue reorder_;

public:
  DX12Decoder() : DecodeReport("dx12") {}
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
          // One extra so a non-reference picture has somewhere to go that no reference occupies.
          dpb_slot_count_ = std::clamp<uint32_t>(h264_.sps[i].num_ref_frames + 2, 3, 17);
          h264_dpb_.configure(dpb_slot_count_,
                              static_cast<uint32_t>(h264_.sps[i].num_ref_frames),
                              1u << (h264_.sps[i].log2_max_frame_num_minus4 + 4));
          bit_depth = static_cast<uint8_t>(h264_.sps[i].bit_depth_luma_minus8 + 8);
          // The D3D12 decode profiles used here are 4:2:0 only; taking a 4:2:2
          // or 4:4:4 stream would silently produce a wrong picture.
          if (h264_.sps[i].chroma_format_idc != 1) return OM_CODEC_NOT_SUPPORTED;
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
          if (!dx_h265::isDecodable(s)) return OM_CODEC_NOT_SUPPORTED;
          chroma_format_idc_ = s.chroma_format_idc;
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

    bit_depth_ = bit_depth;
    if (FAILED(createQueuesAndCommands())) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(createDecoder())) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(createResources())) return OM_CODEC_HWACCEL_FAILED;

    output_format_ = {};
    output_format_.format = dx_surface::hostFormat(surface_format_);
    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.color_space = options.format.video.color_space;
    output_format_.transfer_char = options.format.video.transfer_char;
    output_format_.color_primaries = options.format.video.color_primaries;
    output_format_.color_range = OM_COLOR_RANGE_UNSPECIFIED;
    if (codec_id_ == OM_CODEC_H264 && h264_.has_sps) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_.sps_valid[i]) continue;
        const auto& s = h264_.sps[i];
        if (s.vui_parameters_present_flag && s.vui.colour_description_present_flag) {
          output_format_.color_primaries = color_codes::primariesFromCode(s.vui.colour_primaries);
          output_format_.transfer_char = color_codes::transferFromCode(s.vui.transfer_characteristics);
          output_format_.color_space = color_codes::colorSpaceFromMatrix(s.vui.matrix_coefficients);
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
          output_format_.color_primaries = color_codes::primariesFromCode(s.vui.colour_primaries);
          output_format_.transfer_char = color_codes::transferFromCode(s.vui.transfer_characteristics);
          output_format_.color_space = color_codes::colorSpaceFromMatrix(s.vui.matrix_coeffs);
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
    if (packet.bytes.empty()) return Ok(reorder_.drain());

    if (codec_id_ == OM_CODEC_H264) return decodeH264(packet);
    if (codec_id_ == OM_CODEC_H265) return decodeH265(packet);
    return Err(OM_CODEC_NOT_SUPPORTED);
  }

  void flush() override {
    resetReceiveState();
    h264_dpb_.reset();
    h265_dpb_.reset();
    reorder_.clear();
    h264_.resetPoc();
    if (h265_) h265_->restart();
  }

private:
  auto decodeH264(const Packet& packet) -> Result<std::vector<Frame>, OMError> {
    // HDR10 static metadata only exists in the bitstream for H.26x; pick it
    // out before the packet disappears into the hardware decoder.
    hdr_sei::parseAnnexB(packet.bytes, false, output_format_.mastering_display,
                         output_format_.content_light_level);

    auto parsed = h264_.parseFrame(packet.bytes);
    if (parsed.slice_offsets.empty()) return Ok(std::vector<Frame> {});
    if (parsed.bitstream.size() > BITSTREAM_SIZE) {
      return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.264 frame of {} bytes exceeds the {}-byte bitstream buffer",
                             parsed.bitstream.size(), BITSTREAM_SIZE));
    }

    const auto active = dx_h264::activeParameterSets(h264_, parsed.slice);
    if (!active) {
      const OMError error = active.status == dx_h264::ParameterSetStatus::unsupported_coding
                                ? OM_CODEC_NOT_SUPPORTED
                                : OM_CODEC_DECODE_FAILED;
      return Err(rejectFrame(error, "H.264: {}", dx_h264::describe(active.status)));
    }
    const h264::SPS& sps = *active.sps;
    const h264::PPS& pps = *active.pps;

    std::vector<Frame> pre_output;
    if (parsed.is_intra) {
      pre_output = reorder_.drain();
      dx_h264::restartAtIdr(h264_, h264_dpb_, parsed);
    }

    memcpy(bitstream_ptr_, parsed.bitstream.data(), parsed.bitstream.size());

    // A non-reference picture also must not decode over a surface the stream still refers to.
    h264_dpb_.updatePicNums(static_cast<uint32_t>(parsed.slice.frame_num));
    const uint32_t current_slot = h264_dpb_.acquireSlot();

    HRESULT hr = recordDecode(parsed, sps, pps, current_slot);
    if (FAILED(hr)) {
      return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.264 decode command failed (hr=0x{:08X})",
                             static_cast<uint32_t>(hr)));
    }

    auto picture = download(current_slot);
    if (!picture.has_value()) {
      return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.264 could not read back slot {}", current_slot));
    }

    h264_dpb_.store(current_slot, parsed.poc, parsed.slice, parsed.is_intra, parsed.is_reference);

    Frame frame = {};
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(*picture);
    auto output = reorder_.push(std::move(frame), parsed.poc, dx_h264::reorderDepth(sps));
    if (!pre_output.empty()) {
      pre_output.insert(pre_output.end(), std::make_move_iterator(output.begin()), std::make_move_iterator(output.end()));
      return Ok(std::move(pre_output));
    }
    return Ok(std::move(output));
  }

  auto decodeH265(const Packet& packet) -> Result<std::vector<Frame>, OMError> {
    // HDR10 static metadata only exists in the bitstream for H.26x; pick it
    // out before the packet disappears into the hardware decoder.
    hdr_sei::parseAnnexB(packet.bytes, true, output_format_.mastering_display,
                         output_format_.content_light_level);

    if (!h265_) return Err(OM_CODEC_DECODE_FAILED);
    auto frames = h265_->parse(packet.bytes, true);
    if (frames.empty()) return Ok(std::vector<Frame> {});

    std::vector<Frame> output;
    output.reserve(frames.size());
    for (auto& parsed : frames) {
      if (parsed.slice_offsets.empty() || parsed.slice_headers.empty()) continue;
      const auto slice_data = dx_h265::buildSliceData(parsed);
      if (slice_data.bitstream.empty() || slice_data.slices.empty()) {
        return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 access unit produced no slice data"));
      }
      if (slice_data.bitstream.size() > BITSTREAM_SIZE) {
        return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 frame of {} bytes exceeds the {}-byte bitstream buffer",
                               slice_data.bitstream.size(), BITSTREAM_SIZE));
      }
      const auto& sh = parsed.slice_headers.front();
      if (sh.pps_id < 0 || sh.pps_id >= 64 || !h265_->pps(sh.pps_id).valid) {
        return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 slice refers to PPS {}, which has not been seen", sh.pps_id));
      }
      const auto& pps = h265_->pps(sh.pps_id);
      if (pps.sps_id < 0 || pps.sps_id >= 16 || !h265_->sps(pps.sps_id).valid) {
        return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 PPS {} refers to SPS {}, which has not been seen",
                               sh.pps_id, pps.sps_id));
      }
      const auto& sps = h265_->sps(pps.sps_id);
      if (!dx_h265::isDecodable(sps)) {
        return Err(rejectFrame(OM_CODEC_NOT_SUPPORTED,
                               "H.265 stream is not 4:2:0, 4:2:2 or 4:4:4 with matching luma and chroma depth"));
      }

      const uint8_t bit_depth = static_cast<uint8_t>(sps.bit_depth_luma_minus8 + 8);
      if (surface_format_ != dx_surface::decodeFormat(sps.chroma_format_idc, bit_depth)) {
        bit_depth_ = bit_depth;
        chroma_format_idc_ = sps.chroma_format_idc;
        padded_width_ = dx_h264::alignUp(static_cast<uint32_t>(sps.pic_width_in_luma_samples), 32u);
        padded_height_ = dx_h264::alignUp(static_cast<uint32_t>(sps.pic_height_in_luma_samples), 32u);
        // One spare: the marking process frees a slot only once the stream has said
        // it is done with the picture, which is one picture behind.
        dpb_slot_count_ = std::clamp<uint32_t>(static_cast<uint32_t>(sps.sps_max_dec_pic_buffering_minus1[sps.max_sub_layers_minus1] + 2), 2, 17);
        if (FAILED(createDecoder())) return Err(OM_CODEC_HWACCEL_FAILED);
        if (FAILED(createResources())) return Err(OM_CODEC_HWACCEL_FAILED);
        output_format_.format = dx_surface::hostFormat(surface_format_);
      }

      // The parser is the one that knows which picture 8.3.1 makes the previous one.
      const int32_t poc = parsed.poc;
      const auto start = dx_h265::beginPicture(parsed, sps, sh, h265_dpb_, reorder_, output);
      reportMissingReferences(start.missing_references, poc);

      memcpy(bitstream_ptr_, slice_data.bitstream.data(), slice_data.bitstream.size());
      const uint32_t current_slot = start.slot;
      HRESULT hr = recordDecodeH265(parsed, slice_data, sps, pps, sh, poc, start.reference_set, current_slot);
      if (FAILED(hr)) {
        return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 decode command failed (hr=0x{:08X})",
                               static_cast<uint32_t>(hr)));
      }

      auto picture = download(current_slot);
      if (!picture.has_value()) {
        return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "H.265 could not read back slot {}", current_slot));
      }

      h265_dpb_.store(current_slot, poc, parsed.is_reference);

      Frame frame = {};
      frame.pts = packet.pts;
      frame.dts = packet.dts;
      frame.data = std::move(*picture);
      for (auto& ready : reorder_.push(std::move(frame), poc, dx_h265::reorderDepth(sps))) {
        output.push_back(std::move(ready));
      }
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

  auto surfaceFormat(uint8_t bit_depth) const -> DXGI_FORMAT {
    return dx_surface::decodeFormat(codec_id_ == OM_CODEC_H265 ? chroma_format_idc_ : 1, bit_depth);
  }

  // Every plane of a DPB slot: NV12 and P010 have two subresources, packed formats one.
  void transitionSlot(uint32_t slot, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    const uint32_t planes = dx_surface::planeCount(surface_format_);
    for (uint32_t plane = 0; plane < planes; ++plane) {
      auto& barrier = barriers[plane];
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = dpb_texture_.Get();
      barrier.Transition.StateBefore = before;
      barrier.Transition.StateAfter = after;
      barrier.Transition.Subresource = plane * dpb_slot_count_ + slot;
    }
    video_cmd_->ResourceBarrier(planes, barriers);
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
      bit_depth = bit_depth_;
      config.DecodeProfile = dx_h265::decodeProfile(chroma_format_idc_, bit_depth);
    }
    support.Configuration = config;
    support.DecodeFormat = surfaceFormat(bit_depth);
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

    uint8_t bit_depth = bit_depth_;
    if (codec_id_ == OM_CODEC_H264) {
      if (h264_.has_sps) {
        for (uint32_t i = 0; i < 32; ++i) {
          if (!h264_.sps_valid[i]) continue;
          bit_depth = static_cast<uint8_t>(h264_.sps[i].bit_depth_luma_minus8 + 8);
          break;
        }
      }
    }
    const DXGI_FORMAT fmt = surfaceFormat(bit_depth);
    surface_format_ = fmt;

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

    h265_dpb_.configure(dpb_slot_count_);
    std::fill(std::begin(dpb_states_), std::end(dpb_states_), D3D12_RESOURCE_STATE_COMMON);
    return S_OK;
  }

  auto recordDecode(const dx_h264::ParsedFrame& parsed, const h264::SPS& sps, const h264::PPS& pps, uint32_t current_slot) -> HRESULT {
    HRESULT hr = video_allocator_->Reset();
    if (FAILED(hr)) return hr;
    hr = video_cmd_->Reset(video_allocator_.Get());
    if (FAILED(hr)) return hr;

    if (dpb_states_[current_slot] != D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) {
      transitionSlot(current_slot, dpb_states_[current_slot], D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
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
    dx_h264::fillPicParams(sps, pps, parsed.slice, parsed, current_slot, h264_dpb_, feedback_++, pic);
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
      transitionSlot(current_slot, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
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
                        const dx_h265::ReferenceSet& reference_set,
                        uint32_t current_slot) -> HRESULT {
    HRESULT hr = video_allocator_->Reset();
    if (FAILED(hr)) return hr;
    hr = video_cmd_->Reset(video_allocator_.Get());
    if (FAILED(hr)) return hr;

    if (dpb_states_[current_slot] != D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) {
      transitionSlot(current_slot, dpb_states_[current_slot], D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
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
    dx_h265::fillPicParams(sps, pps, sh, parsed, poc, current_slot, h265_dpb_, reference_set,
                           feedback_++, pic);
    DXVA_Qmatrix_HEVC qmatrix = {};
    if (sps.scaling_list_enabled_flag) dx_h265::fillQMatrix(sps, pps, qmatrix);
    // The 4:2:2 and 4:4:4 profiles are RExt ones and read the extended form.
    DXVA_PicParams_HEVC_RangeExt range_ext = {};
    if (sps.chroma_format_idc != 1) {
      dx_h265::fillRangeExtParams(sps, pps, pic, range_ext);
      input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS,
                                                         sizeof(range_ext), &range_ext};
    } else {
      input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof(pic), &pic};
    }
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
      transitionSlot(current_slot, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
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
    const DXGI_FORMAT format = src->GetDesc().Format;
    const UINT planes = dx_surface::planeCount(format);
    const UINT src_subresource_y = reference_only_ ? 0 : current_slot;
    const UINT src_subresource_uv = reference_only_ ? 1 : dpb_slot_count_ + current_slot;

    D3D12_RESOURCE_DESC single = src->GetDesc();
    single.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT rows[2] = {};
    UINT64 row_sizes[2] = {};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&single, 0, planes, 0, footprints, rows, row_sizes, &total);

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
    if (planes > 1) {
      dst.PlacedFootprint = footprints[1];
      loc_src.SubresourceIndex = src_subresource_uv;
      copy_cmd_->CopyTextureRegion(&dst, 0, 0, 0, &loc_src, nullptr);
    }

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    copy_cmd_->ResourceBarrier(1, &barrier);
    if (FAILED(copy_cmd_->Close())) return std::nullopt;
    ID3D12CommandList* lists[] = {copy_cmd_.Get()};
    copy_queue_->ExecuteCommandLists(1, lists);
    if (FAILED(signal(copy_queue_.Get(), copy_fence_.Get(), copy_fence_value_, copy_event_))) return std::nullopt;
    if (FAILED(wait(copy_fence_.Get(), copy_fence_value_, copy_event_))) return std::nullopt;

    uint8_t* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return std::nullopt;
    Picture pic(dx_surface::hostFormat(format), width_, height_);
    pic.color_space = output_format_.color_space;
    pic.transfer_char = output_format_.transfer_char;
    pic.color_primaries = output_format_.color_primaries;
    pic.color_range = output_format_.color_range;
    pic.mastering_display = output_format_.mastering_display;
    pic.content_light_level = output_format_.content_light_level;
    const uint8_t* chroma = planes > 1 ? data + footprints[1].Offset : nullptr;
    dx_surface::copyToHost(format, data + footprints[0].Offset, footprints[0].Footprint.RowPitch, chroma,
                           footprints[1].Footprint.RowPitch, width_, height_, pic);
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

namespace {

auto dx11Encoder(OMCodecId codec) -> const CodecDescriptor* {
  switch (codec) {
    case OM_CODEC_H264: return &CODEC_DX11_ENC_H264;
    case OM_CODEC_H265: return &CODEC_DX11_ENC_H265;
    case OM_CODEC_AV1: return &CODEC_DX11_ENC_AV1;
    default: return nullptr;
  }
}

} // namespace

// Media Foundation's hardware encoders take D3D11 surfaces only, so this is the
// D3D11 encoder on a D3D11 device of the same adapter; D3D12 pictures reach it
// through a shared handle.
class DX12Encoder final : public Encoder {
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D11Device> d3d11_device_;
  OMDX11Context* d3d11_context_ = nullptr;
  std::unique_ptr<Encoder> inner_;

  static auto fail(OMError error, std::string_view what) -> OMError {
    openmedia::log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "dx12 encoder: {}", what);
    return error;
  }

  auto createD3D11Device() -> bool {
    const LUID luid = device_->GetAdapterLuid();
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) return false;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d11_device_, nullptr, nullptr))) {
      return false;
    }
    OMDX11Init init = {};
    init.device = d3d11_device_.Get();
    init.adapter_index = -1;
    d3d11_context_ = HWD3D11Context_create(init);
    return d3d11_context_ != nullptr;
  }

  void release() {
    inner_.reset(); // before the device it runs on
    if (d3d11_context_) HWD3D11Context_delete(d3d11_context_);
    d3d11_context_ = nullptr;
    d3d11_device_.Reset();
    device_.Reset();
  }

public:
  DX12Encoder() = default;
  ~DX12Encoder() override { release(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    release();
    if (!options.hw_device || options.hw_device->type != HWDeviceType::DX12 || !options.hw_device->context)
      return OM_CODEC_HWACCEL_FAILED;
    device_ = HWD3D12Context_getDevice(static_cast<OMDX12Context*>(options.hw_device->context));
    if (!device_) return OM_CODEC_HWACCEL_FAILED;
    if (!createD3D11Device()) return fail(OM_CODEC_HWACCEL_FAILED, "no D3D11 device on the D3D12 device's adapter");

    const CodecDescriptor* descriptor = dx11Encoder(options.format.codec_id);
    if (!descriptor) return OM_CODEC_NOT_SUPPORTED;
    inner_ = descriptor->encoder_factory();
    EncoderOptions inner_options = options;
    inner_options.hw_device = HWDevice {HWDeviceType::DX11, d3d11_context_};
    return inner_->configure(inner_options);
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!inner_) return Err(OM_COMMON_NOT_INITIALIZED);
    const auto* picture = std::get_if<Picture>(&frame.data);
    const auto* hw = picture ? std::get_if<std::shared_ptr<HardwarePicture>>(&picture->buffer) : nullptr;
    if (!hw || !*hw) return inner_->encode(frame);
    if ((*hw)->getType() != HWDeviceType::DX12) return Err(OM_CODEC_NOT_SUPPORTED);

    // The D3D12 texture must have been created shareable.
    auto dx_pic = std::static_pointer_cast<DX12HardwarePicture>(*hw);
    HANDLE shared_handle = nullptr;
    if (FAILED(device_->CreateSharedHandle(dx_pic->pic->texture, nullptr, GENERIC_ALL, nullptr, &shared_handle))) {
      return Err(fail(OM_CODEC_ENCODE_FAILED, "the D3D12 picture is not shareable (D3D12_HEAP_FLAG_SHARED)"));
    }
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = d3d11_device_.As(&device1);
    if (SUCCEEDED(hr)) hr = device1->OpenSharedResource1(shared_handle, IID_PPV_ARGS(&texture));
    CloseHandle(shared_handle);
    if (FAILED(hr)) return Err(fail(OM_CODEC_ENCODE_FAILED, "cannot open the D3D12 picture on D3D11"));

    OMDX11Picture d3d11_picture = {};
    d3d11_picture.texture = texture.Get();
    Frame d3d11_frame = frame;
    auto& d3d11_pic = std::get<Picture>(d3d11_frame.data);
    d3d11_pic.buffer = std::static_pointer_cast<HardwarePicture>(std::make_shared<DX11HardwarePicture>(&d3d11_picture));
    return inner_->encode(d3d11_frame); // the sample holds its own reference to the texture
  }

  auto finish() -> Result<std::vector<Packet>, OMError> override {
    if (!inner_) return Err(OM_COMMON_NOT_INITIALIZED);
    return inner_->finish();
  }

  auto getInfo() -> EncodingInfo override { return inner_ ? inner_->getInfo() : EncodingInfo {}; }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    return inner_ ? inner_->updateBitrate(rc) : OM_COMMON_NOT_INITIALIZED;
  }

  auto requestKeyframe() -> OMError override {
    return inner_ ? inner_->requestKeyframe() : OM_COMMON_NOT_INITIALIZED;
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
    // NVIDIA's H.264 MFT refuses 10-bit input.
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12, OM_FORMAT_YUV420P}}},
    .encoder_factory = [] { return std::make_unique<DX12Encoder>(); },
};

const CodecDescriptor CODEC_DX12_ENC_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h265_enc",
    .long_name = "DirectX12 H.265 Encoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12, OM_FORMAT_YUV420P, OM_FORMAT_P010}}},
    .encoder_factory = [] { return std::make_unique<DX12Encoder>(); },
};

} // namespace openmedia
