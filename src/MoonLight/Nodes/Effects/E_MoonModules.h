/**
    @title     MoonLight
    @file      E_MoonModules.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

// Written by Ewoud Wijma in 2022, inspired by https://natureofcode.com/book/chapter-7-cellular-automata/ and https://github.com/DougHaber/nlife-color ,
// Modified By: Brandon Butler / @Brandon502 / wildcats08 in 2024
// todo: ewowi check with wildcats08: can background color be removed as it is now easy to add solid as background color (blending ...?)
class GameOfLifeEffect : public Node {
 public:
  static const char* name() { return "Game Of Life"; }
  static uint8_t dim() { return _3D; }  // supports 3D but also 2D (1D as well?)
  static const char* tags() { return "🔥💫🎨"; }

  void placePentomino(uint8_t* futureCells, bool colorByAge) {
    uint8_t pattern[5][2] = {{1, 0}, {0, 1}, {1, 1}, {2, 1}, {2, 2}};  // R-pentomino
    if (!random8(5)) pattern[0][1] = 3;                                // 1/5 chance to use glider
    CRGB color = ColorFromPalette(layer->layerP->palette, random8());
    for (int attempts = 0; attempts < 100; attempts++) {
      int x = random8(1, layer->size.x - 3);
      int y = random8(1, layer->size.y - 5);
      int z = random8(2) * (layer->size.z - 1);
      bool canPlace = true;
      for (int i = 0; i < 5; i++) {
        int nx = x + pattern[i][0];
        int ny = y + pattern[i][1];
        if (getBitValue(futureCells, layer->XYZUnModified(Coord3D(nx, ny, z)))) {
          canPlace = false;
          break;
        }
      }
      if (canPlace || attempts == 99) {
        for (int i = 0; i < 5; i++) {
          int nx = x + pattern[i][0];
          int ny = y + pattern[i][1];
          setBitValue(futureCells, layer->XYZUnModified(Coord3D(nx, ny, z)), true);
          layer->setRGB(Coord3D(nx, ny, z), colorByAge ? CRGB::Green : color);
        }
        return;
      }
    }
  }

  Coord3D bgC = {0, 0, 0};
  uint8_t ruleset = 1;
  char customRuleString[20] = "B/S";
  uint8_t speed = 20;
  uint8_t lifeChance = 32;
  uint8_t mutation = 2;
  bool wrap = true;
  bool disablePause = false;
  bool colorByAge = false;
  bool infinite = false;
  uint8_t blur = 128;

  void setBirthAndSurvive() {
    String ruleString = "";
    if (ruleset == 0)
      ruleString = customRuleString;  // Custom
    else if (ruleset == 1)
      ruleString = "B3/S23";  // Conway's Game of Life
    else if (ruleset == 2)
      ruleString = "B36/S23";  // HighLife
    else if (ruleset == 3)
      ruleString = "B0123478/S34678";  // InverseLife
    else if (ruleset == 4)
      ruleString = "B3/S12345";  // Maze
    else if (ruleset == 5)
      ruleString = "B3/S1234";  // Mazecentric
    else if (ruleset == 6)
      ruleString = "B367/S23";  // DrighLife

    memset(birthNumbers, 0, sizeof(birthNumbers));
    memset(surviveNumbers, 0, sizeof(surviveNumbers));

    // Rule String Parsing
    int slashIndex = ruleString.indexOf('/');
    for (int i = 0; i < ruleString.length(); i++) {
      int num = ruleString.charAt(i) - '0';
      if (num >= 0 && num < 9) {
        if (i < slashIndex)
          birthNumbers[num] = true;
        else
          surviveNumbers[num] = true;
      }
    }
  }

  void setup() override {
    addControl(bgC, "backgroundColor", "coord3D", 0, 255);
    addControl(ruleset, "ruleset", "select");
    addControlValue("Custom B/S");
    addControlValue("Conway's Game of Life B3/S23");
    addControlValue("HighLife B36/S23");
    addControlValue("InverseLife B0123478/S34678");
    addControlValue("Maze B3/S12345");
    addControlValue("Mazecentric B3/S1234");
    addControlValue("DrighLife B367/S23");

    addControl(customRuleString, "customRuleString", "text", 0, 20);

    addControl(speed, "GameSpeed (FPS)", "slider", 0, 100);
    addControl(lifeChance, "startingLifeDensity", "slider", 10, 90);
    addControl(mutation, "mutationChance", "slider", 0, 100);
    addControl(wrap, "wrap", "checkbox");
    addControl(disablePause, "disablePause", "checkbox");
    addControl(colorByAge, "colorByAge", "checkbox");
    addControl(infinite, "infinite", "checkbox");
    addControl(blur, "blur", "slider", 0, 255);
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    if (control["name"] == "ruleset" || control["name"] == "customRuleString") {
      setBirthAndSurvive();
    }
  }

  unsigned long step;
  uint16_t gliderLength;
  uint16_t cubeGliderLength;
  uint16_t oscillatorCRC;
  uint16_t spaceshipCRC;
  uint16_t cubeGliderCRC;
  bool soloGlider;
  uint16_t generation;
  bool birthNumbers[9];
  bool surviveNumbers[9];
  CRGB prevPalette;
  uint8_t* cells = nullptr;
  uint8_t* futureCells = nullptr;
  uint8_t* cellColors = nullptr;

  void startNewGameOfLife() {
    // EXT_LOGD(ML_TAG, "startNewGameOfLife");
    prevPalette = ColorFromPalette(layer->layerP->palette, 0);
    generation = 1;
    disablePause ? step = millis() : step = millis() + 1500;

    if (!cells || !futureCells || !cellColors) return;

    // Setup Grid
    memset(cells, 0, dataSize);
    memset(cellColors, 0, layer->size.x * layer->size.y * layer->size.z);

    for (int x = 0; x < layer->size.x; x++)
      for (int y = 0; y < layer->size.y; y++)
        for (int z = 0; z < layer->size.z; z++) {
          if (layer->layerDimension == _3D && !layer->isMapped(layer->XYZUnModified(Coord3D(x, y, z)))) continue;
          if (random8(100) < lifeChance) {
            int index = layer->XYZUnModified(Coord3D(x, y, z));
            setBitValue(cells, index, true);
            cellColors[index] = random8(1, 255);
            layer->setRGB(Coord3D(x, y, z), colorByAge ? CRGB::Green : ColorFromPalette(layer->layerP->palette, cellColors[index]));
            // layer->setRGB(Coord3D(x,y,z), bgColor); // Color set in redraw loop
          }
        }
    memcpy(futureCells, cells, dataSize);

    soloGlider = false;
    // Change CRCs
    uint16_t crc = crc16((const unsigned char*)cells, dataSize);
    oscillatorCRC = crc, spaceshipCRC = crc, cubeGliderCRC = crc;
    gliderLength = lcm(layer->size.y, layer->size.x) * 4;
    cubeGliderLength = gliderLength * 6;  // Change later for rectangular cuboid
  }

  int dataSize = 0;

  ~GameOfLifeEffect() override {
    freeMB(cells);
    freeMB(futureCells);
    freeMB(cellColors);
  }

  void onSizeChanged(const Coord3D& prevSize) override {
    dataSize = ((layer->size.x * layer->size.y * layer->size.z + 7) / 8);

    freeMB(cells);
    freeMB(futureCells);
    freeMB(cellColors);

    cells = allocMB<uint8_t>(dataSize);
    futureCells = allocMB<uint8_t>(dataSize);
    cellColors = allocMB<uint8_t>(layer->size.x * layer->size.y * layer->size.z);

    if (!cells || !futureCells || !cellColors) {
      EXT_LOGE(ML_TAG, "allocation of cells || !futureCells || !cellColors failed");
    }

    startNewGameOfLife();
  }

  void loop() override {
    if (!cells || !futureCells || !cellColors) return;

    if (generation == 0 && step < millis()) {
      // EXT_LOGD(ML_TAG, "gen / step");
      startNewGameOfLife();
      return;  // show the start
    }

    CRGB bgColor = CRGB(bgC.x, bgC.y, bgC.z);
    CRGB color = ColorFromPalette(layer->layerP->palette, random8());  // Used if all parents died

    int fadedBackground = 0;
    if (blur > 220 && !colorByAge) {  // Keep faded background if blur > 220
      fadedBackground = bgColor.r + bgColor.g + bgColor.b + 20 + (blur - 220);
      blur -= (blur - 220);
    }
    bool blurDead = step > millis() && !fadedBackground;
    // Redraw Loop
    if (generation <= 1 || blurDead) {  // Readd overlay support when implemented
      for (int x = 0; x < layer->size.x; x++)
        for (int y = 0; y < layer->size.y; y++)
          for (int z = 0; z < layer->size.z; z++) {
            uint16_t cIndex = layer->XYZUnModified(Coord3D(x, y, z));  // Current cell index (bit grid lookup)
            Coord3D cLocPos = Coord3D(x, y, z);
            uint16_t cLoc = layer->XYZ(cLocPos);  // Current cell location (led index)
            if (!layer->isMapped(cIndex)) continue;
            bool alive = getBitValue(cells, cIndex);
            bool recolor = (alive && generation == 1 && cellColors[cIndex] == 0 && !random(16));  // Palette change or Initial Color
            // Redraw alive if palette changed, spawn initial colors randomly, age alive cells while paused
            if (alive && recolor) {
              cellColors[cIndex] = random8(1, 255);
              layer->setRGB(cLoc, colorByAge ? CRGB::Green : ColorFromPalette(layer->layerP->palette, cellColors[cIndex]));
            } else if (alive && colorByAge && !generation)
              layer->blendColor(cLoc, CRGB::Red, 248);  // Age alive cells while paused
            else if (alive && cellColors[cIndex] != 0)
              layer->setRGB(cLoc, colorByAge ? CRGB::Green : ColorFromPalette(layer->layerP->palette, cellColors[cIndex]));
            // Redraw dead if palette changed, blur paused game, fade on newgame
            // if      (!alive && (paletteChanged || disablePause)) layer->setRGB(cLoc, bgColor);   // Remove blended dead cells
            else if (!alive && blurDead)
              layer->blendColor(cLoc, bgColor, blur);  // Blend dead cells while paused
            else if (!alive && generation == 1)
              layer->blendColor(cLoc, bgColor, 248);  // Fade dead on new game
          }
    }

    // if (!speed || step > millis() || millis() - step < 1000 / speed) return; // Check if enough time has passed for updating
    if (!speed || step > millis() || (speed != 100 && millis() - step < 1000 / speed)) return;  // Uncapped speed when slider maxed

    // Update Game of Life
    int aliveCount = 0, deadCount = 0;                         // Detect solo gliders and dead grids
    const int zAxis = (layer->layerDimension == _3D) ? 1 : 0;  // Avoids looping through z axis neighbors if 2D
    bool disableWrap = !wrap || soloGlider || generation % 1500 == 0 || zAxis;
    // Loop through all cells. Count neighbors, apply rules, setPixel
    for (int x = 0; x < layer->size.x; x++)
      for (int y = 0; y < layer->size.y; y++)
        for (int z = 0; z < layer->size.z; z++) {
          Coord3D cPos = Coord3D(x, y, z);
          uint16_t cIndex = layer->XYZUnModified(cPos);
          bool cellValue = getBitValue(cells, cIndex);
          if (cellValue)
            aliveCount++;
          else
            deadCount++;
          if (zAxis && !layer->isMapped(cIndex)) continue;  // Skip if not physical led on 3D fixtures
          uint8_t neighbors = 0, colorCount = 0;
          uint8_t nColors[9];

          for (int i = -1; i <= 1; i++)
            for (int j = -1; j <= 1; j++)
              for (int k = -zAxis; k <= zAxis; k++) {
                if (i == 0 && j == 0 && k == 0) continue;  // Ignore itself
                Coord3D nPos = Coord3D(x + i, y + j, z + k);
                if (nPos.isOutofBounds(layer->size)) {
                  // Wrap is disabled when unchecked, for 3D fixtures, every 1500 generations, and solo gliders
                  if (disableWrap) continue;
                  nPos = (nPos + layer->size) % layer->size;  // Wrap around 3D
                }
                uint16_t nIndex = layer->XYZUnModified(nPos);
                // Count neighbors and store up to 9 neighbor colors
                if (getBitValue(cells, nIndex)) {
                  neighbors++;
                  if (cellValue || colorByAge) continue;  // Skip if cell is alive (color already set) or color by age (colors are not used)
                  if (cellColors[nIndex] == 0) continue;  // Skip if neighbor color is 0 (dead cell)
                  nColors[colorCount % 9] = cellColors[nIndex];
                  colorCount++;
                }
              }
          // Rules of Life
          if (cellValue && !surviveNumbers[neighbors]) {
            // Loneliness or Overpopulation
            setBitValue(futureCells, cIndex, false);
            layer->blendColor(cPos, bgColor, blur);
          } else if (!cellValue && birthNumbers[neighbors]) {
            // Reproduction
            setBitValue(futureCells, cIndex, true);
            uint8_t colorIndex = nColors[random8(colorCount)];
            if (random8(100) < mutation) colorIndex = random8();
            cellColors[cIndex] = colorIndex;
            layer->setRGB(cPos, colorByAge ? CRGB::Green : ColorFromPalette(layer->layerP->palette, colorIndex));
          } else {
            // Blending, fade dead cells further causing blurring effect to moving cells
            if (!cellValue) {
              if (fadedBackground) {
                CRGB val = layer->getRGB(cPos);
                if (fadedBackground < val.r + val.g + val.b) layer->blendColor(cPos, bgColor, blur);
              } else
                layer->blendColor(cPos, bgColor, blur);
            } else {  // alive
              if (colorByAge)
                layer->blendColor(cPos, CRGB::Red, 248);
              else
                layer->setRGB(cPos, ColorFromPalette(layer->layerP->palette, cellColors[cIndex]));
            }
          }
        }

    if (aliveCount == 5)
      soloGlider = true;
    else
      soloGlider = false;
    memcpy(cells, futureCells, dataSize);
    uint16_t crc = crc16((const unsigned char*)cells, dataSize);

    bool repetition = false;
    if (!aliveCount || crc == oscillatorCRC || crc == spaceshipCRC || crc == cubeGliderCRC) repetition = true;
    if ((repetition && infinite) || (infinite && !random8(50)) || (infinite && float(aliveCount) / (aliveCount + deadCount) < 0.05)) {
      placePentomino(futureCells, colorByAge);  // Place R-pentomino/Glider if infinite mode is enabled
      memcpy(cells, futureCells, dataSize);
      repetition = false;
    }
    if (repetition) {
      generation = 0;
      disablePause ? step = millis() : step = millis() + 1000;
      return;
    }
    // Update CRC values
    if (generation % 16 == 0) oscillatorCRC = crc;
    if (gliderLength && generation % gliderLength == 0) spaceshipCRC = crc;
    if (cubeGliderLength && generation % cubeGliderLength == 0) cubeGliderCRC = crc;
    (generation)++;
    step = millis();
  }
};  // GameOfLife

// Author: @TroyHacks
// @license GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
class GEQ3DEffect : public Node {
 public:
  static const char* name() { return "GEQ 3D"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥♫🐺"; }

  uint8_t speed = 2;
  uint8_t frontFill = 228;
  uint8_t horizon = 0;
  uint8_t depth = 176;
  uint8_t numBands = 16;
  bool borders = true;
  bool softHack = true;

  void setup() override {
    layer->fadeToBlackBy(16);

    addControl(speed, "speed", "slider", 1, 10);
    addControl(frontFill, "frontFill", "slider");
    addControl(horizon, "horizon", "slider", 0, layer->size.x - 1);  // layer->size.x-1 is not always set here
    addControl(depth, "depth", "slider");
    addControl(numBands, "numBands", "slider", 2, 16);
    addControl(borders, "borders", "checkbox");
    addControl(softHack, "softHack", "checkbox");

    // "GEQ 3D ☾@Speed,Front Fill,Horizon,Depth,Num Bands,Borders,Soft,;!,,Peaks;!;2f;sx=255,ix=228,c1=255,c2=255,c3=15,pal=11";
  }

  uint16_t projector;
  int8_t projector_dir;
  uint32_t counter;

  void loop() override {
    if (numBands == 0) return;  // init Effect

    const int cols = layer->size.x;
    const int rows = layer->size.y;

    if (counter++ % (11 - speed) == 0) projector += projector_dir;
    if (projector >= cols) projector_dir = -1;
    if (projector <= 0) projector_dir = 1;

    layer->fill_solid(CRGB::Black);

    CRGB ledColorTemp;
    const int NUM_BANDS = numBands;
    uint_fast8_t split = ::map(projector, 0, cols, 0, (NUM_BANDS - 1));

    uint8_t heights[NUM_GEQ_CHANNELS] = {0};
    const uint8_t maxHeight = roundf(float(rows) * ((rows < 18) ? 0.75f : 0.85f));  // slightly reduce bar height on small panels
    for (int i = 0; i < NUM_BANDS; i++) {
      unsigned band = i;
      if (NUM_BANDS < NUM_GEQ_CHANNELS) band = ::map(band, 0, NUM_BANDS, 0, NUM_GEQ_CHANNELS);  // always use full range.
      heights[i] = map8(sharedData.bands[band], 0, maxHeight);                                  // cache sharedData.bands[] as data might be updated in parallel by the audioreactive core
    }

    for (int i = 0; i <= split; i++) {  // paint right vertical faces and top - LEFT to RIGHT
      uint16_t colorIndex = ::map(cols / NUM_BANDS * i, 0, cols, 0, 256);
      CRGB ledColor = ColorFromPalette(layer->layerP->palette, colorIndex);
      int linex = i * (cols / NUM_BANDS);

      if (heights[i] > 1) {
        ledColorTemp = blend(ledColor, CRGB::Black, 255 - 32);
        int pPos = max(0, linex + (cols / NUM_BANDS) - 1);
        for (int y = (i < NUM_BANDS - 1) ? heights[i + 1] : 0; y <= heights[i]; y++) {                            // don't bother drawing what we'll hide anyway
          if (rows - y > 0) layer->drawLine(pPos, rows - y - 1, projector, horizon, ledColorTemp, false, depth);  // right side perspective
        }

        ledColorTemp = blend(ledColor, CRGB::Black, 255 - 128);
        if (heights[i] < rows - horizon && (projector <= linex || projector >= pPos)) {  // draw if above horizon AND not directly under projector (special case later)
          if (rows - heights[i] > 1) {                                                   // sanity check - avoid negative Y
            for (uint_fast8_t x = linex; x <= pPos; x++) {
              bool doSoft = softHack && ((x == linex) || (x == pPos));                                     // only first and last line need AA
              layer->drawLine(x, rows - heights[i] - 2, projector, horizon, ledColorTemp, doSoft, depth);  // top perspective
            }
          }
        }
      }
    }

    for (int i = (NUM_BANDS - 1); i > split; i--) {  // paint left vertical faces and top - RIGHT to LEFT
      uint16_t colorIndex = ::map(cols / NUM_BANDS * i, 0, cols - 1, 0, 255);
      CRGB ledColor = ColorFromPalette(layer->layerP->palette, colorIndex);
      int linex = i * (cols / NUM_BANDS);
      int pPos = max(0, linex + (cols / NUM_BANDS) - 1);

      if (heights[i] > 1) {
        ledColorTemp = blend(ledColor, CRGB::Black, 255 - 32);
        for (uint_fast8_t y = (i > 0) ? heights[i - 1] : 0; y <= heights[i]; y++) {                                // don't bother drawing what we'll hide anyway
          if (rows - y > 0) layer->drawLine(linex, rows - y - 1, projector, horizon, ledColorTemp, false, depth);  // left side perspective
        }

        ledColorTemp = blend(ledColor, CRGB::Black, 255 - 128);
        if (heights[i] < rows - horizon && (projector <= linex || projector >= pPos)) {  // draw if above horizon AND not directly under projector (special case later)
          if (rows - heights[i] > 1) {                                                   // sanity check - avoid negative Y
            for (uint_fast8_t x = linex; x <= pPos; x++) {
              bool doSoft = softHack && ((x == linex) || (x == pPos));                                     // only first and last line need AA
              layer->drawLine(x, rows - heights[i] - 2, projector, horizon, ledColorTemp, doSoft, depth);  // top perspective
            }
          }
        }
      }
    }

    for (int i = 0; i < NUM_BANDS; i++) {
      uint16_t colorIndex = ::map(cols / NUM_BANDS * i, 0, cols - 1, 0, 255);
      CRGB ledColor = ColorFromPalette(layer->layerP->palette, colorIndex);
      int linex = i * (cols / NUM_BANDS);
      int pPos = linex + (cols / NUM_BANDS) - 1;
      int pPos1 = linex + (cols / NUM_BANDS);

      if (projector >= linex && projector <= pPos) {  // special case when top perspective is directly under the projector
        if ((heights[i] > 1) && (heights[i] < rows - horizon) && (rows - heights[i] > 1)) {
          ledColorTemp = blend(ledColor, CRGB::Black, 255 - 128);
          for (uint_fast8_t x = linex; x <= pPos; x++) {
            bool doSoft = softHack && ((x == linex) || (x == pPos));                                     // only first and last line need AA
            layer->drawLine(x, rows - heights[i] - 2, projector, horizon, ledColorTemp, doSoft, depth);  // top perspective
          }
        }
      }

      if ((heights[i] > 1) && (rows - heights[i] > 0)) {
        ledColorTemp = blend(ledColor, CRGB::Black, 255 - frontFill);
        for (uint_fast8_t x = linex; x < pPos1; x++) {
          layer->drawLine(x, rows - 1, x, rows - heights[i] - 1, ledColorTemp);  // front fill
        }

        if (!borders && heights[i] > rows - horizon) {
          if (frontFill == 0) ledColorTemp = blend(ledColor, CRGB::Black, 255 - 32);                                           // match side fill if we're in blackout mode
          layer->drawLine(linex, rows - heights[i] - 1, linex + (cols / NUM_BANDS) - 1, rows - heights[i] - 1, ledColorTemp);  // top line to simulate hidden top fill
        }

        if ((borders) && (rows - heights[i] > 1)) {
          layer->drawLine(linex, rows - 1, linex, rows - heights[i] - 1, ledColor);                                                    // left side line
          layer->drawLine(linex + (cols / NUM_BANDS) - 1, rows - 1, linex + (cols / NUM_BANDS) - 1, rows - heights[i] - 1, ledColor);  // right side line
          layer->drawLine(linex, rows - heights[i] - 2, linex + (cols / NUM_BANDS) - 1, rows - heights[i] - 2, ledColor);              // top line
          layer->drawLine(linex, rows - 1, linex + (cols / NUM_BANDS) - 1, rows - 1, ledColor);                                        // bottom line
        }
      }
    }
  }

};  // GEQ3DEffect

// Author: @TroyHacks, from WLED MoonModules
// @license GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
class PaintBrushEffect : public Node {
 public:
  static const char* name() { return "Paintbrush"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥🎨♫🐺"; }

  uint8_t oscillatorOffset = 6 * 160 / 255;
  uint8_t numLines = 255;
  uint8_t fadeRate = 40;
  uint8_t minLength = 0;
  bool color_chaos = false;
  bool soft = true;
  bool phase_chaos = false;

  void setup() override {
    addControl(oscillatorOffset, "oscillatorOffset", "slider", 0, 16);
    addControl(numLines, "numLines", "slider", 2, 255);
    addControl(fadeRate, "fadeRate", "slider", 0, 128);
    addControl(minLength, "minLength", "slider");
    addControl(color_chaos, "color_chaos", "checkbox");
    addControl(soft, "soft", "checkbox");
    addControl(phase_chaos, "phase_chaos", "checkbox");
  }

  uint16_t aux0Hue;
  uint16_t aux1Chaos;

  void loop() override {
    const uint16_t cols = layer->size.x;
    const uint16_t rows = layer->size.y;
    const uint16_t depth = layer->size.z;

    // uint8_t numLines = map8(nrOfLines,1,64);

    (aux0Hue)++;  // hue
    layer->fadeToBlackBy(fadeRate);

    aux1Chaos = phase_chaos ? random8() : 0;

    for (size_t i = 0; i < numLines; i++) {
      uint8_t bin = ::map(i, 0, numLines, 0, NUM_GEQ_CHANNELS - 1);

      uint8_t x1 = beatsin8(oscillatorOffset * 1 + sharedData.bands[0] / NUM_GEQ_CHANNELS, 0, (cols - 1), sharedData.bands[bin], aux1Chaos);
      uint8_t x2 = beatsin8(oscillatorOffset * 2 + sharedData.bands[0] / NUM_GEQ_CHANNELS, 0, (cols - 1), sharedData.bands[bin], aux1Chaos);
      uint8_t y1 = beatsin8(oscillatorOffset * 3 + sharedData.bands[0] / NUM_GEQ_CHANNELS, 0, (rows - 1), sharedData.bands[bin], aux1Chaos);
      uint8_t y2 = beatsin8(oscillatorOffset * 4 + sharedData.bands[0] / NUM_GEQ_CHANNELS, 0, (rows - 1), sharedData.bands[bin], aux1Chaos);
      uint8_t z1;
      uint8_t z2;
      int length;
      if (depth > 1) {
        z1 = beatsin8(oscillatorOffset * 5 + sharedData.bands[0] / NUM_GEQ_CHANNELS, 0, (depth - 1), sharedData.bands[bin], aux1Chaos);
        z2 = beatsin8(oscillatorOffset * 6 + sharedData.bands[0] / NUM_GEQ_CHANNELS, 0, (depth - 1), sharedData.bands[bin], aux1Chaos);

        length = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) + (z2 - z1) * (z2 - z1));
      } else
        length = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));

      length = map8(sharedData.bands[bin], 0, length);

      if (length > max(1, (int)minLength)) {
        CRGB color;
        if (color_chaos)
          color = ColorFromPalette(layer->layerP->palette, i * 255 / numLines + ((aux0Hue) & 0xFF), 255);
        else
          color = ColorFromPalette(layer->layerP->palette, ::map(i, 0, numLines, 0, 255), 255);
        if (depth > 1)
          layer->drawLine3D(x1, y1, z1, x2, y2, z2, color, soft, length);  // no soft implemented in 3D yet
        else
          layer->drawLine(x1, y1, x2, y2, color, soft, length);
      }
    }
  }
};

#endif