# Core Dump

<img width="320" height="441" src="../coredump.png" />

## Introduction

The Core Dump module allows you to download and analyze memory snapshots from your device when it crashes. When a crash occurs, the ESP32 automatically captures the state of the system memory at that moment. This snapshot, called a core dump, is invaluable for debugging firmware issues, as it contains information about what the processor was doing, which functions were running, and what values were stored in memory at the time of the crash.

By downloading and analyzing core dumps, you can identify the root cause of crashes and fix bugs that might be difficult to reproduce in a development environment.

## Controls

The Core Dump interface provides a single control:

**Download Core Dump (coredump.bin)** - This button downloads the most recent core dump file saved on the device. The file is saved as `coredump.bin` to your local machine. If no crash has occurred since the last core dump was retrieved or cleared, the downloaded file may be empty or represent an older crash.

## Details

### Understanding Core Dumps

A core dump is a binary file that captures the exact state of your device's memory at the moment of a crash. This includes:

- CPU register values
- Stack traces showing the call sequence that led to the crash
- Memory contents at the time of the failure
- Information about running tasks and their states

### Matching Core Dumps to Firmware

Each core dump contains a SHA-256 hash that corresponds to the firmware build that was running when the crash occurred. This hash is critical because **you must use the exact `.elf` file from that specific firmware build to successfully decode the core dump**.

The `.elf` file is automatically generated during compilation and is stored in the `build/elf` folder. Without the matching `.elf` file, the core dump cannot be properly decoded.

### Decoding a Core Dump

To decode a core dump, you'll use the `esp-coredump` tool provided by Espressif. The basic command structure is:
```bash
esp-coredump info_corefile --core [COREDUMP_FILE] --gdb ~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb --elf [ELF_FILE]
```

Where:

- `[COREDUMP_FILE]` is the `coredump.bin` file you downloaded
- `[ELF_FILE]` is the matching `.elf` file from your build
- The `--gdb` argument specifies the path to the GDB debugger (optional, but recommended to ensure you use a patched version)

### Recommended Workflow

To streamline the debugging process, consider implementing these automation steps:

1. **Archive ELF files**: a script automatically saves the .elf file in a `build/elf` folder, named with the app hash (the same hash is later available in the coredump) 
2. **Automate decoding**: Create a decoding script that:
    - Reads the firmware hash from the downloaded core dump
    - Locates the matching `.elf` file in your archive
    - Automatically runs the `esp-coredump` command with the correct parameters

With this setup, decoding becomes as simple as running: `./coredump.py coredump.bin`

### Additional Resources

For more detailed technical information about core dumps on ESP32 devices, refer to the official Espressif documentation:  
[ESP-IDF Core Dump Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/core_dump.html)