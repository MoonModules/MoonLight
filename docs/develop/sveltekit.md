# Sveltekit

## Pull-Requests

<img width="300" src="https://github.com/user-attachments/assets/8750fc86-cbde-46ef-9392-9d6810340b52" />


MoonLight has made a number of commits which are general purpose and could be added to upstream [ESP32 SvelteKit](https://github.com/theelims/ESP32-sveltekit). Pending changes are ready to add in pull-requests as they are branched from the upstream repo.
Below lists are ordered in terms of likelyhood to be accepted:

## Pending - Ready to PR

* 🐛 [bug-metadata-parent-wifi](https://github.com/theelims/ESP32-sveltekit/commit/8573d90784f4fbf28de1d30be24b7421e965d0b4): avoid console.log errors
* ⚠️ [components-small-fixes](https://github.com/theelims/ESP32-sveltekit/commit/da2c641eea81799f683bbc452d708d4d159e357e): avoid console.log errors
* ⚠️ [compile-warnings](https://github.com/theelims/ESP32-sveltekit/commit/8a4dc4171c37d04754744f76513c2d2a08b0662d): role and tab index
* 💡 [service-name-string](https://github.com/theelims/ESP32-sveltekit/commit/05a7bd9a12999e087d0b05a62859d263679cc76a): Allow service names to be non string literals (e.g. done in MoonBase-Modules)
    * ⚠️ [EventSocket emitEvent: use String type](https://github.com/theelims/ESP32-sveltekit/commit/54c4a44eb95be2fe344bb78f022c8afcbbd8c731)
* ⚠️ [no-emit-no-clients](https://github.com/MoonModules/MoonLight/commit/c024c2ff656511c67625b3dce3642d6560724482)
* 💡 [system-status-metrics](https://github.com/theelims/ESP32-sveltekit/commit/352cfe3e376b25f7470ad4f764cdf54f7069c645): use max instead of first
* 💡 [help-to-docs](https://github.com/theelims/ESP32-sveltekit/commit/2c2d2fae5c37b220bc61dfb1ba6655485de6547f): Help link to github.io docs e.g. [Lights control](https://moonmodules.org/MoonLight/moonlight/lightscontrol)
* 💡 [System metrics](https://moonmodules.org/MoonLight/system/metrics/)
    * Loops per second (performance)
* 💡 [System status](https://moonmodules.org/MoonLight/system/status/)
    * Loops per second (performance)
* 💡 [menu-href-unique](https://github.com/theelims/ESP32-sveltekit/commit/92acbd046e478bccf7eec469e0dab5dcda53c0ae): Allow multiple modules using same Module.svelte
* 💡 [measure-battery-pin](https://github.com/theelims/ESP32-sveltekit/commit/bcd3abd1f981d48d7a76c5ee71fc99f4fe54eb08)

## Pending - 🚧

* [File Manager](https://moonmodules.org/MoonLight/moonbase/files/)
* [Devices](https://moonmodules.org/MoonLight/moonbase/devices/)
* [MoonBase-Modules](https://moonmodules.org/MoonLight/develop/modules/)

## Submitted

* None ATM

## Merged into ESP32-Sveltekit

* [Expands menu on selected subitem](https://github.com/theelims/ESP32-sveltekit/pull/77)
* [Add file.close in fileHandler handleRequest](https://github.com/theelims/ESP32-sveltekit/pull/73)
* [Refactor System Status and Metrics](https://github.com/theelims/ESP32-sveltekit/pull/78)
    * Add free_psram, used_psram and psram_size in Analytics (models.ts) and analytics_data (analytics.ts) and show in UI (SystemMetrics.svelte)
    * Send psram data only if psramFound (SystemStatus.svelte, AnalyticsService.h, SystemStatus.cpp)
* [Wifi: Multiple edits bug resolved](https://github.com/theelims/ESP32-sveltekit/pull/81)
* [ESPD_LOGx: replace first argument with TAG and define TAG as 🐼](https://github.com/theelims/ESP32-sveltekit/pull/85)

### Other improvements

* Add esp32-s3-n16r8v and LOLIN_WIFI_FIX in pio.ini (including boards folder)
* Add Monitor
    * socket.ts: add else listeners.get("monitor")?.forEach((listener) => listener(new Uint8Array(message.data)));
    * EventSocket.cpp: add void EventSocket::emitEvent with char * argument
* Add MoonBase / MoonLight specific functionality
* ESP32SvelteKit.cpp: 
    * CPU load (and main.cpp)
    * comment response.addHeader("Cache-Control", "public, immutable, max-age=31536000");
* interface/source/routes/+layout.svelte
    * Show monitor only on moon functions (so captive portal on small screen looks fine): #if (page.data.features.monitor && page.url.pathname.includes("moon"))
* main.cpp: esp_log_set_vprintf(my_vprintf); 🚧
* ci pio
* run in loopTask to avoid stack size crashes in httpd
* updatedItems (to see what specifically has been updated)

## Steps made to make Sveltekit ready for MoonLight

This is a checklist, More info on most of the items can be found in the ESP32-Sveltekit specific documentation [ESP32 SvelteKit](https://moonmodules.org/MoonLight/esp32sveltekit/), [Build Tools](https://moonmodules.org/MoonLight/gettingstarted/), [Front-end](https://moonmodules.org/MoonLight/sveltekit/) and [Back-end](https://moonmodules.org/MoonLight/statefulservice/)

* {custom} = MoonLight or name of a forked repo

* docs/media/
    * add {custom}-logo.png (used in mkdocs.yml)
    * replace favicon.png
* factory_settings.ini
    * FACTORY_AP_PASSWORD=\"\" (recommendation)
    * FACTORY_NTP_TIME_ZONE_LABEL=\"Europe/Berlin\"
* package.json
    * name = "{custom}"
    * version: "0.6.0",
* intrerface/source/lib/assets/logo.png
    * replace logo
* interface/source/routes/+layout.ts
    * title: '{custom}'
    * github:
    * copyright
    * appname: '{custom}'
* interface/source/routes/+page.svelte
    * Welcome to {custom}
    * Intro message
    * href="/" 
    * Start {custom}
* interface/source/routes/menu.svelte
    * const discord = { href: 'https://discord.gg/TC8NSUSCdV', active: true };
* interface/static/manifest.json
    * name: "{custom}"
* interface/static/favicon.png
    * replace favicon.png
* lib/framework/APSettingsService.h
    * FACTORY_AP_PASSWORD ""
* mkdocs.yml
    * site_name: {custom}
    * nav: {custom}
    * repo_name and repo_url
    * theme logo: media/{custom}-logo.png
    * analytics: provider: google property: G-R6QYDG0126
    * Copyright
* platformio.ini
    * description = {custom}
    * add [custom] build_flags and lib_deps
    * APP_NAME=\"{custom}\" ;
    * APP_VERSION=\"0.x.0\"
    * CORE_DEBUG_LEVEL=4
* README.md
    *  Custom intro
* vite.config.ts
    * Set target: 'http://192.168.1.xxx'
* setup custom code
    * src/custom
    * interface/src/routes/moonbase
    * interface/src/lib/components/moonbase
    * interface/src/lib/types/moonbase_model.ts
    * interface/src/lib/stores/moonbase_utilities.ts
* Github repo
    * change license
    * change description
    * change webhook
