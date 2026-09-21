#pragma once

#include <openmedia/dictionary.hpp>

namespace openmedia {

// string - Title of the work
constexpr Key TITLE = "title";
// string - Performing artist
constexpr Key ARTIST = "artist";
// string - Artist the album as a whole is credited to
constexpr Key ALBUM_ARTIST = "album_artist";
// string - Album or collection this belongs to
constexpr Key ALBUM = "album";
// string - Release date, as written in the file; often just a year
constexpr Key DATE = "date";
// string - Genre name
constexpr Key GENRE = "genre";
// string - Free-form comment
constexpr Key COMMENT = "comment";
// string - Composer
constexpr Key COMPOSER = "composer";
// string - Software that produced the file
constexpr Key ENCODER = "encoder";
// string - Copyright notice
constexpr Key COPYRIGHT = "copyright";
// string - Lyrics
constexpr Key LYRICS = "lyrics";
// string - Longer description or synopsis
constexpr Key DESCRIPTION = "description";
// string - Work or grouping the piece belongs to
constexpr Key GROUPING = "grouping";
// string - Publisher or label
constexpr Key PUBLISHER = "publisher";
// string - ISO 639-2/T language code of a track
constexpr Key LANGUAGE = "language";
// string - When the file itself was written, as an ISO 8601 UTC timestamp.
// Distinct from DATE, which is when the content was released.
constexpr Key CREATION_TIME = "creation_time";

// int32 - Position of this track within its album
constexpr Key TRACK_NUMBER = "track";
// int32 - How many tracks the album has, when stated
constexpr Key TRACK_TOTAL = "track_total";
// int32 - Position of this disc within a set
constexpr Key DISC_NUMBER = "disc";
// int32 - How many discs the set has, when stated
constexpr Key DISC_TOTAL = "disc_total";
// int32 - Tempo in beats per minute
constexpr Key BPM = "bpm";
// bool - Part of a compilation rather than a single artist's album
constexpr Key COMPILATION = "compilation";

// Sort keys, for collation that differs from the displayed text
constexpr Key SORT_TITLE = "sort_title";
constexpr Key SORT_ARTIST = "sort_artist";
constexpr Key SORT_ALBUM = "sort_album";
constexpr Key SORT_ALBUM_ARTIST = "sort_album_artist";

// binary - Cover artwork, in whatever format the file stored it
constexpr Key COVER_ART = "cover_art";
// string - Media type of COVER_ART, e.g. "image/jpeg"
constexpr Key COVER_ART_MIME = "cover_art.mime";

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

// Images and animations
// binary - Embedded ICC colour profile
constexpr Key ICC_PROFILE = "icc_profile";
// binary - Raw EXIF block, without any container-specific framing
constexpr Key EXIF = "exif";
// binary - XMP packet, as UTF-8 XML
constexpr Key XMP = "xmp";
// int32 - How many times an animation repeats; 0 means forever
constexpr Key ANIMATION_LOOP_COUNT = "animation.loop_count";
// int32 - Canvas colour behind an animation, as 0xAARRGGBB
constexpr Key ANIMATION_BACKGROUND_COLOR = "animation.background_color";

// bool - Track samples are encrypted and need a decryption layer before decode
constexpr Key ENCRYPTED = "encryption.encrypted";
// string - Four-character protection scheme, e.g. "cenc", "cbcs"
constexpr Key ENCRYPTION_SCHEME = "encryption.scheme";

} // namespace openmedia
