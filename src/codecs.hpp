#pragma once

#include <openmedia/codec_api.hpp>

namespace openmedia {

// Audio
extern const CodecDescriptor CODEC_PCM_S16LE;
extern const CodecDescriptor CODEC_PCM_F32LE;
extern const CodecDescriptor CODEC_ALAC;
extern const CodecDescriptor CODEC_FDK_AAC;
extern const CodecDescriptor CODEC_MP3;
extern const CodecDescriptor CODEC_FLAC;
extern const CodecDescriptor CODEC_VORBIS;
extern const CodecDescriptor CODEC_OPUS;
extern const CodecDescriptor CODEC_WMF_AAC;
extern const CodecDescriptor CODEC_WMF_MP3;
extern const CodecDescriptor CODEC_AUDIO_TOOLBOX_ALAC;
extern const CodecDescriptor CODEC_AUDIO_TOOLBOX_MP3;
extern const CodecDescriptor CODEC_AUDIO_TOOLBOX_AAC;
extern const CodecDescriptor CODEC_AUDIO_TOOLBOX_AC3;
extern const CodecDescriptor CODEC_AUDIO_TOOLBOX_EAC3;

// Video - Software
extern const CodecDescriptor CODEC_DAV1D;
extern const CodecDescriptor CODEC_OPENH264;
extern const CodecDescriptor CODEC_VVDEC;
extern const CodecDescriptor CODEC_XEVD;
extern const CodecDescriptor CODEC_XEVE;

// Video - WMF
extern const CodecDescriptor CODEC_WMF_VIDEO_H264;
extern const CodecDescriptor CODEC_WMF_VIDEO_H265;
extern const CodecDescriptor CODEC_WMF_VIDEO_AV1;

// Video - VideoToolbox
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_H263;
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_H264;
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_H265;
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_MPEG2;
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_MPEG4;
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_AV1;
extern const CodecDescriptor CODEC_VIDEOTOOLBOX_PRORES;

// Video - DirectX11
extern const CodecDescriptor CODEC_DX11_H264;
extern const CodecDescriptor CODEC_DX11_ENC_H264;

// Video - VA-API
extern const CodecDescriptor CODEC_VAAPI_H264;
extern const CodecDescriptor CODEC_VAAPI_H265;
extern const CodecDescriptor CODEC_VAAPI_VP9;
extern const CodecDescriptor CODEC_VAAPI_AV1;

// Video - DirectX12
extern const CodecDescriptor CODEC_DX12_H264;
extern const CodecDescriptor CODEC_DX12_H265;
extern const CodecDescriptor CODEC_DX12_VP9;
extern const CodecDescriptor CODEC_DX12_AV1;
extern const CodecDescriptor CODEC_DX12_ENC_H264;
extern const CodecDescriptor CODEC_DX12_ENC_H265;

// Video - AMD AMF
extern const CodecDescriptor CODEC_AMF_H264;
extern const CodecDescriptor CODEC_AMF_H265;
extern const CodecDescriptor CODEC_AMF_AV1;
extern const CodecDescriptor CODEC_AMF_VP9;

// Video - Vulkan
extern const CodecDescriptor CODEC_VULKAN_H264;
extern const CodecDescriptor CODEC_VULKAN_H265;
extern const CodecDescriptor CODEC_VULKAN_AV1;
extern const CodecDescriptor CODEC_VULKAN_VP9;

// Video - MediaCodec
extern const CodecDescriptor CODEC_MEDIACODEC_H264;
extern const CodecDescriptor CODEC_MEDIACODEC_H265;
extern const CodecDescriptor CODEC_MEDIACODEC_VP8;
extern const CodecDescriptor CODEC_MEDIACODEC_VP9;
extern const CodecDescriptor CODEC_MEDIACODEC_AV1;

// Video - NVIDIA
extern const CodecDescriptor CODEC_NVDEC_H264;
extern const CodecDescriptor CODEC_NVDEC_H265;
extern const CodecDescriptor CODEC_NVDEC_VP9;
extern const CodecDescriptor CODEC_NVDEC_AV1;
extern const CodecDescriptor CODEC_NVENC_H264;
extern const CodecDescriptor CODEC_NVENC_H265;
extern const CodecDescriptor CODEC_NVENC_AV1;

// Audio - MediaCodec
extern const CodecDescriptor CODEC_MEDIACODEC_AAC;

// Image
extern const CodecDescriptor CODEC_PNG;
extern const CodecDescriptor CODEC_JPEG;
extern const CodecDescriptor CODEC_WEBP;
extern const CodecDescriptor CODEC_GIF;
extern const CodecDescriptor CODEC_TGA;
extern const CodecDescriptor CODEC_BMP;
extern const CodecDescriptor CODEC_TIFF;

struct CodecRegistry;
void registerFFmpegCodecs(CodecRegistry* registry) noexcept;

}
