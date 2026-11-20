// SPDX-License-Identifier: GPL-3.0-or-later

/// A minimal C wrapper over the piolib calls the HUB75 PIO engine uses, so the C++ backend (hub75_pio_backend_piolib.cpp) reaches piolib through hub75_pio_c_api.c, compiled as C (@xref{why-a-c-wrapper|why a C wrapper}).
///
/// The PIO instance is an opaque `void*` handle and the state machine an `int`. A call that can fail returns 0, a count or an offset on success and a negative errno-style code on failure, which the backend turns into a message with its strerror text; a call that cannot report failure returns void; the two the stop needs, disable and set pins, report it from errno (@xref{silent-calls|silent calls}).
///
/// Beyond the calls: @xref{idle-pins|the pins' idle state}, @xref{the-c-and-c-boundary|the C and C++ boundary}, @xref{building-piolib|building piolib}, @xref{fifo-depth|FIFO depth} and @xref{pins-the-kernel-may-own|the pins the kernel may own}.
///
/// @technical
///
/// ## Why a C wrapper
///
/// piolib.h is C-only and does not compile as C++: its irq_get_exclusive_handler() converts void* to a function pointer, which C++ rejects.
///
/// Rather than compiling the backend with -fpermissive, the piolib calls live behind this small C interface and the C++ backend talks to it. Only the dozen calls the engine needs are wrapped, and the rest of piolib stays out of the C++ code.
///
/// ## The C and C++ boundary
///
/// No exception crosses it, since the C side and everything it calls throw none. Errors cross as return codes rather than errno, which a later call could overwrite.
///
/// The handle is owned by the C file for the life of the process (see hub75_pio_release), and the C++ side only borrows it. Every pointer passed in is valid for the call alone, except the program instructions, which the caller keeps until it removes the program.
///
/// ## Idle pins
///
/// hub75_pio_init_sm() sets the pins to `idle_wide_word` (a wide word, bit 0 = GPIO 2) from the moment PIO takes them, so the panel stays dark until the first frame; hub75_pio_set_pins() sets them again at the stop, with the same call.
///
/// ## Submit semantics
///
/// hub75_pio_xfer() returns 0 or a negative errno-style code, and what a return means depends on the kernel: see the submit section of hub75_pio_backend.h. Every code comes back, -ETIMEDOUT and -EREMOTEIO included, because the pinned piolib's rp1_ioctl returns them (stock piolib from commit bb3e2622 exits the process on those two); on a kernel from 6.18.39 the call can block with no timeout.
///
/// ## Silent configuration
///
/// The piolib calls hub75_pio_init_sm() makes (pin values, directions, mux, state machine init, clock divider) discard their ioctl results, so a refused configuration is silent and the function returns void. The first transfer is where a state machine that failed to start shows, as a submit that fails or blocks.
///
/// ## Silent calls
///
/// The pinned piolib's `pio_sm_set_enabled` and `pio_sm_set_pins_with_mask` return void, discard their ioctl's result and set no error flag (pio_rp1.c, read 2026-11-19), so the call alone cannot say whether the SM was disabled or the pins set.
///
/// A failed ioctl does leave its code in errno and a successful one leaves errno alone, so the shim clears errno before the call and reads it after, and also reads piolib's error flag in case another piolib sets it. The result is the errno as a negative code, or -EIO for the flag alone.
///
/// The stop turns a failure into its error text and a false return, so the engine can say whether the panel is dark. The same calls in hub75_pio_init_sm() and the start's enable stay silent.
///
/// ## Pins the kernel may own
///
/// The mux takes GPIO 2 to 27 for PIO whichever kernel driver owns them. See hub75_board_pins.h.
///
/// ## Building piolib
///
/// piolib must be built with its parameter assertions off (PARAM_ASSERTIONS_ENABLED_PIO 0, its default): rp1_pio_sm_set_consecutive_pindirs asserts `pin_base + pin_count < 28`, and the OUT range here is 2 + 26 = 28, so with assertions on the call would abort.
///
/// ## FIFO depth
///
/// The sources give the TX FIFO as 8 words per direction (RP1_PIO_FIFO_DEPTH, the chip's fifo_depth). They do not say what the joined TX FIFO holds, so no code assumes more than 8 words.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opens pio0 with pio_open(0), which returns the error and does not exit, once per process (a second call would be -EBUSY, so the handle is kept); NULL with the positive errno in `*err` (EINVAL: no instance, EACCES: unreadable node).
void* hub75_pio_open(int* err);

/// Claims a state machine and returns its number, or a negative errno-style code; the one this process held before when it is free, so its DMA claim is reused, else any free one.
int hub75_pio_claim_sm(void* pio);

/// Loads a program (instructions, length in words, .pio_version 0); returns the load offset, or -ENOSPC when the load is refused for lack of free instruction memory, which piolib reports by its error flag alone.
int hub75_pio_add_program(void* pio, const uint16_t* instructions, int length);

/// Configures the DMA transfer ring for one SM (buf_size a multiple of 4, buf_count <= 4); returns 0 or a negative errno-style code: -EBUSY when another client holds the DMA, -EINVAL for a size the kernel refuses.
int hub75_pio_config_xfer(void* pio, int sm, uint32_t buf_size,
                          uint32_t buf_count);

/// What hub75_pio_init_sm() needs about the loaded program and the clock: where the program sits, its wrap bounds, the clock divider and the word the pins idle at.
struct hub75_pio_sm_params {
    int offset;               ///< program load offset from hub75_pio_add_program()
    int wrap_target;          ///< the program's wrap target, relative to the offset
    int wrap;                 ///< the program's wrap, relative to the offset
    uint16_t clkdiv_int;      ///< SM clock divider integer part
    uint8_t clkdiv_frac;      ///< SM clock divider fraction in 1/256ths
    uint32_t idle_wide_word;  ///< the wide word the pins hold from the moment PIO takes them
};

/// Initializes the SM for hub75_wide (GPIO 2..27 out, side-set on 17, shift right, no autopull, TX join, divider, wrap) at the offset, leaving it disabled; silent (@xref{silent-configuration|silent configuration}).
void hub75_pio_init_sm(void* pio, int sm, const struct hub75_pio_sm_params* params);

/// Sets the OUT pins (GPIO 2..27) to a wide word, bit 0 on GPIO 2, through the SM: one firmware message, which works with the SM disabled; returns 0, or a negative errno-style code when the ioctl failed (@xref{silent-calls|silent calls}).
int hub75_pio_set_pins(void* pio, int sm, uint32_t wide_word);

/// Clears the SM's FIFOs.
void hub75_pio_clear_fifos(void* pio, int sm);

/// Enables (1) or disables (0) the SM; returns 0, or a negative errno-style code when the ioctl failed (@xref{silent-calls|silent calls}).
int hub75_pio_set_enabled(void* pio, int sm, int enable);

/// DMA transfer of one frame into the SM's TX FIFO; returns 0 on success or a negative errno-style code, and its blocking depends on the kernel (@xref{submit-semantics|submit semantics}).
int hub75_pio_xfer(void* pio, int sm, void* data, uint32_t bytes);

/// True (1) when the TX FIFO is empty.
int hub75_pio_tx_fifo_empty(void* pio, int sm);

/// Removes a previously loaded program.
void hub75_pio_remove_program(void* pio, const uint16_t* instructions,
                              int length, int offset);

/// Releases the SM; the instance stays open for the process, and the SM's DMA claim stays with it, reused when the next claim gets the same SM.
void hub75_pio_release(void* pio, int sm);

#ifdef __cplusplus
}
#endif
