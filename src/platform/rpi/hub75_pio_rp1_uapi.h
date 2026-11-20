// SPDX-License-Identifier: GPL-3.0-or-later

/// The `/dev/pio0` ioctl interface as the RP1 PIO backend speaks it: the argument structs, the ioctl numbers and the constants it uses, redeclared from the kernel's `include/uapi/misc/rp1_pio_if.h`, and the packers for the four state machine config registers (@xref{why-the-layout-is-redefined-here|why the layout is redefined here}).
///
/// Only what the backend calls is declared, and each block cites the header's lines on rpi-6.12.y and then rpi-6.18.y. Every struct has one size and one field layout on both lines, and so has every ioctl number but the FIFO state's (@xref{the-ioctl-numbers|the ioctl numbers}). The register layouts are in @xref{the-config-registers|the config registers}.
///
/// @attribution Raspberry Pi Linux kernel
///
/// @technical
///
/// ## Why the layout is redefined here
///
/// The kernel's header uses `uint16_t` and its siblings and includes only `<linux/ioctl.h>`, so it compiles only after something else declares them, and a macOS or CI host has no kernel headers at all. Declaring the layout here lets the backend and its fake kernel build and run on every host, and the compiler pins each size, field offset and number against the values computed for arm64, with a test for the numbers.
///
/// The header's first line reads `SPDX-License-Identifier: GPL-2.0 + WITH Linux-syscall-note`, followed by `Copyright (c) 2023-26 Raspberry Pi Ltd.` (2023-24 on 6.12) and `All rights reserved.`
///
/// The syscall note's usage guide (LICENSES/exceptions/Linux-syscall-note) says it marks uapi header files "so they can be included into non GPL compliant user space application code", and its text says that programs using the kernel through system calls are not derived works.
///
/// This file restates the facts a program needs to make the calls: field types, order and sizes, the magic, the command number and the direction of each ioctl, under its own names. The header's comment text stays in the kernel.
///
/// ## The ioctl numbers
///
/// A request number is `direction << 30 | size << 16 | magic << 8 | command` (include/uapi/asm-generic/ioctl.h, which arm64 uses as it stands). The two structs that carry a pointer have the size of a 64-bit process, so the numbers are those of an aarch64 build.
///
/// The driver on both lines dispatches on the command and the size alone, ignoring the magic and the direction, and answers an unknown command with `-EOPNOTSUPP` and a wrong size with `-EINVAL`.
///
/// ## The config registers
///
/// The field positions come from the kernel's in-kernel client header `include/linux/pio_rp1.h` (`PROC_PIO_SM0_*`, GPL-2.0 alone, cited for the bit positions only, which are facts about the hardware) and agree with the RP2040 datasheet's Tables 381, 382, 383 and 386 for every field set here.
///
/// The two differ in `STATUS_SEL` and `STATUS_N`, which the backend leaves at 0. A packer masks each field to its width, so an oversized value cannot reach a neighbor.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::platform::rp1 {

/// The state machines in the RP1 PIO (RP1_PIO_SM_COUNT; rp1_pio_if.h:12 on 6.12, :12 on 6.18).
constexpr int kSmCount = 4;
/// The words of instruction memory (RP1_PIO_INSTRUCTION_COUNT; rp1_pio_if.h:11 on both).
constexpr int kInstructionCount = 32;
/// The GPIO function number that hands a pin to PIO (RP1_GPIO_FUNC_PIO; rp1_pio_if.h:14 on both).
constexpr uint16_t kGpioFunctionPio = 7;
/// The origin that lets the kernel pick the load address (RP1_PIO_ORIGIN_ANY; rp1_pio_if.h:16 on 6.12, :17 on 6.18).
constexpr uint16_t kOriginAny = 0xFFFF;
/// The transfer direction toward the state machine's TX FIFO (RP1_PIO_DIR_TO_SM; rp1_pio_if.h:18 on 6.12, :19 on 6.18).
constexpr uint16_t kDirToSm = 0;

/// The four config registers of a state machine, in the order the kernel's `rp1_pio_sm_config` holds them (rp1_pio_if.h:22-27 on 6.12, :23-28 on 6.18).
struct SmConfig {
    uint32_t clkdiv;
    uint32_t execctrl;
    uint32_t shiftctrl;
    uint32_t pinctrl;
};

/// The ADD_PROGRAM argument, 68 bytes (`rp1_pio_add_program_args`; rp1_pio_if.h:29-33 on 6.12, :30-34 on 6.18).
struct AddProgramArgs {
    uint16_t numInstrs;
    uint16_t origin;
    uint16_t instrs[kInstructionCount];
};

/// The REMOVE_PROGRAM argument, 4 bytes, `origin` being the offset ADD_PROGRAM returned (`rp1_pio_remove_program_args`; rp1_pio_if.h:35-38 on 6.12, :36-39 on 6.18).
struct RemoveProgramArgs {
    uint16_t numInstrs;
    uint16_t origin;
};

/// The SM_CLAIM argument, 2 bytes: the mask of the state machines asked for, 0 for any free one (`rp1_pio_sm_claim_args`; rp1_pio_if.h:40-42 on 6.12, :41-43 on 6.18).
struct SmClaimArgs {
    uint16_t mask;
};

/// The SM_INIT argument, 20 bytes (`rp1_pio_sm_init_args`; rp1_pio_if.h:44-48 on 6.12, :45-49 on 6.18).
struct SmInitArgs {
    uint16_t sm;
    uint16_t initialPc;
    SmConfig config;
};

/// The SM_CLEAR_FIFOS argument, 2 bytes (`rp1_pio_sm_clear_fifos_args`; rp1_pio_if.h:63-65 on 6.12, :64-66 on 6.18).
struct SmClearFifosArgs {
    uint16_t sm;
};

/// The SM_SET_PINS argument, 12 bytes: bit n of `values` and `mask` is GPIO n, and only the pins in `mask` change (`rp1_pio_sm_set_pins_args`; rp1_pio_if.h:74-79 on 6.12, :75-80 on 6.18).
struct SmSetPinsArgs {
    uint16_t sm;
    uint16_t reserved;
    uint32_t values;
    uint32_t mask;
};

/// The SM_SET_PINDIRS argument, 12 bytes, with a 1 in `dirs` for an output (`rp1_pio_sm_set_pindirs_args`; rp1_pio_if.h:81-86 on 6.12, :82-87 on 6.18).
struct SmSetPindirsArgs {
    uint16_t sm;
    uint16_t reserved;
    uint32_t dirs;
    uint32_t mask;
};

/// The SM_SET_ENABLED argument, 4 bytes: `mask` holds one bit per state machine, `enable` is 0 or 1 (`rp1_pio_sm_set_enabled_args`; rp1_pio_if.h:88-92 on 6.12, :89-93 on 6.18).
struct SmSetEnabledArgs {
    uint16_t mask;
    uint8_t enable;
    uint8_t reserved;
};

/// The SM_FIFO_STATE argument, 8 bytes: `tx` picks the FIFO, and `level`, `empty` and `full` come back in the same struct (`rp1_pio_sm_fifo_state_args`; rp1_pio_if.h:127-134 on 6.12, :135-142 on 6.18).
struct SmFifoStateArgs {
    uint16_t sm;
    uint8_t tx;
    uint8_t reserved;
    uint16_t level;
    uint8_t empty;
    uint8_t full;
};

/// The GPIO_SET_FUNCTION argument, 4 bytes (`rp1_gpio_set_function_args`; rp1_pio_if.h:140-143 on 6.12, :156-159 on 6.18).
struct GpioSetFunctionArgs {
    uint16_t gpio;
    uint16_t function;
};

/// The SM_CONFIG_XFER32 argument, 12 bytes, which takes a buffer size above the 16 bits of SM_CONFIG_XFER (`rp1_pio_sm_config_xfer32_args`; rp1_pio_if.h:163-168 on 6.12, :179-184 on 6.18).
struct SmConfigXfer32Args {
    uint16_t sm;
    uint16_t dir;
    uint32_t bufSize;
    uint32_t bufCount;
};

/// The SM_XFER_DATA32 argument, 16 bytes; the kernel rewrites it in place when a signal interrupts the call, and `data` is the user pointer as a 64-bit integer (`rp1_pio_sm_xfer_data32_args`; rp1_pio_if.h:177-182 on 6.12, :237-242 on 6.18).
struct SmXferData32Args {
    uint16_t sm;
    uint16_t dir;
    uint32_t dataBytes;
    uint64_t data;
};

static_assert(sizeof(SmConfig) == 16 && sizeof(AddProgramArgs) == 68 && sizeof(RemoveProgramArgs) == 4 && sizeof(SmClaimArgs) == 2,
              "the argument sizes are part of the ioctl numbers");
static_assert(sizeof(SmInitArgs) == 20 && sizeof(SmClearFifosArgs) == 2 && sizeof(SmSetPinsArgs) == 12 && sizeof(SmSetPindirsArgs) == 12,
              "the argument sizes are part of the ioctl numbers");
static_assert(sizeof(SmSetEnabledArgs) == 4 && sizeof(SmFifoStateArgs) == 8 && sizeof(GpioSetFunctionArgs) == 4,
              "the argument sizes are part of the ioctl numbers");
static_assert(sizeof(SmConfigXfer32Args) == 12 && sizeof(SmXferData32Args) == 16, "the argument sizes are part of the ioctl numbers");

// The field offsets the kernel's structs have on arm64 (computed from the header with LP64 natural alignment and checked by the compiler): a swapped pair of fields fails the build.
static_assert(offsetof(SmConfig, clkdiv) == 0 && offsetof(SmConfig, execctrl) == 4 && offsetof(SmConfig, shiftctrl) == 8 && offsetof(SmConfig, pinctrl) == 12, "SmConfig layout");
static_assert(offsetof(AddProgramArgs, numInstrs) == 0 && offsetof(AddProgramArgs, origin) == 2 && offsetof(AddProgramArgs, instrs) == 4, "AddProgramArgs layout");
static_assert(offsetof(RemoveProgramArgs, numInstrs) == 0 && offsetof(RemoveProgramArgs, origin) == 2, "RemoveProgramArgs layout");
static_assert(offsetof(SmInitArgs, sm) == 0 && offsetof(SmInitArgs, initialPc) == 2 && offsetof(SmInitArgs, config) == 4, "SmInitArgs layout");
static_assert(offsetof(SmSetPinsArgs, sm) == 0 && offsetof(SmSetPinsArgs, reserved) == 2 && offsetof(SmSetPinsArgs, values) == 4 && offsetof(SmSetPinsArgs, mask) == 8, "SmSetPinsArgs layout");
static_assert(offsetof(SmSetPindirsArgs, sm) == 0 && offsetof(SmSetPindirsArgs, reserved) == 2 && offsetof(SmSetPindirsArgs, dirs) == 4 && offsetof(SmSetPindirsArgs, mask) == 8, "SmSetPindirsArgs layout");
static_assert(offsetof(SmSetEnabledArgs, mask) == 0 && offsetof(SmSetEnabledArgs, enable) == 2 && offsetof(SmSetEnabledArgs, reserved) == 3, "SmSetEnabledArgs layout");
static_assert(offsetof(SmFifoStateArgs, sm) == 0 && offsetof(SmFifoStateArgs, tx) == 2 && offsetof(SmFifoStateArgs, reserved) == 3 && offsetof(SmFifoStateArgs, level) == 4 &&
              offsetof(SmFifoStateArgs, empty) == 6 && offsetof(SmFifoStateArgs, full) == 7, "SmFifoStateArgs layout");
static_assert(offsetof(GpioSetFunctionArgs, gpio) == 0 && offsetof(GpioSetFunctionArgs, function) == 2, "GpioSetFunctionArgs layout");
static_assert(offsetof(SmConfigXfer32Args, sm) == 0 && offsetof(SmConfigXfer32Args, dir) == 2 && offsetof(SmConfigXfer32Args, bufSize) == 4 && offsetof(SmConfigXfer32Args, bufCount) == 8, "SmConfigXfer32Args layout");
static_assert(offsetof(SmXferData32Args, sm) == 0 && offsetof(SmXferData32Args, dir) == 2 && offsetof(SmXferData32Args, dataBytes) == 4 && offsetof(SmXferData32Args, data) == 8, "SmXferData32Args layout");

/// The ioctl magic, ASCII `f` (PIO_IOC_MAGIC; rp1_pio_if.h:190 on 6.12, :282 on 6.18).
constexpr uint32_t kIoctlMagic = 102;

/// The ioctl direction a request carries when user space writes its argument (`_IOC_WRITE`, include/uapi/asm-generic/ioctl.h).
constexpr uint32_t kIocWrite = 1;
/// The ioctl direction bit for an argument the kernel writes back (`_IOC_READ`, include/uapi/asm-generic/ioctl.h).
constexpr uint32_t kIocRead = 2;

/// The request number for `direction`, `command` and an argument of `argBytes` bytes, as the generic ioctl layout builds it.
constexpr unsigned long ioctlNumber(uint32_t direction, uint32_t command, size_t argBytes) {
    return static_cast<unsigned long>(direction) << 30 | static_cast<unsigned long>(argBytes) << 16 | static_cast<unsigned long>(kIoctlMagic) << 8 | command;
}

/// The command number of a request, which the driver dispatches on with the size; the fake kernel and the tests read it.
constexpr uint32_t ioctlCommand(unsigned long request) { return static_cast<uint32_t>(request & 0xFF); }

/// The argument size a request encodes, which the driver compares with the handler's; the fake kernel and the tests read it.
constexpr size_t ioctlArgBytes(unsigned long request) { return static_cast<size_t>((request >> 16) & 0x3FFF); }

/// PIO_IOC_SM_XFER_DATA32, command 2 (rp1_pio_if.h:194 on 6.12, :286 on 6.18): one call takes a whole frame, which the driver cuts into ring buffers.
constexpr unsigned long kIocSmXferData32 = ioctlNumber(kIocWrite, 2, sizeof(SmXferData32Args));
/// PIO_IOC_SM_CONFIG_XFER32, command 3 (rp1_pio_if.h:195 on 6.12, :287 on 6.18).
constexpr unsigned long kIocSmConfigXfer32 = ioctlNumber(kIocWrite, 3, sizeof(SmConfigXfer32Args));
/// PIO_IOC_ADD_PROGRAM, command 11 (rp1_pio_if.h:201 on 6.12, :294 on 6.18): returns the load offset, and 0 is a valid one.
constexpr unsigned long kIocAddProgram = ioctlNumber(kIocWrite, 11, sizeof(AddProgramArgs));
/// PIO_IOC_REMOVE_PROGRAM, command 12 (rp1_pio_if.h:202 on 6.12, :295 on 6.18).
constexpr unsigned long kIocRemoveProgram = ioctlNumber(kIocWrite, 12, sizeof(RemoveProgramArgs));
/// PIO_IOC_SM_CLAIM, command 20 (rp1_pio_if.h:205 on 6.12, :298 on 6.18): returns the state machine's index.
constexpr unsigned long kIocSmClaim = ioctlNumber(kIocWrite, 20, sizeof(SmClaimArgs));
/// PIO_IOC_SM_INIT, command 30 (rp1_pio_if.h:209 on 6.12, :302 on 6.18).
constexpr unsigned long kIocSmInit = ioctlNumber(kIocWrite, 30, sizeof(SmInitArgs));
/// PIO_IOC_SM_CLEAR_FIFOS, command 33 (rp1_pio_if.h:212 on 6.12, :305 on 6.18).
constexpr unsigned long kIocSmClearFifos = ioctlNumber(kIocWrite, 33, sizeof(SmClearFifosArgs));
/// PIO_IOC_SM_SET_PINS, command 35 (rp1_pio_if.h:214 on 6.12, :307 on 6.18).
constexpr unsigned long kIocSmSetPins = ioctlNumber(kIocWrite, 35, sizeof(SmSetPinsArgs));
/// PIO_IOC_SM_SET_PINDIRS, command 36 (rp1_pio_if.h:215 on 6.12, :308 on 6.18).
constexpr unsigned long kIocSmSetPindirs = ioctlNumber(kIocWrite, 36, sizeof(SmSetPindirsArgs));
/// PIO_IOC_SM_SET_ENABLED, command 37 (rp1_pio_if.h:216 on 6.12, :309 on 6.18).
constexpr unsigned long kIocSmSetEnabled = ioctlNumber(kIocWrite, 37, sizeof(SmSetEnabledArgs));
/// PIO_IOC_SM_FIFO_STATE, command 44, in the 6.18 form `_IOWR` (rp1_pio_if.h:316): the kernel writes the result back, and the driver on 6.12, which declares `_IOW` (:223), dispatches on command and size, so both lines take this number.
constexpr unsigned long kIocSmFifoState = ioctlNumber(kIocWrite | kIocRead, 44, sizeof(SmFifoStateArgs));
/// PIO_IOC_GPIO_SET_FUNCTION, command 51 (rp1_pio_if.h:227 on 6.12, :322 on 6.18): one call a pin.
constexpr unsigned long kIocGpioSetFunction = ioctlNumber(kIocWrite, 51, sizeof(GpioSetFunctionArgs));

/// Packs CLKDIV: the integer divider in bits 31:16 and the fraction in 1/256ths in bits 15:8 (PROC_PIO_SM0_CLKDIV_INT_LSB and _FRAC_LSB, pio_rp1.h:71-72 on 6.18; RP2040 Table 381).
constexpr uint32_t packClkdiv(uint16_t intPart, uint8_t fracPart) {
    return static_cast<uint32_t>(intPart) << 16 | static_cast<uint32_t>(fracPart) << 8;
}

/// The EXECCTRL fields the backend sets; the rest of the register stays 0 (RP2040 Table 382, pio_rp1.h:67-76 on 6.18).
struct ExecControl {
    bool sideEnable = false;       ///< SIDE_EN, bit 30: the side-set value is optional
    bool sidePindirs = false;      ///< SIDE_PINDIR, bit 29: side-set drives the pin directions
    uint8_t wrapTop = 0;           ///< WRAP_TOP, bits 16:12
    uint8_t wrapBottom = 0;        ///< WRAP_BOTTOM, bits 11:7
};

/// Packs EXECCTRL from its fields, each masked to its width.
constexpr uint32_t packExecctrl(const ExecControl& c) {
    return (c.sideEnable ? 1u << 30 : 0u) | (c.sidePindirs ? 1u << 29 : 0u) |
           (static_cast<uint32_t>(c.wrapTop) & 0x1Fu) << 12 | (static_cast<uint32_t>(c.wrapBottom) & 0x1Fu) << 7;
}

/// The SHIFTCTRL fields the backend sets (RP2040 Table 383, pio_rp1.h:79-94 on 6.18); a threshold of 0 means 32 bits.
struct ShiftControl {
    bool joinRx = false;           ///< FJOIN_RX, bit 31
    bool joinTx = false;           ///< FJOIN_TX, bit 30: the TX FIFO takes the RX FIFO's storage
    uint8_t pullThreshold = 0;     ///< PULL_THRESH, bits 29:25: the autopull threshold
    uint8_t pushThreshold = 0;     ///< PUSH_THRESH, bits 24:20: the autopush threshold
    bool outShiftRight = false;    ///< OUT_SHIFTDIR, bit 19: 1 shifts the OUT register right
    bool inShiftRight = false;     ///< IN_SHIFTDIR, bit 18: 1 shifts the IN register right
    bool autoPull = false;         ///< AUTOPULL, bit 17
    bool autoPush = false;         ///< AUTOPUSH, bit 16
};

/// Packs SHIFTCTRL from its fields, each masked to its width.
constexpr uint32_t packShiftctrl(const ShiftControl& c) {
    return (c.joinRx ? 1u << 31 : 0u) | (c.joinTx ? 1u << 30 : 0u) | (static_cast<uint32_t>(c.pullThreshold) & 0x1Fu) << 25 |
           (static_cast<uint32_t>(c.pushThreshold) & 0x1Fu) << 20 | (c.outShiftRight ? 1u << 19 : 0u) | (c.inShiftRight ? 1u << 18 : 0u) |
           (c.autoPull ? 1u << 17 : 0u) | (c.autoPush ? 1u << 16 : 0u);
}

/// The PINCTRL fields the backend sets (RP2040 Table 386, pio_rp1.h:53-66 on 6.18); the side-set count includes the enable bit.
struct PinControl {
    uint8_t sidesetCount = 0;      ///< SIDESET_COUNT, bits 31:29
    uint8_t setCount = 0;          ///< SET_COUNT, bits 28:26
    uint8_t outCount = 0;          ///< OUT_COUNT, bits 25:20
    uint8_t inBase = 0;            ///< IN_BASE, bits 19:15
    uint8_t sidesetBase = 0;       ///< SIDESET_BASE, bits 14:10
    uint8_t setBase = 0;           ///< SET_BASE, bits 9:5
    uint8_t outBase = 0;           ///< OUT_BASE, bits 4:0
};

/// Packs PINCTRL from its fields, each masked to its width.
constexpr uint32_t packPinctrl(const PinControl& c) {
    return (static_cast<uint32_t>(c.sidesetCount) & 0x7u) << 29 | (static_cast<uint32_t>(c.setCount) & 0x7u) << 26 |
           (static_cast<uint32_t>(c.outCount) & 0x3Fu) << 20 | (static_cast<uint32_t>(c.inBase) & 0x1Fu) << 15 |
           (static_cast<uint32_t>(c.sidesetBase) & 0x1Fu) << 10 | (static_cast<uint32_t>(c.setBase) & 0x1Fu) << 5 |
           (static_cast<uint32_t>(c.outBase) & 0x1Fu);
}

}  // namespace mm::platform::rp1
