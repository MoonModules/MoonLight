# Fixture

## Functional

Fixture definitions must be uploaded to the [file system](/moonbase/files).
Examples can be found here:

* [StarLight json fixtures](https://github.com/MoonModules/WLED-Effects/tree/master/StarLight/Fixtures) (generated by StarLight)
* [StarLight live fixtures and live effects](https://github.com/MoonModules/StarLight/tree/main/misc/LiveScripts])

## Technical

Using component FileEdit, see [Components](https://moonmodules.org/MoonLight/components/#fileedit)

### Server

[FixtureService.h](https://github.com/MoonModules/MoonLight/blob/main/lib/moonlight/FixtureService.h) and [FixtureService.cpp](https://github.com/MoonModules/MoonLight/blob/main/lib/moonlight/FixtureService.cpp)

* StarLight LedModFixture
* FixtureState: StarLight: Variable("Fixture", ...)
* FixtureService: 
    * HttpEndpoint, EventEndpoint, WebSocketServer, FSPersistence
    * loop50ms: socket->emitEvent ledsP

### UI

[Fixture.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/moonlight/fixture/Fixture.svelte)

* FixtureState
* getState(): handleFixtureState handleStarState
* handleFixtureState()
* handleStarState()
* onMount(): socket fixture handleFixtureState
* sendSocket()
* SettingsCard: getState sendSocket
