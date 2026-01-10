# Architecture

## MoonLight Task Architecture & Synchronization

MoonLight uses a multi-core, multi-task architecture on ESP32 to achieve smooth LED effects while maintaining responsive UI and network connectivity. This document explains the task structure, synchronization mechanisms, and why this configuration is optimal.

## Main Tasks

| Task | Core | Priority | Stack Size | Frequency | Purpose |
|------|------|----------|------------|-----------|---------|
| **WiFi/BT** | 0 (PRO_CPU) | 23 | System | Event-driven | System networking stack |
| **lwIP TCP/IP** | 0 (PRO_CPU) | 18 | System | Event-driven | TCP/IP protocol processing |
| **ESP32SvelteKit** | 0 (PRO_CPU) | 2 | System | 10ms | HTTP/WebSocket UI framework |
| **Driver Task** | 1 (APP_CPU) | 3 | 3-4KB | ~60 fps | Output data to LEDs via DMA/I2S/LCD/PARLIO |
| **Effect Task** | 0 (PRO_CPU) | 3 | 3-4KB | ~60 fps | Calculate LED colors and effects |

Effect Task (Core 0, Priority 3)

- **Function**: Pure computation - calculates pixel colors based on effect algorithms
- **Operations**: Reads/writes to `channels` array, performs mathematical calculations
- **Tolerant to preemption**: WiFi interruptions are acceptable as this is non-timing-critical
- **Why Core 0**: Can coexist with WiFi; uses idle CPU cycles when WiFi is not transmitting

Driver Task (Core 1, Priority 3)

- **Function**: Timing-critical hardware operations
- **Operations**: Sends pixel data to LEDs via DMA, I2S (ESP32), LCD (S3), or PARLIO (P4)
- **Requires uninterrupted execution**: DMA timing must be precise to avoid LED glitches
- **Why Core 1**: Isolated from WiFi interference; WiFi on Core 0 cannot preempt this task

ESP32SvelteKit Task (Core 1, Priority 2)

- **Function**: HTTP server and WebSocket handler for UI
- **Operations**: Processes REST API calls, WebSocket messages, JSON serialization
- **Runs every**: 10ms
- **Why Core 1, Priority 2**: Lower priority than system Tasks

## Task Interaction Flow

```mermaid
sequenceDiagram
    participant User
    participant WebUI
    participant SvelteKit
    participant EffectTask
    participant DriverTask
    participant LEDs

    Note over EffectTask,DriverTask: Both tasks synchronized via mutex

    User->>WebUI: Adjust effect parameter
    WebUI->>SvelteKit: WebSocket message
    SvelteKit->>SvelteKit: Update in-memory state
    
    Note over EffectTask: Core 0 (PRO_CPU)
    EffectTask->>EffectTask: Take mutex (10µs)
    EffectTask->>EffectTask: memcpy channelsD → channelsE
    EffectTask->>EffectTask: Release mutex
    EffectTask->>EffectTask: Compute effects (5-15ms)
    EffectTask->>EffectTask: Take mutex (10µs)
    EffectTask->>EffectTask: Swap buffer pointers
    EffectTask->>EffectTask: Release mutex

    Note over DriverTask: Core 1 (APP_CPU)
    DriverTask->>DriverTask: Take mutex (10µs)
    DriverTask->>DriverTask: Capture buffer pointer
    DriverTask->>DriverTask: Release mutex
    DriverTask->>DriverTask: Send via DMA (1-5ms)
    DriverTask->>LEDs: Pixel data
```

## Core Assignments

Why This Configuration is Optimal

```mermaid
graph TB
    subgraph Core0["Core 0 (PRO_CPU)"]
        WiFi[WiFi/BT<br/>Priority 23]
        lwIP[lwIP TCP/IP<br/>Priority 18]
        Effect[Effect Task<br/>Priority 3<br/>Computation Only]
    end
    
    subgraph Core1["Core 1 (APP_CPU)"]
        Driver[Driver Task<br/>Priority 3<br/>Timing-Critical]
        SvelteKit[ESP32SvelteKit<br/>Priority 2<br/>HTTP/WebSocket]
    end
    
    WiFi -.->|Preempts during bursts| Effect
    Effect -.->|Uses idle cycles| WiFi
    Driver -->|Preempts when needed| SvelteKit
    
    Effect <-->|Mutex-protected<br/>buffer swap| Driver
    
    style WiFi fill:#8f8989
    style lwIP fill:#8f8c89
    style Effect fill:#898c8f
    style Driver fill:#898f89
    style SvelteKit fill:#8f8f89
```

Design Principles

1. **Timing-Critical Hardware on Core 1**
    - WiFi/BT run at priority 23 on Core 0
    - If Driver Task were on Core 0, WiFi would constantly preempt it
    - DMA/I2S/LCD/PARLIO require uninterrupted timing
    - **Result**: Core 1 isolation prevents LED glitches

2. **Computation-Heavy Effects on Core 0**
    - Effect computation is pure math (no hardware timing requirements)
    - Can tolerate WiFi preemption (frame computes slightly slower)
    - Uses CPU cycles when WiFi is idle
    - **Result**: Efficient CPU utilization, true dual-core parallelism

3. **SvelteKit on Core 1 with Lower Priority**
    - Driver Task (priority 3) preempts SvelteKit (priority 2)
    - LED output never stalls for HTTP requests
    - SvelteKit processes UI during Driver idle time
    - **Result**: UI remains responsive without affecting LEDs

4. **Minimal Lock Duration**
    - Mutex held for only ~10µs (pointer swap only)
    - 99% of execution is unlocked and parallel
    - Tasks interleave efficiently via FreeRTOS scheduling
    - **Result**: "Full speed ahead" - minimal blocking

## Double Buffering & Synchronization

Buffer Architecture (PSRAM Only)

```mermaid
graph LR
    subgraph MemoryBuffers["Memory Buffers"]
        Effects[Effects Buffer<br/>channelsE*]
        Drivers[Drivers Buffer<br/>channelsD*]
    end
    
    EffectTask[Effect Task<br/>Core 0] -.->|1. memcpy| Effects
    EffectTask -.->|2. Compute effects| Effects
    EffectTask -.->|3. Swap pointers<br/>MUTEX 10µs| Drivers
    
    DriverTask[Driver Task<br/>Core 1] -->|4. Read pixels| Drivers
    DriverTask -->|5. Send via DMA| LEDs[LEDs]
    
    style Effects fill:#898f89
    style Drivers fill:#898c8f
```

Synchronization Flow

**Key Point**: Effects need read-modify-write access (e.g., blur, ripple effects read neighboring pixels), so `memcpy` ensures they see a consistent previous frame.

Performance Impact

| LEDs | Buffer Size | memcpy Time | % of 60fps Frame |
|------|-------------|-------------|------------------|
| 1,000 | 3 KB | 10 µs | 0.06% |
| 5,000 | 15 KB | 50 µs | 0.3% |
| 10,000 | 30 KB | 100 µs | 0.6% |
| 20,000 | 60 KB | 200 µs | 1.2% |

**Conclusion**: Double buffering overhead is negligible (<1% for typical setups).

## Performance Budget at 60fps

Per-Frame Time Budget (16.66ms)

```mermaid
gantt
    title Core 0 Timeline (Effect Task)
    dateFormat X
    axisFormat %L

    section WiFi Bursts
    WiFi burst 1    :0, 200
    WiFi burst 2    :5000, 100
    WiFi burst 3    :12000, 150

    section Effect Computation
    memcpy          :500, 100
    Compute effects :600, 14000
    Swap pointers   :14600, 10
    
    section Idle
    Available       :200, 300
    Available       :14610, 1390
```

```mermaid
gantt
    title Core 1 Timeline (Driver + SvelteKit)
    dateFormat X
    axisFormat %L

    section Driver Task
    Capture pointer :0, 10
    Send via DMA    :10, 3000
    
    section SvelteKit
    Process WebSocket :3000, 2000
    JSON serialize    :5000, 1000
    
    section Driver Task
    Capture pointer :6000, 10
    Send via DMA    :6010, 3000
    
    section Idle
    Available       :9010, 7656
```

Overhead Analysis

| Source | Light Load | Heavy Load | Peak (Flash Write) |
|--------|------------|------------|--------------------|
| WiFi preemption | 0.5-1ms (3-6%) | 2-5ms (12-30%) | 300ms (WiFi scan) |
| SvelteKit | 0.5-2ms (on Core 1) | 2-3ms (on Core 1) | 5ms |
| Double buffer memcpy | 0.1ms (0.6%) | 0.1ms (0.6%) | 0.1ms |
| Mutex locks | 0.02ms (0.1%) | 0.02ms (0.1%) | 0.02ms |
| **Total** | **1-3ms (6-18%)** | **4-8ms (24-48%)** | **Flash: user-triggered** |

**Result**: 

- ✅ 60fps sustained during normal operation
- ✅ 52-60fps during heavy WiFi/UI activity
- ✅ No stutter during UI interaction

## Configuration

Enabling Double Buffering

Double buffering is **automatically enabled** when PSRAM is detected:

```cpp
// In PhysicalLayer::setup()
if (psramFound()) {
  lights.useDoubleBuffer = true;
  lights.channelsE = allocMB<uint8_t>(maxChannels);
  lights.channelsD = allocMB<uint8_t>(maxChannels);
} else {
  lights.useDoubleBuffer = false;
  lights.channelsE = allocMB<uint8_t>(maxChannels);
  lights.channelsD = lights.channelsE;
}
```

Moving ESP32SvelteKit to Core 1

Add to `platformio.ini`:
```ini
build_flags =
  -DESP32SVELTEKIT_RUNNING_CORE=1
```

Or in code before including framework:
```cpp
#define ESP32SVELTEKIT_RUNNING_CORE 1
#include <ESP32SvelteKit.h>
```

## Summary

This architecture achieves optimal performance through:

1. **Core Separation**: Computation (Core 0) vs Timing-Critical I/O (Core 1)
2. **Priority Hierarchy**: Driver > SvelteKit ensures LED timing is never compromised
3. **Minimal Locking**: 10µs mutex locks enable 99% parallel execution
4. **Double Buffering**: Eliminates tearing with <1% overhead

**Result**: Smooth 60fps LED effects with responsive UI and stable networking. 🚀


## Idle Watchdog

For big setups, 16K LEDs typically, Task watchdog got triggered crashes occur more frequently. Mostly in the effects and drivers task but also in other tasks like WiFi occasionally. The workaround to avoid this is adding task yields in the code. This is currently done as follow:

```cpp
void effectOrDriverTask(void* pvParameters) {
  // 🌙
  esp_task_wdt_add(NULL);

  setup();

  while (true) {

    loop();

    esp_task_wdt_reset();
    vTaskDelay(1);
  }
  // Cleanup (never reached in this case, but good practice)
  esp_task_wdt_delete(NULL);
}

void Node::loop() {
    addYield(10);
}

void ArtNetOutDriver::loop() {
    for (each package) {
        writePackage();
        addYield(10);
    }
}

inline void addYield(uint8_t frequency) {
  if (++yieldCallCount % frequency == 0) {
    yieldCounter++;
    vTaskDelay(1); 
  }
}


```

* esp_task_wdt ( #include "esp_task_wdt.h" ) make sure the tasks are in the watchdog system and in the task loop it is reset and vTaskDelay(1) makes sure there is a yield each time
* taskYIELD() is not good enough as it does not give back control to the idle task so we need vTaskDelay(1), taskYIELD() only yields to tasks of equal or higher priority
* increasing the watchdog timer from 5s to 10s might trigger less watchdog crashes but is not eliminating it so this is not added yet. However, for extreme setups (up to 100K LEDs), even with yields, processing time might legitimately exceed 5s. So might be added later.
* Node::loop(): each active node will call addYield(10)
* ArtNetOutDriver::loop(): as a massive amount of packages are blasted (for 16K LEDs 97 universes / packets), addYield(10) is called after each packet
* addYield(10) means: send a vTaskDelay(1) every 10 times.
* Occasional flood of ESP_LOG error messages might also trigger the watchdog so where it happened a vTaskDelay(1) is added e.g. in EventSocket::emitEvent(), failed to send event