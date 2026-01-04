# Tasks module

<img width="320" src="https://github.com/user-attachments/assets/3b6dd789-b281-4131-abad-0b7e8538de3e" />
<img width="320" src="https://github.com/user-attachments/assets/b3e50754-e58b-4b6c-8cdf-0b3a99dd3061" />

Shows system tasks info.

* Core 0: which task is currently running on core 0
* Core 1: which task is currently running on core 1

* Per task:
    * name: task name
    * Summary
       * state: Ready (🧍‍♂️), Running (🏃‍♂️), Blocked (🚧), Suspended (⏸️), deleted (🗑️), unknown (❓)
       * cpu: percentage of CPU time (per core) used
       * prio: task priority
    * stack: stack left
    * runtime: amount of cpu cycles consumed
    * core: allocated core, not necessarily used core (see above)

## Default stack sizes

| Task name (runtime) | Kconfig option | Default stack size (words → bytes) | Notes |
|----------------------|----------------|------------------------------------:|-------|
| AppEffectTask ||(psramFound()?6:4) * 1024|Core 1|
| AppDriverTask ||(psramFound()?4:3) * 1024|Core 0|
| Sveltekit ||(psramFound()?8:6) * 1024|Core 0|
| **wifi** (Wi-Fi driver) | `CONFIG_ESP_WIFI_TASK_STACK_SIZE` | 3072 words → 12288 bytes → 12 KB | Handles Wi-Fi MAC/driver. Needs extra headroom. |
| **tiT** (TCP/IP / lwIP task) | `CONFIG_TCPIP_TASK_STACK_SIZE` | 3072 words → 12288 bytes → 12 KB | All socket callbacks, DHCP, etc. |
| **event** (ESP event loop) | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` (older) / `CONFIG_ESP_EVENT_LOOP_TASK_STACK_SIZE` (newer) | 2048 words → 8192 bytes → 8 KB | Dispatches Wi-Fi/IP/BLE events. |
| **esp_timer** (high-res timers) | `CONFIG_ESP_TIMER_TASK_STACK_SIZE` | 1024 words → 4096 bytes → 4 KB | Runs high-resolution callback handlers (timers/timeouts). |
| **ipc0 / ipc1** (cross-core IPC) | `CONFIG_ESP_IPC_TASK_STACK_SIZE` | 1024 words → 4096 bytes → 4 KB | Used for cross-CPU calls / IPC. |
| **main** (`app_main`) | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 3584 words → 14336 bytes → 14 KB | Your entry point task. |
| **Idle tasks** | *(fixed)* | ~768 bytes each (per CPU) | System FreeRTOS idle tasks. |
| **Timer service task** | `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` | 2048 words → 8192 bytes → 8 KB | FreeRTOS software timers (xTimer). |
| **Arduino loop task** (if using Arduino core) | `CONFIG_ARDUINO_LOOP_STACK_SIZE` | 8192 bytes → 8 KB | Present when Arduino core is used as a component. |
| **btc_task** (classic BT) | `CONFIG_BT_BTC_TASK_STACK_SIZE` | 3072 words → 12288 bytes → 12 KB | Only if Bluetooth Classic enabled. |
| **btu_task** (BT controller) | `CONFIG_BT_BTU_TASK_STACK_SIZE` | 4096 words → 16384 bytes → 16 KB | Heaviest Bluetooth controller task (if BT enabled). |
| **BLE host task** | `CONFIG_BT_BLE_HOST_TASK_STACK_SIZE` | 3072 words → 12288 bytes → 12 KB | Only if BLE enabled. |
