# ESP32-S3-N16R8 Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apply and verify the official stable maximum performance profile for the ESP32-S3-WROOM-1-N16R8 board.

**Architecture:** Reproducible Kconfig defaults define the hardware performance profile, while the existing `Init` component validates the detected hardware at runtime. A standalone PowerShell check catches configuration regressions before the ESP-IDF build.

**Tech Stack:** ESP-IDF 5.4.2, ESP32-S3 Kconfig, ESP-IDF heap/flash/PSRAM APIs, PowerShell, CMake.

---

### Task 1: Configuration Contract

**Files:**
- Create: `tools/check_performance_config.ps1`
- Read: `sdkconfig`

- [x] Add checks for 240 MHz CPU, 16 MB QIO flash at 80 MHz, Octal PSRAM at 80 MHz, performance compiler optimization, cache sizes, camera PSRAM DMA, and camera core 1.
- [x] Run the script against the current configuration and verify that it fails because the current profile still uses Quad PSRAM, DIO flash, debug optimization, smaller caches, and camera core 0.

### Task 2: Reproducible Stable-Max Configuration

**Files:**
- Create: `sdkconfig.defaults`
- Modify: `sdkconfig`

- [x] Set the supported N16R8 memory bus modes and capacities.
- [x] Set 240 MHz CPU, performance compiler optimization, 32 KB instruction cache, 64 KB data cache, and 64-byte data cache lines.
- [x] Enable camera PSRAM DMA and select camera core 1.
- [x] Increase transmit-oriented Wi-Fi/TCP buffers without enabling experimental clock modes.
- [x] Run `tools/check_performance_config.ps1` and verify that every contract check passes.

### Task 3: Runtime Hardware Profile

**Files:**
- Modify: `components/Init/Init.c`
- Modify: `components/Init/include/Init.h`
- Modify: `components/Init/CMakeLists.txt`
- Modify: `main/Demo_ap_config.c`
- Modify: `main/CMakeLists.txt`
- Modify: `components/wifi_manager/wifi_manager.c`

- [x] Replace the placeholder `func()` API with `app_system_init()`.
- [x] Add runtime checks for CPU frequency, 16 MB flash, initialized 8 MB PSRAM, free internal memory, and free PSRAM.
- [x] Keep NVS and Wi-Fi application startup in the `Init` component, leaving `app_main()` as one clear call.
- [x] Disable station power saving after Wi-Fi starts to prioritize sustained upload throughput.
- [x] Return an error on a hardware profile mismatch and log each detected value.

### Task 4: Build Verification

**Files:**
- Verify: `build_codex/`

- [x] Build with `idf.py -B build_codex build` under ESP-IDF 5.4.2.
- [x] Resolve configuration, compilation, linking, and partition errors without entering camera/video feature implementation.
- [x] Re-run the configuration contract after the build to ensure Kconfig regeneration preserved the profile.
- [x] Report the firmware size and the exact boot-log values the user must confirm after flashing.
