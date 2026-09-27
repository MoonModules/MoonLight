#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Effect rendering a rotating Rubik's cube on a 3D layout.
/// @card RubiksCubeEffect.gif
/// Author: WildCats08 / @Brandon502 (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// The cube scrambles itself, then plays the scramble back one slice at a time.
/// Every voxel takes the color of the nearest face, which is what makes a solid read as a cube.
///
/// Prior art: MoonLight's RubiksCube, whose model and move list this reproduces.
///
/// @moreinfo
///
/// ## The model behind the picture
///
/// The cube is a full six-face model, up to 8x8 stickers a face, with the real face, row and column rotations.
/// Each frame it is drawn onto the LED volume by classifying every in-bounds voxel as belonging to whichever outer face it sits nearest, and coloring it from that face's sticker.
/// Turns play at `turnsPerSecond`, and `cubeSize` is the order of the cube, 2 through 8 being real cubes and 1 a degenerate single block.
/// With `randomTurning` the cube tumbles through endless random moves instead of solving a stored scramble.
///
/// ## One difference from the source
///
/// MoonLight has no per-cell mapping mask, so every in-bounds voxel counts as mapped.
/// The source's mapping-driven size adjustments are therefore dropped.
/// The projection uses the extent less one instead, floored at 1.
class RubiksCubeEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, and 3D-native.
    const char* tags() const override { return "💫"; }
    /// A cube needs all three axes, so this effect is volumetric.
    Dim dimensions() const override { return Dim::D3; }

    // Defaults match MoonLight's own RubiksCube.
    /// How many slices turn each second.
    uint8_t turnsPerSecond = 2;
    /// The cube's order, so 3 is the familiar 3x3x3.
    uint8_t cubeSize       = 3;
    /// Turn random slices rather than replaying the scramble backward.
    bool    randomTurning  = false;
    /// Take the six face colors from the active palette, where a primary-ish one stays distinct.
    bool    usePalette     = false;

    /// Publish the turn rate, the cube's order and the two coloring choices.
    void defineControls() override {
        controls_.addControl("turnsPerSecond", turnsPerSecond, 0, 20);
        controls_.addControl("cubeSize", cubeSize, 1, 8);
        controls_.addControl("randomTurning", randomTurning);
        controls_.addControl("usePalette", usePalette);
    }

    /// The six face colors: the classic Rubik's set, or six evenly spaced palette samples.
    std::array<RGB, 6> faceColors() const {
        if (!usePalette)
            return {{{255, 0, 0}, {255, 140, 0}, {0, 0, 255}, {0, 128, 0}, {255, 255, 0}, {255, 255, 255}}};
        std::array<RGB, 6> pal{};
        const Palette& active = *Palettes::active();
        for (int i = 0; i < 6; i++)
            pal[i] = colorFromPalette(active, static_cast<uint8_t>(i * 255 / 5));
        return pal;
    }

    /// Re-scramble when the cube's order or its turning changes, since neither rebuilds any state.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "cubeSize") == 0 || std::strcmp(name, "randomTurning") == 0)
            doInit_ = true;
    }

    /// Scramble when asked, then turn one slice per interval and redraw the cube.
    void tick() MM_NONBLOCKING override {
        const lengthType w = width(), h = height(), d = depth();

        const draw::Canvas cv = canvas();

        const uint32_t now = elapsed();

        // A wrap-safe signed difference: the unsigned form underflows and re-inits every frame.
        const int32_t ahead = static_cast<int32_t>(step_ - now);
        if ((doInit_ && now > step_) || ahead > 3100) {
            step_ = now + 1000;
            doInit_ = false;
            init(cv, w, h, d);
        }

        // Turn pacing: nothing to do until 1000/turnsPerSecond ms have passed since the last turn.
        if (!turnsPerSecond || now - step_ < 1000u / turnsPerSecond || now < step_) return;

        const Move move = randomTurning ? createRandomMoveStruct(cubeSize, prevFaceMoved_)
                                        : unpackMove(moveList_[moveIndex_]);
        // Playback applies the inverse direction so the scramble unwinds toward solved.
        (cube_.*kRotateFuncs[move.face])(!move.direction, static_cast<uint8_t>(move.width + 1));
        cube_.drawCube(cv, w, h, d, faceColors());

        if (!randomTurning && moveIndex_ == 0) {
            step_ = now + 3000;   // solved: hold for 3 s, then re-scramble
            doInit_ = true;
            return;
        }
        if (!randomTurning) moveIndex_--;
        step_ = now;
    }

private:
    // --- The cube model -------------------------------------------------------------------------
    struct Cube {
        uint8_t SIZE = 3;
        static const uint8_t MAX_SIZE = 8;
        using Face = std::array<std::array<uint8_t, MAX_SIZE>, MAX_SIZE>;
        Face front, back, left, right, top, bottom;

        void init(uint8_t order) {
            SIZE = order;
            for (int i = 0; i < MAX_SIZE; i++)
                for (int j = 0; j < MAX_SIZE; j++) {
                    front[i][j] = 0; back[i][j] = 1; left[i][j] = 2;
                    right[i][j] = 3; top[i][j] = 4; bottom[i][j] = 5;
                }
        }

        void rotateFace(Face& face, bool clockwise) {
            Face temp = face;
            if (clockwise)
                for (int i = 0; i < SIZE; i++) for (int j = 0; j < SIZE; j++) face[j][SIZE - 1 - i] = temp[i][j];
            else
                for (int i = 0; i < SIZE; i++) for (int j = 0; j < SIZE; j++) face[SIZE - 1 - j][i] = temp[i][j];
        }

        void rotateRow(int startRow, int stopRow, bool clockwise) {
            std::array<uint8_t, MAX_SIZE> temp;
            for (int row = startRow; row <= stopRow; row++) {
                if (clockwise)
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = left[row][i];
                        left[row][i] = front[row][i]; front[row][i] = right[row][i];
                        right[row][i] = back[row][i]; back[row][i] = temp[i];
                    }
                else
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = left[row][i];
                        left[row][i] = back[row][i]; back[row][i] = right[row][i];
                        right[row][i] = front[row][i]; front[row][i] = temp[i];
                    }
            }
        }

        void rotateColumn(int startCol, int stopCol, bool clockwise) {
            std::array<uint8_t, MAX_SIZE> temp;
            for (int col = startCol; col <= stopCol; col++) {
                if (clockwise)
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = top[i][col];
                        top[i][col] = front[i][col]; front[i][col] = bottom[i][col];
                        bottom[i][col] = back[SIZE - 1 - i][SIZE - 1 - col]; back[SIZE - 1 - i][SIZE - 1 - col] = temp[i];
                    }
                else
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = top[i][col];
                        top[i][col] = back[SIZE - 1 - i][SIZE - 1 - col]; back[SIZE - 1 - i][SIZE - 1 - col] = bottom[i][col];
                        bottom[i][col] = front[i][col]; front[i][col] = temp[i];
                    }
            }
        }

        void rotateFaceLayer(bool clockwise, int startLayer, int endLayer) {
            for (int layer = startLayer; layer <= endLayer; layer++) {
                std::array<uint8_t, MAX_SIZE> temp;
                for (int i = 0; i < SIZE; i++) temp[i] = clockwise ? top[SIZE - 1 - layer][i] : bottom[layer][i];
                for (int i = 0; i < SIZE; i++) {
                    if (clockwise) {
                        top[SIZE - 1 - layer][i] = left[SIZE - 1 - i][SIZE - 1 - layer];
                        left[SIZE - 1 - i][SIZE - 1 - layer] = bottom[layer][SIZE - 1 - i];
                        bottom[layer][SIZE - 1 - i] = right[i][layer];
                        right[i][layer] = temp[i];
                    } else {
                        bottom[layer][SIZE - 1 - i] = left[SIZE - 1 - i][SIZE - 1 - layer];
                        left[SIZE - 1 - i][SIZE - 1 - layer] = top[SIZE - 1 - layer][i];
                        top[SIZE - 1 - layer][i] = right[i][layer];
                        right[i][layer] = temp[SIZE - 1 - i];
                    }
                }
            }
        }

        void rotateFront(bool clockwise, uint8_t width) {
            rotateFaceLayer(clockwise, 0, width - 1);
            rotateFace(front, clockwise);
            if (width >= SIZE) rotateFace(back, !clockwise);
        }
        void rotateBack(bool clockwise, uint8_t width) {
            rotateFaceLayer(!clockwise, SIZE - width, SIZE - 1);
            rotateFace(back, clockwise);
            if (width >= SIZE) rotateFace(front, !clockwise);
        }
        void rotateLeft(bool clockwise, uint8_t width) {
            rotateFace(left, clockwise);
            rotateColumn(0, width - 1, !clockwise);
            if (width >= SIZE) rotateFace(right, !clockwise);
        }
        void rotateRight(bool clockwise, uint8_t width) {
            rotateFace(right, clockwise);
            rotateColumn(SIZE - width, SIZE - 1, clockwise);
            if (width >= SIZE) rotateFace(left, !clockwise);
        }
        void rotateTop(bool clockwise, uint8_t width) {
            rotateFace(top, clockwise);
            rotateRow(0, width - 1, clockwise);
            if (width >= SIZE) rotateFace(bottom, !clockwise);
        }
        void rotateBottom(bool clockwise, uint8_t width) {
            rotateFace(bottom, clockwise);
            rotateRow(SIZE - width, SIZE - 1, !clockwise);
            if (width >= SIZE) rotateFace(top, !clockwise);
        }

        // Every in-bounds voxel takes the color of the outer face it sits nearest.
        /// Paint every surface voxel in the color of its nearest face.
        void drawCube(const draw::Canvas& cv, lengthType sx, lengthType sy, lengthType sz,
                      const std::array<RGB, 6>& COLOR_MAP) const {
            // Surface voxels only, so without this wipe the old pose's stickers accrete.
            draw::fill(cv, {0, 0, 0});
            const int sizeX = MAXi(sx - 1, 1), sizeY = MAXi(sy - 1, 1), sizeZ = MAXi(sz - 1, 1);
            // The integer form of a rounded divide, so the hot loop needs no float multiply.
            const int num = 2 * (SIZE + 1);
            const int denX = 2 * sizeX, denY = 2 * sizeY, denZ = 2 * sizeZ;
            const int halfX = sizeX / 2, halfY = sizeY / 2, halfZ = sizeZ / 2;

            for (int x = 0; x < sx; x++)
                for (int y = 0; y < sy; y++)
                    for (int z = 0; z < sz; z++) {
                        const Coord3D led{static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)};
                        const int nX = constrainI((x * num + sizeX) / denX - 1, 0, SIZE - 1);
                        const int nY = constrainI((y * num + sizeY) / denY - 1, 0, SIZE - 1);
                        const int nZ = constrainI((z * num + sizeZ) / denZ - 1, 0, SIZE - 1);
                        const int distX = MINi(x, sizeX - x), distY = MINi(y, sizeY - y), distZ = MINi(z, sizeZ - z);
                        const int dist = MINi(distX, MINi(distY, distZ));

                        if      (dist == distZ && z < halfZ)  draw::pixel(cv, led, COLOR_MAP[front[nY][nX]]);
                        else if (dist == distX && x < halfX)  draw::pixel(cv, led, COLOR_MAP[left[nY][SIZE - 1 - nZ]]);
                        else if (dist == distY && y < halfY)  draw::pixel(cv, led, COLOR_MAP[top[SIZE - 1 - nZ][nX]]);
                        else if (dist == distZ && z >= halfZ) draw::pixel(cv, led, COLOR_MAP[back[nY][SIZE - 1 - nX]]);
                        else if (dist == distX && x >= halfX) draw::pixel(cv, led, COLOR_MAP[right[nY][nZ]]);
                        else if (dist == distY && y >= halfY) draw::pixel(cv, led, COLOR_MAP[bottom[nZ][nX]]);
                    }
        }
    };

    // One move packed into a byte: 3 bits of face, 3 of width, 1 of direction.
    struct Move { uint8_t face, width, direction; };

    Move createRandomMoveStruct(uint8_t size, uint8_t prevFace) {
        Move move;
        do { move.face = rng_.below(6); } while (move.face / 2 == prevFace / 2);
        move.width = (size > 2) ? rng_.below(static_cast<uint8_t>(size - 2)) : 0;  // random(cubeSize-2)
        move.direction = rng_.below(2);
        return move;
    }
    static uint8_t packMove(Move m) {
        return static_cast<uint8_t>((m.face & 0b111) | ((m.width << 3) & 0b111000) | ((m.direction << 6) & 0b1000000));
    }
    static Move unpackMove(uint8_t p) {
        Move m;
        m.face = static_cast<uint8_t>(p & 0b111);
        m.width = static_cast<uint8_t>((p >> 3) & 0b111);
        m.direction = static_cast<uint8_t>((p >> 6) & 0b1);
        return m;
    }

    using RotateFunc = void (Cube::*)(bool, uint8_t);
    static constexpr RotateFunc kRotateFuncs[6] = {
        &Cube::rotateFront, &Cube::rotateBack, &Cube::rotateLeft,
        &Cube::rotateRight, &Cube::rotateTop, &Cube::rotateBottom};

    // A solved cube, a few whole-cube turns, then a stored scramble playback can reverse.
    void init(const draw::Canvas& cv, lengthType w, lengthType h, lengthType d) {
        cube_.init(cubeSize);
        const int moveCount = cubeSize * 10 + rng_.below(20);

        for (int x = 0; x < 3; x++) {
            if (rng_.below(2)) cube_.rotateRight(true, cubeSize);
            if (rng_.below(2)) cube_.rotateTop(true, cubeSize);
            if (rng_.below(2)) cube_.rotateFront(true, cubeSize);
        }

        const int cappedMoves = (moveCount > kMaxMoves) ? kMaxMoves : moveCount;
        for (int i = 0; i < cappedMoves; i++) {
            Move move = createRandomMoveStruct(cubeSize, prevFaceMoved_);
            prevFaceMoved_ = move.face;
            moveList_[i] = packMove(move);
            (cube_.*kRotateFuncs[move.face])(move.direction, static_cast<uint8_t>(move.width + 1));
        }
        moveIndex_ = static_cast<uint8_t>(cappedMoves - 1);
        cube_.drawCube(cv, w, h, d, faceColors());
    }

    // Kept local so the code above reads like the MoonLight source it follows.
    static int MINi(int a, int b) { return a < b ? a : b; }
    static int MAXi(int a, int b) { return a > b ? a : b; }
    static int constrainI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    /// The move list's capacity, which the largest cube's scramble stays under.
    static constexpr int kMaxMoves = 100;

    Cube     cube_;                         ///< 384 bytes, small enough to keep inline
    uint8_t  moveList_[kMaxMoves] = {};     ///< the packed scramble, for reverse playback
    uint8_t  moveIndex_ = 0;                ///< how far through that playback the cube is
    uint8_t  prevFaceMoved_ = 0;            ///< so a scramble never turns one face twice running
    uint32_t step_ = 0;                     ///< when the next turn is due
    bool     doInit_ = true;                ///< request a fresh scramble
    Random8  rng_{0x52554249u};             ///< fixed-seed, so the goldens reproduce
};

} // namespace mm
