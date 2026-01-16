/**
    @title     MoonLight
    @file      Hub75.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

class HUB75Driver : public Node {
 public:
  static const char* name() { return "HUB75 Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️🚧"; }

  void setup() override {}
  void loop() override {};

  bool hasOnLayout() const override { return true; }
  void onLayout() override {};
};

#endif