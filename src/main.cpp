/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2024 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <ESP32SvelteKit.h>
#include <LightMqttSettingsService.h>
#include <LightStateService.h>
#include <PsychicHttpServer.h>

#define SERIAL_BAUD_RATE 115200

PsychicHttpServer server;

ESP32SvelteKit esp32sveltekit(&server, 120);

LightMqttSettingsService lightMqttSettingsService = LightMqttSettingsService(&server,
                                                                             &esp32sveltekit);

LightStateService lightStateService = LightStateService(&server,
                                                        &esp32sveltekit,
                                                        &lightMqttSettingsService);


#include "mainStar.h"

void setup()
{
    // start serial and filesystem
    Serial.begin(SERIAL_BAUD_RATE);

    // delay(500); //see WLED/StarLight / Serial needs time to init ..., add ifdef?

    setupStar(); //before ESK as ESK loop uses star stuff

    // start ESP32-SvelteKit
    esp32sveltekit.begin();

    // load the initial light settings
    lightStateService.begin();
    // start the light service
    lightMqttSettingsService.begin();

    // ESP_LOGV("Star", "Starting loop task");
    // xTaskCreateUniversal(
    //     [](void * par) { loopStar(); },            // Function that should be called
    //     "StarLoop",     // Name of the task (for debugging)
    //     8192,                       // Stack size (bytes)
    //     nullptr,                       // Pass reference to this class instance
    //     (tskIDLE_PRIORITY + 1),     // task priority
    //     NULL,                       // Task handle
    //     ARDUINO_RUNNING_CORE // Pin to application core
    // );

}

void loop()
{
    // Delete Arduino loop task, as it is not needed in this example
    // vTaskDelete(NULL);

    loopStar();
}
