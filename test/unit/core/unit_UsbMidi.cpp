/// @module UsbMidi

/// USB-MIDI event packets: a desk's messages in and out of the 4-byte packets the USB MIDI class carries.

#include "doctest.h"
#include "core/util/UsbMidi.h"

#include <cstring>

using namespace mm;

// A knob turn and a pad press, as an APC40 sends them over USB.
TEST_CASE("a USB-MIDI packet carries one channel message, its CIN the status nibble") {
    const uint8_t knob[4] = {0x0B, 0xB0, 0x30, 0x40}, press[4] = {0x09, 0x90, 0x20, 0x7F};
    uint8_t m[3] = {};
    REQUIRE(usbmidi::decode(knob, m) == 3);
    CHECK((m[0] == 0xB0 && m[1] == 0x30 && m[2] == 0x40));
    REQUIRE(usbmidi::decode(press, m) == 3);
    CHECK((m[0] == 0x90 && m[1] == 0x20 && m[2] == 0x7F));
    const uint8_t sysexPiece[4] = {0x04, 0xF0, 0x47, 0x7F};
    CHECK(usbmidi::decode(sysexPiece, m) == 0);   // a desk's input never needs SysEx
}

// The way back: a light, and the APC40's greeting cut into SysEx packets that end with however many bytes are left.
TEST_CASE("a message is cut into USB-MIDI packets, a SysEx one ending with the bytes that are left") {
    uint8_t out[8][4] = {};
    const uint8_t light[3] = {0x90, 0x20, 0x15};
    REQUIRE(usbmidi::encode(light, 3, out, 8) == 1);
    CHECK((out[0][0] == 0x09 && out[0][1] == 0x90 && out[0][2] == 0x20 && out[0][3] == 0x15));

    const uint8_t hello[12] = {0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x42, 0x01, 0x00, 0x00, 0xF7};
    REQUIRE(usbmidi::encode(hello, 12, out, 8) == 4);
    CHECK(out[0][0] == 0x04);
    CHECK(out[2][0] == 0x04);
    CHECK((out[2][3] == 0x01 && out[3][0] == 0x07 && out[3][3] == 0xF7));   // three bytes left: CIN 7
    const uint8_t two[5] = {0xF0, 0x01, 0x02, 0x03, 0xF7};
    REQUIRE(usbmidi::encode(two, 5, out, 8) == 2);
    CHECK((out[1][0] == 0x06 && out[1][1] == 0x03 && out[1][2] == 0xF7 && out[1][3] == 0x00));   // two left: CIN 6
}

// Robustness: what is not a message, or does not fit, writes nothing.
TEST_CASE("a malformed or oversized message is not cut into packets") {
    uint8_t out[2][4] = {};
    const uint8_t data[2] = {0x20, 0x30}, unterminated[4] = {0xF0, 0x01, 0x02, 0x03};
    const uint8_t longSysex[9] = {0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7};
    CHECK(usbmidi::encode(data, 2, out, 2) == 0);
    CHECK(usbmidi::encode(unterminated, 4, out, 2) == 0);
    CHECK(usbmidi::encode(longSysex, 9, out, 2) == 0);   // three packets do not fit in two
    CHECK(usbmidi::encode(nullptr, 3, out, 2) == 0);
}
