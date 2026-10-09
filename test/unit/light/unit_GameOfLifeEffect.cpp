/// @module GameOfLifeEffect

#include "doctest.h"
#include "light/effects/GameOfLifeEffect.h"

using namespace mm;

// The B#/S# parser turns a rule string into birth/survive neighbor sets. Conway = B3/S23.
TEST_CASE("GameOfLife: ruleset parser reads B/S sets") {
    GameOfLifeEffect gol;
    gol.ruleset = GameOfLifeEffect::kRulesetConway;                       // "Conway B3/S23"
    gol.parseRulesetForTest();
    CHECK(gol.birthForTest(3));
    CHECK_FALSE(gol.birthForTest(2));
    CHECK(gol.surviveForTest(2));
    CHECK(gol.surviveForTest(3));
    CHECK_FALSE(gol.surviveForTest(1));

    gol.ruleset = GameOfLifeEffect::kRulesetHighLife;                       // "HighLife B36/S23"
    gol.parseRulesetForTest();
    CHECK(gol.birthForTest(3));
    CHECK(gol.birthForTest(6));            // the HighLife extra birth rule
    CHECK_FALSE(gol.birthForTest(5));
}

// A 2×2 block is a Conway still life: every live cell has 3 neighbors (survives), and the surrounding dead cells never have exactly 3 (no births). It must be identical after a step.
TEST_CASE("GameOfLife: a 2x2 block is a stable still life") {
    GameOfLifeEffect gol;
    gol.ruleset = GameOfLifeEffect::kRulesetConway;
    REQUIRE(gol.allocateForTest(6, 6, 1));
    gol.setCellForTest(2, 2, 0, true);
    gol.setCellForTest(3, 2, 0, true);
    gol.setCellForTest(2, 3, 0, true);
    gol.setCellForTest(3, 3, 0, true);

    gol.stepForTest();

    // The block is unchanged…
    CHECK(gol.isAliveForTest(2, 2, 0));
    CHECK(gol.isAliveForTest(3, 2, 0));
    CHECK(gol.isAliveForTest(2, 3, 0));
    CHECK(gol.isAliveForTest(3, 3, 0));
    // …and nothing else was born.
    CHECK_FALSE(gol.isAliveForTest(1, 1, 0));
    CHECK_FALSE(gol.isAliveForTest(4, 4, 0));
}

// Regression: a 3D grid gives a cell up to 26 neighbors (3×3×3 minus self), but the B/S rule tables are sized 9 (single-digit Conway notation, 0..8). A dense 3D neighborhood must not read those tables out of bounds, a count ≥9 is in no single-digit ruleset, so the cell dies / stays dead. This fills a 3×3×3 cube (the center has all 26 neighbors alive) and just steps: the test passing under ASan/bounds-checking is the OOB-read pin; behaviorally the over-crowded center dies (26 ∉ S) and the dense interior doesn't survive.
TEST_CASE("GameOfLife: a dense 3D neighborhood never indexes the B/S tables out of bounds") {
    GameOfLifeEffect gol;
    gol.ruleset = GameOfLifeEffect::kRulesetConway;                                  // Conway B3/S23
    REQUIRE(gol.allocateForTest(5, 5, 5));            // a real 3D grid (depth>1 enables the z axis)
    for (lengthType z = 1; z <= 3; z++)              // a solid 3×3×3 block, centre at (2,2,2)
        for (lengthType y = 1; y <= 3; y++)
            for (lengthType x = 1; x <= 3; x++)
                gol.setCellForTest(x, y, z, true);

    gol.stepForTest();                                // must not OOB-read birth/surviveNumbers_[9..26]

    // The center (26 live neighbors) is wildly overcrowded → dies. No single-digit ruleset survives 26, so the over-dense interior clears rather than persisting.
    CHECK_FALSE(gol.isAliveForTest(2, 2, 2));
}

// A horizontal 3-cell blinker oscillates to vertical after one step (period-2 oscillator). This is the canonical "the rules actually run" check: birth on 3, death of the ends (1 neighbor each).
TEST_CASE("GameOfLife: a 3-cell blinker flips orientation each step") {
    GameOfLifeEffect gol;
    gol.ruleset = GameOfLifeEffect::kRulesetConway;
    REQUIRE(gol.allocateForTest(7, 7, 1));
    // Horizontal: (2,3) (3,3) (4,3)
    gol.setCellForTest(2, 3, 0, true);
    gol.setCellForTest(3, 3, 0, true);
    gol.setCellForTest(4, 3, 0, true);

    gol.stepForTest();

    // Now vertical: (3,2) (3,3) (3,4); the horizontal ends died (1 neighbor), the center survives, and the cells above/below the center were born (3 neighbors).
    CHECK(gol.isAliveForTest(3, 2, 0));
    CHECK(gol.isAliveForTest(3, 3, 0));
    CHECK(gol.isAliveForTest(3, 4, 0));
    CHECK_FALSE(gol.isAliveForTest(2, 3, 0));   // end died
    CHECK_FALSE(gol.isAliveForTest(4, 3, 0));   // end died

    gol.stepForTest();                          // flips back to horizontal
    CHECK(gol.isAliveForTest(2, 3, 0));
    CHECK(gol.isAliveForTest(4, 3, 0));
    CHECK_FALSE(gol.isAliveForTest(3, 2, 0));
}

// A lone cell (0 neighbors) dies, the dead-by-isolation rule, and a sanity check that an empty grid stays empty (no spontaneous births at count 0 under Conway).
TEST_CASE("GameOfLife: a lone cell dies and an empty grid stays empty") {
    GameOfLifeEffect gol;
    gol.ruleset = GameOfLifeEffect::kRulesetConway;
    REQUIRE(gol.allocateForTest(5, 5, 1));
    gol.setCellForTest(2, 2, 0, true);
    gol.stepForTest();
    CHECK_FALSE(gol.isAliveForTest(2, 2, 0));   // isolated → dead
    // Whole grid empty now.
    for (lengthType y = 0; y < 5; y++)
        for (lengthType x = 0; x < 5; x++)
            CHECK_FALSE(gol.isAliveForTest(x, y, 0));
}
