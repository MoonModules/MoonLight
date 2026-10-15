# Light supporting modules

The light-domain machinery the catalog modules (effects, modifiers, layouts, drivers) lean on. The containers and shared modules are the cards below; the classes that are no module each link to their header.

- **[Buffer](../../../src/light/layers/Buffer.h)**: the contiguous light data every effect writes and every driver reads.
- **[MappingLUT](../../../src/light/layers/MappingLUT.h)**: the table mapping each logical light to the physical lights it drives, one, none or many, with no table at all on the sequential identity fast path.
- **[Scripted palette](../../../src/light/moonlive/MoonLivePalette.h)**: a palette authored as code rather than stored as data.
- **[Script particle pool](../../../src/light/moonlive/MoonLiveParticles.h)**: one scripted module's particle pool, with the frame clock that makes the physics run evenly.
- **[Effect base](../../../src/light/effects/EffectBase.h)**: what every effect shares; the [effects catalog](effects.md) is built on it.
- **[Modifier base](../../../src/light/modifiers/ModifierBase.h)**: what every modifier shares; the [modifiers catalog](modifiers.md) is built on it.
- **[Driver base](../../../src/light/drivers/DriverBase.h)**: what every driver shares, the output correction and the source window; the [drivers catalog](drivers.md) is built on it.
- **[Layout base](../../../src/light/layouts/LayoutBase.h)**: what every layout shares; the [layouts catalog](layouts.md) is built on it.
- **[Slot encoder](../../../src/light/drivers/ParallelSlots.h)**: the WS2812 encode for parallel buses, the contract between a parallel driver and its peripheral.
- **[Pin list](../../../src/light/drivers/PinList.h)**: the light-domain half of pin and count list parsing for multi-output LED drivers.
- **[Parallel LED driver](drivers.md#parallelled)**: the driver every parallel peripheral plugs into.
