/**
    @title     MoonBase
    @file      SharedEventEndpoint.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef SharedEventEndpoint_h
#define SharedEventEndpoint_h

#include <EventSocket.h>
#include <StatefulService.h>

#include "Module.h"

class SharedEventEndpoint {
 private:
  EventSocket* _socket;

 public:
  SharedEventEndpoint(EventSocket* socket) : _socket(socket) {}

  void registerModule(Module* module) {
    const char* eventName = module->_moduleName;

    // Register the event with the socket
    _socket->registerEvent(eventName);

    // ADDED: Register handler for INCOMING events (client -> server updates)
    _socket->onEvent(eventName, [this, module](JsonObject& root, int originId) { module->update(root, ModuleState::update, String(originId)); });

    // ADDED: Register handler for new subscriptions (send state when client subscribes)
    _socket->onSubscribe(eventName, [this, module](const String& originId) { syncState(module, originId, true); });

    // Register this module for state updates (server -> clients)
    module->addUpdateHandler([this, module](const String& originId) { syncState(module, originId, false); }, false);
  }

  void begin() {
    // All events are registered during registerModule
  }

 private:
  void syncState(Module* module, const String& originId, bool sync = false) {
    JsonDocument doc;

    // CHANGED: Use JsonObject overload, not buffer
    doc["event"] = module->_moduleName;
    JsonObject root = doc["data"].to<JsonObject>();
    module->read(root, ModuleState::read, originId);

    _socket->emitEvent(doc, originId.c_str(), sync);
  }
};

#endif