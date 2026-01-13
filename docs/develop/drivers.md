# Drivers

## Parallel LED Driver

### Parlio (ESP32-P4)

### Parallel LED Driver Technical Implementation

For end-user documentation, see [Parallel LED Driver for ESP32-P4](#parallel-led-driver-for-esp32-p4).

**Architecture Overview:**

The driver implements variable LEDs-per-pin support without intermediate buffers by padding during the transpose operation. Located in `src/MoonLight/Nodes/Drivers/parlio.cpp` with header `parlio.h`, called from `D_ParallelLEDDriver.h`.

**Key Functions:**

1. **`show_parlio()`** (parlio.cpp:409-414): Entry point receiving compact `channelsD` buffer and `leds_per_output[]` array. Computes two global state variables:
   - `max_leds_per_output`: Maximum across all pins
   - `first_index_per_output[16]`: Cumulative offsets for random access into compact buffer

2. **`create_transposed_led_output_optimized()`** (parlio.cpp:297): Main processing loop iterating `0..max_leds_per_output`. Applies color remapping (`offsetR/G/B/W`) and brightness LUTs before calling transpose.

3. **`transpose_32_slices()`** (parlio.cpp:181-183): **Core padding mechanism** using single conditional:

```cpp
const uint8_t data_byte = pixel_in_pin < pixels_per_pin[pin]
    ? brightness_cache[input_buffer[component_idx]]  // Actual LED
    : 0;                                              // Padding (black)
```

When pixel_in_pin exceeds a pin's actual LED count, outputs zero, creating black padding pixels.

Data Flow: Input buffer remains compact (Σ(leds_per_pin[i]) × channels) → show_parlio() builds indexing structures → create_transposed_led_output_optimized() iterates to max → transpose_32_slices() conditionally reads/pads → DMA buffer written with uniform alignment → PARLIO hardware transmits.

Performance: Zero memory overhead for padding, O(1) offset lookup via first_index_per_output[], minimal branching in hot path. Chunking logic (parlio.cpp:480-505) handles large LED counts exceeding DMA limits.