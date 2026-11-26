/// @module InstallationId

#include "doctest.h"
#include "core/system/MoonCloudModule.h"
#include "core/util/sha256.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "platform/platform_system.h"   // getMacAddress

namespace {
std::string id() {
    char buf[mm::kInstallationIdChars + 1] = {};
    mm::installationId(buf);
    return std::string(buf);
}
}  // namespace

/// The id is 32 lowercase hex characters, which is what the server stores and what the privacy policy describes.
TEST_CASE("the installation id is 32 hex characters") {
    const std::string value = id();
    CHECK(value.size() == mm::kInstallationIdChars);
    for (char c : value) {
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(isHex);
    }
}

/// The same installation reports the same id every time, which is the property the whole feature rests on: without it an upgrade cannot be told from a new install.
TEST_CASE("the installation id is stable across calls") {
    CHECK(id() == id());
}

/// The id is not the MAC address, in any recognizable form.
///
/// The privacy policy promises the address itself is never sent.
/// A hash that happened to contain the address as a substring, or that were simply the address in hex, would break that promise while still looking like an opaque identifier.
TEST_CASE("the installation id does not contain the underlying address") {
    uint8_t mac[6] = {};
    mm::platform::getMacAddress(mac);

    char macHex[13] = {};
    std::snprintf(macHex, sizeof(macHex), "%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const std::string value = id();
    CHECK(value.find(macHex) == std::string::npos);

    // Nor any four-byte run of it, which would leak most of the address.
    for (int i = 0; i + 4 <= 6; i++) {
        char run[9] = {};
        std::snprintf(run, sizeof(run), "%02x%02x%02x%02x",
                      mac[i], mac[i + 1], mac[i + 2], mac[i + 3]);
        CHECK(value.find(run) == std::string::npos);
    }
}

/// A different address gives a different id, so two installations are told apart.
///
/// Checked through the hash directly rather than by moving the platform's address, which a unit test cannot do: the property under test is that the construction separates its inputs.
TEST_CASE("a different address produces a different installation id") {
    // The REAL salt, not a copy of its text: retyping it means a salt change passes this test while silently re-identifying every installation in the world.
    const size_t saltLen = std::strlen(mm::kMoonCloudSalt);
    uint8_t a[64] = {}, b[64] = {};
    std::memcpy(a, mm::kMoonCloudSalt, saltLen);
    std::memcpy(b, mm::kMoonCloudSalt, saltLen);
    const uint8_t macA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t macB[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x56};  // one bit apart
    std::memcpy(a + saltLen, macA, 6);
    std::memcpy(b + saltLen, macB, 6);

    char idA[33] = {}, idB[33] = {};
    mm::sha256Hex(a, saltLen + 6, idA, 16);
    mm::sha256Hex(b, saltLen + 6, idB, 16);
    CHECK(std::string(idA) != std::string(idB));
}

/// The MoonStats id differs from any other identifier derived from the same address, so a report cannot be matched against the MQTT topic or Home Assistant unique_id a device publishes on the user's own network.
/// That separation is the salt's job.
TEST_CASE("the installation id cannot be correlated with the network identity") {
    uint8_t mac[6] = {};
    mm::platform::getMacAddress(mac);

    // What an integration identity exposes: the last three bytes, in hex, on the local network.
    char networkIdentity[7] = {};
    std::snprintf(networkIdentity, sizeof(networkIdentity), "%02x%02x%02x", mac[3], mac[4], mac[5]);

    // An unsalted hash of the same address, which is what a naive implementation would send.
    char unsalted[33] = {};
    mm::sha256Hex(mac, sizeof(mac), unsalted, 16);

    const std::string value = id();
    CHECK(value.find(networkIdentity) == std::string::npos);
    CHECK(value != std::string(unsalted));
}
