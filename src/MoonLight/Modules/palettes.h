/**
    @title     MoonLight
    @file      palette.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/livescripts/
    @Copyright © 2026 Github MoonLight Commit Authors, originated from WLED-MM.
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

/*
 * Color palettes for FastLED effects (65-73).
 */

// From ColorWavesWithPalettes by Mark Kriegsman: https://gist.github.com/kriegsman/8281905786e8b2632aeb
// Unfortunately, these are stored in RAM!

// Gradient palette "ib_jul01_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/ing/xmas/tn/ib_jul01.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 16 uint8_ts of program space.

#ifndef Palettes_h
#define Palettes_h

const uint8_t ib_jul01_gp[] = {0, 194, 1, 1, 94, 1, 29, 18, 132, 57, 131, 28, 255, 113, 1, 1};

// Gradient palette "es_vintage_57_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/vintage/tn/es_vintage_57.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t es_vintage_57_gp[] = {0, 2, 1, 1, 53, 18, 1, 0, 104, 69, 29, 1, 153, 167, 135, 10, 255, 46, 56, 4};

// Gradient palette "es_vintage_01_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/vintage/tn/es_vintage_01.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 32 uint8_ts of program space.

const uint8_t es_vintage_01_gp[] = {0, 4, 1, 1, 51, 16, 0, 1, 76, 97, 104, 3, 101, 255, 131, 19, 127, 67, 9, 4, 153, 16, 0, 1, 229, 4, 1, 1, 255, 4, 1, 1};

// Gradient palette "es_rivendell_15_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/rivendell/tn/es_rivendell_15.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t es_rivendell_15_gp[] = {0, 1, 14, 5, 101, 16, 36, 14, 165, 56, 68, 30, 242, 150, 156, 99, 255, 150, 156, 99};

// Gradient palette "rgi_15_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/ds/rgi/tn/rgi_15.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 36 uint8_ts of program space.
// Edited to be brighter

const uint8_t rgi_15_gp[] = {0, 4, 1, 70, 31, 55, 1, 30, 63, 255, 4, 7, 95, 59, 2, 29, 127, 11, 3, 50, 159, 39, 8, 60, 191, 112, 19, 40, 223, 78, 11, 39, 255, 29, 8, 59};

// Gradient palette "retro2_16_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/ma/retro2/tn/retro2_16.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 8 uint8_ts of program space.

const uint8_t retro2_16_gp[] = {0, 188, 135, 1, 255, 46, 7, 1};

// Gradient palette "Analogous_1_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/red/tn/Analogous_1.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t Analogous_1_gp[] = {0, 3, 0, 255, 63, 23, 0, 255, 127, 67, 0, 255, 191, 142, 0, 45, 255, 255, 0, 0};

// Gradient palette "es_pinksplash_08_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/pink_splash/tn/es_pinksplash_08.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t es_pinksplash_08_gp[] = {0, 126, 11, 255, 127, 197, 1, 22, 175, 210, 157, 172, 221, 157, 3, 112, 255, 157, 3, 112};

// Gradient palette "es_ocean_breeze_036_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/ocean_breeze/tn/es_ocean_breeze_036.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 16 uint8_ts of program space.

const uint8_t es_ocean_breeze_036_gp[] = {0, 1, 6, 7, 89, 1, 99, 111, 153, 144, 209, 255, 255, 0, 73, 82};

// Gradient palette "departure_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/mjf/tn/departure.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 88 uint8_ts of program space.

const uint8_t departure_gp[] = {0, 8, 3, 0, 42, 23, 7, 0, 63, 75, 38, 6, 84, 169, 99, 38, 106, 213, 169, 119, 116, 255, 255, 255, 138, 135, 255, 138, 148, 22, 255, 24, 170, 0, 255, 0, 191, 0, 136, 0, 212, 0, 55, 0, 255, 0, 55, 0};

// Gradient palette "es_landscape_64_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/landscape/tn/es_landscape_64.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 36 uint8_ts of program space.

const uint8_t es_landscape_64_gp[] = {0, 0, 0, 0, 37, 2, 25, 1, 76, 15, 115, 5, 127, 79, 213, 1, 128, 126, 211, 47, 130, 188, 209, 247, 153, 144, 182, 205, 204, 59, 117, 250, 255, 1, 37, 192};

// Gradient palette "es_landscape_33_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/landscape/tn/es_landscape_33.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 24 uint8_ts of program space.

const uint8_t es_landscape_33_gp[] = {0, 1, 5, 0, 19, 32, 23, 1, 38, 161, 55, 1, 63, 229, 144, 1, 66, 39, 142, 74, 255, 1, 4, 1};

// Gradient palette "rainbowsherbet_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/ma/icecream/tn/rainbowsherbet.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 28 uint8_ts of program space.

const uint8_t rainbowsherbet_gp[] = {0, 255, 33, 4, 43, 255, 68, 25, 86, 255, 7, 25, 127, 255, 82, 103, 170, 255, 255, 242, 209, 42, 255, 22, 255, 87, 255, 65};

// Gradient palette "gr65_hult_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/hult/tn/gr65_hult.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 24 uint8_ts of program space.

const uint8_t gr65_hult_gp[] = {0, 247, 176, 247, 48, 255, 136, 255, 89, 220, 29, 226, 160, 7, 82, 178, 216, 1, 124, 109, 255, 1, 124, 109};

// Gradient palette "gr64_hult_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/hult/tn/gr64_hult.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 32 uint8_ts of program space.

const uint8_t gr64_hult_gp[] = {0, 1, 124, 109, 66, 1, 93, 79, 104, 52, 65, 1, 130, 115, 127, 1, 150, 52, 65, 1, 201, 1, 86, 72, 239, 0, 55, 45, 255, 0, 55, 45};

// Gradient palette "GMT_drywet_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/gmt/tn/GMT_drywet.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 28 uint8_ts of program space.

const uint8_t GMT_drywet_gp[] = {0, 47, 30, 2, 42, 213, 147, 24, 84, 103, 219, 52, 127, 3, 219, 207, 170, 1, 48, 214, 212, 1, 1, 111, 255, 1, 7, 33};

// Gradient palette "ib15_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/ing/general/tn/ib15.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 24 uint8_ts of program space.

const uint8_t ib15_gp[] = {0, 113, 91, 147, 72, 157, 88, 78, 89, 208, 85, 33, 107, 255, 29, 11, 141, 137, 31, 39, 255, 59, 33, 89};

// Gradient palette "Tertiary_01_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/vermillion/tn/Tertiary_01.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t Tertiary_01_gp[] = {0, 0, 1, 255, 63, 3, 68, 45, 127, 23, 255, 0, 191, 100, 68, 1, 255, 255, 1, 4};

// Gradient palette "lava_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/neota/elem/tn/lava.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 52 uint8_ts of program space.

const uint8_t lava_gp[] = {0, 0, 0, 0, 46, 18, 0, 0, 96, 113, 0, 0, 108, 142, 3, 1, 119, 175, 17, 1, 146, 213, 44, 2, 174, 255, 82, 4, 188, 255, 115, 4, 202, 255, 156, 4, 218, 255, 203, 4, 234, 255, 255, 4, 244, 255, 255, 71, 255, 255, 255, 255};

// Gradient palette "fierce_ice_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/neota/elem/tn/fierce-ice.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 28 uint8_ts of program space.

const uint8_t fierce_ice_gp[] = {0, 0, 0, 0, 59, 0, 9, 45, 119, 0, 38, 255, 149, 3, 100, 255, 180, 23, 199, 255, 217, 100, 235, 255, 255, 255, 255, 255};

// Gradient palette "Colorfull_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/atmospheric/tn/Colorfull.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 44 uint8_ts of program space.

const uint8_t Colorfull_gp[] = {0, 10, 85, 5, 25, 29, 109, 18, 60, 59, 138, 42, 93, 83, 99, 52, 106, 110, 66, 64, 109, 123, 49, 65, 113, 139, 35, 66, 116, 192, 117, 98, 124, 255, 255, 137, 168, 100, 180, 155, 255, 22, 121, 174};

// Gradient palette "Pink_Purple_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/atmospheric/tn/Pink_Purple.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 44 uint8_ts of program space.

const uint8_t Pink_Purple_gp[] = {0, 19, 2, 39, 25, 26, 4, 45, 51, 33, 6, 52, 76, 68, 62, 125, 102, 118, 187, 240, 109, 163, 215, 247, 114, 217, 244, 255, 122, 159, 149, 221, 149, 113, 78, 188, 183, 128, 57, 155, 255, 146, 40, 123};

// Gradient palette "Sunset_Real_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/atmospheric/tn/Sunset_Real.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 28 uint8_ts of program space.

const uint8_t Sunset_Real_gp[] = {0, 120, 0, 0, 22, 179, 22, 0, 51, 255, 104, 0, 85, 167, 22, 18, 135, 100, 0, 103, 198, 16, 0, 130, 255, 0, 0, 160};

// Gradient palette "Sunset_Yellow_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/atmospheric/tn/Sunset_Yellow.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 44 uint8_ts of program space.

const uint8_t Sunset_Yellow_gp[] = {0, 10, 62, 123, 36, 56, 130, 103, 87, 153, 225, 85, 100, 199, 217, 68, 107, 255, 207, 54, 115, 247, 152, 57, 120, 239, 107, 61, 128, 247, 152, 57, 180, 255, 207, 54, 223, 255, 227, 48, 255, 255, 248, 42};

// Gradient palette "Beech_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/atmospheric/tn/Beech.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 60 uint8_ts of program space.

const uint8_t Beech_gp[] = {0, 255, 252, 214, 12, 255, 252, 214, 22, 255, 252, 214, 26, 190, 191, 115, 28, 137, 141, 52, 28, 112, 255, 205, 50, 51, 246, 214, 71, 17, 235, 226, 93, 2, 193, 199, 120, 0, 156, 174, 133, 1, 101, 115, 136, 1, 59, 71, 136, 7, 131, 170, 208, 1, 90, 151, 255, 0, 56, 133};

// Gradient palette "Another_Sunset_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/atmospheric/tn/Another_Sunset.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 32 uint8_ts of program space.

const uint8_t Another_Sunset_gp[] = {0, 110, 49, 11, 29, 55, 34, 10, 68, 22, 22, 9, 68, 239, 124, 8, 97, 220, 156, 27, 124, 203, 193, 61, 178, 33, 53, 56, 255, 0, 1, 52};

// Gradient palette "es_autumn_19_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/es/autumn/tn/es_autumn_19.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 52 uint8_ts of program space.

const uint8_t es_autumn_19_gp[] = {0, 26, 1, 1, 51, 67, 4, 1, 84, 118, 14, 1, 104, 137, 152, 52, 112, 113, 65, 1, 122, 133, 149, 59, 124, 137, 152, 52, 135, 113, 65, 1, 142, 139, 154, 46, 163, 113, 13, 1, 204, 55, 3, 1, 249, 17, 1, 1, 255, 17, 1, 1};

// Gradient palette "BlacK_Blue_Magenta_White_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/basic/tn/BlacK_Blue_Magenta_White.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 28 uint8_ts of program space.

const uint8_t BlacK_Blue_Magenta_White_gp[] = {0, 0, 0, 0, 42, 0, 0, 45, 84, 0, 0, 255, 127, 42, 0, 255, 170, 255, 0, 255, 212, 255, 55, 255, 255, 255, 255, 255};

// Gradient palette "BlacK_Magenta_Red_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/basic/tn/BlacK_Magenta_Red.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t BlacK_Magenta_Red_gp[] = {0, 0, 0, 0, 63, 42, 0, 45, 127, 255, 0, 255, 191, 255, 0, 45, 255, 255, 0, 0};

// Gradient palette "BlacK_Red_Magenta_Yellow_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/basic/tn/BlacK_Red_Magenta_Yellow.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 28 uint8_ts of program space.

const uint8_t BlacK_Red_Magenta_Yellow_gp[] = {0, 0, 0, 0, 42, 42, 0, 0, 84, 255, 0, 0, 127, 255, 0, 45, 170, 255, 0, 255, 212, 255, 55, 45, 255, 255, 255, 0};

// Gradient palette "Blue_Cyan_Yellow_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/nd/basic/tn/Blue_Cyan_Yellow.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t Blue_Cyan_Yellow_gp[] = {0, 0, 0, 255, 63, 0, 55, 255, 127, 0, 255, 255, 191, 42, 255, 45, 255, 255, 255, 0};

// Custom palette by Aircoookie

const uint8_t Orange_Teal_gp[] = {0, 0, 150, 92, 55, 0, 150, 92, 200, 255, 72, 0, 255, 255, 72, 0};

// Custom palette by Aircoookie

const uint8_t Tiamat_gp[] = {0,   1,   2,   14,   // gc
                             33,  2,   5,   35,   // gc from 47, 61,126
                             100, 13,  135, 92,   // gc from 88,242,247
                             120, 43,  255, 193,  // gc from 135,255,253
                             140, 247, 7,   249,  // gc from 252, 69,253
                             160, 193, 17,  208,  // gc from 231, 96,237
                             180, 39,  255, 154,  // gc from 130, 77,213
                             200, 4,   213, 236,  // gc from 57,122,248
                             220, 39,  252, 135,  // gc from 177,254,255
                             240, 193, 213, 253,  // gc from 203,239,253
                             255, 255, 249, 255};

// Custom palette by Aircoookie

const uint8_t April_Night_gp[] = {0,   1, 5, 45,                                       // deep blue
                                  10,  1, 5, 45, 25,  5, 169, 175,                     // light blue
                                  40,  1, 5, 45, 61,  1, 5,   45,  76,  45,  175, 31,  // green
                                  91,  1, 5, 45, 112, 1, 5,   45,  127, 249, 150, 5,   // yellow
                                  143, 1, 5, 45, 162, 1, 5,   45,  178, 255, 92,  0,   // pastel orange
                                  193, 1, 5, 45, 214, 1, 5,   45,  229, 223, 45,  72,  // pink
                                  244, 1, 5, 45, 255, 1, 5,   45};

const uint8_t Orangery_gp[] = {0, 255, 95, 23, 30, 255, 82, 0, 60, 223, 13, 8, 90, 144, 44, 2, 120, 255, 110, 17, 150, 255, 69, 0, 180, 158, 13, 11, 210, 241, 82, 17, 255, 213, 37, 4};

// inspired by Mark Kriegsman https://gist.github.com/kriegsman/756ea6dcae8e30845b5a
const uint8_t C9_gp[] = {0,   184, 4,  0,                    // red
                         60,  184, 4,  0, 65,  144, 44, 2,   // amber
                         125, 144, 44, 2, 130, 4,   96, 2,   // green
                         190, 4,   96, 2, 195, 7,   7,  88,  // blue
                         255, 7,   7,  88};

const uint8_t Sakura_gp[] = {0, 196, 19, 10, 65, 255, 69, 45, 130, 223, 45, 72, 195, 255, 82, 103, 255, 223, 13, 17};

const uint8_t Aurora_gp[] = {0,   1, 5,   45,                                  // deep blue
                             64,  0, 200, 23, 128, 0, 255, 0,                  // green
                             170, 0, 243, 45, 200, 0, 135, 7, 255, 1, 5, 45};  // deep blue

const uint8_t Atlantica_gp[] = {0,   0,  28,  112,                   // #001C70
                                50,  32, 96,  255,                   // #2060FF
                                100, 0,  243, 45,  150, 12, 95, 82,  // #0C5F52
                                200, 25, 190, 95,                    // #19BE5F
                                255, 40, 170, 80};                   // #28AA50

const uint8_t C9_2_gp[] = {0,   6,   126, 2,                       // green
                           45,  6,   126, 2,   45,  4,   30, 114,  // blue
                           90,  4,   30,  114, 90,  255, 5,  0,    // red
                           135, 255, 5,   0,   135, 196, 57, 2,    // amber
                           180, 196, 57,  2,   180, 137, 85, 2,    // yellow
                           255, 137, 85,  2};

// C9, but brighter and with a less purple blue
const uint8_t C9_new_gp[] = {0,   255, 5,   0,                       // red
                             60,  255, 5,   0,  60,  196, 57,  2,    // amber (start 61?)
                             120, 196, 57,  2,  120, 6,   126, 2,    // green (start 126?)
                             180, 6,   126, 2,  180, 4,   30,  114,  // blue (start 191?)
                             255, 4,   30,  114};

// Gradient palette "temperature_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/arendal/tn/temperature.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 144 uint8_ts of program space.

const uint8_t temperature_gp[] = {0, 1, 27, 105, 14, 1, 40, 127, 28, 1, 70, 168, 42, 1, 92, 197, 56, 1, 119, 221, 70, 3, 130, 151, 84, 23, 156, 149, 99, 67, 182, 112, 113, 121, 201, 52, 127, 142, 203, 11, 141, 224, 223, 1, 155, 252, 187, 2, 170, 247, 147, 1, 184, 237, 87, 1, 198, 229, 43, 1, 226, 171, 2, 2, 240, 80, 3, 3, 255, 80, 3, 3};

const uint8_t Aurora2_gp[] = {
    0,   17,  177, 13,   // Greenish
    64,  121, 242, 5,    // Greenish
    128, 25,  173, 121,  // Turquoise
    192, 250, 77,  127,  // Pink
    255, 171, 101, 221   // Purple
};

// Gradient palette "bhw1_01_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_01.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 12 uint8_ts of program space.

const uint8_t retro_clown_gp[] = {0, 227, 101, 3, 117, 194, 18, 19, 255, 92, 8, 192};

// Gradient palette "bhw1_04_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_04.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 20 uint8_ts of program space.

const uint8_t candy_gp[] = {0, 229, 227, 1, 15, 227, 101, 3, 142, 40, 1, 80, 198, 17, 1, 79, 255, 0, 0, 45};

// Gradient palette "bhw1_05_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_05.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 8 uint8_ts of program space.

const uint8_t toxy_reaf_gp[] = {0, 1, 221, 53, 255, 73, 3, 178};

// Gradient palette "bhw1_06_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_06.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 16 uint8_ts of program space.

const uint8_t fairy_reaf_gp[] = {0, 184, 1, 128, 160, 1, 193, 182, 219, 153, 227, 190, 255, 255, 255, 255};

// Gradient palette "bhw1_14_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_14.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 36 uint8_ts of program space.

const uint8_t semi_blue_gp[] = {0, 0, 0, 0, 12, 1, 1, 3, 53, 8, 1, 22, 80, 4, 6, 89, 119, 2, 25, 216, 145, 7, 10, 99, 186, 15, 2, 31, 233, 2, 1, 5, 255, 0, 0, 0};

// Gradient palette "bhw1_three_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_three.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 32 uint8_ts of program space.

const uint8_t pink_candy_gp[] = {0, 255, 255, 255, 45, 7, 12, 255, 112, 227, 1, 127, 112, 227, 1, 127, 140, 255, 255, 255, 155, 227, 1, 127, 196, 45, 1, 99, 255, 255, 255, 255};

// Gradient palette "bhw1_w00t_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw1/tn/bhw1_w00t.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 16 uint8_ts of program space.

const uint8_t red_reaf_gp[] = {0, 3, 13, 43, 104, 78, 141, 240, 188, 255, 0, 0, 255, 28, 1, 1};

// Gradient palette "bhw2_23_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw2/tn/bhw2_23.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Red & Flash in SR
// Size: 28 uint8_ts of program space.

const uint8_t aqua_flash_gp[] = {0, 0, 0, 0, 66, 57, 227, 233, 96, 255, 255, 8, 124, 255, 255, 255, 153, 255, 255, 8, 188, 57, 227, 233, 255, 0, 0, 0};

// Gradient palette "bhw2_xc_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw2/tn/bhw2_xc.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// YBlue in SR
// Size: 28 uint8_ts of program space.

const uint8_t yelblu_hot_gp[] = {0, 4, 2, 9, 58, 16, 0, 47, 122, 24, 0, 16, 158, 144, 9, 1, 183, 179, 45, 1, 219, 220, 114, 2, 255, 234, 237, 1};

// Gradient palette "bhw2_45_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw2/tn/bhw2_45.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 24 uint8_ts of program space.

const uint8_t lite_light_gp[] = {0, 0, 0, 0, 9, 1, 1, 1, 40, 5, 5, 6, 66, 5, 5, 6, 101, 10, 1, 12, 255, 0, 0, 0};

// Gradient palette "bhw2_22_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw2/tn/bhw2_22.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Pink Plasma in SR
// Size: 20 uint8_ts of program space.

const uint8_t red_flash_gp[] = {0, 0, 0, 0, 99, 227, 1, 1, 130, 249, 199, 95, 155, 227, 1, 1, 255, 0, 0, 0};

// Gradient palette "bhw3_40_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw3/tn/bhw3_40.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 32 uint8_ts of program space.

const uint8_t blink_red_gp[] = {0, 1, 1, 1, 43, 4, 1, 11, 76, 10, 1, 3, 109, 161, 4, 29, 127, 255, 86, 123, 165, 125, 16, 160, 204, 35, 13, 223, 255, 18, 2, 18};

// Gradient palette "bhw3_52_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw3/tn/bhw3_52.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Yellow2Blue in SR
// Size: 28 uint8_ts of program space.

const uint8_t red_shift_gp[] = {0, 31, 1, 27, 45, 34, 1, 16, 99, 137, 5, 9, 132, 213, 128, 10, 175, 199, 22, 1, 201, 199, 9, 6, 255, 1, 0, 1};

// Gradient palette "bhw4_097_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw4/tn/bhw4_097.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Yellow2Red in SR
// Size: 44 uint8_ts of program space.

const uint8_t red_tide_gp[] = {0, 247, 5, 0, 28, 255, 67, 1, 43, 234, 88, 11, 58, 234, 176, 51, 84, 229, 28, 1, 114, 113, 12, 1, 140, 255, 225, 44, 168, 113, 12, 1, 196, 244, 209, 88, 216, 255, 28, 1, 255, 53, 1, 1};

// Gradient palette "bhw4_017_gp", originally from
// http://soliton.vm.uint8_tmark.co.uk/pub/cpt-city/bhw/bhw4/tn/bhw4_017.png.index.html
// converted for FastLED with gammas (2.6, 2.2, 2.5)
// Size: 40 uint8_ts of program space.

const uint8_t candy2_gp[] = {0, 39, 33, 34, 25, 4, 6, 15, 48, 49, 29, 22, 73, 224, 173, 1, 89, 177, 35, 5, 130, 4, 6, 15, 163, 255, 114, 6, 186, 224, 173, 1, 211, 39, 33, 34, 255, 1, 1, 1};

// WLEDMM netmindz ar palette
// Start off as just RGB, but replace in runtime with colors relating to the music
const uint8_t audio_responsive_gp[] = {0, 255, 0, 0, 125, 0, 255, 0, 255, 0, 0, 255};

const uint8_t fastled_cloud_gp[] = {255, 1, 0, 0};
const uint8_t fastled_forest_gp[] = {255, 1, 1, 0};
const uint8_t fastled_heat_gp[] = {255, 1, 2, 0};
const uint8_t fastled_lava_gp[] = {255, 1, 3, 0};
const uint8_t fastled_ocean_gp[] = {255, 1, 4, 0};
const uint8_t fastled_party_gp[] = {255, 1, 5, 0};
const uint8_t fastled_rainbow_gp[] = {255, 1, 6, 0};
const uint8_t fastled_rainbow_stripe_gp[] = {255, 1, 7, 0};
const uint8_t moonlight_mm_gp[] = {255, 2, 0, 0};
const uint8_t moonlight_random_gp[] = {255, 2, 1, 0};
const uint8_t moonlight_red_gp[] = {255, 2, 2, 0};
const uint8_t moonlight_green_gp[] = {255, 2, 3, 0};
const uint8_t moonlight_blue_gp[] = {255, 2, 4, 0};
const uint8_t moonlight_orange_gp[] = {255, 2, 5, 0};
const uint8_t moonlight_purple_gp[] = {255, 2, 6, 0};
const uint8_t moonlight_cyan_gp[] = {255, 2, 7, 0};
const uint8_t moonlight_w_white_gp[] = {255, 2, 8, 0};
const uint8_t moonlight_c_white_gp[] = {255, 2, 9, 0};

// Single array of defined cpt-city color palettes.
// This will let us programmatically choose one based on
// a number, rather than having to activate each explicitly
// by name every time.
const uint8_t* const gGradientPalettes[] = {
    // Sorted alphabetically by palette name, audio palettes first
    fastled_cloud_gp,             // Cloud
    fastled_forest_gp,            // Forest
    fastled_heat_gp,              // Heat
    fastled_lava_gp,              // Lava
    fastled_ocean_gp,             // Ocean
    fastled_party_gp,             // Party
    fastled_rainbow_gp,           // Rainbow
    fastled_rainbow_stripe_gp,    // Rainbow bands
    moonlight_mm_gp,              // MoonLight 💫
    moonlight_random_gp,          // Random
    moonlight_red_gp,             // Red
    moonlight_green_gp,           // Green
    moonlight_blue_gp,            // Blue
    moonlight_orange_gp,          // Orange
    moonlight_purple_gp,          // Purple
    moonlight_cyan_gp,            // Cyan
    moonlight_w_white_gp,         // Warm White
    moonlight_c_white_gp,         // Cold White
    audio_responsive_gp,          // Audio Hue ☾
    audio_responsive_gp,          // Audio Ramp ☾
    audio_responsive_gp,          // Audio Ratio ☾
    Analogous_1_gp,               // Analogous
    April_Night_gp,               // April Night
    aqua_flash_gp,                // Aqua Flash
    Atlantica_gp,                 // Atlantica
    Aurora_gp,                    // Aurora
    Aurora2_gp,                   // Aurora 2
    es_autumn_19_gp,              // Autumn
    es_landscape_33_gp,           // Beach
    Beech_gp,                     // Beech
    blink_red_gp,                 // Blink Red
    es_ocean_breeze_036_gp,       // Breeze
    C9_gp,                        // C9
    C9_2_gp,                      // C9 2
    C9_new_gp,                    // C9 New
    candy_gp,                     // Candy
    candy2_gp,                    // Candy2
    Colorfull_gp,                 // Cyane
    departure_gp,                 // Departure
    GMT_drywet_gp,                // Drywet
    fairy_reaf_gp,                // Fairy Reaf
    lava_gp,                      // Fire
    ib15_gp,                      // Grintage
    gr65_hult_gp,                 // Hult
    gr64_hult_gp,                 // Hult 64
    fierce_ice_gp,                // Icefire
    ib_jul01_gp,                  // Jul
    es_landscape_64_gp,           // Landscape
    Pink_Purple_gp,               // Light Pink
    lite_light_gp,                // Lite Light
    BlacK_Blue_Magenta_White_gp,  // Magenta
    BlacK_Magenta_Red_gp,         // Magred
    Orange_Teal_gp,               // Orange & Teal
    Orangery_gp,                  // Orangery
    Sunset_Yellow_gp,             // Pastel
    pink_candy_gp,                // Pink Candy
    rgi_15_gp,                    // Red & Blue
    red_flash_gp,                 // Red Flash
    red_reaf_gp,                  // Red Reaf
    red_shift_gp,                 // Red Shift
    red_tide_gp,                  // Red Tide
    retro_clown_gp,               // Retro Clown
    ib15_gp,                      // Rewhi
    es_rivendell_15_gp,           // Rivendell
    Sakura_gp,                    // Sakura
    semi_blue_gp,                 // Semi Blue
    rainbowsherbet_gp,            // Sherbet
    es_pinksplash_08_gp,          // Splash
    Sunset_Real_gp,               // Sunset
    Another_Sunset_gp,            // Sunset 2
    temperature_gp,               // Temperature
    Tertiary_01_gp,               // Tertiary
    Tiamat_gp,                    // Tiamat
    toxy_reaf_gp,                 // Toxy Reaf
    es_vintage_01_gp,             // Vintage
    yelblu_hot_gp,                // Yelblu Hot
    Blue_Cyan_Yellow_gp,          // Yelblu
    BlacK_Red_Magenta_Yellow_gp,  // Yelmag
    retro2_16_gp,                 // Yellowout
};

const char* const palette_names[] = {"Cloud⚡️",       "Forest⚡️",    "Heat⚡️",      "Lava⚡️",
                                     "Ocean⚡️",       "Party⚡️",     "Rainbow⚡️",   "Rainbow Bands⚡️",
                                     "MoonLight💫",   "Random💫",    "Red",         "Green",
                                     "Blue",          "Orange",      "Purple",      "Cyan",
                                     "Warm White",    "Cold White",  "Audio Hue🌙", "Audio Ramp🌙",
                                     "Audio Ratio🌙", "Analogous",   "April Night", "Aqua Flash",
                                     "Atlantica",     "Aurora",      "Aurora 2",    "Autumn",
                                     "Beach",  //
                                     "Beech",         "Blink Red",   "Breeze",      "C9",
                                     "C9 2",          "C9 New",      "Candy",       "Candy2",
                                     "Cyane",         "Departure",
                                     "Drywet",  //
                                     "Fairy Reaf",    "Fire",        "Grintage",    "Hult",
                                     "Hult 64",       "Icefire",     "Jul",         "Landscape",
                                     "Light Pink",  //
                                     "Lite Light",    "Magenta",     "Magred",      "Orange & Teal",
                                     "Orangery",      "Pastel",      "Pink Candy",  "Red & Blue",
                                     "Red Flash",     "Red Reaf",    "Red Shift",   "Red Tide",
                                     "Retro Clown",   "Rewhi",       "Rivendell",   "Sakura",
                                     "Semi Blue",     "Sherbet",     "Splash",      "Sunset",
                                     "Sunset 2",      "Temperature", "Tertiary",    "Tiamat",
                                     "Toxy Reaf",     "Vintage",     "Yelblu Hot",  "Yelblu",
                                     "Yelmag",        "Yellowout"};

CRGBPalette16 getGradientPalette(uint8_t index) {
  CRGBPalette16 palette;
  const byte* gpArray = gGradientPalettes[index];

  if (gpArray[0] == 255) {  // fastled and moonlight palettes
    if (gpArray[1] == 1) {  // fastled
      switch (gpArray[2]) {
      case 0:
        palette = CloudColors_p;
        break;
      case 1:
        palette = ForestColors_p;
        break;
      case 2:
        palette = HeatColors_p;
        break;
      case 3:
        palette = LavaColors_p;
        break;
      case 4:
        palette = OceanColors_p;
        break;
      case 5:
        palette = PartyColors_p;
        break;
      case 6:
        palette = RainbowColors_p;
        break;
      case 7:
        palette = RainbowStripeColors_p;
        break;
      }
    } else if (gpArray[1] == 2) {  // MoonLight palettes
      switch (gpArray[2]) {
      case 0:  // MoonLight
        for (int i = 0; i < 16; i++) {
          palette[i] = CRGB(map(i, 0, 15, 255, 0), map(i, 0, 15, 31, 0), map(i, 0, 15, 0, 255));  // from orange to blue
        }
        break;
      case 1:  // random
        for (int i = 0; i < 16; i++) {
          palette[i] = CHSV(random8(), 255, 255);  // take the max saturation, max brightness of the colorwheel
        }
        break;
      case 2:  // Red
        for (int i = 0; i < 16; i++) palette[i] = CRGB::Red;
        break;
      case 3:  // Green
        for (int i = 0; i < 16; i++) palette[i] = CRGB::Green;
        break;
      case 4:  // Blue
        for (int i = 0; i < 16; i++) palette[i] = CRGB::Blue;
        break;
      case 5:  // Orange
        for (int i = 0; i < 16; i++) palette[i] = CRGB(255, 31, 0);
        // CRGB::Orange is too yellow
        break;
      case 6:  // Purple
        for (int i = 0; i < 16; i++) palette[i] = CRGB::Purple;
        break;
      case 7:  // Cyan
        for (int i = 0; i < 16; i++) palette[i] = CRGB::Cyan;
        break;
      case 8:  // Warm White
        for (int i = 0; i < 16; i++) palette[i] = CRGB(255, 120, 20);
        // Very Warm / Candle-like (≈2200K) or CRGB(255, 160, 60) - Softer Warm White or CRGB(255, 147, 41) - Typical Warm White (≈2700K)
        break;
      case 9:  // Cold White
        for (int i = 0; i < 16; i++) palette[i] = CRGB::White;
        break;
      }
    }
  } else {                 // gradient array palettes
    byte tcp[76] = {255};  // WLEDMM: prevent out-of-range access in loadDynamicGradientPalette()
    memcpy(tcp, (byte*)pgm_read_dword(&(gGradientPalettes[index])), 72);
    palette.loadDynamicGradientPalette(tcp);
  }

  return palette;
}

String getPaletteHexString(uint8_t index) {
  CRGBPalette16 palette = getGradientPalette(index);
  const byte* gpArray = gGradientPalettes[index];
  String hexString = "";

  if (gpArray[0] == 255) {  // fastled and moonlight palettes
    char buf[9];
    for (int j = 0; j < 16; j++) {
      // Add index (0, 16, 32, ... 240)
      // Add R, G, B
      snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", j * 16, palette[j].r, palette[j].g, palette[j].b);
      hexString += buf;
    }

    // Add final entry at index 255
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", 255, palette[15].r, palette[15].g, palette[15].b);
    hexString += buf;
  } else {  // gradient array palettes
    int j = 0;

    // Read 4-byte entries (index, r, g, b) until index == 255
    while (j < 100) {  // Safety limit
      for (int k = 0; k < 4; k++) {
        char buf[3];
        sprintf(buf, "%02x", gpArray[j++]);
        hexString += buf;
      }
      // Check if we just wrote the final entry (index was 255)
      if (gpArray[j - 4] == 255) break;
    }
  }
  return hexString;
}

#endif
