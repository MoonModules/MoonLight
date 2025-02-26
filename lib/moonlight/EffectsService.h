#ifndef EffectsService_h
#define EffectsService_h

/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2024 theelims
 *   Copyright (C) 2025 - 2025 ewowi
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <EventSocket.h>
#include <HttpEndpoint.h>
#include <EventEndpoint.h>
#include <WebSocketServer.h>
#include <PsychicHttp.h>
#include <FSPersistence.h>
#include "FixtureService.h"

class EffectsState
{
public:

    uint16_t effect = UINT16_MAX;
    uint16_t projection = UINT16_MAX;

    static void read(EffectsState &state, JsonObject &root);

    static StateUpdateResult update(JsonObject &root, EffectsState &state);
};

class EffectsService : public StatefulService<EffectsState>
{
public:
    EffectsService(PsychicHttpServer *server,
                      ESP32SvelteKit *sveltekit, FixtureService *fixtureService);

    void begin();

protected:
    EventSocket *_socket;

private:
    HttpEndpoint<EffectsState> _httpEndpoint;
    EventEndpoint<EffectsState> _eventEndpoint;
    WebSocketServer<EffectsState> _webSocketServer;
    FSPersistence<EffectsState> _fsPersistence;

    FixtureService *_fixtureService;

    void onConfigUpdated();
};

#endif
