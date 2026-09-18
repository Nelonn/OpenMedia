#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace openmedia {

// Containers each count from their own epoch - Matroska from 2001, MP4 from
// 1904 - so the conversion to a civil date is shared rather than reimplemented
// per demuxer.
namespace date_time {

struct CivilDate {
  int64_t year;
  unsigned month;
  unsigned day;
};

// Days since 1970-01-01 to a civil date, by the usual era-based algorithm.
// Correct for any date in the proleptic Gregorian calendar.
inline auto civilFromDays(int64_t days) -> CivilDate {
  days += 719'468;
  const int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
  const int64_t day_of_era = days - era * 146'097;
  const int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
  const int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const int64_t mp = (5 * day_of_year + 2) / 153;

  const auto day = static_cast<unsigned>(day_of_year - (153 * mp + 2) / 5 + 1);
  const auto month = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  const int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);

  return {year, month, day};
}

// Seconds since the Unix epoch as an ISO 8601 UTC timestamp. Negative values
// work: the division floors, so a time before 1970 lands on the right day.
inline auto formatIso8601Utc(int64_t unix_seconds) -> std::string {
  constexpr int64_t SECONDS_PER_DAY = 86'400;

  int64_t days = unix_seconds / SECONDS_PER_DAY;
  int64_t seconds_of_day = unix_seconds % SECONDS_PER_DAY;
  if (seconds_of_day < 0) {
    seconds_of_day += SECONDS_PER_DAY;
    --days;
  }

  const CivilDate date = civilFromDays(days);
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", date.year, date.month,
                     date.day, seconds_of_day / 3600, (seconds_of_day / 60) % 60,
                     seconds_of_day % 60);
}

} // namespace date_time
} // namespace openmedia
