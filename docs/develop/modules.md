# Modules

<img width="829" alt="image" src="https://github.com/user-attachments/assets/3384f3ba-b5e6-4993-9a8b-80c25878e176" />

A module is a generic building block to create server and UI functionality which can be activated through the menu.

See [Lights Control](https://moonmodules.org/MoonLight/moonlight/lightscontrol) or [Devices](https://moonmodules.org/MoonLight/moonbase/devices) for examples

Press the ? on any module to go to the documentation.

With Moonbase Modules it is possible to create new module entirely from one c++ class by only defining a json document describing the data structure and a function catching all the changes in the data. Http endpont and websockets are created automatically. There is no need to create any UI code, it is entirely driven by the json document.

Each module has each own documentation which can be accessed by pressing the ? and is defined in the [docs](https://github.com/MoonModules/MoonLight/tree/main/docs) folder.

To create a new module:

* Create a **class** which inherits from Module
* Call the Module **constructor** with the name of the module.
    * This name will be used to set up http rest api and webserver sockets

```cpp
class ModuleDemo : public Module
{
public:

ModuleDemo(PsychicHttpServer *server
      , ESP32SvelteKit *sveltekit
      , FileManager *fileManager
    ) : Module("demo", server, sveltekit) {
        EXT_LOGD(MB_TAG, "constructor");
    }
}
```

* Implement function **setupDefinition** to create a json document with the datastructure
    * Store data on the file system
    * Generate the UI
    * Initialy create the module data

```cpp
void setupDefinition(const JsonArray& controls) override{
    JsonObject control; // state.data has one or more properties
    JsonArray rows; // if a control is an array, this is the rows of the array

    control = addControl(controls, "hostName", "text"); 
    control["default"] = "MoonLight";
    control = addControl(controls, "connectionMode", "select"); 
    control["default"] = "Signal Strength";
    addControlValue(control, "Offline");
    addControlValue(control, "Signal Strength");
    addControlValue(control, "Priority");

    control = addControl(controls, "savedNetworks", "rows"); 
    rows = control["n"].to<JsonArray>();
    {
        control = addControl(rows, "SSID", "text", 3, 32); 
        control["default"] = "ewtr";
        control = addControl(rows, "Password", "password"); 
        control["default"] = "";
    }
}

```

* Implement function **onUpdate** to define what happens if data changes
    * struct UpdatedItem defines the update (parent control (including index in case of multiple records), name of control and value)
    * UpdatedItems are created in the httpd / webserver task and via the loop() function sent to onUpdate running in the Sveltekit task. e.g. see [ModuleLightsControl](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/ModuleLightsControl.h)

```cpp
    void onUpdate(const UpdatedItem &updatedItem) override
    {
        if (updatedItem.name == "lightsOn" || updatedItem.name == "brightness") {
            EXT_LOGD(MB_TAG, "handle %s = %s -> %s", updatedItem.name, updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
            FastLED.setBrightness(_state.data["lightsOn"]?_state.data["brightness"]:0);
        } else if (updatedItem.parent[0] == "nodes" && updatedItem.name == "name") {    
            EXT_LOGD(MB_TAG, "handle %s = %s -> %s", updatedItem.name, updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
            if (updatedItem.oldValue.length())
                EXT_LOGD(MB_TAG, "delete %s ...", updatedItem.oldValue.c_str());
            if (updatedItem.value.as<String>().length())
                compileAndRun(updatedItem.value);
        } else
            EXT_LOGD(MB_TAG, "no handle for %s.%s[%d] = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.name.c_str(), updatedItem.index[0], updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
    }
```

* Implement function **loop1s** to send [readonly data](#readonly-data) from the server to the UI
    * Optionally, only when a module has readonly data

```cpp
    void loop1s() {
        if (!_socket->getConnectedClients()) return; // 🌙 No need for UI tasks

        JsonDocument newData; //to only send updatedData
        JsonArray scripts = newData["scripts"].to<JsonArray>(); //to: remove old array
        LiveScriptNode node;
        node.getScriptsJson(scripts);

        //only if changed
        if (_state.data["scripts"] != newData["scripts"]) {
            _state.data["scripts"] = newData["scripts"]; //update without compareRecursive -> without handles
            update([&](ModuleState &state) {
                return StateUpdateResult::CHANGED; // notify StatefulService by returning CHANGED
            }, _moduleName);
        }
    }
```

* Add the module in [main.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/main.cpp)

```cpp
ModuleDemo moduleDemo = ModuleDemo(&server, &esp32sveltekit, &fileManager);
...
moduleDemo.begin();
...
moduleDemo.loop();
...
moduleDemo.loop1s();
```

* Add the module in [menu.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/menu.svelte) (this will be automated in the future)

```ts
submenu: [
   {
      title: 'Module Demo',
      icon: BulbIcon,
      href: '/moonbase/module?group=moon&module=demo',
      feature: page.data.features.moonlight,
   },
]
```

* This is all to create a fully functioning new module

### Readonly data

A module can consist of data which is edited by the user (e.g. selecting a live script to run) and data which is send from the server to the UI (e.g. a list of running processes). Currently both type of values are stored in state data and definition. Distinguished by control["ro"] = true in setupDefinition. So the client uses state data and definition to build a screen with both types visually mixed together (what is desirable). Currently there are 2 websocket events: one for the entire state (including readonly) and one only for readonly which only contains the changed values. Module.svelte handles readonly differently by the function handleRO which calls updateRecursive which only update the parts of the data which has changed.

It might be arguable that readonly variables are not stored in state data.

### Server

* [Module.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Module.h) and [Module.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Module.cpp) will generate all the required server code

### UI
* [Module.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/moonbase/module/Module.svelte) will deal with the UI
* [FieldRenderer.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/lib/components/moonbase/FieldRenderer.svelte) is used by Module.svelte to display the right UI widget based on what is defined in the definition json
* Modifications done in [menu.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/menu.svelte) do identify a module by href and not by title alone

### Example of json

```json
{
  "nrOfLights": 256,
  "channelsPerLight": 3,
  "maxChannels": 12288,
  "size": {
    "x": 16,
    "y": 16,
    "z": 1
  },
  "nodes#": 2,
  "layers": [
    {
      "nrOfLights": 256,
      "size": {
        "x": 16,
        "y": 16,
        "z": 1
      },
      "mappingTable#": 256,
      "nrOfZeroLights": 0,
      "nrOfOneLight": 256,
      "mappingTableIndexes#": 0,
      "nrOfMoreLights": 0,
      "nodes#": 1
    }
  ]
}
```