# HUB75 test pattern

A MoonLive script, `hub75-test.mle` (in `moonlive/effects/basics/`, in the catalog after a rebuild; the short name fits the 15-character module-name limit), lights one row and one column at a time, so a photo and a stopwatch show whether a panel's rows and columns land where the mapping puts them. The Stripe mapping in `hub75_gpio_multiplex.h` follows hzeller's Stripe algorithm.

The mapping renders right on the Pi 4 sign: red rows step top to bottom, green columns move left to right, and held red layer row 0 alone lights the top of the sign.

## The mapping it checks

Stripe drives a 32x64 panel as 16 rows x 128 columns (stretch factor 2): logical rows 0..7 and 16..23 fold to driven columns 64..127, rows 8..15 and 24..31 stay at columns 0..63, and the address is `(y / 16) * 8 + y % 8`: 8 address steps, each lighting four logical rows. Every lit row and column lands where that table puts it.

## The loop

The script runs `2 + height + width` seconds, then repeats:

1. **Seconds 0 and 1:** a full-panel white flash marks the loop start.
2. **Then one red row at a time, 1 s each, top to bottom** (rows 0 to height - 1): second `s` lights row `s - 2`.
3. **Then one green column at a time, 1 s each, left to right:** second `s` lights column `s - 2 - height`.

The loop runs on time since boot, so a watcher who joins mid-loop sees a mid-sweep step.

## The two frames

The script counts in the layer frame, the logical canvas. On the sign that is 96 wide by 64 high, landscape as viewed, so the loop is 2 + 64 + 96 = **162 s**, a red row step is a horizontal line sweeping down and a green column step a vertical line sweeping across.

The driver draws the same picture in the panel frame, its 64x96 framebuffer, unrotated. The layout's Rotate:90 mapper presents that framebuffer as the 96x64 canvas, so a lit layer row is one panel column in the panel frame and a lit layer column one panel row. The rotation belongs to the layout engine, and the GPIO output implements none: the driving, the Stripe mapping and the clock, latch and address sequence are the panel frame's.

| Second | What is lit |
| ------ | ----------- |
| 0 to 1 | full-panel white flash |
| 2 | row 0, red: a horizontal line at the top of the sign |
| 3 | row 1, red: the line one step down |
| ... | one horizontal line a second, sweeping down the sign |
| 65 | row 63, red: the line at the bottom edge |
| 66 | column 0, green: a vertical line at one edge of the sign |
| 67 | column 1, green: the line one step across |
| ... | one vertical line a second, sweeping across the sign |
| 161 | column 95, green: the line at the far edge |
| 162 | the flash again: the next loop |

During the row steps one unbroken horizontal line moves one position a second down the sign, and during the column steps one unbroken vertical line moves one position a second across it. Which edge a sweep starts at depends on the layout's rotation sense; one whole line per step, moving in one direction, is the mark of a right mapping.

## What a fault looks like

- A line that lands whole at the wrong step points at the address formula.
- A line split into halves points at the column fold.
- A line that spreads, one column filling every column or one row lighting the whole sign, points at the engine's clock, latch and address sequence, since a wrong mapping only moves or splits whole lines.
- With `multiplexing` normal on Stripe panels the picture is a washed, shearing blend: the engine sends 64 columns a row where the panel's driven row holds 128, so each lit row lands as a half-width segment on the wrong row.
