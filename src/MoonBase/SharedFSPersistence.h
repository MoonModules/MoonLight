/**
    @title     MoonBase
    @file      SharedFSPersistence.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef SharedFSPersistence_h
#define SharedFSPersistence_h

#include <FS.h>
#include <StatefulService.h>

#include "Module.h"

// ADDED: Global delayed writes queue (matches templated version)
inline std::vector<std::function<void(char)>> sharedDelayedWrites;

class SharedFSPersistence {
 private:
  FS* _fs;
  struct ModuleInfo {
    Module* module;
    String filePath;
    bool delayedWriting;
    bool hasDelayedWrite;
    update_handler_id_t updateHandlerId;

    ModuleInfo() : module(nullptr), delayedWriting(false), hasDelayedWrite(false), updateHandlerId(0) {}
  };
  struct CStrComparator {
    bool operator()(const char* a, const char* b) const { return strcmp(a, b) < 0; }
  };
  std::map<const char*, ModuleInfo, CStrComparator> _modules;  // moduleName -> info

 public:
  SharedFSPersistence(FS* fs) : _fs(fs) {}

  // ADDED: Support for delayed writing parameter
  void registerModule(Module* module, bool delayedWriting = false) {
    ModuleInfo info;
    info.module = module;
    info.filePath = String("/.config/") + module->_moduleName + ".json";
    info.delayedWriting = delayedWriting;
    info.hasDelayedWrite = false;

    // Register update handler
    info.updateHandlerId = module->addUpdateHandler([this, module](const String& originId) { writeToFS(module->_moduleName); }, false);

    _modules[module->_moduleName] = info;

    // Read initial state from filesystem
    readFromFS(module->_moduleName);
  }

  void begin() {
    // All setup happens in registerModule
  }

  // ADDED: Enable/disable update handler for specific module
  void disableUpdateHandler(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it != _modules.end() && it->second.updateHandlerId) {
      it->second.module->removeUpdateHandler(it->second.updateHandlerId);
      it->second.updateHandlerId = 0;
    }
  }

  void enableUpdateHandler(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it != _modules.end() && !it->second.updateHandlerId) {
      it->second.updateHandlerId = it->second.module->addUpdateHandler(
          [this, module = it->second.module](const String& originId) {
            ESP_LOGD(SVK_TAG, "writeToFS %s", originId.c_str());
            writeToFS(module->_moduleName);
          },
          false);
    }
  }

  void readFromFS(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it == _modules.end()) return;

    ModuleInfo& info = it->second;
    File file = _fs->open(info.filePath.c_str(), "r");

    if (file) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (!error && doc.is<JsonObject>()) {
        JsonObject obj = doc.as<JsonObject>();
        info.module->updateWithoutPropagation(obj, ModuleState::update, moduleName);
        return;
      }
    }

    // ADDED: Apply defaults if file doesn't exist or is corrupted
    applyDefaults(info, moduleName);
    writeToFSNow(moduleName);
  }

  void writeToFS(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it == _modules.end()) return;

    ModuleInfo& info = it->second;

    // ADDED: Delayed write support
    if (info.delayedWriting) {
      if (!info.hasDelayedWrite) {
        ESP_LOGD(SVK_TAG, "delayedWrites: Add %s", info.filePath.c_str());

        sharedDelayedWrites.push_back([this, module = info.module](char writeOrCancel) {
          auto it = _modules.find(module->_moduleName);
          if (it == _modules.end()) return;

          ESP_LOGD(SVK_TAG, "delayedWrites: %c %s", writeOrCancel, it->second.filePath.c_str());

          if (writeOrCancel == 'W') {
            this->writeToFSNow(module->_moduleName);
          } else {
            // Cancel: read old state back from FS
            this->readFromFS(module->_moduleName);
            // Update UI with restored state
            it->second.module->update([](ModuleState& state) { return StateUpdateResult::CHANGED; }, SVK_TAG);
          }
          it->second.hasDelayedWrite = false;
        });

        info.hasDelayedWrite = true;
      }
    } else {
      writeToFSNow(moduleName);
    }
  }

  bool writeToFSNow(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it == _modules.end()) return false;

    ModuleInfo& info = it->second;

    // ADDED: Create directories if needed
    mkdirs(info.filePath);

    // Create JSON document
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    info.module->read(root, ModuleState::read, moduleName);

    // Write to file
    File file = _fs->open(info.filePath.c_str(), "w");
    if (!file) return false;

    serializeJson(doc, file);
    file.close();
    return true;
  }

  // ADDED: Static method to process all delayed writes
  static void writeToFSDelayed(char writeOrCancel) {
    ESP_LOGD(SVK_TAG, "calling %d writeFuncs from delayedWrites", sharedDelayedWrites.size());

    for (auto& writeFunc : sharedDelayedWrites) {
      writeFunc(writeOrCancel);
    }
    sharedDelayedWrites.clear();
  }

 private:
  // ADDED: Create directories if they don't exist
  void mkdirs(const String& filePath) {
    int index = 0;
    while ((index = filePath.indexOf('/', index + 1)) != -1) {
      String segment = filePath.substring(0, index);
      if (!_fs->exists(segment)) {
        _fs->mkdir(segment);
      }
    }
  }

  // ADDED: Apply defaults from empty object
  void applyDefaults(ModuleInfo& info, const char* moduleName) {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    info.module->updateWithoutPropagation(obj, ModuleState::update, moduleName);
  }
};

#endif