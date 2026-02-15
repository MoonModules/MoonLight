/**
    @title     MoonLight
    @file      PhysicalLayer.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include "PhysicalLayer.h"

  #include <ESP32SvelteKit.h>  //for safeModeMB

  #include "MoonBase/Nodes.h"
  #include "MoonBase/Utilities.h"
  #include "VirtualLayer.h"

extern SemaphoreHandle_t swapMutex;

PhysicalLayer layerP;  // global declaration of the physical layer

PhysicalLayer::PhysicalLayer() {
  EXT_LOGD(ML_TAG, "constructor");

  // initLightsToBlend();

  // create one layer - temporary
  layers.push_back(new VirtualLayer());
  layers[0]->layerP = this;

  if (effectsMutex == nullptr) EXT_LOGE(ML_TAG, "Failed to create effectsMutex");
  if (driversMutex == nullptr) EXT_LOGE(ML_TAG, "Failed to create driversMutex");
}

PhysicalLayer::~PhysicalLayer() {
  if (effectsMutex) {
    vSemaphoreDelete(effectsMutex);
    effectsMutex = NULL;
  }
  if (driversMutex) {
    vSemaphoreDelete(driversMutex);
    driversMutex = NULL;
  }
}

void PhysicalLayer::setup() {
  // allocate lights.channelsE/D

  if (psramFound()) {
    lights.maxChannels = MIN(ESP.getPsramSize() / 4, 128 * 64 * 16 * 3);  // fill max 2 * 25% of PSRAM with channels, supporting Virtual driver which is 120 pins * 512..1024 LEDs, max 16 Hub75 128x64 panels
    lights.useDoubleBuffer = true;                                        // Enable double buffering
  } else {
    lights.maxChannels = 2048 * 3;   // esp32-d0: max 1024->2048->4096->2048 Leds ATM (4096 is too bleeding edge still - hope is replacing PhysicHTTP by Async webserver solves this)
    lights.useDoubleBuffer = false;  // Single buffer mode
  }

  lights.channelsE = allocMB<uint8_t>(lights.maxChannels);

  if (lights.channelsE) {
    EXT_LOGD(ML_TAG, "allocated %d bytes in %s", lights.maxChannels, isInPSRAM(lights.channelsE) ? "PSRAM" : "RAM");
    // Allocate back buffer only if PSRAM available
    if (lights.useDoubleBuffer) {
      lights.channelsD = allocMB<uint8_t>(lights.maxChannels);
      if (!lights.channelsD) {
        EXT_LOGW(ML_TAG, "Failed to allocate back buffer, disabling double buffering");
        lights.useDoubleBuffer = false;
      }
    } else {
      lights.channelsD = lights.channelsE;  // share the same array
    }
  } else {
    EXT_LOGE(ML_TAG, "failed to allocated %d bytes of RAM or PSRAM", lights.maxChannels);
    lights.maxChannels = 0;
  }

  for (VirtualLayer* layer : layers) {
    layer->setup();
  }
}

void PhysicalLayer::loop() {
  if (lights.header.nrOfChannels >= lights.maxChannels) return;  // in case alloc mem is not successful
  // runs the loop of all effects / nodes in the layer
  for (VirtualLayer* layer : layers) {
    if (layer) {
      layer->loop();  // if (layer) needed when deleting rows ...
    }
  }
}

void PhysicalLayer::loop20ms() {
  // runs the loop of all effects / nodes in the layer
  for (VirtualLayer* layer : layers) {
    if (layer) layer->loop20ms();  // if (layer) needed when deleting rows ...
  }
}

void PhysicalLayer::loopDrivers() {
  // run mapping in the drivers task

  if (requestMapPhysical) {
    EXT_LOGD(ML_TAG, "mapLayout physical requested");

    pass = 1;
    mapLayout();

    requestMapPhysical = false;
  }

  if (requestMapVirtual) {
    EXT_LOGD(ML_TAG, "mapLayout virtual requested");

    pass = 2;
    mapLayout();

    requestMapVirtual = false;
  }

  // for physical layer nodes
  if (prevSize != lights.header.size) EXT_LOGD(ML_TAG, "onSizeChanged P %d,%d,%d -> %d,%d,%d", prevSize.x, prevSize.y, prevSize.z, lights.header.size.x, lights.header.size.y, lights.header.size.z);

  for (Node* node : nodes) {
    if (prevSize != lights.header.size) {
      xSemaphoreTake(*node->layerMutex, portMAX_DELAY);
      node->onSizeChanged(prevSize);
      xSemaphoreGive(*node->layerMutex);
    }
    if (node->on) {
      xSemaphoreTake(*node->layerMutex, portMAX_DELAY);
      node->loop();
      xSemaphoreGive(*node->layerMutex);
      addYield(10);
    }
  }

  prevSize = lights.header.size;
}

void PhysicalLayer::loop20msDrivers() {
  // runs the loop of all effects / nodes in the layer
  for (Node* node : nodes) {
    if (node->on) {
      xSemaphoreTake(*node->layerMutex, portMAX_DELAY);
      node->loop20ms();
      xSemaphoreGive(*node->layerMutex);
      addYield(10);
    }
  }
}

void PhysicalLayer::mapLayout() {
  onLayoutPre();
  for (Node* node : nodes) {
    if (node->on) {  // && node->hasOnLayout
      xSemaphoreTake(*node->layerMutex, portMAX_DELAY);
      node->onLayout();
      xSemaphoreGive(*node->layerMutex);
    }
  }
  onLayoutPost();
}

void PhysicalLayer::onLayoutPre() {
  EXT_LOGD(ML_TAG, "pass %d mp:%d", pass, monitorPass);

  if (pass == 1) {
    // Hold mutex while modifying shared state!
    if (layerP.lights.useDoubleBuffer) xSemaphoreTake(swapMutex, portMAX_DELAY);

    lights.header.nrOfLights = 0;  // for pass1 and pass2 as in pass2 virtual layer needs it
    lights.header.size = {0, 0, 0};
    EXT_LOGD(ML_TAG, "positions in progress (%d -> 1)", lights.header.isPositions);
    lights.header.isPositions = 1;  // Stops effectTask from starting NEW frames

    if (layerP.lights.useDoubleBuffer) xSemaphoreGive(swapMutex);

    delay(100);  // Wait for any in-progress frame to complete

    // Now safe to zero the buffer - effectTask won't start new frames while isPositions == 1
    if (layerP.lights.useDoubleBuffer) xSemaphoreTake(swapMutex, portMAX_DELAY);
    memset(lights.channelsE, 0, lights.maxChannels);
    if (layerP.lights.useDoubleBuffer) xSemaphoreGive(swapMutex);

    // dealloc pins (non-critical, can be outside mutex)
    if (!monitorPass) {
      memset(ledsPerPin, 0xFF, sizeof(ledsPerPin));  // UINT16_MAX is 2 * 0xFF
      memset(ledPinsAssigned, 0, sizeof(ledPinsAssigned));
      nrOfAssignedPins = 0;
    }
  } else if (pass == 2) {
    indexP = 0;
    for (VirtualLayer* layer : layers) {
      // add the lights in the virtual layer
      layer->onLayoutPre();
    }
  }
}

void packCoord3DInto3Bytes(uint8_t* buf, Coord3D position) {  // max size supported is 255x255x255
  buf[0] = MIN(position.x, 255);
  buf[1] = MIN(position.y, 255);
  buf[2] = MIN(position.z, 255);
}
void PhysicalLayer::addLight(Coord3D position) {
  if (safeModeMB && lights.header.nrOfLights > 1023) {
    // EXT_LOGW(ML_TAG, "Safe mode enabled, not adding lights > 1023");
    return;
  }

  if (position.x < 0) position.x = 0;
  if (position.y < 0) position.y = 0;
  if (position.z < 0) position.z = 0;

  if (pass == 1) {
    // EXT_LOGD(ML_TAG, "%d,%d,%d", position.x, position.y, position.z);
    if (lights.header.nrOfLights < lights.maxChannels / 3) {
      packCoord3DInto3Bytes(&lights.channelsE[lights.header.nrOfLights * 3], position);  // positions in channelsE
    }

    lights.header.size = lights.header.size.maximum(position);
    lights.header.nrOfLights++;
  } else {  // pass == 2
    for (VirtualLayer* layer : layers) {
      // add the position in the virtual layer
      layer->addLight(position);
    }
    indexP++;
  }
}

void PhysicalLayer::nextPin(uint8_t ledPinDIO) {
  if (pass == 1 && !monitorPass) {
    nrOfLights_t prevNrOfLights = 0;
    uint8_t i = 0;
    while (i < MAXLEDPINS && ledsPerPin[i] != UINT16_MAX) {
      prevNrOfLights += ledsPerPin[i];
      i++;
    }
    // ledsPerPin[i] is the first empty slot
    if (i < MAXLEDPINS) {
      ledsPerPin[i] = (lights.header.nrOfLights - prevNrOfLights) * ((lights.header.lightPreset == lightPreset_RGB2040) ? 2 : 1);  // RGB2040 has empty channels
      if (ledPinDIO != UINT8_MAX)
        ledPinsAssigned[i] = ledPinDIO;  // override order
      else
        ledPinsAssigned[i] = i;  // default order
      nrOfAssignedPins = i + 1;
      EXT_LOGD(ML_TAG, "nextPin #%d ledsPerPin:%d of %d assigned:%d", i, ledsPerPin[i], MAXLEDPINS, ledPinsAssigned[i]);
    }
  }
}

void PhysicalLayer::onLayoutPost() {
  if (pass == 1) {
    lights.header.size += Coord3D{1, 1, 1};
    lights.header.nrOfChannels = lights.header.nrOfLights * lights.header.channelsPerLight * ((lights.header.lightPreset == lightPreset_RGB2040) ? 2 : 1);  // RGB2040 has empty channels
    EXT_LOGD(ML_TAG, "pass %d mp:%d #:%d / %d s:%d,%d,%d", pass, monitorPass, lights.header.nrOfLights, lights.header.nrOfChannels, lights.header.size.x, lights.header.size.y, lights.header.size.z);
    // send the positions to the UI _socket_emit
    if (layerP.lights.useDoubleBuffer) xSemaphoreTake(swapMutex, portMAX_DELAY);
    EXT_LOGD(ML_TAG, "positions stored (%d -> %d)", lights.header.isPositions, lights.header.nrOfLights ? 2 : 3);
    lights.header.isPositions = lights.header.nrOfLights ? 2 : 3;  // filled with positions, set back to 3 in ModuleEffects, or direct to 3 if no lights (effects will move it to 0)
    if (layerP.lights.useDoubleBuffer) xSemaphoreGive(swapMutex);

    // initLightsToBlend();

    // ledsDriver.init(lights, sortedPins); //init the driver with the sorted pins and lights
  } else if (pass == 2) {
    EXT_LOGD(ML_TAG, "pass %d indexP: %d", pass, indexP);
    for (VirtualLayer* layer : layers) {
      // add the position in the virtual layer
      layer->onLayoutPost();
    }
  }
}
// an effect is using a virtual layer: tell the effect in which layer to run...

// // to be called in setup, if more then one effect
// void PhysicalLayer::initLightsToBlend() {
//     lightsToBlend.reserve(lights.header.nrOfLights);

//     for (nrOfLights_t indexP = 0; indexP < lightsToBlend.size(); indexP++)
//       lightsToBlend[indexP] = false;
// }

#endif  // FT_MOONLIGHT