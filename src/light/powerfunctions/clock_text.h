#pragma once

#include "platform/platform_clock.h"   // local civil time, spelled per platform

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/// Clock tokens in the shape WLED's scrolling text reads. `#HH:#MM` is the hour and the minute.
namespace mm {

/// A civil clock reading. `month` is 1 to 12 and `weekday` is 0 for Sunday, matching `struct tm`.
struct Civil {
    /// Four-digit year.
    int year = 1970;
    /// Month number. January is 1.
    int month = 1;
    /// Day of the month.
    int day = 1;
    /// Hour from midnight.
    int hour = 0;
    /// Minute past the hour.
    int minute = 0;
    /// Second past the minute.
    int second = 0;
    /// Weekday. Sunday is 0.
    int weekday = 0;
};

/// The device's local civil time. A board whose clock was never set reads the epoch.
inline Civil civilNow() {
    std::tm tm{};
    if (!platform::localTm(tm)) return {};
    Civil c;
    c.year = tm.tm_year + 1900;
    c.month = tm.tm_mon + 1;
    c.day = tm.tm_mday;
    c.hour = tm.tm_hour;
    c.minute = tm.tm_min;
    c.second = tm.tm_sec;
    c.weekday = tm.tm_wday;
    return c;
}

/// One token, longest key first, so a shorter key matches only when no longer one fits.
struct ClockTok {
    /// The key, including its #.
    const char* key;
    /// Length of `key`.
    uint8_t len;
    /// Which writer the key selects.
    uint8_t kind;
};

/// Short month names, January first.
inline constexpr const char* kClockMon[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
/// Full month names, January first.
inline constexpr const char* kClockMonL[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"};
/// Short weekday names, Sunday first.
inline constexpr const char* kClockDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
/// Full weekday names, Sunday first.
inline constexpr const char* kClockDayL[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
/// The token table, longest key first.
inline constexpr ClockTok kClockToks[] = {
    {"#DATE", 5, 0}, {"#DDMM", 5, 1}, {"#MMDD", 5, 2}, {"#TIME", 5, 3},
    {"#HHMM", 5, 4}, {"#YYYY", 5, 5}, {"#MONL", 5, 6}, {"#DDDD", 5, 7},
    {"#MON", 4, 8}, {"#DAY", 4, 9},
    {"#YY", 3, 10}, {"#HH", 3, 11}, {"#MM", 3, 12}, {"#SS", 3, 13},
    {"#MO", 3, 14}, {"#DD", 3, 15},
};

/// A month number kept inside January to December. Anything else reads as January.
inline int clockMonth(int month) {
    return (month >= 1 && month <= 12) ? month : 1;
}

/// A weekday kept inside Sunday to Saturday. Anything else reads as Sunday.
inline int clockWeekday(int weekday) {
    return (weekday >= 0 && weekday <= 6) ? weekday : 0;
}

/// The hour a token prints. With `ampm`, 0 becomes 12 and `pm` records the half of the day.
inline int clockHour(const Civil& clock, bool ampm, bool& pm) {
    pm = false;
    int hour = clock.hour;
    if (!ampm) return hour;
    pm = hour >= 12;
    hour %= 12;
    return hour == 0 ? 12 : hour;
}

/// Fold the token at `p` into uppercase in `window`. Returns how many characters were read.
inline int foldToken(char* window, const char* p) {
    int avail = 0;
    while (avail < 6 && p[avail]) {
        const unsigned char ch = static_cast<unsigned char>(p[avail]);
        window[avail] = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        ++avail;
    }
    window[avail] = '\0';
    return avail;
}

/// The token `window` starts, or null when none of the keys fit.
inline const ClockTok* findClockTok(const char* window, int avail) {
    for (const ClockTok& t : kClockToks)
        if (avail >= t.len && std::strncmp(window, t.key, t.len) == 0) return &t;
    return nullptr;
}

/// The fields a token prints, derived once from the civil reading.
struct ClockParts {
    /// Month number clamped to 1 to 12.
    int month = 1;
    /// Weekday clamped to 0 to 6.
    int wday = 0;
    /// Hour the token prints. 01–12 when `ampm` is set.
    int hour = 0;
    /// True when the hour is in the afternoon.
    bool pm = false;
    /// True when the hour is a 12-hour clock.
    bool ampm = false;
};

/// A date or a weekday token. Kinds outside that set leave `piece` empty.
inline void writeDateTok(char* piece, size_t cap, uint8_t kind, const Civil& clock, const ClockParts& parts) {
    switch (kind) {
    case 0: std::snprintf(piece, cap, "%02d.%02d.%04d", clock.day, parts.month, clock.year); break;
    case 1: std::snprintf(piece, cap, "%02d.%02d", clock.day, parts.month); break;
    case 2: std::snprintf(piece, cap, "%02d/%02d", parts.month, clock.day); break;
    case 5: std::snprintf(piece, cap, "%04d", clock.year); break;
    case 6: std::snprintf(piece, cap, "%s", kClockMonL[parts.month - 1]); break;
    case 7: std::snprintf(piece, cap, "%s", kClockDayL[parts.wday]); break;
    case 8: std::snprintf(piece, cap, "%s", kClockMon[parts.month - 1]); break;
    case 9: std::snprintf(piece, cap, "%s", kClockDay[parts.wday]); break;
    default: break;
    }
}

/// `#TIME`, with the half of the day when `ampm` is set.
inline void writeTimeOfDay(char* piece, size_t cap, const Civil& clock, const ClockParts& parts) {
    if (parts.ampm) std::snprintf(piece, cap, "%02d:%02d:%02d %s", parts.hour, clock.minute, clock.second, parts.pm ? "PM" : "AM");
    else std::snprintf(piece, cap, "%02d:%02d:%02d", parts.hour, clock.minute, clock.second);
}

/// A time token. Kinds outside that set leave `piece` empty.
inline void writeTimeTok(char* piece, size_t cap, uint8_t kind, const Civil& clock, const ClockParts& parts) {
    switch (kind) {
    case 3: writeTimeOfDay(piece, cap, clock, parts); break;
    case 4:  std::snprintf(piece, cap, "%02d:%02d", parts.hour, clock.minute); break;
    case 10: std::snprintf(piece, cap, "%02d", clock.year % 100); break;
    case 11: std::snprintf(piece, cap, "%02d", parts.hour); break;
    case 12: std::snprintf(piece, cap, "%02d", clock.minute); break;
    case 13: std::snprintf(piece, cap, "%02d", clock.second); break;
    case 14: std::snprintf(piece, cap, "%02d", parts.month); break;
    case 15: std::snprintf(piece, cap, "%02d", clock.day); break;
    default: break;
    }
}

/// The text of one token. Date kinds and time kinds have their own writers.
inline void writeClockTok(char* piece, size_t cap, uint8_t kind, const Civil& clock, const ClockParts& parts) {
    piece[0] = '\0';
    if (kind < 10 && kind != 3 && kind != 4) writeDateTok(piece, cap, kind, clock, parts);
    else writeTimeTok(piece, cap, kind, clock, parts);
}

/// Copy `piece` onto the end of `out`, stopping one byte short of `cap`.
inline void appendPiece(char* out, size_t cap, size_t& n, const char* piece) {
    for (const char* s = piece; *s && n + 1 < cap; ++s) out[n++] = *s;
}

/// Write `in` into `out`, expanding clock tokens. Two-digit fields are zero-padded. A trailing `0` is accepted and changes nothing. With `ampm`, `#HH` runs 01 to 12 and `#TIME` ends in AM or PM.
inline size_t expandClock(char* out, size_t cap, const char* in, const Civil& clock, bool ampm) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;

    ClockParts parts;
    parts.month = clockMonth(clock.month);
    parts.wday = clockWeekday(clock.weekday);
    parts.hour = clockHour(clock, ampm, parts.pm);
    parts.ampm = ampm;

    size_t n = 0;
    const char* p = in;
    while (*p && n + 1 < cap) {
        if (*p != '#') { out[n++] = *p++; continue; }
        char window[8];
        const ClockTok* hit = findClockTok(window, foldToken(window, p));
        if (!hit) { out[n++] = *p++; continue; }
        char piece[24];
        writeClockTok(piece, sizeof(piece), hit->kind, clock, parts);
        p += hit->len;
        if (*p == '0') ++p;   // WLED's padding mark; the fields above are already padded
        appendPiece(out, cap, n, piece);
    }
    out[n] = '\0';
    return n;
}

}  // namespace mm
