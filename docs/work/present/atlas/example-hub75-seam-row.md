# MoonAtlas example: a bright row at a HUB75 panel's seam

A report from the bench: two 64x64 HUB75 panels chained side by side, 128x64 on an ESP32, show a bright line along row 32 or 33. The maintainer knows the driver's name and nothing of its wire format. Three queries and one 60-line read take them from the symptom to a hypothesis with predictions to check on the panels, and to the test that missed it, and a MoonLive script then puts the prediction on the panels with no firmware build. Output from the tree on 2026-10-29; the transcript at the end is the session as it ran.

## What it is

```
$ uv run moondeck/docs/show_atlas.py what Hub75Driver
module:Hub75Driver: Drives HUB75 LED panels straight from the board's own pins, with no receiving card in between.
  defined_in -> src/light/drivers/Hub75Driver.h
  documented_by -> docs/moonmodules/light/drivers.md#hub75
  … eleven has_control lines, bitDepth to whiteMode …
  is -> Hub75Driver  src/light/drivers/Hub75Driver.h:34
  tests <- test/unit/light/unit_Hub75Driver_pins.cpp
  tests <- test/unit/light/unit_Hub75Slots.cpp
class:Hub75Driver  src/light/drivers/Hub75Driver.h:34
  derives -> DriverBase  src/light/drivers/DriverBase.h:24
  defines <- src/light/drivers/Hub75Driver.h [34]
  is <- module:Hub75Driver
  … three mentions lines: two shipped plans and the MoonCore plan …
```

The driver, its doc section and its two test files, one of them named for the encoder.

## Where the wire format lives

```
$ uv run moondeck/docs/show_atlas.py impact Hub75Bus.h
included by (4)
  src/light/drivers/Hub75Driver.h
  src/light/drivers/Hub75Slots.h
  src/module_types.cpp
  src/platform/esp32/platform_esp32_hub75.cpp
unit tests (2)
  test/unit/light/unit_Hub75Driver_pins.cpp
  test/unit/light/unit_Hub75Slots.cpp
scenarios: none
MoonWalk steps (1)
  step:driver-setup/add-hub75  Add the HUB75 driver.
docs (1)
  docs/work/present/Plan-20261025 - MoonCore under other apps (2 of 12).md
claims (2)
  docs/work/present/atlas/areas/platform-fable.json#plat-014  A HUB75 geometry encodes only with 2 to 6 bit planes, a height that divides by its scan rate, and an even number of rows per address step, since each step drives an upper and a lower row together.
  docs/work/present/atlas/areas/platform-fable.json#plat-016  In the 16-bit HUB75 bus word the OE bit is set where the panel is lit and the platform inverts the pin, since OE is active low and a peripheral idles its lines low.
```

The bus contract has one encoder, `Hub75Slots.h`, and one platform port. A search of the claims for HUB75 adds the one that matters most:

> `drv-101`: hub75Encode lays the frame plane-major with the latch riding the last column's word, the data words naming the row latched one step earlier, and brightness as the number of lit words per row, doubling per plane.

Two facts now frame the symptom. On a 64-row panel at 1/32 scan, address step r lights row r and row r + 32 (`plat-014`), so row 31, the last of the upper half, sits at the seam. And the row a panel lights is the one latched a step earlier (`drv-101`), so the first row of each pass lights whatever the previous pass left in the latch.

## The read

`hub75Encode` (`src/light/drivers/Hub75Slots.h:40-102`) loops plane, then scan row, then column. Its comment at line 68 says what row 0 shows: "for row 0 the last row of the previous plane or frame". The lit window at lines 75-77 is computed from the current `plane`.

## The hypothesis

The last scan row is lit with the next plane's on-time. While row r ≥ 1 of plane p clocks in, the panel lights row r − 1 of the same plane p, for plane p's window: correct. While row 0 of plane p clocks in, the latch holds row 31 of plane p − 1, lit for plane p's window, twice as long as its own. Row 31's top plane carries into the next frame's plane 0 and lights for plane 0's window.

So rows 31 and 63 show each lower bit at twice its weight and the top bit at a fraction of it. Row 31 is row 32 counted from one: the seam the bench reports.

The panels confirm it (On the bench, below). Its predictions:

1. The bottom edge row of each panel, row 63, is off the same way.
2. The error depends on the value. Full white (255) looks right, since the doubled lower bits and the shrunken top bit sum to the same total; a value below 128 shows about twice as bright (0x7F reads as 254); a value just above 128 shows nearly dark.
3. One panel alone shows it, so the chain plays no part.

## Why the tests pass

`test/unit/light/unit_Hub75Slots.cpp:185`, "the DMA wrap lights the last row once, from row 0's data words", fills every pixel with 255, the one value where the error cancels, and checks that the last row is lit, not for how long per plane. The comment below it records an earlier bug of the same family on the same panel: "row 63 ghosting onto row 32, rows 0 and 32 brighter, flashing", fixed then through the output-enable polarity.

## The fix

Row 0 of each plane takes the lit window of the plane its latched data came from: plane p − 1, and the top plane for plane 0. The test that pins it sums each row's on-time per plane for a mid value such as 0x7F and requires every row to agree; it fails against the old encoder, where rows 31 and 63 light 123 words against every other row's 62.

## Where the graph stops

The graph pointed at the right file and handed over the two facts that frame the symptom; the timing argument came from reading the encoder, since the graph holds file, class and test edges and no data flow. The earlier ghosting bug sat in a test comment, which the graph indexes as a test of the module and not as a lesson.

## On the bench

`ramp.mle` (in the transcript below) runs on the MatrixPortal S3 driving the two chained panels, loaded as a script with no firmware build.

1. **Before the fix, in sync with the panel:** a short dark segment on the seam row, right of the middle, darkest at its left end and fading to the right. That is prediction 2 with the driver's brightness curve on, which maps the ramp's values there to just above 128 on the wire.
2. **Before the fix, the camera pushed out of sync:** the shutter separates the bit planes' time slots, and a bright line stands out on the last row of each half, the seam and the bottom edge, rows 31 and 63. It lines up with the upper section's bottom row, not the lower section's top row, which rules out rows 0 and 32.
3. **After the fix, flashed over the air through MoonBase:** the seam and the bottom edge match their neighbors across the whole ramp.

## Transcript

The session as it ran, from the product owner's report to the test pattern that checks it on the panels. Other work in the same session is left out; the tool steps are summarized in italics.

**James:** example scenario. A multi panel hub75 with 64x64 panels, 2 wide 1 deep on esp32, the display shows a brightness along the 32 or 33 row.

*Claude runs `what Hub75Driver`, `why Hub75Driver` and `impact Hub75Bus.h`, searches the claims for HUB75, reads `hub75Encode` in `Hub75Slots.h` and the geometry in `Hub75Bus.h`, and lists the encoder's tests.*

**Claude:** Here's how the graph got me to a hypothesis, which fits your symptom closely. It's a hypothesis, not a confirmed root cause, until it's checked on the panels.

How the graph got there, in three queries and one read:

- `what Hub75Driver` named the header, its doc section and its two unit tests.
- `impact Hub75Bus.h` showed the encoder, `Hub75Slots.h`, is the one place the wire format lives.
- Two claims gave the key facts. `drv-101`: row r's data words are shown while the panel is still addressed to row r−1. `plat-014`: each address step lights an upper and a lower row together.

Then I read `hub75Encode` itself, about 60 lines.

The hypothesis: the last scan row is lit with the wrong bit plane's on-time. Your chain is 128×64 at 1/32 scan, so there are 32 scan rows, and step r lights row r and row r+32.

- Why the address lags: the panel shows the row last latched, so while row r's data clocks in, row r−1 is lit. For r ≥ 1, row r−1 of plane p is lit for plane p's window, which is correct.
- The wrap: at row 0 of plane p, the latch still holds row 31 from plane p−1, the last row of the plane before. `Hub75Slots.h:69-77` computes the lit window from the current plane p, not from p−1.
- The effect on row 31: every lower bit shows for twice its correct time. The top bit, carried into the next frame's plane 0, shows for only plane 0's short window.
- That row's position: 0-based row 31 is your 1-based row 32. It's the last row of the upper half, right at the seam between the halves. Its partner is row 63, the panel's bottom edge.

Predictions you can check on the panels:

1. The bottom edge row of each panel (row 64, 1-based) should be off in the same way as the seam row.
2. It depends on color value. Full white (255) should look correct, because the doubled lower bits and the shrunken top bit happen to add up to the same total. Values below 128 should look about twice as bright (0x7F comes out as roughly 254). Values just above 128 should look nearly dark.
3. It shouldn't depend on the chain. One 64×64 panel alone should show it too.

Why the tests miss it: the wrap test at `unit_Hub75Slots.cpp:185` fills every pixel with 255, the one value where the error cancels out. It checks that the last row is lit once, not for how long per plane. The comment just after it records an earlier bug in the same family: "row 63 ghosting onto row 32, rows 0 and 32 brighter, flashing", fixed then through the output-enable polarity.

If it's confirmed, the fix is small. For row 0 of each plane, compute the lit window from the plane the latched data came from: plane p−1, or the top plane for plane 0. The test adds up each row's on-time per plane for a mid value such as 0x7F and requires every row to come out the same; that test fails against today's code.

Your MatrixPortal S3 drives HUB75, so it could check predictions 1 and 2 directly.

**James:** make a test pattern that highlights the hub75 issue

*Claude reads the MoonLive README and three of the basic effect scripts, then `gradient.mle`, and checks the driver's brightness curve.*

**James:** could mmlive be used?

**Claude:** Yes, this is exactly what MoonLive is for. A MoonLive effect compiles on the device, so the pattern can be pasted into a MoonLive effect card on the MatrixPortal and runs with no firmware build or flash.

`gradient.mle` won't do. Its value follows the light index, so every row has a different color and an odd row can't stand out. What the diagnosis needs is a ramp across the width that is identical on every row. Then a row whose bit planes are mis-timed shows up as a stripe that doesn't match the rows around it.

*Claude writes `moonlive/effects/basics/ramp.mle`:*

```cpp
// Ramp: gray rising from black on the left to white on the right, the same on every row, the test pattern for a panel's bit planes.
// Every row matches its neighbors, so a row that differs is a fault in how the panel is driven, not in the picture:
// a HUB75 row lit with the wrong plane's on-time reads brighter than its neighbors below half gray and darker just above it.

class RampEffect {

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void tick() {
    int span = width - 1;
    if (span < 1) { span = 1; }
    for (int x = 0; x < width; x = x + 1) {
      int v = div(x * 255, span);
      for (int y = 0; y < height; y = y + 1) {
        setRGB(y * width + x, v, v, v);
      }
    }
  }
}
```

**Claude:** The ramp draws gray from black at the left to white at the right, identical on every row and spanning both chained panels. If the hypothesis holds, rows 31 and 63 (rows 32 and 64 counting from one) show a brighter stripe in the left half of the ramp, below mid-gray. Just right of the middle, the stripe turns dark, and at the far right, white, it disappears.

It also works with gamma or CIE correction on: the ramp still crosses every bit boundary, just at different x positions. For the cleanest reading, set the driver's brightness curve to linear.

To try it on the MatrixPortal before any release carries it: add a MoonLive effect, create a new script, paste the file's contents and save. It compiles on the next tick.
