/**
    @title     MoonBase
    @file      SharedHttpEndpoint.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef SharedHttpEndpoint_h
#define SharedHttpEndpoint_h

#include <PsychicHttp.h>
#include <SecurityManager.h>

#include <map>

#include "Module.h"

class SharedHttpEndpoint {
 private:
  PsychicHttpServer* _server;
  SecurityManager* _securityManager;
  AuthenticationPredicate _authenticationPredicate;

 public:
  SharedHttpEndpoint(PsychicHttpServer* server, SecurityManager* securityManager, AuthenticationPredicate authenticationPredicate = AuthenticationPredicates::IS_ADMIN) : _server(server), _securityManager(securityManager), _authenticationPredicate(authenticationPredicate) {}

  // Register a module with its path
  void registerModule(Module* module) {
    String path = "/rest/" + module->_moduleName;

// ADDED: CORS OPTIONS handler
#ifdef ENABLE_CORS
    _server->on(path.c_str(), HTTP_OPTIONS, _securityManager->wrapRequest([](PsychicRequest* request) { return request->reply(200); }, AuthenticationPredicates::IS_AUTHENTICATED));
#endif

    // GET handler
    _server->on(path.c_str(), HTTP_GET, _securityManager->wrapRequest([this](PsychicRequest* request) { return handleGet(request); }, _authenticationPredicate));

    // POST handler
    _server->on(path.c_str(), HTTP_POST, _securityManager->wrapCallback([this](PsychicRequest* request, JsonVariant& json) { return handlePost(request, json); }, _authenticationPredicate));

    ESP_LOGV(SVK_TAG, "Registered HTTP endpoint: %s", path.c_str());
  }

  void begin() {}

 private:
  esp_err_t handleGet(PsychicRequest* request) {
    EXT_LOGD(ML_TAG, "search module %s", request->path().c_str());
    Module* module = findModule(request->path());
    if (!module) return request->reply(404);

    PsychicJsonResponse response = PsychicJsonResponse(request, false);
    JsonObject jsonObject = response.getRoot();
    module->read(jsonObject, ModuleState::read);
    return response.send();
  }

  // CHANGED: POST handler returns updated state
  esp_err_t handlePost(PsychicRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>()) return request->reply(400);

    Module* module = findModule(request->path());
    if (!module) return request->reply(404);

    JsonObject jsonObject = json.as<JsonObject>();

    // CHANGED: Use updateWithoutPropagation pattern
    StateUpdateResult outcome = module->updateWithoutPropagation(jsonObject, ModuleState::update);

    if (outcome == StateUpdateResult::ERROR) {
      return request->reply(400);
    } else if (outcome == StateUpdateResult::CHANGED) {
      // Persist changes to FS
      module->callUpdateHandlers(HTTP_ENDPOINT_ORIGIN_ID);
    }

    // ADDED: Return updated state in response
    PsychicJsonResponse response = PsychicJsonResponse(request, false);
    JsonObject responseObj = response.getRoot();
    module->read(responseObj, ModuleState::read);
    return response.send();
  }

  Module* findModule(const String& path) {
    for (Module* module : modules) {
      if (path.endsWith(module->_moduleName.c_str())) return module;
    }
    EXT_LOGE(MB_TAG, "%s not found", path.c_str());
    return nullptr;
  }
};

#endif