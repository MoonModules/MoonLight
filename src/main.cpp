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
#include <PsychicHttpServer.h>

// 🌙
#if FT_ENABLED(FT_MOONBASE)
    #include "MoonBase/FileManager.h"
    #include "MoonBase/ModuleInstances.h"
    #include "MoonBase/ModuleTasks.h"

    // 💫
    #if FT_ENABLED(FT_MOONLIGHT)
        #include "MoonLight/ModuleLightsControl.h"
        #include "MoonLight/ModuleEditor.h"
        #if FT_ENABLED(FT_LIVESCRIPT)
            #include "MoonLight/ModuleLiveScripts.h"
        #endif
        #include "MoonLight/ModuleChannelView.h"
        #include "MoonLight/ModuleMoonLightInfo.h"
        #ifdef STEPHANELEC_LED_DRIVER_V1
            #include "MoonLight/ModuleStephanElecDriverInfo.h"
        #endif
    #endif
#endif

#define SERIAL_BAUD_RATE 115200

#undef TAG
#define TAG "🌙"

PsychicHttpServer server;

ESP32SvelteKit esp32sveltekit(&server, 160); //increase number of endpoints to 160, default is 120

// 🌙
#if FT_ENABLED(FT_MOONBASE)
    FileManager fileManager = FileManager(&server, &esp32sveltekit);
    ModuleInstances moduleInstances = ModuleInstances(&server, &esp32sveltekit);
    ModuleTasks moduleTasks = ModuleTasks(&server, &esp32sveltekit);

    // 💫
    #if FT_ENABLED(FT_MOONLIGHT)
        ModuleLightsControl moduleLightsControl = ModuleLightsControl(&server, &esp32sveltekit, &fileManager);
        ModuleEditor moduleEditor = ModuleEditor(&server, &esp32sveltekit, &fileManager);
        #if FT_ENABLED(FT_LIVESCRIPT)
            ModuleLiveScripts moduleLiveScripts = ModuleLiveScripts(&server, &esp32sveltekit);
        #endif
        ModuleChannelView moduleChannelView = ModuleChannelView(&server, &esp32sveltekit);
        ModuleMoonLightInfo moduleMoonLightInfo = ModuleMoonLightInfo(&server, &esp32sveltekit);
        #ifdef STEPHANELEC_LED_DRIVER_V1
            ModuleStephanElecDriverInfo moduleStephanElecDriverInfo = ModuleStephanElecDriverInfo(&server, &esp32sveltekit);
        #endif
    #endif
#endif
    
void moonTask(void* pvParameters) {
    // 🌙
    #if FT_ENABLED(FT_MOONBASE)
        fileManager.begin();
            moduleInstances.begin();
            moduleTasks.begin();

            #if FT_ENABLED(FT_MOONLIGHT)
                moduleLightsControl.begin();
                moduleEditor.begin();
                #if FT_ENABLED(FT_LIVESCRIPT)
                    moduleLiveScripts.begin();
                #endif
                moduleChannelView.begin();
                moduleMoonLightInfo.begin();
        #endif
    #endif

    for (;;) {
        esp32sveltekit.lps++; // 🌙

        #if FT_ENABLED(FT_MOONBASE)

            #if FT_ENABLED(FT_MOONLIGHT)
                moduleEditor.loop();
            #endif

            static unsigned long lastTime50ms = 0;
            if (millis() - lastTime50ms > 50)
            {
                lastTime50ms = millis();
                #if FT_ENABLED(FT_MOONLIGHT)
                    moduleLightsControl.loop50ms();
                #endif
                
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

                    static unsigned long lastTime10s = 0;
                    if (millis() - lastTime10s > 10000)
                    {
                        lastTime10s = millis();
                        moduleInstances.loop10s();
                        moduleTasks.loop10s();
                    }
                }
            }

            while (!runInOtherTask.empty()) {
                runInOtherTask.front()();
                runInOtherTask.erase(runInOtherTask.begin());
            }

        #endif

        vTaskDelay(1); // yield to other tasks, 1 tick (~1ms)
    }
}

TaskHandle_t moonTaskHandle = NULL;

void setup()
{
    // start serial and filesystem
    Serial.begin(SERIAL_BAUD_RATE);

    // delay(5000); // 🌙 to capture all the serial output

    // 🌙 safeMode
    if (esp_reset_reason() != ESP_RST_UNKNOWN && esp_reset_reason() != ESP_RST_POWERON && esp_reset_reason() != ESP_RST_SW) { //see verbosePrintResetReason
        safeModeMB = true;
    }

    // start ESP32-SvelteKit
    esp32sveltekit.begin();

    // 🌙
    xTaskCreatePinnedToCore(
        moonTask,              // task function
        "moonTask",            // name
        16 * 1024,             // stack size in words
        NULL,                  // parameter
        8,                     // priority (between 5 and 10: ASYNC_WORKER_TASK_PRIORITY and Restart/Sleep)
        &moonTaskHandle,       // task handle
        1                      // core (0 or 1)
    );

    // #if CONFIG_IDF_TARGET_ESP32
    //     pinMode(19, OUTPUT); digitalWrite(19, HIGH); // for serg shield boards: to be done: move to new pin manager module, switch off for S3!!!! tbd: add pin manager
    // #endif
}

void loop()
{
    // Delete Arduino loop task, as it is not needed in this example
    vTaskDelete(NULL);
}
