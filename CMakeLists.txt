cmake_minimum_required(VERSION 3.22)
project(OpenMedia VERSION 0.1.0 LANGUAGES C CXX)

include(Options.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(aui.boot.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif ()

if (OPENMEDIA_STATIC)
    add_library(OpenMedia STATIC)
else ()
    add_library(OpenMedia SHARED)
endif ()

target_sources(OpenMedia PRIVATE
        include/openmedia/audio.hpp
        include/openmedia/buffer.hpp
        include/openmedia/byte.h
        include/openmedia/codec_api.hpp
        include/openmedia/codec_defs.h
        include/openmedia/codec_extra.hpp
        include/openmedia/codec_registry.hpp
        include/openmedia/dictionary.hpp
        include/openmedia/error.h
        include/openmedia/format_api.hpp
        include/openmedia/format_defs.h
        include/openmedia/format_detector.hpp
        include/openmedia/format_registry.hpp
        include/openmedia/frame.hpp
        include/openmedia/io.hpp
        include/openmedia/log.hpp
        include/openmedia/macro.h
        include/openmedia/media.h
        include/openmedia/media.hpp
        include/openmedia/metadata_keys.hpp
        include/openmedia/packet.hpp
        include/openmedia/result.hpp
        include/openmedia/track.hpp
        include/openmedia/video.hpp
        src/image/icns.cpp
        src/util/bit_reader.cpp
        src/util/bit_reader.hpp
        src/util/cpp.hpp
        src/util/demuxer_base.hpp
        src/util/dynamic_loader.hpp
        src/util/io_util.cpp
        src/util/io_util.hpp
        src/audio/pcm.cpp
        src/bitstream_filter.hpp
        src/bmff_demuxer.cpp
        src/codec_defs.cpp
        src/codec_registry.cpp
        src/codecs.hpp
        src/format_detector.cpp
        src/format_registry.cpp
        src/formats.hpp
        src/annexb.hpp
        src/io.cpp
        src/log.cpp
        src/media.cpp
        src/video/parser/h264_parser.cpp
        src/video/parser/h264_parser.hpp
        src/video/parser/h264_internal.hpp
        src/video/parser/h264_nal.cpp
        src/video/parser/h264_pps.cpp
        src/video/parser/h264_scaling.cpp
        src/video/parser/h264_slice.cpp
        src/video/parser/h264_sps.cpp
        src/video/parser/h264_types.hpp
        src/video/parser/h264_vui.cpp
        src/video/parser/h265_parser.cpp
        src/video/parser/h265_parser.hpp
        src/video/parser/vp9_parser.cpp
        src/video/parser/vp9_parser.hpp
        src/video/parser/av1_parser.cpp
        src/video/parser/av1_parser.hpp
        src/video/parser/start_code.cpp
        src/video/parser/start_code.hpp
        src/video/parser/start_code_ssse3.cpp
        src/video/parser/start_code_avx2.cpp
        src/video/parser/start_code_avx512.cpp
        src/video/parser/start_code_neon.cpp
        src/video/parser/start_code_sve.cpp
        src/video/raw_video_decoder.cpp
)

target_include_directories(OpenMedia PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(OpenMedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(OpenMedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/headeronly)

get_target_property(OPENMEDIA_LIB_TYPE OpenMedia TYPE)
if (OPENMEDIA_LIB_TYPE STREQUAL "SHARED_LIBRARY")
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DYNAMIC)
    target_compile_definitions(OpenMedia INTERFACE OPENMEDIA_DYNAMIC_IMPORT)
endif ()

add_library(OpenMedia::OpenMedia ALIAS OpenMedia)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" OPENMEDIA_SYSTEM_PROCESSOR)
if (OPENMEDIA_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|i[3-6]86)")
    if (MSVC)
        set_source_files_properties(src/video/parser/start_code_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else ()
        set_source_files_properties(src/video/parser/start_code_ssse3.cpp PROPERTIES COMPILE_OPTIONS "-mssse3")
        set_source_files_properties(src/video/parser/start_code_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-mavx512f -mavx512bw" OPENMEDIA_COMPILER_SUPPORTS_AVX512)
        if (OPENMEDIA_COMPILER_SUPPORTS_AVX512)
            set_source_files_properties(src/video/parser/start_code_avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw")
            set_source_files_properties(src/video/parser/start_code_avx512.cpp PROPERTIES COMPILE_DEFINITIONS OPENMEDIA_ENABLE_AVX512_START_CODE)
            target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_ENABLE_AVX512_START_CODE)
        endif ()
    endif ()
elseif (OPENMEDIA_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64)")
    if (NOT MSVC)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=armv8-a+sve" OPENMEDIA_COMPILER_SUPPORTS_SVE)
        if (OPENMEDIA_COMPILER_SUPPORTS_SVE)
            set_source_files_properties(src/video/parser/start_code_sve.cpp PROPERTIES COMPILE_OPTIONS "-march=armv8-a+sve")
            set_source_files_properties(src/video/parser/start_code_sve.cpp PROPERTIES COMPILE_DEFINITIONS OPENMEDIA_ENABLE_SVE_START_CODE)
            target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_ENABLE_SVE_START_CODE)
        endif ()
    endif ()
elseif (OPENMEDIA_SYSTEM_PROCESSOR MATCHES "(^arm|armv7)")
    if (NOT MSVC)
        set_source_files_properties(src/video/parser/start_code_neon.cpp PROPERTIES COMPILE_OPTIONS "-mfpu=neon")
    endif ()
endif ()

if (OPENMEDIA_VULKAN_VIDEO)
    if (AUI_BOOT)
        auib_import(VulkanHeaders https://github.com/KhronosGroup/Vulkan-Headers
                VERSION f6a6f7ab165cedbfa2a7d0c93fe27a2d01ce09c8)
    elseif (NOT TARGET Vulkan::Headers)
        add_subdirectory(third_party/external/Vulkan-Headers)
    endif ()
    target_link_libraries(OpenMedia PRIVATE Vulkan::Headers)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_vulkan.h)
    target_sources(OpenMedia PRIVATE src/hw_vulkan.cpp)
    target_sources(OpenMedia PRIVATE src/hw_vulkan_priv.hpp)
    target_sources(OpenMedia PRIVATE src/video/vulkan_video.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_VULKAN)
endif ()

if (OPENMEDIA_NVIDIA)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_cuda.h)
    target_sources(OpenMedia PRIVATE src/hw_cuda.cpp)
    target_sources(OpenMedia PRIVATE src/video/nvdec.cpp src/video/nvenc.cpp)
    target_include_directories(OpenMedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffnvcodec)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_NVIDIA)
endif ()

if (OPENMEDIA_PNG OR OPENMEDIA_MATROSKA)
    if (AUI_BOOT)
        auib_import(ZLIB https://github.com/Nelonn/zlib-ng
                VERSION 788c485d828c017b884946470bff1cc4373cc056)
    elseif (NOT TARGET ZLIB::ZLIB)
        add_subdirectory(third_party/external/zlib-ng)
    endif ()
endif ()

if (WIN32)
    target_sources(OpenMedia PRIVATE src/audio/wmf_audio.cpp)
    #target_sources(OpenMedia PRIVATE src/video/wmf.cpp) # doesn't work
endif ()

if (APPLE)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(OpenMedia PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-elaborated-enum-base>)
    endif ()

    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
    target_link_libraries(OpenMedia PRIVATE
            ${AUDIOTOOLBOX_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
            ${COREMEDIA_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
            ${VIDEOTOOLBOX_FRAMEWORK})
    target_sources(OpenMedia PRIVATE src/audio/audio_toolbox.cpp)
    target_sources(OpenMedia PRIVATE src/video/video_toolbox.cpp)
endif ()

if (OPENMEDIA_AMF)
    target_sources(OpenMedia PRIVATE src/video/amf.cpp)
    target_include_directories(OpenMedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf-headers)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_AMF)
    if (WIN32)
        if (NOT OPENMEDIA_DX11_VIDEO)
            target_sources(OpenMedia PRIVATE include/openmedia/hw_dx11.h)
            target_sources(OpenMedia PRIVATE src/hw_dx11.cpp)
            target_sources(OpenMedia PRIVATE src/hw_dx11_priv.hpp)
        endif ()
        target_link_libraries(OpenMedia PRIVATE d3d11.lib dxgi.lib dxguid.lib)
        target_include_directories(OpenMedia PRIVATE ${CMAKE_WINDOWS_KITS_INCLUDE_DIRS})
    endif ()
endif ()

if (OPENMEDIA_DX11_VIDEO)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_dx11.h)
    target_sources(OpenMedia PRIVATE src/hw_dx11.cpp)
    target_sources(OpenMedia PRIVATE src/hw_dx11_priv.hpp)
    target_sources(OpenMedia PRIVATE src/video/dx11.cpp)
    target_link_libraries(OpenMedia PRIVATE d3d11.lib dxgi.lib dxva2.lib mf.lib mfplat.lib mfuuid.lib mfreadwrite.lib)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DX11_VIDEO)
endif ()

if (OPENMEDIA_DX12_VIDEO)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_dx12.h)
    target_sources(OpenMedia PRIVATE src/hw_dx12.cpp)
    target_sources(OpenMedia PRIVATE src/hw_dx12_priv.hpp)
    target_sources(OpenMedia PRIVATE src/video/dx12.cpp)
    target_link_libraries(OpenMedia PRIVATE d3d12.lib dxgi.lib dxva2.lib mf.lib mfplat.lib mfuuid.lib mfreadwrite.lib)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DX12_VIDEO)
endif ()

if (OPENMEDIA_VAAPI)
    include(CheckIncludeFile)
    check_include_file("va/va.h" HAVE_VA_VA_H)
    if (NOT HAVE_VA_VA_H)
        message(STATUS "va/va.h not found, disabling VA-API support")
        set(OPENMEDIA_VAAPI OFF)
    endif ()
endif ()

if (OPENMEDIA_VAAPI)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_vaapi.h)
    target_sources(OpenMedia PRIVATE src/hw_vaapi.cpp)
    target_sources(OpenMedia PRIVATE src/hw_vaapi_priv.hpp)
    target_sources(OpenMedia PRIVATE src/video/vaapi_loader.cpp src/video/vaapi_loader.hpp)
    target_sources(OpenMedia PRIVATE src/video/vaapi.cpp)
    target_include_directories(OpenMedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/h265bitstream)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_VAAPI)
endif ()

if (OPENMEDIA_FFMPEG)
    find_package(FFMPEG COMPONENTS avformat avcodec avutil)

    if (FFMPEG_FOUND)
        target_include_directories(OpenMedia PRIVATE ${FFMPEG_INCLUDE_DIRS})
        if (APPLE)
            target_compile_definitions(OpenMedia PRIVATE
                    "OPENMEDIA_FFMPEG_AVUTIL_LIBRARY=\"${FFMPEG_avutil_LIBRARY}\""
                    "OPENMEDIA_FFMPEG_AVCODEC_LIBRARY=\"${FFMPEG_avcodec_LIBRARY}\""
                    "OPENMEDIA_FFMPEG_AVFORMAT_LIBRARY=\"${FFMPEG_avformat_LIBRARY}\"")
        endif ()
        target_sources(OpenMedia PRIVATE src/avutil.cpp src/avutil.hpp)
        target_sources(OpenMedia PRIVATE src/avcodec.cpp src/avcodec.hpp)
        target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_AVCODEC)
        target_sources(OpenMedia PRIVATE src/avformat.cpp src/avformat.hpp)
        target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_AVFORMAT)
    endif ()
endif ()

if (ANDROID)
    target_link_libraries(OpenMedia PRIVATE log mediandk)
    target_sources(OpenMedia PRIVATE src/mediacodec.cpp)
    target_sources(OpenMedia PRIVATE src/mediaextractor.cpp)
endif ()

if (OPENMEDIA_MATROSKA)
    if (AUI_BOOT)
        auib_import(webm https://github.com/Nelonn/libwebm
                VERSION 97c21a1c8ff698b417c6eadb378527ae45ca4a3c)
    elseif (NOT TARGET webm::webm)
        add_subdirectory(third_party/external/libwebm)
    endif ()
    target_link_libraries(OpenMedia PRIVATE webm::webm)
    target_sources(OpenMedia PRIVATE src/matroska.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_MATROSKA)
endif ()

if (OPENMEDIA_FDK_AAC)
    auib_import(fdk-aac https://github.com/mstorsjo/fdk-aac
            VERSION d8e6b1a3aa606c450241632b64b703f21ea31ce3)
    target_link_libraries(OpenMedia PRIVATE FDK-AAC::fdk-aac)
    target_sources(OpenMedia PRIVATE src/audio/fdk_aac.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_FDK_AAC)
endif ()

if (OPENMEDIA_ALAC AND NOT APPLE)
    if (AUI_BOOT)
        auib_import(alac https://github.com/Nelonn/alac
                VERSION 2068fcd85ab974488d0752fd8da40baf3535e093)
    elseif (NOT TARGET alac::alac)
        add_subdirectory(third_party/external/alac)
    endif ()
    target_link_libraries(OpenMedia PRIVATE alac::alac)
    target_sources(OpenMedia PRIVATE src/audio/alac.cpp)
endif ()

if (OPENMEDIA_OGG)
    if (AUI_BOOT)
        auib_import(Ogg https://github.com/xiph/ogg VERSION v1.3.6)
    elseif (NOT TARGET Ogg::ogg)
        add_subdirectory(third_party/external/ogg)
    endif ()
    target_link_libraries(OpenMedia PRIVATE Ogg::ogg)
    target_sources(OpenMedia PRIVATE src/audio/ogg.cpp)
endif ()

if (OPENMEDIA_EXR)
    if (AUI_BOOT)
        auib_import(OpenEXR https://github.com/AcademySoftwareFoundation/openexr
                VERSION v3.4.4)
    elseif (NOT TARGET OpenEXR::OpenEXR)
        add_subdirectory(third_party/external/Imath)
        set(OPENEXR_FORCE_INTERNAL_IMATH ON)
        add_subdirectory(third_party/external/openexr)
    endif ()
    target_link_libraries(OpenMedia PRIVATE OpenEXR::OpenEXR)
endif ()

if (OPENMEDIA_FLAC)
    if (AUI_BOOT)
        auib_import(FLAC https://github.com/Nelonn/flac
                VERSION 9b5fa29ba7532581e4b77a7ff00b59089d82ff91
                CMAKE_ARGS -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF)
    elseif (NOT TARGET FLAC::FLAC)
        set(BUILD_CXXLIBS OFF)
        set(BUILD_PROGRAMS OFF)
        set(BUILD_EXAMPLES OFF)
        set(BUILD_TESTING OFF)
        set(BUILD_DOCS OFF)
        set(INSTALL_MANPAGES OFF)
        set(INSTALL_PKGCONFIG_MODULES OFF)
        set(INSTALL_CMAKE_CONFIG_MODULE OFF)
        add_subdirectory(third_party/external/flac)
    endif ()
    target_link_libraries(OpenMedia PRIVATE FLAC::FLAC)
    target_sources(OpenMedia PRIVATE src/audio/flac_codec.cpp)
    target_sources(OpenMedia PRIVATE src/audio/flac_format.cpp)
endif ()

if (OPENMEDIA_OPUS AND NOT (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm" AND CMAKE_C_COMPILER_ID STREQUAL "Clang")) # TODO: fix opus
    if (AUI_BOOT)
        auib_import(Opus https://github.com/aui-framework/opus
                VERSION 9f719ec0eec5bbe3b8b9a86145e71bac6f45457c)
    elseif (NOT TARGET Opus::opus)
        add_subdirectory(third_party/external/opus)
    endif ()
    target_link_libraries(OpenMedia PRIVATE Opus::opus)
    target_sources(OpenMedia PRIVATE src/audio/opus.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_OPUS)
endif ()

if (OPENMEDIA_VORBIS)
    if (AUI_BOOT)
        auib_import(Vorbis https://github.com/Nelonn/vorbis
                VERSION 4ab2df62b9a755210a2ff561cbc143b72bcee3ec)
    elseif (NOT TARGET Vorbis::vorbis)
        add_subdirectory(third_party/external/vorbis)
    endif ()
    target_link_libraries(OpenMedia PRIVATE Vorbis::vorbis Vorbis::vorbisenc Vorbis::vorbisfile)
    target_sources(OpenMedia PRIVATE src/audio/vorbis.cpp)
endif ()

if (OPENMEDIA_WEBP)
    if (AUI_BOOT)
        set(WEBP_COMPONENTS_TO_DISABLE ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
        foreach(_component ${WEBP_COMPONENTS_TO_DISABLE})
            list(APPEND WEBP_CMAKE_ARGS "-DWEBP_BUILD_${_component}=OFF")
        endforeach()
        auib_import(WebP https://github.com/webmproject/libwebp VERSION v1.6.0 CMAKE_ARGS ${WEBP_CMAKE_ARGS})
        target_link_libraries(OpenMedia PRIVATE WebP::webpdemux WebP::webpdecoder)
    elseif (NOT TARGET WebP::webp)
        set(WEBP_BUILD_ANIM_UTILS OFF)
        set(WEBP_BUILD_CWEBP OFF)
        set(WEBP_BUILD_DWEBP OFF)
        set(WEBP_BUILD_GIF2WEBP OFF)
        set(WEBP_BUILD_IMG2WEBP OFF)
        set(WEBP_BUILD_VWEBP OFF)
        set(WEBP_BUILD_WEBPINFO OFF)
        set(WEBP_BUILD_WEBPMUX OFF)
        set(WEBP_BUILD_EXTRAS OFF)
        add_subdirectory(third_party/external/libwebp)
        target_link_libraries(OpenMedia PRIVATE webpdemux webpdecoder)
    endif ()
    target_sources(OpenMedia PRIVATE src/image/webp_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/webp_decoder.cpp)
endif ()

if (OPENMEDIA_JPEG)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" proc_lower)
    set(_allow_jpeg_simd ON)
    if(WIN32 AND proc_lower MATCHES "arm")
        set(_allow_jpeg_simd OFF)
    endif()
    auib_import(libjpeg-turbo
            https://github.com/libjpeg-turbo/libjpeg-turbo
            VERSION 3.1.90
            CMAKE_ARGS -DCMAKE_INSTALL_DEFAULT_LIBDIR=lib -DWITH_TOOLS=OFF -DWITH_SIMD=${_allow_jpeg_simd})
    target_link_libraries(OpenMedia PRIVATE libjpeg-turbo::jpeg-static)
    target_sources(OpenMedia PRIVATE src/image/jpeg_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/jpeg_decoder.cpp)
endif ()

if (OPENMEDIA_BMP)
    target_sources(OpenMedia PRIVATE src/image/bmp_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/bmp_decoder.cpp)
endif ()

if (OPENMEDIA_DDS)
    target_sources(OpenMedia PRIVATE src/image/dds_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/dds_decoder.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DDS)
endif ()

if (OPENMEDIA_HDR)
    target_sources(OpenMedia PRIVATE src/image/hdr_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/hdr_decoder.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_HDR)
endif ()

if (OPENMEDIA_GIF)
    if (AUI_BOOT)
        auib_import(giflib https://github.com/Nelonn/giflib
                VERSION 47133bd4234655ce377ee6a41500adc9bbf28f99)
    elseif (NOT TARGET giflib::giflib)
        add_subdirectory(third_party/external/giflib)
    endif ()
    target_link_libraries(OpenMedia PRIVATE giflib::giflib)
    target_sources(OpenMedia PRIVATE src/image/gif_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/gif_decoder.cpp)
endif ()

if (OPENMEDIA_PNG)
    if (AUI_BOOT)
        auib_import(PNG https://github.com/Nelonn/libpng
                VERSION dd47360484d97fd1e4925eddc8102f084f5c2ce9)
    elseif (NOT TARGET PNG::png)
        add_subdirectory(third_party/external/libpng)
    endif ()
    target_link_libraries(OpenMedia PRIVATE PNG::png)
    target_sources(OpenMedia PRIVATE src/image/png_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/png_decoder.cpp)
endif ()

if (OPENMEDIA_TGA)
    if (AUI_BOOT)
        auib_import(tga https://github.com/Nelonn/tga
                VERSION 8e547e4f942c2167e292aca76591bda11b76b84f)
    elseif (NOT TARGET tga::tga)
        add_subdirectory(third_party/external/tga)
    endif ()
    target_link_libraries(OpenMedia PRIVATE tga::tga)
    target_sources(OpenMedia PRIVATE src/image/tga_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/tga_decoder.cpp)
endif ()

if (OPENMEDIA_WAV)
    target_sources(OpenMedia PRIVATE src/audio/wav.cpp)
endif ()

if (OPENMEDIA_MP3 AND NOT APPLE)
    target_sources(OpenMedia PRIVATE src/audio/mp3_decoder.cpp)
    target_sources(OpenMedia PRIVATE src/audio/mp3_demuxer.cpp)
endif ()

if (OPENMEDIA_MP3 AND APPLE)
    target_sources(OpenMedia PRIVATE src/audio/mp3_demuxer.cpp)
endif ()

if (OPENMEDIA_DAV1D)
    if (AUI_BOOT)
        auib_import(dav1d https://github.com/Nelonn/dav1d
                VERSION 87301c133c40ab9f296ccbb778fbaca37d6ce704)
    elseif (NOT TARGET dav1d::dav1d)
        add_subdirectory(third_party/external/dav1d)
    endif ()
    target_link_libraries(OpenMedia PRIVATE dav1d::dav1d)
    target_sources(OpenMedia PRIVATE src/video/dav1d.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DAV1D)
endif ()

#if (OPENMEDIA_AVIF)
#    if (AUI_BOOT)
#        auib_import(libavif https://github.com/aomediacodec/libavif
#                VERSION 253f0d0f04e771dd94b37682a211b10a9cfe702e
#                CMAKE_ARGS -DAVIF_LIBYUV=LOCAL -DAVIF_CODEC_DAV1D_ENABLED=ON)
#    endif ()
#    target_link_libraries(OpenMedia PRIVATE avif)
#endif ()

if (OPENMEDIA_VPX)
    if (AUI_BOOT)
        auib_import(vpx https://github.com/Nelonn/libvpx-cmake
                VERSION 2216dd6647a283b794543a8257009c1ba712ef65)
    elseif (NOT TARGET vpx::vpx)
        add_subdirectory(third_party/external/vpx)
    endif ()
    target_link_libraries(OpenMedia PRIVATE vpx::vpx)
    target_sources(OpenMedia PRIVATE src/video/vpx.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_VPX)
endif ()

if (OPENMEDIA_OPENH264)
    target_sources(OpenMedia PRIVATE src/video/openh264.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_OPENH264)
    target_include_directories(OpenMedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openh264)
endif ()

if (OPENMEDIA_VVDEC)
    #if (AUI_BOOT)
    #    auib_import(vvdec https://github.com/fraunhoferhhi/vvdec
    #            VERSION 7df9d1f978433092d06537b3fe2f92a7fc33adcc
    #            CMAKE_ARGS -DVVDEC_ENABLE_X86_SIMD=${VVDEC_ENABLE_X86_SIMD})
    #elseif (NOT TARGET dav1d::dav1d)
    #    add_subdirectory(third_party/external/vvdec)
    #endif ()
    add_subdirectory(third_party/external/vvdec)
    target_link_libraries(OpenMedia PRIVATE vvdec)
    target_sources(OpenMedia PRIVATE src/video/vvdec.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_VVDEC)
endif ()

if (OPENMEDIA_XEVD)
    if (AUI_BOOT)
        auib_import(xevd https://github.com/Nelonn/xevd
                VERSION d3ef577f51e489260a94bc0abd330674d62f7885
                CMAKE_ARGS -DXEVD_INSTALL=ON)
    elseif (NOT TARGET XEVD::xevd)
        add_subdirectory(third_party/external/xevd)
    endif ()
    target_link_libraries(OpenMedia PRIVATE XEVD::xevd)
    target_sources(OpenMedia PRIVATE src/video/xevd.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_XEVD)
endif ()

if (OPENMEDIA_XEVE)
    if (AUI_BOOT)
        auib_import(xeve https://github.com/Nelonn/xeve
                VERSION 865dce7ea5104ab2770980713d74e7cc96bc98a8
                CMAKE_ARGS -DXEVD_INSTALL=ON)
    elseif (NOT TARGET XEVE::xeve)
        add_subdirectory(third_party/external/xeve)
    endif ()
    target_link_libraries(OpenMedia PRIVATE XEVE::xeve)
    target_sources(OpenMedia PRIVATE src/video/xeve.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_XEVE)
endif ()

if (OPENMEDIA_TIFF)
    if (AUI_BOOT)
        auib_import(TIFF https://github.com/libsdl-org/libtiff
                VERSION f324415f50cb5c90f7712e9dfe69831f5d2ea88d
                CMAKE_ARGS -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF -Dtiff-docs=OFF -Dtiff-install=ON)
    elseif (NOT TARGET TIFF::tiff)
        set(tiff-tools OFF)
        set(tiff-tests OFF)
        set(tiff-contrib OFF)
        set(tiff-docs OFF)
        add_subdirectory(third_party/external/libtiff)
    endif ()
    target_link_libraries(OpenMedia PRIVATE TIFF::tiff)
    target_sources(OpenMedia PRIVATE src/image/tiff_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/tiff_decoder.cpp)
endif ()

if (OPENMEDIA_INSTALL)
    include(GNUInstallDirs)

    install(TARGETS OpenMedia
        EXPORT OpenMediaTargets
        INCLUDES DESTINATION include
    )

    install(DIRECTORY include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )

    install(EXPORT OpenMediaTargets
        FILE OpenMediaConfig.cmake
        NAMESPACE OpenMedia::
        DESTINATION lib/cmake/OpenMedia
    )
endif ()

if (OPENMEDIA_EXAMPLES)
    if (AUI_BOOT)
        auib_import(SDL3 https://github.com/libsdl-org/SDL
                VERSION c20a058ff431d046c5806bd22aba0835b25d9904)
    elseif (NOT TARGET SDL3::SDL3)
        add_subdirectory(third_party/external/SDL)
    endif ()
    add_subdirectory(example)
endif ()
