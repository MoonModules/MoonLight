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

#include <ElectricityInputService.h>

ElectricityInputService::ElectricityInputService(EventSocket *socket) : _socket(socket)
{
}

void ElectricityInputService::updateVIN(float voltage)
{
    _vin = (int)round(voltage);
    electricityInputEvent();
}

void ElectricityInputService::updateIIN(float current)
{
    _iin = (int)round(current);
    electricityInputEvent();
}

int ElectricityInputService::getVIN()
{
    return _vin;
}

int ElectricityInputService::getIIN()
{
    return _iin;
}

void ElectricityInputService::begin()
{
    _socket->registerEvent(EVENT_ELECTRICITYINPUT);
}

void ElectricityInputService::electricityInputEvent()
{
    if (!_socket->getConnectedClients()) return;  // 🌙 No need for UI tasks

    JsonDocument doc;
    doc["vin"] = _vin;
    doc["iin"] = _iin;
    JsonObject jsonObject = doc.as<JsonObject>();
    _socket->emitEvent(EVENT_ELECTRICITYINPUT, jsonObject);
}
