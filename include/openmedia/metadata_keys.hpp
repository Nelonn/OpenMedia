#pragma once

#include <openmedia/dictionary.hpp>

namespace openmedia {

// Dolby Vision
// bool - Track carries Dolby Vision metadata or uses a Dolby Vision sample entry
constexpr Key DOLBY_VISION_PRESENT = "dolby_vision.present";
// string - BMFF sample entry fourcc (dvh1/dvhe/dva1/dvav), when present.
constexpr Key DOLBY_VISION_SAMPLE_ENTRY = "dolby_vision.sample_entry";
// int32 - Matroska BlockAddID value for Dolby Vision block additions
constexpr Key DOLBY_VISION_BLOCK_ADD_ID = "dolby_vision.block_add_id";
// binary - Raw dvcC/dvvC Dolby Vision configuration record
constexpr Key DOLBY_VISION_CONFIG = "dolby_vision.config";
// int32 - Dolby Vision configuration version major
constexpr Key DOLBY_VISION_VERSION_MAJOR = "dolby_vision.version_major";
// int32 - Dolby Vision configuration version minor
constexpr Key DOLBY_VISION_VERSION_MINOR = "dolby_vision.version_minor";
// int32 - Dolby Vision profile parsed from dvcC/dvvC
constexpr Key DOLBY_VISION_PROFILE = "dolby_vision.profile";
// int32 - Dolby Vision level parsed from dvcC/dvvC
constexpr Key DOLBY_VISION_LEVEL = "dolby_vision.level";
// bool - Reference Processing Unit metadata is present
constexpr Key DOLBY_VISION_RPU_PRESENT = "dolby_vision.rpu_present";
// bool - Enhancement layer is present
constexpr Key DOLBY_VISION_EL_PRESENT = "dolby_vision.el_present";
// bool - Base layer is present
constexpr Key DOLBY_VISION_BL_PRESENT = "dolby_vision.bl_present";
// int32 - Base-layer signal compatibility id, when present in dvcC/dvvC
constexpr Key DOLBY_VISION_BL_SIGNAL_COMPATIBILITY_ID = "dolby_vision.bl_signal_compatibility_id";

} // namespace openmedia
