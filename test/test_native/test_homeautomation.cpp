/**
    @title     MoonLight Unit Tests — ModuleHomeAutomation
    @file      test_homeautomation.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for the pure color-conversion helpers in ModuleHomeAutomation.
    Functions are copied here to avoid ESP32/HomeSpan header dependencies.
    Run with: pio test -e native
**/

#include "doctest.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

using std::max;
using std::min;
using std::fabs;
using std::fmod;

// ============================================================
// Copied from ModuleHomeAutomation.h — keep in sync
// ============================================================

static void rgbToHsv(uint8_t r, uint8_t g, uint8_t b, float &h, float &s, float &v) {
  float rf=r/255.f, gf=g/255.f, bf=b/255.f;
  float mx=max(max(rf,gf),bf), mn=min(min(rf,gf),bf), d=mx-mn;
  v=mx; s=(mx>0)?d/mx:0;
  if(d<1e-6f){h=0;} else if(mx==rf){h=60.f*fmod((gf-bf)/d,6.f);} else if(mx==gf){h=60.f*((bf-rf)/d+2);} else{h=60.f*((rf-gf)/d+4);}
  if(h<0)h+=360.f;
}

static void hsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
  float c=v*s, x=c*(1.f-fabs(fmod(h/60.f,2.f)-1.f)), m=v-c, rf,gf,bf;
  if(h<60){rf=c;gf=x;bf=0;} else if(h<120){rf=x;gf=c;bf=0;} else if(h<180){rf=0;gf=c;bf=x;}
  else if(h<240){rf=0;gf=x;bf=c;} else if(h<300){rf=x;gf=0;bf=c;} else{rf=c;gf=0;bf=x;}
  r=(uint8_t)((rf+m)*255.5f); g=(uint8_t)((gf+m)*255.5f); b=(uint8_t)((bf+m)*255.5f);
}

// ============================================================
// Tests
// ============================================================

TEST_CASE("rgbToHsv: pure red") {
  float h, s, v;
  rgbToHsv(255, 0, 0, h, s, v);
  CHECK(h == doctest::Approx(0.f).epsilon(1.f));   // 0° or 360°
  CHECK(s == doctest::Approx(1.f).epsilon(0.01f));
  CHECK(v == doctest::Approx(1.f).epsilon(0.01f));
}

TEST_CASE("rgbToHsv: pure green") {
  float h, s, v;
  rgbToHsv(0, 255, 0, h, s, v);
  CHECK(h == doctest::Approx(120.f).epsilon(1.f));
  CHECK(s == doctest::Approx(1.f).epsilon(0.01f));
  CHECK(v == doctest::Approx(1.f).epsilon(0.01f));
}

TEST_CASE("rgbToHsv: pure blue") {
  float h, s, v;
  rgbToHsv(0, 0, 255, h, s, v);
  CHECK(h == doctest::Approx(240.f).epsilon(1.f));
  CHECK(s == doctest::Approx(1.f).epsilon(0.01f));
  CHECK(v == doctest::Approx(1.f).epsilon(0.01f));
}

TEST_CASE("rgbToHsv: black (epsilon edge case — d≈0)") {
  float h, s, v;
  rgbToHsv(0, 0, 0, h, s, v);
  CHECK(h == doctest::Approx(0.f).epsilon(0.01f));
  CHECK(s == doctest::Approx(0.f).epsilon(0.01f));
  CHECK(v == doctest::Approx(0.f).epsilon(0.01f));
}

TEST_CASE("rgbToHsv: white (epsilon edge case — d≈0)") {
  float h, s, v;
  rgbToHsv(255, 255, 255, h, s, v);
  CHECK(h == doctest::Approx(0.f).epsilon(0.01f));
  CHECK(s == doctest::Approx(0.f).epsilon(0.01f));
  CHECK(v == doctest::Approx(1.f).epsilon(0.01f));
}

TEST_CASE("rgbToHsv: grey (epsilon edge case — d≈0)") {
  float h, s, v;
  rgbToHsv(128, 128, 128, h, s, v);
  CHECK(s == doctest::Approx(0.f).epsilon(0.01f));
}

TEST_CASE("hsvToRgb: pure red") {
  uint8_t r, g, b;
  hsvToRgb(0.f, 1.f, 1.f, r, g, b);
  CHECK(r == 255); CHECK(g == 0); CHECK(b == 0);
}

TEST_CASE("hsvToRgb: pure green") {
  uint8_t r, g, b;
  hsvToRgb(120.f, 1.f, 1.f, r, g, b);
  CHECK(r == 0); CHECK(g == 255); CHECK(b == 0);
}

TEST_CASE("hsvToRgb: pure blue") {
  uint8_t r, g, b;
  hsvToRgb(240.f, 1.f, 1.f, r, g, b);
  CHECK(r == 0); CHECK(g == 0); CHECK(b == 255);
}

TEST_CASE("hsvToRgb: black") {
  uint8_t r, g, b;
  hsvToRgb(0.f, 0.f, 0.f, r, g, b);
  CHECK(r == 0); CHECK(g == 0); CHECK(b == 0);
}

TEST_CASE("hsvToRgb: white") {
  uint8_t r, g, b;
  hsvToRgb(0.f, 0.f, 1.f, r, g, b);
  CHECK(r == 255); CHECK(g == 255); CHECK(b == 255);
}

TEST_CASE("round-trip RGB→HSV→RGB: orange") {
  uint8_t ri=255, gi=128, bi=0, ro, go, bo;
  float h, s, v;
  rgbToHsv(ri, gi, bi, h, s, v);
  hsvToRgb(h, s, v, ro, go, bo);
  CHECK(abs((int)ri-(int)ro) <= 1);
  CHECK(abs((int)gi-(int)go) <= 1);
  CHECK(abs((int)bi-(int)bo) <= 1);
}

TEST_CASE("round-trip RGB→HSV→RGB: teal") {
  uint8_t ri=0, gi=128, bi=128, ro, go, bo;
  float h, s, v;
  rgbToHsv(ri, gi, bi, h, s, v);
  hsvToRgb(h, s, v, ro, go, bo);
  CHECK(abs((int)ri-(int)ro) <= 1);
  CHECK(abs((int)gi-(int)go) <= 1);
  CHECK(abs((int)bi-(int)bo) <= 1);
}
