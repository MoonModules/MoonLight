/// The installer's state documents over the USB serial port, so a device model's defaults apply right after an install.
///
/// @technical
///
/// ## Only the state document
///
/// The Teensy has no WiFi to provision, so it takes the one serial command that carries a state document (APPLY_OP) and refuses the rest. The same core helper the ESP32 uses handles it.
/// It shares the port with the log, as the ESP32's listener does: a frame's start marker and checksum pick it out of everything else.
///
/// ## Polled, since there is no task
///
/// The core has no RTOS, so the module's tick drains the port through @ref improvPoll, a bounded batch per tick so a flood cannot hold the render loop.

#include "platform/platform.h"
#include "core/util/ImprovFrame.h"
#include "core/util/ImprovOpReassembler.h"

#include <Arduino.h>

#include <atomic>
#include <cstdio>

namespace mm::platform {

namespace {
char* g_opOut = nullptr;
size_t g_opOutLen = 0;
std::atomic<bool>* g_opReady = nullptr;
ImprovFrameParser g_parser;

// A reply goes out only when it fits the USB buffer at once, so a host that stopped reading cannot stall the tick; the installer does not wait for one.
void send(ImprovFrameType type, const uint8_t* payload, size_t len) MM_NONBLOCKING {
    uint8_t frame[6 + 1 + 1 + 1 + kImprovMaxPayload + 1];
    const size_t n = buildImprovFrame(type, payload, len, frame, sizeof(frame));
    if (n && static_cast<size_t>(Serial.availableForWrite()) >= n) Serial.write(frame, n);
}

void dispatch() MM_NONBLOCKING {
    if (g_parser.lastType() != static_cast<uint8_t>(ImprovFrameType::Rpc)) return;
    const uint8_t* payload = g_parser.lastPayload();
    const uint8_t len = g_parser.lastPayloadLen();
    if (len >= 1 && payload[0] == kImprovCmdApplyOp) {
        static ImprovOpReassembler reasm(g_opOut, g_opOutLen);   // bound once, the buffer living as long as the module
        if (improvApplyOp(reasm, payload, len, *g_opReady) == ImprovApplyOpReply::Ack) {
            const uint8_t ack[] = {kImprovCmdApplyOp, 0};   // an empty response for the command
            send(ImprovFrameType::RpcResponse, ack, sizeof(ack));
        } else {
            send(ImprovFrameType::ErrorState, &kImprovErrorInvalidOp, 1);
        }
        return;
    }
    send(ImprovFrameType::ErrorState, &kImprovErrorUnknownRpc, 1);
}
}  // namespace

bool improvProvisioningInit(const ImprovDeviceInfo&, char*, size_t, char*, size_t, std::atomic<bool>*,
                            char* statusBuf, size_t statusBufLen, uint8_t*, std::atomic<bool>*,
                            char* opOut, size_t opOutLen, std::atomic<bool>* opReady) {
    if (!opOut || opOutLen == 0 || !opReady) {
        if (statusBuf && statusBufLen) std::snprintf(statusBuf, statusBufLen, "error: no buffer for state documents");
        return false;
    }
    g_opOut = opOut;
    g_opOutLen = opOutLen;
    g_opReady = opReady;
    if (statusBuf && statusBufLen) std::snprintf(statusBuf, statusBufLen, "listening on USB serial");
    return true;
}

void improvPoll() MM_NONBLOCKING {
    if (!g_opReady) return;
    for (int i = 0; i < 256 && Serial.available() > 0; i++) {
        switch (g_parser.feed(static_cast<uint8_t>(Serial.read()))) {
            case ImprovFeedResult::FrameReady: dispatch(); break;
            case ImprovFeedResult::BadChecksum: send(ImprovFrameType::ErrorState, &kImprovErrorInvalidRpc, 1); break;
            default: break;
        }
    }
}

}  // namespace mm::platform
