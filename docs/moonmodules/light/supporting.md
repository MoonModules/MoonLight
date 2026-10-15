# Light supporting modules

The light-domain machinery the catalog modules (effects, modifiers, layouts, drivers) lean on. The containers and shared modules are the cards below; the classes that are no module each link to their generated technical page.

- **[Buffer](moxygen/Buffer.md)**: the contiguous light data every effect writes and every driver reads.
- **[MappingLUT](moxygen/MappingLUT.md)**: the table mapping each logical light to the physical lights it drives, one, none or many, with no table at all on the sequential identity fast path.
- **[Scripted palette](moxygen/MoonLivePalette.md)**: a palette authored as code rather than stored as data.
- **[Script particle pool](moxygen/MoonLiveParticles.md)**: one scripted module's particle pool, with the frame clock that makes the physics run evenly.
- **[Effect base](moxygen/EffectBase.md)**: what every effect shares; the [effects catalog](effects.md) is built on it.
- **[Modifier base](moxygen/ModifierBase.md)**: what every modifier shares; the [modifiers catalog](modifiers.md) is built on it.
- **[Driver base](moxygen/DriverBase.md)**: what every driver shares, the output correction and the source window; the [drivers catalog](drivers.md) is built on it.
- **[Layout base](moxygen/LayoutBase.md)**: what every layout shares; the [layouts catalog](layouts.md) is built on it.
- **[Slot encoder](moxygen/ParallelSlots.md)**: the WS2812 encode for parallel buses, the contract between a parallel driver and its peripheral.
- **[Pin list](moxygen/drivers_PinList.md)**: the light-domain half of pin and count list parsing for multi-output LED drivers.
- **[Parallel LED driver](moxygen/ParallelLedDriver.md)**: the driver every parallel peripheral plugs into.
