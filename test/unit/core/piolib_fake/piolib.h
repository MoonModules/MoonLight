/// A declarations-only stand-in for piolib.h, written for unit_platform_hub75pio_c_api.cpp from the calls the shim makes and the signatures of the Pi OS package's header (compiled against the real header to check them). It is NOT piolib and carries none of its code: the test defines these functions as a script.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int uint;
typedef struct pio_instance *PIO;
typedef struct { const uint16_t *instructions; uint8_t length; int8_t origin; uint8_t pio_version; } pio_program_t;
typedef struct { uint32_t clkdiv, execctrl, shiftctrl, pinctrl; } pio_sm_config;

#define PIO_ORIGIN_INVALID ((uint)-1)
#define PIO_ERR(x) ((PIO)(uintptr_t)(x))
#define PIO_IS_ERR(x) (((uintptr_t)(x) >= (uintptr_t)-200))
#define PIO_ERR_VAL(x) ((int)(uintptr_t)(x))
enum { PIO_DIR_TO_SM = 0, PIO_DIR_FROM_SM = 1 };
enum { PIO_FIFO_JOIN_NONE = 0, PIO_FIFO_JOIN_TX = 1 };

PIO pio_open(uint idx);
void pio_select(PIO pio);
void pio_enable_fatal_errors(PIO pio, bool enable);
void pio_clear_error(PIO pio);
bool pio_get_error(PIO pio);
int pio_claim_unused_sm(PIO pio, bool required);
bool pio_sm_is_claimed(PIO pio, uint sm);
void pio_sm_claim(PIO pio, uint sm);
void pio_sm_unclaim(PIO pio, uint sm);
uint pio_add_program(PIO pio, const pio_program_t *program);
void pio_remove_program(PIO pio, const pio_program_t *program, uint loaded_offset);
int pio_sm_config_xfer(PIO pio, uint sm, uint dir, uint buf_size, uint buf_count);
int pio_sm_xfer_data(PIO pio, uint sm, uint dir, uint data_bytes, void *data);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t pinvals, uint32_t pin_mask);
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pins, uint count, bool is_out);
void pio_gpio_init(PIO pio, uint pin);
pio_sm_config pio_get_default_sm_config(void);
void sm_config_set_wrap(pio_sm_config *c, uint wrap_target, uint wrap);
void sm_config_set_sideset(pio_sm_config *c, uint bit_count, bool optional, bool pindirs);
void sm_config_set_out_pins(pio_sm_config *c, uint out_base, uint out_count);
void sm_config_set_sideset_pins(pio_sm_config *c, uint sideset_base);
void sm_config_set_out_shift(pio_sm_config *c, bool shift_right, bool autopull, uint pull_threshold);
void sm_config_set_fifo_join(pio_sm_config *c, uint join);
void sm_config_set_clkdiv_int_frac(pio_sm_config *c, uint16_t div_int, uint8_t div_frac);
void pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm);

#ifdef __cplusplus
}
#endif
