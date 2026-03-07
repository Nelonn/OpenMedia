
if (NOT APPLE)
    set(NOT_APPLE ON)
endif ()

option(AUI_BOOT "AUI Boot compatibility" ON)

option(OPENMEDIA_INSTALL "Install OpenMedia" ON)

# Universal
option(OPENMEDIA_FFMPEG "FFmpeg" ${LINUX})

# Container
option(OPENMEDIA_MATROSKA "MKV/MKA/WEBM" ON)

# Audio
option(OPENMEDIA_FDK_AAC "FDK-AAC" OFF) # patent
option(OPENMEDIA_ALAC "ALAC" ON)
option(OPENMEDIA_FLAC "FLAC" ON)
option(OPENMEDIA_OGG "OGG" ON)
option(OPENMEDIA_OPUS "OPUS" ON)
option(OPENMEDIA_VORBIS "VORBIS" ON)
option(OPENMEDIA_WAV "WAV" ON)
option(OPENMEDIA_MP3 "MP3" ON)

# Image
option(OPENMEDIA_BMP "BMP" ON)
option(OPENMEDIA_GIF "GIF" ON)
option(OPENMEDIA_PNG "PNG" ON)
option(OPENMEDIA_TIFF "TIFF" ON)
option(OPENMEDIA_WEBP "WEBP" ON)
option(OPENMEDIA_EXR "EXR" OFF)
option(OPENMEDIA_JPEG "JPEG" ON)
option(OPENMEDIA_TGA "TGA" ON)

# Video
option(OPENMEDIA_OPENH264 "OpenH264" OFF)
option(OPENMEDIA_DAV1D "dav1d" ON)
option(OPENMEDIA_VULKAN_VIDEO "Vulkan Video" ${NOT_APPLE})
option(OPENMEDIA_DX11_VIDEO "DirectX11 Video" ${WIN32})
option(OPENMEDIA_DX12_VIDEO "DirectX12 Video" ${WIN32})
option(OPENMEDIA_AMF "AMD AMF" ${WIN32}) # Also available on Linux, but everyone uses opensource driver

option(OPENMEDIA_EXAMPLE_PLAYER "Enable example player" OFF)
