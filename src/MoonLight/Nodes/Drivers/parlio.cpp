/**
    @title     MoonLight
    @file      parlio.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main (TroyHacks)
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors (TroyHacks)
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#include "parlio.h"  //so it is compiled before Parallel LED Driver use it

#include "MoonBase/Utilities.h"
#include "soc/soc_caps.h"  // for SOC_PARLIO_SUPPORTED

#ifdef SOC_PARLIO_SUPPORTED
static_assert(SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH <= 16, "parlio.cpp assumes max data width <= 16 (packing/bit_width/bit shifts).");

  #include "driver/parlio_tx.h"
  #include "portmacro.h"

// Access the global LED driver to use its LUT tables directly
  #include "I2SClocklessLedDriver.h"
extern I2SClocklessLedDriver ledsDriver;

// The max_leds_per_output and first_index_per_output are modified in show_parlio and read in transpose_32_slices / create_transposed_led_output_optimized. This is safe given the driver runs on a dedicated core (APP_CPU),
uint16_t max_leds_per_output = 0;
uint32_t first_index_per_output[SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH];

// --- Namespace for specialized, high-performance worker functions ---
namespace LedMatrixDetail {

// This intermediate step is common to all packing functions.
// It transposes the data for 32 time-slices into a cache-friendly temporary buffer.
inline void transpose_32_slices(uint32_t (&transposed_slices)[32], uint8_t* mappedBuffer, const uint8_t component_in_pixel, const uint32_t num_active_pins, const uint8_t COMPONENTS_PER_PIXEL, const uint32_t* waveform_cache) {
  memset(transposed_slices, 0, sizeof(uint32_t) * 32);

  for (uint32_t pin = 0; pin < num_active_pins; ++pin) {
    const uint32_t component_idx = (pin * COMPONENTS_PER_PIXEL) + component_in_pixel;  // ← Now uses input_component!
    const uint8_t data_byte = mappedBuffer[component_idx];
    const uint32_t waveform = waveform_cache[data_byte];
    const uint32_t pin_bit = (1 << pin);

    uint8_t b;

    b = waveform & 0xFF;
    if ((b >> 7) & 1) transposed_slices[0] |= pin_bit;
    if ((b >> 6) & 1) transposed_slices[1] |= pin_bit;
    if ((b >> 5) & 1) transposed_slices[2] |= pin_bit;
    if ((b >> 4) & 1) transposed_slices[3] |= pin_bit;
    if ((b >> 3) & 1) transposed_slices[4] |= pin_bit;
    if ((b >> 2) & 1) transposed_slices[5] |= pin_bit;
    if ((b >> 1) & 1) transposed_slices[6] |= pin_bit;
    if ((b >> 0) & 1) transposed_slices[7] |= pin_bit;

    b = (waveform >> 8) & 0xFF;
    if ((b >> 7) & 1) transposed_slices[8] |= pin_bit;
    if ((b >> 6) & 1) transposed_slices[9] |= pin_bit;
    if ((b >> 5) & 1) transposed_slices[10] |= pin_bit;
    if ((b >> 4) & 1) transposed_slices[11] |= pin_bit;
    if ((b >> 3) & 1) transposed_slices[12] |= pin_bit;
    if ((b >> 2) & 1) transposed_slices[13] |= pin_bit;
    if ((b >> 1) & 1) transposed_slices[14] |= pin_bit;
    if ((b >> 0) & 1) transposed_slices[15] |= pin_bit;

    b = (waveform >> 16) & 0xFF;
    if ((b >> 7) & 1) transposed_slices[16] |= pin_bit;
    if ((b >> 6) & 1) transposed_slices[17] |= pin_bit;
    if ((b >> 5) & 1) transposed_slices[18] |= pin_bit;
    if ((b >> 4) & 1) transposed_slices[19] |= pin_bit;
    if ((b >> 3) & 1) transposed_slices[20] |= pin_bit;
    if ((b >> 2) & 1) transposed_slices[21] |= pin_bit;
    if ((b >> 1) & 1) transposed_slices[22] |= pin_bit;
    if ((b >> 0) & 1) transposed_slices[23] |= pin_bit;

    b = (waveform >> 24) & 0xFF;
    if ((b >> 7) & 1) transposed_slices[24] |= pin_bit;
    if ((b >> 6) & 1) transposed_slices[25] |= pin_bit;
    if ((b >> 5) & 1) transposed_slices[26] |= pin_bit;
    if ((b >> 4) & 1) transposed_slices[27] |= pin_bit;
    if ((b >> 3) & 1) transposed_slices[28] |= pin_bit;
    if ((b >> 2) & 1) transposed_slices[29] |= pin_bit;
    if ((b >> 1) & 1) transposed_slices[30] |= pin_bit;
    if ((b >> 0) & 1) transposed_slices[31] |= pin_bit;
  }
}

void __attribute__((hot)) process_1bit(uint8_t* buffer, const uint32_t* transposed_slices) {
  uint32_t packed_word = 0;
  for (int i = 0; i < 32; ++i) {
    if (transposed_slices[i]) {
      packed_word |= (1 << i);
    }
  }
  reinterpret_cast<uint32_t*>(buffer)[0] = packed_word;
}

void __attribute__((hot)) process_2bit(uint8_t* buffer, const uint32_t* transposed_slices) {
  uint32_t* out = reinterpret_cast<uint32_t*>(buffer);
  uint32_t word0 = 0, word1 = 0;
  for (int i = 0; i < 16; ++i) word0 |= (transposed_slices[i] << (i * 2));
  for (int i = 0; i < 16; ++i) word1 |= (transposed_slices[i + 16] << (i * 2));
  out[0] = word0;
  out[1] = word1;
}

void __attribute__((hot)) process_4bit(uint8_t* buffer, const uint32_t* transposed_slices) {
  uint32_t* out = reinterpret_cast<uint32_t*>(buffer);
  uint32_t word0 = 0, word1 = 0, word2 = 0, word3 = 0;
  for (int i = 0; i < 8; ++i) word0 |= (transposed_slices[i] << (i * 4));
  for (int i = 0; i < 8; ++i) word1 |= (transposed_slices[i + 8] << (i * 4));
  for (int i = 0; i < 8; ++i) word2 |= (transposed_slices[i + 16] << (i * 4));
  for (int i = 0; i < 8; ++i) word3 |= (transposed_slices[i + 24] << (i * 4));
  out[0] = word0;
  out[1] = word1;
  out[2] = word2;
  out[3] = word3;
}

void __attribute__((hot)) process_8bit(uint8_t* buffer, const uint32_t* transposed_slices) {
  // Cast the output buffer to write 32-bit words at a time.
  uint32_t* out = reinterpret_cast<uint32_t*>(buffer);

  // We have 32 bytes to write, so we do it in 8 chunks of 4 bytes (uint32_t).
  for (int i = 0; i < 8; ++i) {
    const int base_idx = i * 4;
    // Manually pack four 8-bit values into one 32-bit word.
    // The values from transposed_slices are implicitly truncated to bytes.
    uint32_t packed_word = (transposed_slices[base_idx + 0]) | (transposed_slices[base_idx + 1] << 8) | (transposed_slices[base_idx + 2] << 16) | (transposed_slices[base_idx + 3] << 24);
    // Perform a single, efficient 32-bit write.
    out[i] = packed_word;
  }
}

void __attribute__((hot)) process_16bit(uint16_t* buffer, const uint32_t* transposed_slices) {
  // Cast the output buffer to write 32-bit words at a time.
  uint32_t* out = reinterpret_cast<uint32_t*>(buffer);

  // We have 64 bytes to write, so we do it in 16 chunks of 4 bytes (uint32_t).
  for (int i = 0; i < 16; ++i) {
    const int base_idx = i * 2;
    // Manually pack two 16-bit values into one 32-bit word.
    uint32_t packed_word = (transposed_slices[base_idx + 0]) | (transposed_slices[base_idx + 1] << 16);
    // Perform a single, efficient 32-bit write.
    out[i] = packed_word;
  }
}

}  // namespace LedMatrixDetail

void rgbwBufferMapping(uint8_t* packetRGBChannel, const uint8_t* lightsRGBChannel, const uint8_t offsetRed, const uint8_t offsetGreen, const uint8_t offsetBlue, const uint8_t offsetWhite) {
  // use ledsDriver.__rbg_map[0]; for super fast brightness and gamma correction! see secondPixel in ESP32-LedDriver!
  // apply the LUT to the RGB channels !

  uint8_t red = lightsRGBChannel[0];
  uint8_t green = lightsRGBChannel[1];
  uint8_t blue = lightsRGBChannel[2];
  // extract White from RGB
  if (offsetWhite != UINT8_MAX) {
    uint8_t white = lightsRGBChannel[3];
    // if white is filled, use that and do not extract rgbw
    if (!white) {
      white = MIN(MIN(red, green), blue);
      red -= white;
      green -= white;
      blue -= white;
    }
    packetRGBChannel[offsetWhite] = ledsDriver.__white_map[white];
  }

  packetRGBChannel[offsetRed] = ledsDriver.__red_map[red];
  packetRGBChannel[offsetGreen] = ledsDriver.__green_map[green];
  packetRGBChannel[offsetBlue] = ledsDriver.__blue_map[blue];
}

// 1. Add the RGB first_index_per_outputs parameter to the function signature
// pixels_per_pin = leds_per_output
void create_transposed_led_output_optimized(const uint8_t* input_buffer, uint16_t* output_buffer, const uint16_t* pixels_per_pin, const uint32_t num_active_pins, const uint8_t COMPONENTS_PER_PIXEL, const uint8_t offsetR, const uint8_t offsetG, const uint8_t offsetB, const uint8_t offsetW) {
  // Only keep waveform cache (for WS2812 protocol timing)
  static uint32_t waveform_cache[256];
  static bool waveform_cache_initialized = false;

  static const uint16_t bitpatterns[16] = {
      0b1000100010001000, 0b1000100010001110, 0b1000100011101000, 0b1000100011101110, 0b1000111010001000, 0b1000111010001110, 0b1000111011101000, 0b1000111011101110, 0b1110100010001000, 0b1110100010001110, 0b1110100011101000, 0b1110100011101110, 0b1110111010001000, 0b1110111010001110, 0b1110111011101000, 0b1110111011101110,
  };

  // Initialize waveform cache ONCE (doesn't depend on brightness)
  if (!waveform_cache_initialized) {
    for (int i = 0; i < 256; ++i) {
      const uint16_t p1 = bitpatterns[i >> 4];
      const uint16_t p2 = bitpatterns[i & 0x0F];
      waveform_cache[i] = (uint32_t(p2) << 16) | p1;
    }
    waveform_cache_initialized = true;
  }

  const uint32_t WAVEFORM_WORDS_PER_PIXEL = COMPONENTS_PER_PIXEL * 32;
  const uint32_t total_output_words = max_leds_per_output * WAVEFORM_WORDS_PER_PIXEL;

  if (total_output_words == 0) return;

  uint8_t bit_width;
  if (num_active_pins <= 1)
    bit_width = 1;
  else if (num_active_pins <= 2)
    bit_width = 2;
  else if (num_active_pins <= 4)
    bit_width = 4;
  else if (num_active_pins <= 8)
    bit_width = 8;
  else
    bit_width = 16;

  const size_t total_bytes = (total_output_words * bit_width + 7) / 8;
  memset(output_buffer, 0, total_bytes);

  uint8_t* out_base_ptr = reinterpret_cast<uint8_t*>(output_buffer);

  // --- Main Processing Loop ---
  for (uint32_t pixel_in_pin = 0; pixel_in_pin < max_leds_per_output; ++pixel_in_pin) {  // loop over all the pixels per pin, first all the first pixels for all pins, then the second...

    // 🌙 rgbwBufferMapping for all pins for this pixel in pin
    uint8_t mappedBuffer[COMPONENTS_PER_PIXEL * SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH];  // rgbw for all pins

    for (uint32_t pin = 0; pin < num_active_pins; ++pin) {
      const uint32_t pixel_idx = first_index_per_output[pin] + pixel_in_pin;
      const uint32_t component_idx = (pixel_idx * COMPONENTS_PER_PIXEL);

      // rgbwBufferMapping: re order, DIM and white extraction
      if (pixel_in_pin < pixels_per_pin[pin]) {
        rgbwBufferMapping(&mappedBuffer[pin * COMPONENTS_PER_PIXEL], &input_buffer[component_idx], offsetR, offsetG, offsetB, offsetW);
      } else
        memset(&mappedBuffer[pin * COMPONENTS_PER_PIXEL], 0, COMPONENTS_PER_PIXEL);  // this is the magic trick to pad pixels if pixels_per_pin < max pixels_per_pin !
    }

    for (uint32_t component_in_pixel = 0; component_in_pixel < COMPONENTS_PER_PIXEL; ++component_in_pixel) {
      uint32_t transposed_slices[32];

      LedMatrixDetail::transpose_32_slices(transposed_slices, mappedBuffer, component_in_pixel, num_active_pins, COMPONENTS_PER_PIXEL, waveform_cache);

      const uint32_t component_start_word = (pixel_in_pin * WAVEFORM_WORDS_PER_PIXEL) + (component_in_pixel * 32);
      uint8_t* current_out_ptr = out_base_ptr + (component_start_word * bit_width / 8);

      switch (bit_width) {
      case 1:
        LedMatrixDetail::process_1bit(current_out_ptr, transposed_slices);
        break;
      case 2:
        LedMatrixDetail::process_2bit(current_out_ptr, transposed_slices);
        break;
      case 4:
        LedMatrixDetail::process_4bit(current_out_ptr, transposed_slices);
        break;
      case 8:
        LedMatrixDetail::process_8bit(current_out_ptr, transposed_slices);
        break;
      case 16:
        LedMatrixDetail::process_16bit(reinterpret_cast<uint16_t*>(current_out_ptr), transposed_slices);
        break;
      }
    }
  }
}

parlio_tx_unit_handle_t parlio_tx_unit = NULL;
parlio_tx_unit_config_t parlio_config = parlio_tx_unit_config_t();
parlio_transmit_config_t transmit_config = {.idle_value = 0x00,  // the idle value will force the OE line to low, thus enable the output
                                            .flags = {
                                                .queue_nonblocking = 1,
                                                .loop_transmission = 0,
                                            }};

static portMUX_TYPE parlio_spinlock = portMUX_INITIALIZER_UNLOCKED;

// parallelPins = array of pin GPIO's
// length = nrOfLights
// buffer_in = channels array
uint8_t IRAM_ATTR __attribute__((hot)) show_parlio(uint8_t* parallelPins, uint32_t length, uint8_t* buffer_in, uint8_t components, uint8_t outputs, uint16_t* leds_per_output, uint8_t offsetR, uint8_t offsetG, uint8_t offsetB, uint8_t offsetW) {
  // 💫 this is only the case if all leds_per_output for all outputs is the same (we pad everything smaller than that)
  // if (length != outputs * max_leds_per_output) {
  //   delay(100);
  //   EXT_LOGD(ML_TAG, "Parallel IO isn't set correctly. Check length, outputs, and LEDs per output. (%d != %d x %d)", length, outputs, max_leds_per_output);
  //   return 1;
  // }

  #ifdef PARLIO_TIMER
  unsigned long timer = micros();
  #endif

  static bool parlio_setup_done = false;
  static int last_outputs = -1;
  static int last_leds_per_output = -1;

  outputs = outputs > SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH ? SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH : outputs;

  // setup (only the first time)
  if (!parlio_setup_done || outputs != last_outputs || max_leds_per_output != last_leds_per_output) {
    // 💫 calculate max leds per output (for padding)
    max_leds_per_output = 0;
    first_index_per_output[0] = 0;
    for (uint8_t i = 0; i < outputs; i++) {
      if (leds_per_output[i] > max_leds_per_output) max_leds_per_output = leds_per_output[i];
      if (i > 0) first_index_per_output[i] = first_index_per_output[i - 1] + leds_per_output[i - 1];
    }

    parlio_config.clk_src = PARLIO_CLK_SRC_DEFAULT;
    if (outputs <= 1)
      parlio_config.data_width = 1;
    else if (outputs <= 2)
      parlio_config.data_width = 2;
    else if (outputs <= 4)
      parlio_config.data_width = 4;
    else if (outputs <= 8)
      parlio_config.data_width = 8;
    else
      parlio_config.data_width = 16;
    parlio_config.clk_in_gpio_num = gpio_num_t(-1);
    parlio_config.valid_gpio_num = gpio_num_t(-1);
    parlio_config.clk_out_gpio_num = gpio_num_t(-1);
    for (int i = 0; i < SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH; ++i) {
      parlio_config.data_gpio_nums[i] = (i < outputs) ? gpio_num_t(parallelPins[i]) : gpio_num_t(-1);  // @troyhacks update 20251015
    }
  #ifdef PARLIO_AUTO_OVERCLOCK  // This has caused minor annoying glitching.
    if (max_leds_per_output <= 256) {
      parlio_config.output_clk_freq_hz = 1200000 * 4;
    } else if (max_leds_per_output <= 512) {
      parlio_config.output_clk_freq_hz = 1100000 * 4;
    } else {
      parlio_config.output_clk_freq_hz = 800000 * 4;
    }
  #else
    parlio_config.output_clk_freq_hz = 800000 * 4;
  #endif
    parlio_config.valid_start_delay = 0;  // 16-bit max any number >0 seems to fail.
    parlio_config.valid_stop_delay = 0;   // 16-bit max but any number >0 seems to fail.
    parlio_config.dma_burst_size = 64;    // may not exceed 64 on PSRAM and must be power of 2 (1,2,4,8,16,32,64) and <=4 fails.
    parlio_config.trans_queue_depth = 16;
    parlio_config.max_transfer_size = 65535;
    parlio_config.flags.clk_gate_en = 0;
    parlio_config.flags.io_loop_back = 0;
    parlio_config.flags.allow_pd = 0;
    parlio_config.flags.invert_valid_out = 0;

    if (parlio_tx_unit != NULL) {
      ESP_ERROR_CHECK(parlio_tx_unit_wait_all_done(parlio_tx_unit, portMAX_DELAY));  // wait forever
      ESP_ERROR_CHECK(parlio_tx_unit_disable(parlio_tx_unit));
      ESP_ERROR_CHECK(parlio_del_tx_unit(parlio_tx_unit));
      parlio_tx_unit = NULL;
    }

    ESP_ERROR_CHECK(parlio_new_tx_unit(&parlio_config, &parlio_tx_unit));
    ESP_ERROR_CHECK(parlio_tx_unit_enable(parlio_tx_unit));
    last_outputs = outputs;
    last_leds_per_output = max_leds_per_output;
    parlio_setup_done = true;
    ESP_LOGD(ML_TAG, "Parallel IO configured for %u bit width and clock speed %u KHz and %u outputs.", parlio_config.data_width, parlio_config.output_clk_freq_hz / 1000 / 4, outputs);  // do not use EXT_LOG, unsafe if called from ISR
    for (uint8_t i = 0; i < SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH; i++) {
      const char* status = "";
      if (i >= parlio_config.data_width || (i >= outputs && i < parlio_config.data_width)) {
        status = "[ignored]";
      } else if (i < outputs) {  // 💫 not <=
        if (parlio_config.data_gpio_nums[i] == -1) status = "[missing]";
      }
      ESP_LOGD(ML_TAG, "Parallel IO Output %u = GPIO %d %s", (unsigned int)(i + 1), parlio_config.data_gpio_nums[i], status);  // do not use EXT_LOG, unsafe if called from ISR
    }
    return 0;  // let's give it a frame to set up.
  }

  static byte* parallel_buffer_remapped = NULL;
  #ifdef WLEDMM_REMAP_AT_OUTPUT
  static byte* parallel_buffer_remapped1 = (byte*)heap_caps_calloc_prefer((1024 * 16 * 4) + 15, sizeof(byte), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, MALLOC_CAP_DMA);
  static byte* parallel_buffer_remapped2 = (byte*)heap_caps_calloc_prefer((1024 * 16 * 4) + 15, sizeof(byte), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, MALLOC_CAP_DMA);
  #endif
  static uint16_t* parallel_buffer_repacked = NULL;
  static uint16_t* parallel_buffer_repacked1 = (uint16_t*)heap_caps_calloc_prefer((1024 * 16 * 16), 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED, MALLOC_CAP_DMA);
  static uint16_t* parallel_buffer_repacked2 = (uint16_t*)heap_caps_calloc_prefer((1024 * 16 * 16), 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED, MALLOC_CAP_DMA);

  if (parallel_buffer_repacked == NULL) parallel_buffer_repacked = parallel_buffer_repacked1;
  #ifdef WLEDMM_REMAP_AT_OUTPUT
  if (parallel_buffer_remapped == NULL) parallel_buffer_remapped = parallel_buffer_remapped1;

  uint32_t* mappingTable = strip.getCustomMappingTable();
  uint32_t mappingTableSize = strip.getCustomMappingTableSize();
  uint8_t my_bytes_per_pixel = components;

  uint32_t buf_pos = 0;

  for (uint32_t i = 0; i < length; i++) {
    uint32_t dest_pixel_index = i;
    if (mappingTable && i < mappingTableSize) {
      dest_pixel_index = mappingTable[i];
    }
    uint32_t dest_byte_pos = dest_pixel_index * my_bytes_per_pixel;
    parallel_buffer_remapped[dest_byte_pos + 0] = buffer_in[buf_pos++];
    parallel_buffer_remapped[dest_byte_pos + 1] = buffer_in[buf_pos++];
    parallel_buffer_remapped[dest_byte_pos + 2] = buffer_in[buf_pos++];
    if (components == 4) {  // rgbw
      parallel_buffer_remapped[dest_byte_pos + 3] = buffer_in[buf_pos++];
    }
  }
  #else
  parallel_buffer_remapped = buffer_in;
    // why should it be resetted here?
    //  color_order = COL_ORDER_RGB; // This isn't actually changing the color order - we're already there from the BusNetwork doing the right thing pixel-by-pixel.
    //  offsetR = 0;
    //  offsetG = 1;
    //  offsetB = 2;
    //  offsetW = 3;
  #endif

  create_transposed_led_output_optimized(parallel_buffer_remapped, parallel_buffer_repacked, leds_per_output, outputs, components, offsetR, offsetG, offsetB, offsetW);

  // Calculate the exact size of ONE PIXEL's data in bits and bytes.
  const uint32_t symbols_per_pixel = components * 32;  // isRGBW ? 128 : 96;
  const uint32_t bits_per_pixel = symbols_per_pixel * parlio_config.data_width;
  const uint32_t bytes_per_pixel = (bits_per_pixel + 7) / 8;
  const uint32_t HW_MAX_BYTES_PER_CHUNK = parlio_config.max_transfer_size;
  const uint16_t max_leds_per_chunk = (bytes_per_pixel > 0) ? (HW_MAX_BYTES_PER_CHUNK / bytes_per_pixel) : 0;
  const uint8_t num_chunks = (max_leds_per_output + max_leds_per_chunk - 1) / max_leds_per_chunk;

  uint32_t chunk_bits[4];
  const uint8_t* chunk_ptrs[4];
  uint32_t leds_remaining = max_leds_per_output;
  const size_t chunk_stride_bytes = (size_t)max_leds_per_chunk * bytes_per_pixel;

  // Chunk 1
  uint32_t leds_in_chunk = (leds_remaining < max_leds_per_chunk) ? leds_remaining : max_leds_per_chunk;
  chunk_bits[0] = leds_in_chunk * bits_per_pixel;
  chunk_ptrs[0] = (const uint8_t*)parallel_buffer_repacked;
  leds_remaining -= leds_in_chunk;

  // Chunk 2
  leds_in_chunk = (leds_remaining < max_leds_per_chunk) ? leds_remaining : max_leds_per_chunk;
  chunk_bits[1] = leds_in_chunk * bits_per_pixel;
  chunk_ptrs[1] = chunk_ptrs[0] + chunk_stride_bytes;
  leds_remaining -= leds_in_chunk;

  // Chunk 3
  leds_in_chunk = (leds_remaining < max_leds_per_chunk) ? leds_remaining : max_leds_per_chunk;
  chunk_bits[2] = leds_in_chunk * bits_per_pixel;
  chunk_ptrs[2] = chunk_ptrs[1] + chunk_stride_bytes;
  leds_remaining -= leds_in_chunk;

  // Chunk 4
  chunk_bits[3] = leds_remaining * bits_per_pixel;
  chunk_ptrs[3] = chunk_ptrs[2] + chunk_stride_bytes;

  unsigned long before = micros();
  ESP_ERROR_CHECK(parlio_tx_unit_wait_all_done(parlio_tx_unit, portMAX_DELAY));
  unsigned long after = micros();

  #ifdef WLEDMM_REMAP_AT_OUTPUT
  parallel_buffer_remapped = (parallel_buffer_remapped == parallel_buffer_remapped1) ? parallel_buffer_remapped2 : parallel_buffer_remapped1;
  #endif
  parallel_buffer_repacked = (parallel_buffer_repacked == parallel_buffer_repacked1) ? parallel_buffer_repacked2 : parallel_buffer_repacked1;

  if (after - before < 50) delayMicroseconds(20);

  // portENTER_CRITICAL(&parlio_spinlock);
  for (int i = 0; i < num_chunks && i < 4; ++i) {
    ESP_ERROR_CHECK(parlio_tx_unit_transmit(parlio_tx_unit, chunk_ptrs[i], chunk_bits[i], &transmit_config));
  }
  // portEXIT_CRITICAL(&parlio_spinlock);

  #ifdef PARLIO_TIMER
  if (micros() % 100 < 3) {
    EXT_LOGD(ML_TAG, "Parallel IO for %u pixels took %lu micros at %u FPS.", length, micros() - timer, strip.getFps());
  }
  #endif

  return 0;
}
#endif