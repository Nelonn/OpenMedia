#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <openmedia/dictionary.hpp>
#include <openmedia/metadata_keys.hpp>
#include <span>
#include <string>
#include <string_view>
#include <util/byte_reader.hpp>
#include <util/id3_parser.hpp>
#include <vector>

namespace openmedia {

inline auto base64Decode(std::string_view in) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  out.reserve((in.size() * 3) / 4);
  int val = 0;
  int valb = -8;
  for (char c : in) {
    int d = -1;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '+') d = 62;
    else if (c == '/') d = 63;
    else if (c == '=') break;
    if (d >= 0) {
      val = (val << 6) | d;
      valb += 6;
      if (valb >= 0) {
        out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
        valb -= 8;
      }
    }
  }
  return out;
}

inline void parseFlacPictureBlock(std::span<const uint8_t> body, Dictionary& metadata) {
  if (body.size() < 32) return;
  ByteReader r(body);
  uint32_t picture_type = r.u32be();
  uint32_t mime_len = r.u32be();
  std::string_view mime = r.str(mime_len);
  uint32_t desc_len = r.u32be();
  r.skip(desc_len);
  r.skip(16);
  uint32_t data_len = r.u32be();
  auto data = r.bytes(data_len);
  if (!r.ok() || data.empty()) return;
  if (picture_type == 3 || !metadata.contains(COVER_ART)) {
    metadata.setBinary(COVER_ART, data);
    if (!mime.empty()) {
      metadata.setString(COVER_ART_MIME, mime);
    }
  }
}

inline void parseVorbisComment(std::span<const uint8_t> data, Dictionary& metadata) {
  if (data.size() < 4) return;
  ByteReader r(data);
  uint32_t vendor_len = r.u32le();
  std::string_view vendor = r.str(vendor_len);
  if (r.ok() && !vendor.empty() && !metadata.contains(ENCODER)) {
    metadata.setString(ENCODER, vendor);
  }
  if (!r.ok() || r.remaining() < 4) return;
  uint32_t user_comment_list_len = r.u32le();
  for (uint32_t i = 0; i < user_comment_list_len && r.ok(); ++i) {
    if (r.remaining() < 4) break;
    uint32_t comment_len = r.u32le();
    std::string_view comment = r.str(comment_len);
    if (!r.ok()) break;
    size_t eq = comment.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view name = comment.substr(0, eq);
    std::string_view value = comment.substr(eq + 1);
    if (value.empty()) continue;

    if (iequals(name, "TITLE")) {
      metadata.setString(TITLE, value);
    } else if (iequals(name, "ARTIST")) {
      metadata.setString(ARTIST, value);
    } else if (iequals(name, "ALBUMARTIST") || iequals(name, "ALBUM_ARTIST") || iequals(name, "ALBUM ARTIST")) {
      metadata.setString(ALBUM_ARTIST, value);
    } else if (iequals(name, "ALBUM")) {
      metadata.setString(ALBUM, value);
    } else if (iequals(name, "DATE") || iequals(name, "YEAR")) {
      if (!metadata.contains(DATE)) {
        metadata.setString(DATE, value);
      }
    } else if (iequals(name, "GENRE")) {
      metadata.setString(GENRE, value);
    } else if (iequals(name, "COMMENT") || iequals(name, "DESCRIPTION")) {
      metadata.setString(COMMENT, value);
    } else if (iequals(name, "COMPOSER")) {
      metadata.setString(COMPOSER, value);
    } else if (iequals(name, "ENCODER") || iequals(name, "VENDOR") || iequals(name, "ENCODED_BY") || iequals(name, "ENCODED-BY")) {
      metadata.setString(ENCODER, value);
    } else if (iequals(name, "COPYRIGHT")) {
      metadata.setString(COPYRIGHT, value);
    } else if (iequals(name, "LYRICS")) {
      metadata.setString(LYRICS, value);
    } else if (iequals(name, "PUBLISHER") || iequals(name, "ORGANIZATION")) {
      metadata.setString(PUBLISHER, value);
    } else if (iequals(name, "LANGUAGE")) {
      metadata.setString(LANGUAGE, value);
    } else if (iequals(name, "TRACKNUMBER") || iequals(name, "TRACK_NUMBER") || iequals(name, "TRACK")) {
      parseNumberPair(value, TRACK_NUMBER, TRACK_TOTAL, metadata);
    } else if (iequals(name, "TRACKTOTAL") || iequals(name, "TRACK_TOTAL") || iequals(name, "TOTALTRACKS")) {
      int total = 0;
      auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), total);
      if (ec == std::errc {} && total > 0) metadata.setInt32(TRACK_TOTAL, total);
    } else if (iequals(name, "DISCNUMBER") || iequals(name, "DISC_NUMBER") || iequals(name, "DISC")) {
      parseNumberPair(value, DISC_NUMBER, DISC_TOTAL, metadata);
    } else if (iequals(name, "DISCTOTAL") || iequals(name, "DISC_TOTAL") || iequals(name, "TOTALDISCS")) {
      int total = 0;
      auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), total);
      if (ec == std::errc {} && total > 0) metadata.setInt32(DISC_TOTAL, total);
    } else if (iequals(name, "BPM") || iequals(name, "TEMPO")) {
      int bpm = 0;
      auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), bpm);
      if (ec == std::errc {} && bpm > 0) metadata.setInt32(BPM, bpm);
    } else if (iequals(name, "COMPILATION")) {
      metadata.setBool(COMPILATION, value == "1");
    } else if (iequals(name, "SORT_TITLE") || iequals(name, "TITLESORT")) {
      metadata.setString(SORT_TITLE, value);
    } else if (iequals(name, "SORT_ARTIST") || iequals(name, "ARTISTSORT")) {
      metadata.setString(SORT_ARTIST, value);
    } else if (iequals(name, "SORT_ALBUM") || iequals(name, "ALBUMSORT")) {
      metadata.setString(SORT_ALBUM, value);
    } else if (iequals(name, "SORT_ALBUM_ARTIST") || iequals(name, "ALBUMARTISTSORT")) {
      metadata.setString(SORT_ALBUM_ARTIST, value);
    } else if (iequals(name, "METADATA_BLOCK_PICTURE")) {
      std::vector<uint8_t> pic_bytes = base64Decode(value);
      if (!pic_bytes.empty()) {
        parseFlacPictureBlock(pic_bytes, metadata);
      }
    }
  }
}

}
