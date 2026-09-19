#include "vaapi_codecs.hpp"

#include <codecs.hpp>

namespace openmedia {

const CodecDescriptor CODEC_VAAPI_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_h264",
    .long_name = "VA-API H.264/AVC Codec",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return createVAAPIDecoder(); },
    .encoder_factory = [] { return createVAAPIEncoder(); },
};

const CodecDescriptor CODEC_VAAPI_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_h265",
    .long_name = "VA-API H.265/HEVC Codec",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return createVAAPIDecoder(); },
    .encoder_factory = [] { return createVAAPIEncoder(); },
};

const CodecDescriptor CODEC_VAAPI_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_vp9",
    .long_name = "VA-API VP9 Codec",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return createVAAPIDecoder(); },
    .encoder_factory = [] { return createVAAPIEncoder(); },
};

const CodecDescriptor CODEC_VAAPI_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_av1",
    .long_name = "VA-API AV1 Codec",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return createVAAPIDecoder(); },
    .encoder_factory = [] { return createVAAPIEncoder(); },
};

} // namespace openmedia
