/**
    @title     MoonLight
    @file      NodeManager.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/develop/nodes/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef NodeManager_h
#define NodeManager_h

#if FT_MOONLIGHT

  #include "MoonBase/Module.h"
  #include "Nodes.h"  //Nodes.h will include VirtualLayer.h which will include PhysicalLayer.h

class NodeManager : public Module {
 public:
  Char<20> defaultNodeName;

 protected:
  PsychicHttpServer* _server;
  FileManager* _fileManager;

  std::vector<Node*, VectorRAMAllocator<Node*>>* nodes;

  NodeManager(const String& moduleName, PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager) : Module(moduleName, server, sveltekit) {
    EXT_LOGV(ML_TAG, "constructor");
    _server = server;
    _fileManager = fileManager;
  }

  void begin() {
    Module::begin();
    // if (false)
    // if file changes, read the file and bring into state
    // create a handler which recompiles the live script when the file of a current running live script changes in the File Manager
    _fileManager->addUpdateHandler([&](const String& originId) {
      EXT_LOGV(ML_TAG, "FileManager::updateHandler %s", originId.c_str());
      // read the file state (read all files and folders on FS and collect changes)
      _fileManager->read([&](FilesState& filesState) {
        // loop over all changed files (normally only one)
        for (auto updatedItem : filesState.updatedItems) {
          // if file is the current live script, recompile it (to do: multiple live effects)
          EXT_LOGV(ML_TAG, "updateHandler updatedItem %s", updatedItem.c_str());
          Char<32> name;
          name.format("/.config/%s.json", _moduleName);
          if (equal(updatedItem.c_str(), name.c_str())) {
            EXT_LOGV(ML_TAG, " %s updated -> call update %s", name.c_str(), updatedItem.c_str());
            readFromFS();  // repopulates the state, processing file changes
          }
          // uint8_t index = 0;
          // for (JsonObject nodeState: _state.data["nodes"].as<JsonArray>()) {

          //     if (updatedItem == nodeState["name"]) {
          //         EXT_LOGV(ML_TAG, "updateHandler equals current item -> livescript compile %s", updatedItem.c_str());
          //         LiveScriptNode *liveScriptNode = (LiveScriptNode *)layerP.layers[0]->findLiveScriptNode(nodeState["name"]);
          //         if (liveScriptNode) {
          //             liveScriptNode->compileAndRun();

          //             //wait until setup has been executed?

          //             _moduleEffects->requestUIUpdate = true; //update the Effects UI
          //         }

          //         EXT_LOGV(ML_TAG, "update due to new node %s done", name.c_str());
          //     }
          //     index++;
          // }
        }
      });
    });
  }

  virtual void addNodes(const JsonObject& control) {}

  virtual Node* addNode(const uint8_t index, const char* name, const JsonArray& controls) const { return nullptr; }

  // define the data model
  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(ML_TAG, "");
    JsonObject control;  // state.data has one or more properties
    JsonArray rows;      // if a control is an array, this is the rows of the array

    control = addControl(controls, "nodes", "rows");
    rows = control["n"].to<JsonArray>();
    {
      control = addControl(rows, "name", "selectFile");
      control["default"] = defaultNodeName.c_str();
      // values = control["values"].to<JsonArray>();
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

  // implement business logic
  void onUpdate(const UpdatedItem& updatedItem) override {
    // EXT_LOGD(ML_TAG, "%s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());

    // handle nodes
    if (updatedItem.parent[0] == "nodes") {  // onNodes
      JsonVariant nodeState = _state.data["nodes"][updatedItem.index[0]];
      // serializeJson(nodeState, Serial); Serial.println();

      if (updatedItem.name == "name" && updatedItem.parent[1] == "") {  // nodes[i].name

        Node* oldNode = updatedItem.index[0] < nodes->size() ? (*nodes)[updatedItem.index[0]] : nullptr;  // find the node in the nodes list
        bool newNode = false;

        // remove or add Nodes (incl controls)
        if (!updatedItem.value.isNull()) {  // if name changed // == updatedItem.value

          // // if old node exists then remove it's controls
          if (updatedItem.oldValue != "") {
            // EXT_LOGD(ML_TAG, "remove controls %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1],
            // updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
            nodeState.remove("controls");  // remove the controls from the nodeState
          }

          // migration 20251204
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

          // String xx;
          // serializeJson(nodeState["controls"], xx);
          // EXT_LOGD(ML_TAG, "add %s[%d]%s[%d].%s = %s -> %s (%s)", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str(), xx.c_str());

          // invalidate controls
          if (nodeState["controls"].isNull()) {     // if controls are not set, create empty array
            nodeState["controls"].to<JsonArray>();  // clear the controls
          } else {
            for (JsonObject control : nodeState["controls"].as<JsonArray>()) {
              control["valid"] = false;
            }
          }

          Node* nodeClass = addNode(updatedItem.index[0], updatedItem.value, nodeState["controls"]);  // set controls to valid

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

            // wait until setup has been executed?

            requestUIUpdate = true;

            // EXT_LOGD(ML_TAG, "update due to new node %s done", updatedItem.value.as<String>().c_str());

            // make sure "p" is also updated

            nodeClass->requestMappings();
          } else
            EXT_LOGW(ML_TAG, "Nodeclass %s not found", updatedItem.value.as<String>().c_str());

          // if (updatedItem.oldValue.indexOf("Driver") != -1 && updatedItem.value.as<String>().indexOf("Driver") != -1) {
          //     EXT_LOGW(ML_TAG, "Restart needed");
          //     restartNeeded = true;
          // }
        }

        // if a node existed and no new node in place, remove
        if (updatedItem.oldValue != "" && oldNode) {
          // EXT_LOGD(ML_TAG, "remove %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(),
          // updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
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

          EXT_LOGD(ML_TAG, "remove oldNode: %d p:%p", nodes->size(), oldNode);
          // delete node; //causing assert failed: multi_heap_free multi_heap_poisoning.c:259 (head != NULL) ATM
          // EXT_LOGD(MB_TAG, "destructing object (inPR:%d)", isInPSRAM(node));
          oldNode->~Node();

          freeMBObject(oldNode);
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
      }  // nodes[i].name

      else if (updatedItem.name == "on" && updatedItem.parent[1] == "") {  // nodes[i].on
        // EXT_LOGD(ML_TAG, "handle %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(),
        // updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
        if (updatedItem.index[0] < nodes->size()) {
          EXT_LOGD(ML_TAG, "%s on: %s (#%d)", nodeState["name"].as<const char*>(), updatedItem.value.as<String>().c_str(), nodes->size());
          Node* nodeClass = (*nodes)[updatedItem.index[0]];
          if (nodeClass != nullptr) {
            nodeClass->on = updatedItem.value.as<bool>();  // set nodeclass on/off
            // EXT_LOGD(ML_TAG, "  nodeclass 🔘:%d 🚥:%d 💎:%d", nodeClass->on, nodeClass->hasOnLayout(), nodeClass->hasModifier());
            nodeClass->requestMappings();
          } else
            EXT_LOGW(ML_TAG, "Nodeclass %s not found", nodeState["name"].as<const char*>());
        }
      }  // nodes[i].on

      else if (updatedItem.parent[1] == "controls" && updatedItem.name == "value" && updatedItem.index[1] < nodeState["controls"].size()) {  // nodes[i].controls[j].value
        JsonObject control = nodeState["controls"][updatedItem.index[1]];
        // serializeJson(control, Serial);
        // EXT_LOGD(ML_TAG, "handle control value %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());

        if (updatedItem.index[0] < nodes->size()) {
          Node* nodeClass = (*nodes)[updatedItem.index[0]];
          if (nodeClass != nullptr) {
            nodeClass->updateControl(control);
            nodeClass->onUpdate(updatedItem.oldValue, control);  // custom onUpdate for the node

            nodeClass->requestMappings();
          } else
            EXT_LOGW(ML_TAG, "nodeClass not found %s", nodeState["name"].as<const char*>());
        }

      }  // nodes[i].controls[j].value
      // else
      //     EXT_LOGD(ML_TAG, "no handle for %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1],
      //     updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
      // end Nodes
    }
    // else
    // EXT_LOGD(ML_TAG, "no handle for %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(),
    // updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
  }

  void onReOrderSwap(uint8_t stateIndex, uint8_t newIndex) override {
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

 public:
  #if FT_LIVESCRIPT
  Node* findLiveScriptNode(const char* animation) {
    if (!nodes) return nullptr;

    for (Node* node : *nodes) {
      if (node && node->isLiveScriptNode()) {
        LiveScriptNode* liveScriptNode = (LiveScriptNode*)node;
        if (equal(liveScriptNode->animation, animation)) {
          EXT_LOGV(ML_TAG, "found %s", animation);
          return liveScriptNode;
        }
      }
    }
    return nullptr;
  }
  #endif
};

#endif
#endif