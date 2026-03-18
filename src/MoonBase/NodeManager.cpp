/**
    @title     MoonLight
    @file      NodeManager.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/develop/nodes/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include "NodeManager.h"

  #include "MoonBase/SharedFSPersistence.h"

extern SharedFSPersistence* sharedFsPersistence;

NodeManager::NodeManager(const char* moduleName, PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager) : Module(moduleName, server, sveltekit) {
  EXT_LOGV(ML_TAG, "constructor %s", moduleName);
  _fileManager = fileManager;
}

void NodeManager::begin() {
  Module::begin();
  // if file changes, read the file and bring into state
  // create a handler which recompiles the live script when the file of a current running live script changes in the File Manager
  _fileManager->addUpdateHandler([this](const String& originId) {
    // EXT_LOGD(ML_TAG, "FileManager::updateHandler %s", originId.c_str());
    // read the file state (read all files and folders on FS and collect changes)
    _fileManager->read(
        [&](FilesState& filesState) {
          // loop over all changed files (normally only one)
          Char<32> name;
          name.format("/.config/%s.json", _moduleName);
          for (int i = filesState.updatedItems.size() - 1; i >= 0; i--) {
            if (equal(filesState.updatedItems[i].c_str(), name.c_str())) {
              EXT_LOGD(ML_TAG, " %s updated -> call update %s", name.c_str(), filesState.updatedItems[i].c_str());
              filesState.updatedItems.erase(filesState.updatedItems.begin() + i);  // consume the item so it doesn't trigger again
              sharedFsPersistence->readFromFS(_moduleName);  // repopulates the state, processing file changes
            }
          }
        },
        originId);
  });
}

void NodeManager::loop20ms() {
  Module::loop20ms();  // requestUIUpdate

  #if FT_LIVESCRIPT
  // Process deferred LiveScript compilations (when another compile was in progress during setup).
  if (nodes) {
    for (Node* node : *nodes) {
      if (node && node->isLiveScriptNode()) {
        LiveScriptNode* lsn = static_cast<LiveScriptNode*>(node);
        if (lsn->needsCompile) {
          lsn->needsCompile = false;
          lsn->startCompile();
          break;  // one at a time
        }
      }
    }
  }
  #endif
}

void NodeManager::setupDefinition(const JsonArray& controls) {
  EXT_LOGV(ML_TAG, "");
  JsonObject control;  // state.data has one or more properties
  JsonArray rows;      // if a control is an array, this is the rows of the array

  control = addControl(controls, "nodes", "rows");
  rows = control["n"].to<JsonArray>();
  {
    control = addControl(rows, "name", "selectFile");
    control["default"] = defaultNodeName.c_str();
    addNodes(control);

    control = addControl(rows, "on", "checkbox");
    control["default"] = true;
    control = addControl(rows, "controls", "controls");
    rows = control["n"].to<JsonArray>();
    {
      control = addControl(rows, "name", "text");
      control["default"] = "speed";
      control = addControl(rows, "type", "select");
      control["default"] = "number";
      addControlValue(control, "number");
      addControlValue(control, "slider");
      addControlValue(control, "text");
      addControlValue(control, "coordinate");
      control = addControl(rows, "value", "text");
      control["default"] = "128";
    }
  }
}

void NodeManager::onUpdate(const UpdatedItem& updatedItem) {
  // handle nodes
  if (updatedItem.parent[0] == "nodes") {  // onNodes
    JsonVariant nodeState = _state.data["nodes"][updatedItem.index[0]];

    if (updatedItem.name == "name" && updatedItem.parent[1] == "") {  // nodes[i].name
      handleNodeNameChange(updatedItem, nodeState);
    } else if (updatedItem.name == "on" && updatedItem.parent[1] == "") {  // nodes[i].on
      handleNodeOnChange(updatedItem, nodeState);
    } else if (updatedItem.parent[1] == "controls" && (updatedItem.name == "value" || updatedItem.name == "default") && updatedItem.index[1] < nodeState["controls"].size()) {  // nodes[i].controls[j].{value|default}
      handleNodeControlValueChange(updatedItem, nodeState);
    }
  }
}

void NodeManager::handleNodeNameChange(const UpdatedItem& updatedItem, JsonVariant nodeState) {
  Node* oldNode = updatedItem.index[0] < nodes->size() ? (*nodes)[updatedItem.index[0]] : nullptr;  // find the node in the nodes list
  bool newNode = false;

  // remove or add Nodes (incl controls)
  if (!updatedItem.value.isNull()) {  // if name changed // == updatedItem.value

    // // if old node exists then remove it's controls
    if (updatedItem.oldValue != "") {
      nodeState.remove("controls");  // remove the controls from the nodeState
    }

    // Migration 20251204: this is optional as we accept data updates from legacy driver names (migration is mandatory for changes in the data definitions)
    // When adding new migrations, follow the same pattern with contains() + getNameAndTags<T>().
    if (contains(updatedItem.value.as<const char*>(), "Physical Driver")) {
      EXT_LOGD(ML_TAG, "update [%s] to ...", updatedItem.value.as<const char*>());
      nodeState["name"] = getNameAndTags<ParallelLEDDriver>();  // set to current combination of name and tags
      EXT_LOGD(ML_TAG, "... to [%s]", updatedItem.value.as<const char*>());
    }
    if (contains(updatedItem.value.as<const char*>(), "IR Driver")) {
      EXT_LOGD(ML_TAG, "update [%s] to ...", updatedItem.value.as<const char*>());
      nodeState["name"] = getNameAndTags<IRDriver>();  // set to current combination of name and tags
      EXT_LOGD(ML_TAG, "... to [%s]", updatedItem.value.as<const char*>());
    }

    // invalidate controls
    if (nodeState["controls"].isNull()) {     // if controls are not set, create empty array
      nodeState["controls"].to<JsonArray>();  // clear the controls
    } else {
      for (JsonObject control : nodeState["controls"].as<JsonArray>()) {
        control["valid"] = false;
      }
    }

    char name[32];
    strlcpy(name, updatedItem.value, 32);
    Node* nodeClass = addNode(updatedItem.index[0], name, nodeState["controls"]);  // set controls to valid
    if (updatedItem.value != name) nodeState["name"] = name;                       //  if the non AZaz09 part of the name changed, reassign the right name

    // remove invalid controls
    // Iterate backwards to avoid index shifting issues
    for (int i = nodeState["controls"].as<JsonArray>().size() - 1; i >= 0; i--) {
      JsonObject control = nodeState["controls"][i];
      if (!control["valid"].as<bool>()) {
        EXT_LOGD(ML_TAG, "remove control %d", i);
        nodeState["controls"].remove(i);
      }
    }

    if (nodeClass != nullptr) {
      nodeClass->on = nodeState["on"];
      newNode = true;

      // make sure "p" is also updated
      nodeClass->requestMappings();
    } else
      EXT_LOGW(ML_TAG, "Nodeclass %s not found", updatedItem.value.as<String>().c_str());
  }  // name change

  // if a node existed and no new node in place, remove
  if (updatedItem.oldValue != "" && oldNode) {
    if (!newNode) {
      // remove oldNode from the nodes list
      for (uint8_t i = 0; i < nodes->size(); i++) {
        if ((*nodes)[i] == oldNode) {
          EXT_LOGD(ML_TAG, "remove node %d %s", i, updatedItem.oldValue.c_str());
          nodes->erase(nodes->begin() + i);
          break;
        }
      }
      EXT_LOGD(ML_TAG, "No newnode - remove! %d s:%d", updatedItem.index[0], nodes->size());
    }

    oldNode->requestMappings();

    // Wait for any in-progress loop()/loop20ms() on this node to complete.
    // The task loops in PhysicalLayer/VirtualLayer take layerMutex around each node call,
    // so acquiring it here guarantees the node isn't mid-execution.
    if (oldNode->layerMutex) {
      xSemaphoreTake(*oldNode->layerMutex, portMAX_DELAY);
      xSemaphoreGive(*oldNode->layerMutex);
    }

    EXT_LOGD(ML_TAG, "remove oldNode: %d p:%p", nodes->size(), oldNode);
    freeMBObject(oldNode);  // calls virtual destructor + frees memory
  }

  if (newNode) {
    requestUIUpdate = true;
  }

  #if FT_ENABLED(FT_LIVESCRIPT)
  // if (updatedItem.oldValue.length()) {
  //     EXT_LOGV(ML_TAG, "delete %s %s ...", updatedItem.name.c_str(), updatedItem.oldValue.c_str());
  //     LiveScriptNode *liveScriptNode = findLiveScriptNode(node["name"]);
  //     if (liveScriptNode) liveScriptNode->kill();
  //     else EXT_LOGW(ML_TAG, "liveScriptNode not found %s", node["name"].as<const char*>());
  // }
  // if (!node["name"].isNull() && !node["type"].isNull()) {
  //     LiveScriptNode *liveScriptNode = findLiveScriptNode(node["name"]); //todo: can be 2 nodes with the same name ...
  //     if (liveScriptNode) liveScriptNode->compileAndRun();
  //     // not needed as creating the node is already running it ...
  // }
  #endif
}

void NodeManager::handleNodeOnChange(const UpdatedItem& updatedItem, JsonVariant nodeState) {
  if (updatedItem.index[0] < nodes->size()) {
    const char* name = nodeState["name"];
    EXT_LOGD(ML_TAG, "%s on: %s (#%d)", name ? name : "", updatedItem.value.as<String>().c_str(), nodes->size());
    Node* nodeClass = (*nodes)[updatedItem.index[0]];
    if (nodeClass != nullptr) {
      nodeClass->on = updatedItem.value.as<bool>();  // set nodeclass on/off
      xSemaphoreTake(*nodeClass->layerMutex, portMAX_DELAY);
      nodeClass->onUpdate(nodeState);  // custom onUpdate for the node
      xSemaphoreGive(*nodeClass->layerMutex);
      nodeClass->requestMappings();
    } else
      EXT_LOGW(ML_TAG, "Nodeclass %s not found", name ? name : "");
  }
}

void NodeManager::handleNodeControlValueChange(const UpdatedItem& updatedItem, JsonVariant nodeState) {
  JsonObject control = nodeState["controls"][updatedItem.index[1]];

  // if (control[updatedItem.name] == updatedItem.value) {
  //   return;  // avoid re-applying stale compareRecursive emissions
  // }

  if (updatedItem.index[0] < nodes->size()) {
    Node* nodeClass = (*nodes)[updatedItem.index[0]];
    if (nodeClass != nullptr) {
      xSemaphoreTake(*nodeClass->layerMutex, portMAX_DELAY);
      nodeClass->updateControl(control);
      nodeClass->onUpdate(control);  // custom onUpdate for the node
      xSemaphoreGive(*nodeClass->layerMutex);

      nodeClass->requestMappings();
    } else {
      const char* name = nodeState["name"];
      EXT_LOGW(ML_TAG, "nodeClass not found %s", name ? name : "");
    }
  }
}

void NodeManager::onReOrderSwap(uint8_t stateIndex, uint8_t newIndex) {
  EXT_LOGD(ML_TAG, "%d %d %d", nodes->size(), stateIndex, newIndex);
  // swap nodes
  Node* nodeS = (*nodes)[stateIndex];
  Node* nodeN = (*nodes)[newIndex];
  (*nodes)[stateIndex] = nodeN;
  (*nodes)[newIndex] = nodeS;

  // modifiers and layouts trigger remaps
  nodeS->requestMappings();
  nodeN->requestMappings();
}

  #if FT_LIVESCRIPT
Node* NodeManager::findLiveScriptNode(const char* animation) {
  if (!nodes) return nullptr;

  for (Node* node : *nodes) {
    if (node && node->isLiveScriptNode()) {
      LiveScriptNode* liveScriptNode = (LiveScriptNode*)node;
      if (equal(liveScriptNode->animation.c_str(), animation)) {
        EXT_LOGV(ML_TAG, "found %s", animation);
        return liveScriptNode;
      }
    }
  }
  return nullptr;
}
  #endif

#endif
