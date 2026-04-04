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

// Audio - FFmpeg
extern const CodecDescriptor CODEC_FFMPEG_AAC;
extern const CodecDescriptor CODEC_FFMPEG_MP3;
extern const CodecDescriptor CODEC_FFMPEG_OPUS;
extern const CodecDescriptor CODEC_FFMPEG_VORBIS;
extern const CodecDescriptor CODEC_FFMPEG_FLAC;

// Video - Software
extern const CodecDescriptor CODEC_DAV1D;
//extern const CodecDescriptor CODEC_OPENH264;
extern const CodecDescriptor CODEC_VVDEC;
extern const CodecDescriptor CODEC_XEVD;
extern const CodecDescriptor CODEC_XEVE;

// Video - WMF
extern const CodecDescriptor CODEC_WMF_VIDEO_H264;
extern const CodecDescriptor CODEC_WMF_VIDEO_H265;
extern const CodecDescriptor CODEC_WMF_VIDEO_AV1;

// Video - FFmpeg
extern const CodecDescriptor CODEC_FFMPEG_H264;
extern const CodecDescriptor CODEC_FFMPEG_H265;
extern const CodecDescriptor CODEC_FFMPEG_H266;
extern const CodecDescriptor CODEC_FFMPEG_EVC;
extern const CodecDescriptor CODEC_FFMPEG_VP8;
extern const CodecDescriptor CODEC_FFMPEG_VP9;
extern const CodecDescriptor CODEC_FFMPEG_AV1;

// Video - DirectX11
extern const CodecDescriptor CODEC_DX11_H264;

// Video - DirectX12
extern const CodecDescriptor CODEC_DX12_H264;
extern const CodecDescriptor CODEC_DX12_H265;
extern const CodecDescriptor CODEC_DX12_VP9;
extern const CodecDescriptor CODEC_DX12_AV1;

// Video - AMD AMF
//extern const CodecDescriptor CODEC_AMF_H264;
//extern const CodecDescriptor CODEC_AMF_H265;
//extern const CodecDescriptor CODEC_AMF_AV1;

// Image
extern const CodecDescriptor CODEC_PNG;
extern const CodecDescriptor CODEC_JPEG;
extern const CodecDescriptor CODEC_WEBP;
extern const CodecDescriptor CODEC_GIF;
extern const CodecDescriptor CODEC_TGA;
extern const CodecDescriptor CODEC_BMP;
extern const CodecDescriptor CODEC_TIFF;

}
