#pragma once

/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2023 - 2025 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <EventSocket.h>
#include <JsonUtils.h>

#define EVENT_ELECTRICITYINPUT "electricityinput"

class ElectricityInputService
{
public:
    ElectricityInputService(EventSocket *socket);

    void begin();

    void updateVIN(float voltage);

    void updateIIN(float current);

    int getVIN();

    int getIIN();

private:
    EventSocket *_socket;
    int _vin = 0;
    int _iin = 0;

    void electricityInputEvent();
};
