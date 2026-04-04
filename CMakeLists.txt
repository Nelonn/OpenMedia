cmake_minimum_required(VERSION 3.21)
project(OpenMedia VERSION 0.1.0 LANGUAGES C CXX)

include(Options.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

file(DOWNLOAD
        https://raw.githubusercontent.com/aui-framework/aui/b09bbf34115f55dd63a5a34948b7f62ce60b2918/aui.boot.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/aui.boot.cmake)
include(${CMAKE_CURRENT_BINARY_DIR}/aui.boot.cmake)

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

add_library(OpenMedia)

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
        include/openmedia/packet.hpp
        include/openmedia/result.hpp
        include/openmedia/track.hpp
        include/openmedia/video.hpp
        src/util/cpp.hpp
        src/util/demuxer_base.hpp
        src/util/dynamic_loader.hpp
        src/util/io_util.cpp
        src/util/io_util.hpp
        src/audio/pcm.cpp
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
)

target_include_directories(OpenMedia PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(OpenMedia PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(OpenMedia PRIVATE ${PROJECT_SOURCE_DIR}/third_party/headeronly)

get_target_property(OPENMEDIA_LIB_TYPE OpenMedia TYPE)
if (OPENMEDIA_LIB_TYPE STREQUAL "SHARED_LIBRARY")
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DYNAMIC)
    target_compile_definitions(OpenMedia INTERFACE OPENMEDIA_DYNAMIC_IMPORT)
endif ()

add_library(OpenMedia::OpenMedia ALIAS OpenMedia)

if (OPENMEDIA_VULKAN_VIDEO)
    if (AUI_BOOT)
        auib_import(VulkanHeaders https://github.com/KhronosGroup/Vulkan-Headers
                VERSION 49f1a381e2aec33ef32adf4a377b5a39ec016ec4)
    elseif (NOT TARGET Vulkan::Headers)
        add_subdirectory(third_party/external/Vulkan-Headers)
    endif ()
    target_link_libraries(OpenMedia PRIVATE Vulkan::Headers)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_vulkan.h)
    target_sources(OpenMedia PRIVATE src/hw_vulkan.cpp)
endif ()

if (OPENMEDIA_PNG OR OPENMEDIA_MATROSKA)
    if (AUI_BOOT)
        auib_import(ZLIB https://github.com/Nelonn/zlib-ng
                VERSION 381792764c59489a0c968968c1ddd8a2a6d8556c)
    elseif (NOT TARGET ZLIB::ZLIB)
        add_subdirectory(third_party/external/zlib-ng)
    endif ()
endif ()

if (WIN32)
    target_sources(OpenMedia PRIVATE src/audio/wmf_audio.cpp)
    #target_sources(OpenMedia PRIVATE src/video/wmf.cpp) # doesn't work
endif ()

if (OPENMEDIA_AMF)
    #target_sources(OpenMedia PRIVATE src/video/amf.cpp)
endif ()

if (OPENMEDIA_DX11_VIDEO)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_dx11.h)
    target_sources(OpenMedia PRIVATE src/hw_dx11.cpp)
    target_sources(OpenMedia PRIVATE src/video/dx11.cpp)
    target_link_libraries(OpenMedia PRIVATE d3d11.lib dxgi.lib dxva2.lib mf.lib mfplat.lib mfuuid.lib mfreadwrite.lib)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DX11_VIDEO)
endif ()

if (OPENMEDIA_DX12_VIDEO)
    target_sources(OpenMedia PRIVATE include/openmedia/hw_dx12.h)
    target_sources(OpenMedia PRIVATE src/hw_dx12.cpp)
    target_sources(OpenMedia PRIVATE src/video/dx12.cpp)
    target_link_libraries(OpenMedia PRIVATE d3d12.lib dxgi.lib dxva2.lib mf.lib mfplat.lib mfuuid.lib mfreadwrite.lib)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DX12_VIDEO)
endif ()

if (OPENMEDIA_FFMPEG)
    find_package(FFMPEG COMPONENTS avformat avcodec avutil)

    if (FFMPEG_FOUND)
        target_include_directories(OpenMedia PRIVATE ${FFMPEG_INCLUDE_DIRS})
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
                VERSION f93f4a26cea155f32d46c52290777d4ba6757dbd)
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

if (OPENMEDIA_OPUS)
    if (AUI_BOOT)
        auib_import(Opus https://github.com/aui-framework/opus
                VERSION df980346de0b5e14dbdb922951fae21f4ea6c1c0)
    elseif (NOT TARGET Opus::opus)
        add_subdirectory(third_party/external/opus)
    endif ()
    target_link_libraries(OpenMedia PRIVATE Opus::opus)
    target_sources(OpenMedia PRIVATE src/audio/opus.cpp)
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
    auib_import(libjpeg-turbo
            https://github.com/libjpeg-turbo/libjpeg-turbo
            VERSION 3.1.3
            CMAKE_ARGS -DCMAKE_INSTALL_DEFAULT_LIBDIR=lib)
    target_link_libraries(OpenMedia PRIVATE libjpeg-turbo::jpeg-static)
    target_sources(OpenMedia PRIVATE src/image/jpeg_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/jpeg_decoder.cpp)
endif ()

if (OPENMEDIA_BMP)
    target_sources(OpenMedia PRIVATE src/image/bmp_demuxer.cpp)
    target_sources(OpenMedia PRIVATE src/image/bmp_decoder.cpp)
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

if (OPENMEDIA_TIFF)
    if (AUI_BOOT)
        auib_import(TIFF https://github.com/libsdl-org/libtiff
                VERSION f324415f50cb5c90f7712e9dfe69831f5d2ea88d
                CMAKE_ARGS -Dtiff-tools=OFF -Dtiff-docs=OFF)
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
    target_sources(OpenMedia PRIVATE src/audio/wav_demuxer.cpp)
endif ()

if (OPENMEDIA_MP3)
    target_sources(OpenMedia PRIVATE src/audio/mp3_decoder.cpp)
    target_sources(OpenMedia PRIVATE src/audio/mp3_demuxer.cpp)
endif ()

if (OPENMEDIA_DAV1D)
    if (AUI_BOOT)
        auib_import(dav1d https://github.com/Nelonn/dav1d
                VERSION 952d5b5db330928908e733d423675b335690c94e)
    elseif (NOT TARGET dav1d::dav1d)
        add_subdirectory(third_party/external/dav1d)
    endif ()
    target_link_libraries(OpenMedia PRIVATE dav1d::dav1d)
    target_sources(OpenMedia PRIVATE src/video/dav1d.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_DAV1D)
endif ()

if (OPENMEDIA_OPENH264)
    target_sources(OpenMedia PRIVATE src/video/openh264.cpp)
    target_compile_definitions(OpenMedia PRIVATE OPENMEDIA_OPENH264)
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
