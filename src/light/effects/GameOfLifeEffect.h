#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Conway's Game of Life cellular-automaton effect.
/// @card GameOfLifeEffect.gif
/// Author: Ewoud Wijma (2022), modifications by Brandon Butler / @Brandon502 / wildcats08, https://natureofcode.com/book/chapter-7-cellular-automata/ , https://github.com/DougHaber/nlife-color , https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h
///
/// A live cell survives when its live-neighbor count is in the ruleset's survive set.
/// A dead cell is born when its count is in the birth set.
/// Neighbors are the 8 around a cell in 2D and the 26 in 3D, optionally wrapping toroidally.
///
/// Prior art: MoonLight's GameOfLife, after natureofcode chapter 7 and DougHaber/nlife-color.
///
/// @moreinfo
///
/// ## How a game ends, and the next begins
///
/// The board fingerprints itself with crc16 at three periods.
/// Every 16 generations catches oscillators, every lcm(h,w)*4 catches spaceships.
/// Every sixth of those catches cube gliders in a volume.
/// A recurring fingerprint, a die-out, a density floor or a random nudge respawns or resets it.
/// A respawn injects an R-pentomino, or a glider one time in five.
/// Each new game opens with a 1.5 second settle pause, unless `disablePause` is set.
class GameOfLifeEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, MoonModules.
    const char* tags() const override { return "💫🌙🧬"; }
    /// Volumetric: a 3D board uses the 26-cell neighborhood.
    Dim dimensions() const override { return Dim::D3; }

    // The menu labels are descriptive only, since parsing reads the digits around the slash.
    static constexpr const char* kRulesetOptions[] = {
        "Custom B/S",
        "Conway's Game of Life B3/S23",
        "HighLife B36/S23",
        "InverseLife B0123478/S34678",
        "Maze B3/S12345",
        "Mazecentric B3/S1234",
        "DrighLife B367/S23"};
    /// How many rulesets the select offers, including the custom entry at index 0.
    static constexpr uint8_t kRulesetCount = 7;

    // Kept separate from the label, so a preset parses its rule rather than its menu wording.
    static constexpr const char* kRulesetStrings[] = {
        nullptr,            // 0: custom → customRuleString
        "B3/S23",           // Conway
        "B36/S23",          // HighLife
        "B0123478/S34678",  // InverseLife
        "B3/S12345",        // Maze
        "B3/S1234",         // Mazecentric
        "B367/S23"};        // DrighLife

    // Defaults match MoonLight's own GameOfLife.
    /// The background's red channel, which dead cells fade toward.
    uint8_t backgroundColorR = 0;
    /// The background's green channel.
    uint8_t backgroundColorG = 0;
    /// The background's blue channel.
    uint8_t backgroundColorB = 0;
    /// The selected ruleset, 0 reading `customRuleString`.
    uint8_t ruleset    = 1;
    /// A user-typed rule, parsed as digits around a slash.
    char    customRuleString[20] = "B/S";
    /// Generations a second, where 100 runs uncapped.
    uint8_t speed      = 20;
    /// What percentage of cells a new game starts alive.
    uint8_t lifeChance = 32;
    /// How often a birth takes a random color rather than inheriting one.
    uint8_t mutation   = 2;
    /// Wrap the board toroidally, so a glider leaving one edge returns at the other.
    bool    wrap       = true;
    /// Skip the settle pause a new game opens with.
    bool    disablePause = false;
    /// Age a live cell from green toward red rather than painting its palette color.
    bool    colorByAge = false;
    /// Respawn a static board rather than letting it sit.
    bool    infinite   = true;
    /// How far a dead cell blurs toward the background, leaving a trail.
    uint8_t blur       = 128;

    /// Publish the ruleset, the speed, the density and the coloring.
    void defineControls() override {
        // Three uint8s rather than one color control, which the project has no type for.
        controls_.addControl("backgroundColorR", backgroundColorR, 0, 255);
        controls_.addControl("backgroundColorG", backgroundColorG, 0, 255);
        controls_.addControl("backgroundColorB", backgroundColorB, 0, 255);
        controls_.addSelect("ruleset", ruleset, kRulesetOptions, kRulesetCount);
        controls_.addText("customRuleString", customRuleString, sizeof(customRuleString));
        controls_.addControl("GameSpeed (FPS)", speed, 0, 100);
        controls_.addControl("startingLifeDensity", lifeChance, 10, 90);
        controls_.addControl("mutationChance", mutation, 0, 100);
        controls_.addControl("wrap", wrap);
        controls_.addControl("disablePause", disablePause);
        controls_.addControl("colorByAge", colorByAge);
        controls_.addControl("infinite", infinite);
        controls_.addControl("blur", blur, 0, 255);
    }

    /// Size the planes on the heap: an inline array here bootlooped a P4 by overflowing its stack.
    void prepare() override {
        const nrOfLightsType count = nrOfLights();
        if (count > 0) {
            const size_t planeBytes = (static_cast<size_t>(count) + 7) / 8;
            if (count != cellCount_) {
                // Resize all three, then test: every buffer is sized even if an earlier one fails.
                const bool a = cells_.resize(planeBytes);
                const bool b = future_.resize(planeBytes);
                const bool c = colors_.resize(count);
                const bool ok = a && b && c;
                if (ok) {
                    cellCount_ = count;
                    planeBytes_ = planeBytes;
                    generation_ = 0;   // force a fresh fill on the next loop
                } else {
                    cells_.resize(0); future_.resize(0); colors_.resize(0);
                    cellCount_ = 0; planeBytes_ = 0;
                }
            }
        } else {
            cells_.resize(0); future_.resize(0); colors_.resize(0);
            cellCount_ = 0; planeBytes_ = 0;
        }
    }

    /// Reparse the rule whenever the ruleset or the custom string changes.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "ruleset") == 0 || std::strcmp(name, "customRuleString") == 0)
            parseRuleset();
    }

    /// Test seam: size the grid without a Layer, so a test can drive the automaton deterministically.
    bool allocateForTest(lengthType w, lengthType h, lengthType d) {
        testW_ = w; testH_ = h; testD_ = d;
        const nrOfLightsType count = static_cast<nrOfLightsType>(w) * h * d;
        const size_t planeBytes = (static_cast<size_t>(count) + 7) / 8;
        // Resize all three, then test: every buffer is sized even if an earlier one fails.
        const bool a = cells_.resize(planeBytes);
        const bool b = future_.resize(planeBytes);
        const bool c = colors_.resize(count);
        if (!(a && b && c)) { cells_.resize(0); future_.resize(0); colors_.resize(0); cellCount_ = 0; planeBytes_ = 0; return false; }
        cellCount_ = count; planeBytes_ = planeBytes;
        // Unconditional: resize() zero-fills only on a size change, so a re-seed inherits stale cells.
        std::memset(cells_.data(), 0, planeBytes);
        std::memset(future_.data(), 0, planeBytes);
        std::memset(colors_.data(), 0, count);
        generation_ = 1;   // skip the random-fill path
        return true;
    }
    /// Test seam: seed one cell of the pattern.
    void setCellForTest(lengthType x, lengthType y, lengthType z, bool on) {
        setBit(cells_.data(), idx(x, y, z, testW_, testH_), on);
    }
    /// Test seam: read one cell back.
    bool isAliveForTest(lengthType x, lengthType y, lengthType z) const {
        return getBit(cells_.data(), idx(x, y, z, testW_, testH_));
    }
    /// Test seam: run exactly one generation, with no rendering or respawn.
    void stepForTest() { parseRuleset(); evolveAutomaton(testW_, testH_, testD_, true); }
    /// Test seam: parse the selected rule into the birth and survive sets.
    void parseRulesetForTest() { parseRuleset(); }
    /// Test seam: whether a dead cell with `n` live neighbors is born.
    bool birthForTest(uint8_t n) const { return birthNumbers_[n]; }
    /// Test seam: whether a live cell with `n` live neighbors survives.
    bool surviveForTest(uint8_t n) const { return surviveNumbers_[n]; }

    /// Run a generation when the speed allows, painting births, deaths and the blur trail.
    void tick() MM_NONBLOCKING override {
        if (!cells_ || !future_ || !colors_ || cellCount_ == 0) return;
        const lengthType w = width(), h = height(), d = depth();

        parseRuleset();

        // Generation 0 is between games: wait out the delay, then fill and show before stepping.
        if (generation_ == 0) {
            if (now() < step_) { renderInitial(w, h, d); return; }
            startNewGame(w, h, d);
            renderInitial(w, h, d);
            return;
        }

        const draw::Canvas cv = canvas();
        const RGB bg{backgroundColorR, backgroundColorG, backgroundColorB};

        // Above 220 the background keeps a floor rather than clearing dead cells outright.
        int fadedBackground = 0;
        uint8_t frameBlur = blur;
        if (blur > 220 && !colorByAge) {
            fadedBackground = bg.r + bg.g + bg.b + 20 + (blur - 220);
            frameBlur = static_cast<uint8_t>(blur - (blur - 220));  // == 220
        }
        const bool blurDead = step_ > now() && !fadedBackground;  // still in the settle pause

        // Redraw pass: paints a new fill, ages paused cells, and blurs dead ones while paused.
        if (generation_ <= 1 || blurDead) {
            for (lengthType z = 0; z < d; z++)
                for (lengthType y = 0; y < h; y++)
                    for (lengthType x = 0; x < w; x++) {
                        const nrOfLightsType i = idx(x, y, z, w, h);
                        const Coord3D p{x, y, z};
                        const bool alive = getBit(cells_.data(), i);
                        const bool recolor = alive && generation_ == 1 && colors_[i] == 0 && !rng_.below(16);
                        if (alive && recolor) {
                            colors_[i] = rng_.below(1, 255);
                            draw::pixel(cv, p, liveColor(colors_[i]));
                        } else if (alive && colorByAge && generation_ == 0) {
                            draw::blendPixel(cv, p, RGB{255, 0, 0}, 248);  // age while paused
                        } else if (alive && colors_[i] != 0) {
                            draw::pixel(cv, p, liveColor(colors_[i]));
                        } else if (!alive && blurDead) {
                            draw::blendPixel(cv, p, bg, frameBlur);   // blur dead while paused
                        } else if (!alive && generation_ == 1) {
                            draw::blendPixel(cv, p, bg, 248);         // fade dead on new game
                        }
                    }
        }

        // Speed throttle: 100 runs uncapped; otherwise advance only once 1000/speed ms have passed.
        if (!speed || step_ > now() || (speed != 100 && now() - step_ < 1000u / speed)) return;

        evolveAutomaton(w, h, d, false, &cv, bg, frameBlur, fadedBackground);
    }

private:
    // cellCount_ and planeBytes_ carry automaton semantics the loops need, not buffer sizes.
    ScratchBuffer<uint8_t> cells_{*this};    ///< bit-packed alive/dead, current generation
    ScratchBuffer<uint8_t> future_{*this};   ///< bit-packed next generation, swapped in
    ScratchBuffer<uint8_t> colors_{*this};   ///< palette index per cell, 0 marking dead
    nrOfLightsType cellCount_ = 0;
    size_t   planeBytes_ = 0;

    uint32_t generation_ = 0;
    uint32_t step_ = 0;           // ms timestamp gating the next step / settle pause
    Random8  rng_{0x6C0FFEE5u};

    bool     birthNumbers_[9]   = {};   ///< a dead cell with n live neighbors is born
    bool     surviveNumbers_[9] = {};   ///< a live cell with n live neighbors survives

    // Three stasis fingerprints sampled at three periods, plus the solo-glider flag.
    uint16_t oscillatorCRC_ = 0, spaceshipCRC_ = 0, cubeGliderCRC_ = 0;
    uint16_t gliderLength_ = 0, cubeGliderLength_ = 0;
    bool     soloGlider_ = false;

    lengthType testW_ = 0, testH_ = 0, testD_ = 0;  ///< the test seam's own grid dimensions

    uint32_t now() const { return elapsed(); }

    // --- bit-packed cell access ---
    static bool getBit(const uint8_t* plane, nrOfLightsType i) { return (plane[i >> 3] >> (i & 7)) & 1; }
    static void setBit(uint8_t* plane, nrOfLightsType i, bool on) {
        const uint8_t m = static_cast<uint8_t>(1u << (i & 7));
        if (on) plane[i >> 3] |= m; else plane[i >> 3] = static_cast<uint8_t>(plane[i >> 3] & ~m);
    }
    static nrOfLightsType idx(lengthType x, lengthType y, lengthType z, lengthType w, lengthType h) {
        return static_cast<nrOfLightsType>((static_cast<size_t>(z) * h + y) * w + x);
    }

    // Green under colorByAge, since it ages toward red from there.
    RGB liveColor(uint8_t colorIndex) const {
        return colorByAge ? RGB{0, 255, 0} : colorFromPalette(*Palettes::active(), colorIndex);
    }

    // Digits before the slash are birth counts and after are survive, so "36/23" parses.
    void parseRuleset() {
        const char* r = (ruleset == 0) ? customRuleString
                                       : (ruleset < kRulesetCount ? kRulesetStrings[ruleset] : kRulesetStrings[1]);
        std::memset(birthNumbers_, 0, sizeof(birthNumbers_));
        std::memset(surviveNumbers_, 0, sizeof(surviveNumbers_));
        if (!r) return;
        const char* slash = std::strchr(r, '/');
        const long slashIndex = slash ? (slash - r) : -1;
        for (const char* p = r; *p; p++) {
            const int num = *p - '0';
            if (num >= 0 && num < 9) {
                if (slashIndex >= 0 && (p - r) < slashIndex) birthNumbers_[num] = true;
                else                                          surviveNumbers_[num] = true;
            }
        }
    }

    // Integer gcd and lcm: the spaceship sampling period is lcm(h,w)*4.
    static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { const uint32_t t = a % b; a = b; b = t; } return a; }
    static uint32_t lcm(uint32_t a, uint32_t b) { if (!a || !b) return 0; return a / gcd(a, b) * b; }

    // Fill at lifeChance density, seed the three CRCs, and set the settle pause.
    void startNewGame(lengthType w, lengthType h, lengthType d) {
        generation_ = 1;
        step_ = disablePause ? now() : now() + 1500;

        std::memset(cells_.data(), 0, planeBytes_);
        std::memset(colors_.data(), 0, cellCount_);

        const draw::Canvas cv = canvas();
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++) {
                    if (rng_.below(100) < lifeChance) {
                        const nrOfLightsType i = idx(x, y, z, w, h);
                        setBit(cells_.data(), i, true);
                        colors_[i] = rng_.below(1, 255);  // never 0 (0 = dead marker)
                        draw::pixel(cv, {x, y, z}, liveColor(colors_[i]));
                    }
                }
        std::memcpy(future_.data(), cells_.data(), planeBytes_);

        soloGlider_ = false;
        const uint16_t crc = crc16(cells_.data(), planeBytes_);
        oscillatorCRC_ = spaceshipCRC_ = cubeGliderCRC_ = crc;
        gliderLength_ = static_cast<uint16_t>(lcm(static_cast<uint32_t>(h), static_cast<uint32_t>(w)) * 4);
        cubeGliderLength_ = static_cast<uint16_t>(gliderLength_ * 6);  // rectangular-cuboid case left as-is
    }

    // The frame between games: startNewGame already set the cells, so this is a straight repaint.
    void renderInitial(lengthType w, lengthType h, lengthType d) {
        const draw::Canvas cv = canvas();
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++) {
                    const nrOfLightsType i = idx(x, y, z, w, h);
                    if (getBit(cells_.data(), i) && colors_[i] != 0)
                        draw::pixel(cv, {x, y, z}, liveColor(colors_[i]));
                }
    }

    // An R-pentomino, or a glider one time in five, in up to 100 attempts avoiding overlap.
    void placePentomino(lengthType w, lengthType h, lengthType d, const draw::Canvas* cv) {
        // R-pentomino offsets; pattern[0][1] becomes 3 for the glider variant.
        uint8_t pattern[5][2] = {{1, 0}, {0, 1}, {1, 1}, {2, 1}, {2, 2}};
        if (!rng_.below(5)) pattern[0][1] = 3;
        const uint8_t colorIndex = rng_.below(1, 255);   // 1..254, never 0 (0 = dead marker)
        const RGB color = colorFromPalette(*Palettes::active(), colorIndex);

        // random8(1, size-N) needs size>N; guard tiny grids (degenerate axes collapse to 0).
        const uint8_t xHi = w > 3 ? static_cast<uint8_t>(w > 258 ? 255 : w - 3) : 1;
        const uint8_t yHi = h > 5 ? static_cast<uint8_t>(h > 260 ? 255 : h - 5) : 1;

        for (int attempts = 0; attempts < 100; attempts++) {
            const lengthType x = static_cast<lengthType>(xHi > 1 ? rng_.below(1, xHi) : 0);
            const lengthType y = static_cast<lengthType>(yHi > 1 ? rng_.below(1, yHi) : 0);
            const lengthType z = static_cast<lengthType>(d > 1 ? rng_.below(2) * (d - 1) : 0);
            bool canPlace = true;
            for (int i = 0; i < 5; i++) {
                const lengthType nx = static_cast<lengthType>(x + pattern[i][0]);
                const lengthType ny = static_cast<lengthType>(y + pattern[i][1]);
                if (nx >= w || ny >= h) continue;
                if (getBit(future_.data(), idx(nx, ny, z, w, h))) { canPlace = false; break; }
            }
            if (canPlace || attempts == 99) {
                for (int i = 0; i < 5; i++) {
                    const lengthType nx = static_cast<lengthType>(x + pattern[i][0]);
                    const lengthType ny = static_cast<lengthType>(y + pattern[i][1]);
                    if (nx >= w || ny >= h) continue;
                    const nrOfLightsType i2 = idx(nx, ny, z, w, h);
                    setBit(future_.data(), i2, true);
                    // A non-zero index, so inheritance sees these injected cells as live.
                    colors_[i2] = colorIndex;
                    if (cv) draw::pixel(*cv, {nx, ny, z}, colorByAge ? RGB{0, 255, 0} : color);
                }
                return;
            }
        }
    }

    // One generation, where `testMode` runs the automaton alone with no rendering or respawn.
    void evolveAutomaton(lengthType w, lengthType h, lengthType d, bool testMode,
                         const draw::Canvas* cv = nullptr, RGB bg = {},
                         uint8_t frameBlur = 0, int fadedBackground = 0) {
        int aliveCount = 0, deadCount = 0;
        const int zAxis = (d > 1) ? 1 : 0;
        const bool disableWrap = !wrap || soloGlider_ || generation_ % 1500 == 0 || zAxis;

        for (lengthType x = 0; x < w; x++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType z = 0; z < d; z++) {
                    const nrOfLightsType cIndex = idx(x, y, z, w, h);
                    const bool cellValue = getBit(cells_.data(), cIndex);
                    if (cellValue) aliveCount++; else deadCount++;

                    uint8_t neighbors = 0, colorCount = 0;
                    uint8_t nColors[9];
                    for (int i = -1; i <= 1; i++)
                        for (int j = -1; j <= 1; j++)
                            for (int k = -zAxis; k <= zAxis; k++) {
                                if (i == 0 && j == 0 && k == 0) continue;
                                lengthType nx = static_cast<lengthType>(x + i);
                                lengthType ny = static_cast<lengthType>(y + j);
                                lengthType nz = static_cast<lengthType>(z + k);
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= w || ny >= h || nz >= d) {
                                    if (disableWrap) continue;
                                    nx = static_cast<lengthType>((nx + w) % w);
                                    ny = static_cast<lengthType>((ny + h) % h);
                                    nz = static_cast<lengthType>((nz + d) % d);
                                }
                                const nrOfLightsType nIndex = idx(nx, ny, nz, w, h);
                                if (getBit(cells_.data(), nIndex)) {
                                    neighbors++;
                                    if (cellValue || colorByAge) continue;  // color not needed
                                    if (colors_[nIndex] == 0) continue;      // dead-marker color
                                    // Capped at 9: a 3D cell has up to 26 neighbors, which would overrun nColors.
                                    if (colorCount < 9) nColors[colorCount++] = colors_[nIndex];
                                }
                            }

                    const Coord3D p{x, y, z};
                    // Clamped: the tables hold 9 single digits, and a 3D cell reaches 26 neighbors.
                    const bool survives = neighbors < 9 && surviveNumbers_[neighbors];
                    const bool born     = neighbors < 9 && birthNumbers_[neighbors];
                    if (cellValue && !survives) {
                        // Loneliness or overpopulation: it dies and blurs toward the background.
                        setBit(future_.data(), cIndex, false);
                        if (!testMode && cv) draw::blendPixel(*cv, p, bg, frameBlur);
                    } else if (!cellValue && born) {
                        // Birth inherits a neighbor's color, and never index 0, which marks dead.
                        setBit(future_.data(), cIndex, true);
                        uint8_t colorIndex = (colorCount > 0) ? nColors[rng_.below(colorCount)] : rng_.below(1, 255);
                        if (rng_.below(100) < mutation) colorIndex = rng_.below(1, 255);
                        colors_[cIndex] = colorIndex;
                        if (!testMode && cv) draw::pixel(*cv, p, liveColor(colorIndex));
                    } else {
                        // Unchanged: a dead cell blurs, and a live one ages or repaints.
                        if (!cellValue) {
                            setBit(future_.data(), cIndex, false);
                            if (!testMode && cv) {
                                if (fadedBackground) {
                                    const RGB val = draw::get(*cv, p);
                                    if (fadedBackground < val.r + val.g + val.b)
                                        draw::blendPixel(*cv, p, bg, frameBlur);
                                } else {
                                    draw::blendPixel(*cv, p, bg, frameBlur);
                                }
                            }
                        } else {
                            setBit(future_.data(), cIndex, true);
                            if (!testMode && cv) {
                                if (colorByAge) draw::blendPixel(*cv, p, RGB{255, 0, 0}, 248);
                                else            draw::pixel(*cv, p, liveColor(colors_[cIndex]));
                            }
                        }
                    }
                }

        soloGlider_ = (aliveCount == 5);
        std::memcpy(cells_.data(), future_.data(), planeBytes_);

        // The pure automaton: a deterministic blinker must not meet the respawn machinery.
        if (testMode) return;

        const uint16_t crc = crc16(cells_.data(), planeBytes_);

        bool repetition = false;
        if (!aliveCount || crc == oscillatorCRC_ || crc == spaceshipCRC_ || crc == cubeGliderCRC_)
            repetition = true;

        // Respawn on stasis, a random nudge, or a density under 5% in integer form.
        const int total = aliveCount + deadCount;
        const bool densityFloor = total > 0 && aliveCount * 20 < total;
        if ((repetition && infinite) || (infinite && !rng_.below(50)) || (infinite && densityFloor)) {
            placePentomino(w, h, d, testMode ? nullptr : cv);
            std::memcpy(cells_.data(), future_.data(), planeBytes_);
            repetition = false;
        }
        if (repetition) {
            generation_ = 0;
            step_ = disablePause ? now() : now() + 1000;
            return;
        }

        // Each fingerprint is sampled at its own period, so each catches its own cycle length.
        if (generation_ % 16 == 0) oscillatorCRC_ = crc;
        if (gliderLength_ && generation_ % gliderLength_ == 0) spaceshipCRC_ = crc;
        if (cubeGliderLength_ && generation_ % cubeGliderLength_ == 0) cubeGliderCRC_ = crc;
        generation_++;
        step_ = now();
    }
};

} // namespace mm
