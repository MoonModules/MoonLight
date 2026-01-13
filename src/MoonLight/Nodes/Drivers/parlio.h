/**
    @title     MoonLight
    @file      parlio.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once
#include <stdint.h>  // uint8...

#if FT_MOONLIGHT

uint8_t show_parlio(uint8_t* parallelPins, uint32_t length, uint8_t* buffer_in, uint8_t components, uint8_t outputs, uint16_t* leds_per_output, uint8_t offsetR, uint8_t offsetG, uint8_t offsetB, uint8_t offsetW);

#endif