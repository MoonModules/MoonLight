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

// 🌙
#if FT_ENABLED(FT_MOONBASE)
    #include "MoonBase/FilesService.h"
    #include "MoonBase/ModuleInstances.h"

    // 💫
    #if FT_ENABLED(FT_MOONLIGHT)
        #include "MoonLight/ModuleLightsControl.h"
        #include "MoonLight/ModuleEditor.h"
        #if FT_ENABLED(FT_LIVESCRIPT)
            #include "MoonLight/ModuleLiveScripts.h"
        #endif
        #include "MoonLight/ModuleArtnet.h"
        #include "MoonLight/ModuleChannelView.h"
    #endif
#endif

#define SERIAL_BAUD_RATE 115200

#undef TAG
#define TAG "🌙"

// 💫 
#if FT_ENABLED(FT_LIVESCRIPT)
    SET_LOOP_TASK_STACK_SIZE(16 * 1024); // 16KB, otherwise 8K, to allow multiple scripts loaded at boot
    //to do: needed as during boot scripts are run in looptask, later in httpd. Move scripts to same task...
#endif

PsychicHttpServer server;

ESP32SvelteKit esp32sveltekit(&server, 160); //increase number of endpoints to 160, default is 120

LightMqttSettingsService lightMqttSettingsService = LightMqttSettingsService(&server,
                                                                             &esp32sveltekit);

LightStateService lightStateService = LightStateService(&server,
                                                        &esp32sveltekit,
                                                        &lightMqttSettingsService);

// 🌙
#if FT_ENABLED(FT_MOONBASE)
    FilesService filesService = FilesService(&server, &esp32sveltekit);
    ModuleInstances moduleInstances = ModuleInstances(&server, &esp32sveltekit, &filesService);

    // 💫
    #if FT_ENABLED(FT_MOONLIGHT)
        ModuleLightsControl moduleLightsControl = ModuleLightsControl(&server, &esp32sveltekit, &filesService);
        ModuleEditor moduleEditor = ModuleEditor(&server, &esp32sveltekit, &filesService);
        #if FT_ENABLED(FT_LIVESCRIPT)
            ModuleLiveScripts moduleLiveScripts = ModuleLiveScripts(&server, &esp32sveltekit, &filesService);
        #endif
        ModuleArtnet moduleArtnet = ModuleArtnet(&server, &esp32sveltekit, &filesService);
        ModuleChannelView moduleChannelView = ModuleChannelView(&server, &esp32sveltekit, &filesService);
    #endif
#endif
    
void setup()
{
    // start serial and filesystem
    Serial.begin(SERIAL_BAUD_RATE);

    // delay(5000); // 🌙 to capture all the serial output

    // start ESP32-SvelteKit
    esp32sveltekit.begin();

    // 🌙
    #if FT_ENABLED(FT_MOONBASE)
        filesService.begin();
        moduleInstances.begin();

        #if FT_ENABLED(FT_MOONLIGHT)
            moduleLightsControl.begin();
            moduleEditor.begin();
            #if FT_ENABLED(FT_LIVESCRIPT)
                moduleLiveScripts.begin();
            #endif
            moduleArtnet.begin();
            moduleChannelView.begin();
        #endif
    #endif

    // load the initial light settings
    lightStateService.begin();
    // start the light service
    lightMqttSettingsService.begin();

    // String shortString = "Hello world";
    // String longString = "Hello world, this is a long string that is longer than 32 characters";
    // Char<32> averageChar; averageChar = "Hello world";

    // String microString = "Hi";
    // ESP_LOGD(TAG, "microString: %s", microString.c_str());
    // ESP_LOGD(TAG, "microString length: %d", microString.length());
    // ESP_LOGD(TAG, "microString size: %d", sizeof(microString));

    // ESP_LOGD(TAG, "shortString: %s", shortString.c_str());
    // ESP_LOGD(TAG, "longString: %s", longString.c_str());
    // ESP_LOGD(TAG, "averageChar: %s", averageChar.c_str());
    // ESP_LOGD(TAG, "shortString length: %d", shortString.length());
    // ESP_LOGD(TAG, "longString length: %d", longString.length());
    // ESP_LOGD(TAG, "averageChar length: %d", averageChar.length());
    // ESP_LOGD(TAG, "shortString size: %d", sizeof(shortString));
    // ESP_LOGD(TAG, "longString size: %d", sizeof(longString));
    // ESP_LOGD(TAG, "averageChar size: %d", sizeof(averageChar));
    // ESP_LOGD(TAG, "UpdatedItem size: %d = %d + %d + %d ...", sizeof(UpdatedItem), sizeof(JsonString), sizeof(JsonVariant), sizeof(String));
}

void loop()
{
    esp32sveltekit.lps++; // 🌙

    // 🌙
    #if FT_ENABLED(FT_MOONBASE)

        // 💫
        #if FT_ENABLED(FT_MOONLIGHT)
            moduleEditor.loop();
            moduleLightsControl.loop();
        #endif

        //20ms loop
        static unsigned long lastTime20ms = 0;
        if (millis() - lastTime20ms > 20)
        {
            lastTime20ms = millis();
            #if FT_ENABLED(FT_MOONLIGHT)
                moduleArtnet.loop20ms();
            #endif
        }

        //50ms loop
        static unsigned long lastTime50ms = 0;
        if (millis() - lastTime50ms > 50)
        {
            lastTime50ms = millis();
            #if FT_ENABLED(FT_MOONLIGHT)
                moduleLightsControl.loop50ms();
            #endif
            
            //1s loop
            static unsigned long lastTime1s = 0;
            if (millis() - lastTime1s > 1000)
            {
                lastTime1s = millis();
                moduleInstances.loop1s();
                #if FT_ENABLED(FT_MOONLIGHT)
                    #if FT_ENABLED(FT_LIVESCRIPT)
                        moduleLiveScripts.loop1s();
                    #endif
                #endif

                //10s loop
                static unsigned long lastTime10s = 0;
                if (millis() - lastTime10s > 10000)
                {
                    lastTime10s = millis();
                    moduleInstances.loop10s();
                }
            }
        }

        // 🌙
        while (!runInLoopTask.empty()) {
            // ESP_LOGD(TAG, "Running queued function");
            runInLoopTask.front()(); // calls the first function
            runInLoopTask.erase(runInLoopTask.begin());
        }

    #endif
}
