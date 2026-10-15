# Lights

Everything a light show is built from, and the pages here follow the order light flows through: a **layout** says where the lights physically are, **effects** stacked in layers write color into them, **modifiers** reshape how that pattern lands, and a **driver** sends the result out to the strip, the panel or the network.

Each page is a catalog: one block per module, with its preview, what it does, and what every control means together. How the pipeline fits together is [MoonLight](../../explanation/architecture/moonlight.md); this is what you pick from while building.

| Page | What you reach for it |
|---|---|
| [Effects](effects.md) | The animation itself: fire, noise, a spectrum, a game of life |
| [Layouts](layouts.md) | Where the lights are: a grid, a ring, a spiral, a shape you wired yourself |
| [Modifiers](modifiers.md) | Reshaping the result: mirror, rotate, swap axes |
| [Drivers](drivers.md) | Getting it out: LED strips, Art-Net, DMX, a panel card, the browser preview |
| Decks | Every catalog page is a deck of cards to browse, filter and turn over |
| [MoonLive](moonlive.md) | The script language, and the library that ships with it |
| [Power functions](power-functions.md) | The shared drawing, field and motion routines every effect composes from |
| [Supporting](supporting.md) | The pieces the four above are built on |

A light show stacks them: one layout, one or more layers of effects with modifiers on top, and the drivers that output the whole composite. Every one of them is a [MoonModule](../../explanation/architecture/moonmodule.md), so they are added, replaced and reordered live from the same interface, and a new one is a new file rather than a change to the framework.

## Source

The pieces the whole pipeline is assembled from: [Layer](supporting.md#layer) holds the effects and composites them, [Layouts](supporting.md#layouts) and [Effects](supporting.md#effects) and [Drivers](supporting.md#drivers) are the three containers a user adds children to, and [Buffer](../../../src/light/layers/Buffer.h) is the pixel memory they all read and write.
