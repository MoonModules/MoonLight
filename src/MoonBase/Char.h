/**
    @title     MoonBase
    @file      Char.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#include "ArduinoJson.h"

// See https://discord.com/channels/473448917040758787/718943978636050542/1357670679196991629
template <size_t N>
struct Char {
  char s[N] = "";

  // Constructors
  Char() = default;                                      // Keep default constructor
  Char(const char* str) { strlcpy(s, str, sizeof(s)); }  // Constructor to allow initialization from string literal
  template <size_t M>
  Char(const Char<M>& other) {  // Converting constructor from different-sized Char
    strlcpy(s, other.c_str(), sizeof(s));
  }

  // assign operators
  Char& operator=(const char* rhs) {
    // if (strlen(rhs) >= N) EXT_LOGW("Char", "Truncating '%s' from %d to %d chars", rhs, strlen(rhs), N - 1);
    strlcpy(s, rhs, sizeof(s));
    return *this;
  }
  Char& operator=(const JsonVariant rhs) {
    if (!rhs.isNull())
      return (*this = rhs.as<String>().c_str());  //.as<String>().c_str() as rhs can also contain non strings
    else
      return (*this = "");
  }
  Char& operator=(const JsonString rhs) {
    if (!rhs.isNull())
      return (*this = rhs.c_str());
    else
      return (*this = "");
  }
  Char& operator=(const String& rhs) { return (*this = rhs.c_str()); }
  // FIX: Make this accept ANY size Char, not just same size
  template <size_t M>
  Char& operator=(const Char<M>& rhs) {
    return (*this = rhs.c_str());
  }

  // conversion
  // operator const char*() const { return s; }
  // Or explicit to avoid implicit conversions:
  explicit operator const char*() const { return s; }

  Char operator+(const char* rhs) const {
    Char result(*this);
    result += rhs;
    return result;
  }
  Char& operator+=(const char* rhs) {
    strlcat(s, rhs, sizeof(s));
    return *this;
  }
  Char& operator+=(const int rhs) {  // add integer to string
    char buffer[12];                 // enough for 32-bit int
    snprintf(buffer, sizeof(buffer), "%d", rhs);
    strlcat(s, buffer, sizeof(s));
    return *this;
  }
  Char& operator+=(const String& rhs) {
    strlcat(s, rhs.c_str(), sizeof(s));
    return *this;
  }
  template <size_t M>
  Char& operator+=(const Char<M>& rhs) {
    strlcat(s, rhs.c_str(), sizeof(s));
    return *this;
  }

  // compare operators
  bool operator==(const char* rhs) const { return strcmp(s, rhs) == 0; }
  bool operator==(const Char& rhs) const { return strcmp(s, rhs.s) == 0; }
  bool operator!=(const char* rhs) const { return strcmp(s, rhs) != 0; }

  char operator[](const uint16_t indexV) const { return (indexV < sizeof(s)) ? s[indexV] : '\0'; }

  Char<N> substring(uint16_t begin, uint16_t end = sizeof(s) - 1) {
    Char<N> sub;
    if (begin >= sizeof(s) || end >= sizeof(s) || end < begin)
      sub = "";
    else {
      strlcpy(sub.s, s + begin, end - begin + 1);
    }
    return sub;
  }

  size_t length() const { return strnlen(s, sizeof(s)); }
  int toInt() const { return atoi(s); }
  float toFloat() const { return atof(s); }
  bool contains(const char* rhs) const { return strnstr(s, rhs, sizeof(s)) != nullptr; }
  size_t indexOf(const char* token) const {
    const char* pos = strnstr(s, token, sizeof(s));
    return pos ? (pos - s) : SIZE_MAX;
  }
  const char* c_str() const { return s; }

  Char& format(const char* format, ...) {
    va_list args;
    va_start(args, format);
    size_t len = vsnprintf(s, sizeof(s), format, args);
    va_end(args);
    return *this;
  }

  void split(const char* splitter, std::function<void(const char*, uint8_t)> callback) {
    char savedS[N];
    strlcpy(savedS, s, N);              // save s
    char* token = strtok(s, splitter);  // eat s
    uint8_t sequence = 0;
    while (token != nullptr) {
      callback(token, sequence);
      sequence++;
      token = strtok(nullptr, splitter);
    }
    strlcpy(s, savedS, N);  // restore s
  }

  void formatTime(time_t time, const char* format) {
    strftime(s, N, format, localtime(&time));  // or gmtime
  }

  // for parsing strings
  const char* eat() {
    // todo
    return nullptr;
  }
};

// ADD: Non-member operator+ for string + Char
// template outside the class
template <size_t N>
Char<N> operator+(const char* lhs, const Char<N>& rhs) {
  Char<N> result;
  strlcpy(result.s, lhs, sizeof(result.s));
  strlcat(result.s, rhs.c_str(), sizeof(result.s));
  return result;
}

// Example of split:
// Char<32> test;
// test = "one - two - three";
// test.split(" - ", [](const char *token) {
//     EXT_LOGV(MB_TAG, "token: %s", token);
// });

// convenience function to compare two char strings
inline bool equal(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return strcmp(a, b) == 0;
}

// compare two char strings, ignoring non-alphanumeric characters
inline bool equalAZaz09(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;

  while (*a || *b) {
    while (*a && !((*a >= '0' && *a <= '9') || (*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z'))) a++;
    while (*b && !((*b >= '0' && *b <= '9') || (*b >= 'A' && *b <= 'Z') || (*b >= 'a' && *b <= 'z'))) b++;
    if (*a != *b) return false;
    if (*a) {
      a++;
      b++;
    }
  }
  return true;
}

inline bool contains(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return strstr(a, b) != nullptr;
}
