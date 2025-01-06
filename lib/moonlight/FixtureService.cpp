/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2024 theelims
 *file
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <FixtureService.h>

#define EVENT_MONITOR "monitor"

void FixtureState::read(FixtureState &fixtureState, JsonObject &root)
{
    root["lightsOn"] = fixtureState.lightsOn;
    root["brightness"] = fixtureState.brightness;
    root["width"] = fixtureState.width;
    root["height"] = fixtureState.height;
    root["depth"] = fixtureState.depth;
    root["driverOn"] = fixtureState.driverOn;
    root["monitorOn"] = fixtureState.monitorOn;
    root["pin"] = fixtureState.pin;
}

StateUpdateResult FixtureState::update(JsonObject &root, FixtureState &fixtureState)
{
    bool changed = false;

    if (fixtureState.lightsOn != root["lightsOn"]) {fixtureState.lightsOn = root["lightsOn"]; changed = true;}
    if (fixtureState.brightness != root["brightness"]) {fixtureState.brightness = root["brightness"]; changed = true;}
    if (fixtureState.width != root["width"]) {fixtureState.width = root["width"]; changed = true;}
    if (fixtureState.height != root["height"]) {fixtureState.height = root["height"]; changed = true;}
    if (fixtureState.depth != root["depth"]) {fixtureState.depth = root["depth"]; changed = true;}
    if (fixtureState.driverOn != root["driverOn"]) {fixtureState.driverOn = root["driverOn"]; changed = true;}
    if (fixtureState.monitorOn != root["monitorOn"]) {fixtureState.monitorOn = root["monitorOn"]; changed = true;}
    if (fixtureState.pin != root["pin"]) {fixtureState.pin = root["pin"]; changed = true;}

    if (changed)
        Serial.printf("FixtureState::update o:%d b:%d\n", fixtureState.lightsOn, fixtureState.brightness);

    return changed?StateUpdateResult::CHANGED:StateUpdateResult::UNCHANGED;
}

FixtureService::FixtureService(PsychicHttpServer *server,
                                     EventSocket *socket,
                                     SecurityManager *securityManager, FS *fs) : _httpEndpoint(FixtureState::read,
                                                                                                         FixtureState::update,
                                                                                                         this,
                                                                                                         server,
                                                                                                         "/rest/fixtureState",
                                                                                                         securityManager,
                                                                                                         AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                           _eventEndpoint(FixtureState::read,
                                                                                                          FixtureState::update,
                                                                                                          this,
                                                                                                          socket,
                                                                                                          "fixture"),
                                                                                           _webSocketServer(FixtureState::read,
                                                                                                            FixtureState::update,
                                                                                                            this,
                                                                                                            server,
                                                                                                            "/ws/fixtureState",
                                                                                                            securityManager,
                                                                                                            AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                            _socket(socket),
                                                                                             _fsPersistence(FixtureState::read,
                                                                                                      FixtureState::update,
                                                                                                      this,
                                                                                                      fs,
                                                                                                      "/config/fixtureState.json")
{

    // configure settings service update handler to update state
    addUpdateHandler([&](const String &originId)
                     { onConfigUpdated(); },
                     false);
}

void FixtureService::begin()
{
    _httpEndpoint.begin();
    _eventEndpoint.begin();
    _fsPersistence.readFromFS();

    onConfigUpdated();

    FastLED.setMaxPowerInMilliWatts(10 * 1000); // 5v, 2000mA

    FastLED.addLeds<NEOPIXEL, 16>(_state.ledsP, 0, 256).setCorrection(TypicalLEDStrip);

    _socket->registerEvent(EVENT_MONITOR);
}

void FixtureService::onConfigUpdated()
{
    Serial.printf("FixtureService::onConfigUpdated o:%d b:%d\n", _state.lightsOn, _state.brightness);

    FastLED.setBrightness(_state.lightsOn?_state.brightness/8:0);
}

void FixtureService::loop()
{
    nrOfFrames++;
    if (_state.driverOn)
        FastLED.show();

    if (_state.monitorOn)
        _socket->emitEvent(EVENT_MONITOR, (char *)_state.ledsP, min(_state.width*_state.height*_state.depth, STARLIGHT_MAXLEDS) * sizeof(CRGB)); // * sizeof(CRGB)
}