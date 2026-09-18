#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace openmedia {

// The numeric genre list ID3v1 defined and Winamp extended. It long outlived
// ID3v1 itself: MP4's `gnre` atom, WMA and several tagging tools still store a
// genre as an index into it, so the table is shared rather than owned by any
// one demuxer.
//
// Entries 0-79 are the original ID3v1 set; 80-125 are the Winamp extension.
// The list continues past 125 in the wild, but those later entries are
// inconsistently implemented and one of them is an ethnic slur, so lookup stops
// here and callers fall back to reporting the raw number.
inline constexpr std::string_view ID3V1_GENRES[] = {
    "Blues",             "Classic Rock",     "Country",           "Dance",
    "Disco",             "Funk",             "Grunge",            "Hip-Hop",
    "Jazz",              "Metal",            "New Age",           "Oldies",
    "Other",             "Pop",              "R&B",               "Rap",
    "Reggae",            "Rock",             "Techno",            "Industrial",
    "Alternative",       "Ska",              "Death Metal",       "Pranks",
    "Soundtrack",        "Euro-Techno",      "Ambient",           "Trip-Hop",
    "Vocal",             "Jazz+Funk",        "Fusion",            "Trance",
    "Classical",         "Instrumental",     "Acid",              "House",
    "Game",              "Sound Clip",       "Gospel",            "Noise",
    "Alternative Rock",  "Bass",             "Soul",              "Punk",
    "Space",             "Meditative",       "Instrumental Pop",  "Instrumental Rock",
    "Ethnic",            "Gothic",           "Darkwave",          "Techno-Industrial",
    "Electronic",        "Pop-Folk",         "Eurodance",         "Dream",
    "Southern Rock",     "Comedy",           "Cult",              "Gangsta",
    "Top 40",            "Christian Rap",    "Pop/Funk",          "Jungle",
    "Native American",   "Cabaret",          "New Wave",          "Psychedelic",
    "Rave",              "Showtunes",        "Trailer",           "Lo-Fi",
    "Tribal",            "Acid Punk",        "Acid Jazz",         "Polka",
    "Retro",             "Musical",          "Rock & Roll",       "Hard Rock",
    "Folk",              "Folk-Rock",        "National Folk",     "Swing",
    "Fast Fusion",       "Bebob",            "Latin",             "Revival",
    "Celtic",            "Bluegrass",        "Avantgarde",        "Gothic Rock",
    "Progressive Rock",  "Psychedelic Rock", "Symphonic Rock",    "Slow Rock",
    "Big Band",          "Chorus",           "Easy Listening",    "Acoustic",
    "Humour",            "Speech",           "Chanson",           "Opera",
    "Chamber Music",     "Sonata",           "Symphony",          "Booty Bass",
    "Primus",            "Porn Groove",      "Satire",            "Slow Jam",
    "Club",              "Tango",            "Samba",             "Folklore",
    "Ballad",            "Power Ballad",     "Rhythmic Soul",     "Freestyle",
    "Duet",              "Punk Rock",        "Drum Solo",         "A Capella",
    "Euro-House",        "Dance Hall",
};

// Empty when the index names nothing this table knows.
inline auto id3v1GenreName(size_t index) -> std::string_view {
  return index < std::size(ID3V1_GENRES) ? ID3V1_GENRES[index] : std::string_view {};
}

} // namespace openmedia
