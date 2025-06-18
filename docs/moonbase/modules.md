# Modules

<img width="829" alt="image" src="https://github.com/user-attachments/assets/3384f3ba-b5e6-4993-9a8b-80c25878e176" />

## Functional

A module is a generic building block to create server and UI functionality which can be activated through the menu.

MoonBase-Modules are inspired by WLED usermods, further developed in StarBase and now in MoonLight (using the ESP32-Sveltekit infrastructure)

See [Demo](module/demo.md), [Instances](module/instances.md) and [Editor](module/editor.md) for examples

Press the ? on any module to go to the documentation.

## Technical

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
      , FilesService *filesService
    ) : Module("demo", server, sveltekit, filesService) {
        ESP_LOGD(TAG, "constructor");
    }
}
```

* Implement function **setupDefinition** to create a json document with the datastructure
    * Store data on the file system
    * Generate the UI
    * Initialy create the module data

```cpp
void setupDefinition(JsonArray root) override{
    JsonObject property; // state.data has one or more properties
    JsonArray details; // if a property is an array, this is the details of the array
    JsonArray values; // if a property is a select, this is the values of the select

    property = root.add<JsonObject>(); property["name"] = "hostName"; property["type"] = "text"; property["default"] = "MoonLight";
    property = root.add<JsonObject>(); property["name"] = "connectionMode"; property["type"] = "select"; property["default"] = "Signal Strength"; values = property["values"].to<JsonArray>();
    values.add("Offline");
    values.add("Signal Strength");
    values.add("Priority");

    property = root.add<JsonObject>(); property["name"] = "savedNetworks"; property["type"] = "array"; details = property["n"].to<JsonArray>();
    {
        property = details.add<JsonObject>(); property["name"] = "SSID"; property["type"] = "text"; property["default"] = "ewtr"; property["min"] = 3; property["max"] = 32; 
        property = details.add<JsonObject>(); property["name"] = "Password"; property["type"] = "password"; property["default"] = "";
    }

}

```

* Implement function **onUpdate** to define what happens if data changes
    * struct UpdatedItem defines the update (parent property (including index in case of multiple records), name of property and value)
    * This runs in the httpd / webserver task. To run it in the main (application task use runInLoopTask - see [ModuleEditor](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/ModuleEditor.h)) - as httpd stack has been increased runInLoopTask is less needed

```cpp
    void onUpdate(UpdatedItem &updatedItem) override
    {
        if (updatedItem.name == "lightsOn" || updatedItem.name == "brightness") {
            ESP_LOGD(TAG, "handle %s = %s -> %s", updatedItem.name, updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
            FastLED.setBrightness(_state.data["lightsOn"]?_state.data["brightness"]:0);
        } else if (updatedItem.parent[0] == "nodes" && updatedItem.name == "name") {    
            ESP_LOGD(TAG, "handle %s = %s -> %s", updatedItem.name, updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
            if (updatedItem.oldValue.length())
                ESP_LOGD(TAG, "delete %s ...", updatedItem.oldValue.c_str());
            if (updatedItem.value.as<String>().length())
                compileAndRun(updatedItem.value);
        } else
            ESP_LOGD(TAG, "no handle for %s.%s[%d] = %s -> %s", updatedItem.parent[0], updatedItem.name, updatedItem.index[0], updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
    }
```

* Implement function **loop1s** to send [readonly data](#Readonly_data) from the server to the UI
    * Optionally, only when a module has readonly data

```cpp
    void loop1s() {
        if (!_socket->getConnectedClients()) return; 

        JsonDocument newData; //to only send updatedData
        JsonArray scripts = newData["scripts"].to<JsonArray>(); //to: remove old array
        LiveScriptNode node;
        node.getScriptsJson(scripts);

        //only if changed
        if (_state.data["scripts"] != newData["scripts"]) {
            _state.data["scripts"] = newData["scripts"]; //update without compareRecursive -> without handles
            update([&](ModuleState &state) {
                return StateUpdateResult::CHANGED; // notify StatefulService by returning CHANGED
            }, "server");
        }
    }
```

* Add the module in [main.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/main.cpp)

```cpp
ModuleDemo moduleDemo = ModuleDemo(&server, &esp32sveltekit, &filesService);
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
      href: '/moonbase/module?module=demo',
      feature: page.data.features.moonlight,
   },
]
```

* This is all to create a fully functioning new module

### Readonly data

A module can consist of data which is edited by the user (e.g. selecting a live script to run) and data which is send from the server to the UI (e.g. a list of running processes). Currently both type of valuas are stored in state data and definition. Distinguished by property["ro"] = true in setupDefinition. So the client uses state data and definition to build a screen with both types visually mixed together (what is desirable). Currently there are 2 websocket events: one for the entire state (including readonly) and one only for readonly which only contains the changed values. Module.svelte handles readonly differently by the function handleRO which calls updateRecursive which only update the parts of the data which has changed.

It might be arguable that readonly variables are not stored in state data.

### Server

* [Module.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Module.h) and [Module.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Module.cpp) will generate all the required server code

### UI
* [Module.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/moonbase/module/Module.svelte) will deal with the UI
* [MultiInput.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/lib/components/moonbase/MultiInput.svelte) is used by Module.svelte to display the right UI widget based on what is defined in the definition json
* Modifications done in [menu.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/menu.svelte) do identify a module by href and not by title alone
