# ESP32-S3-N16R8 Performance Design

## Goal

Configure the ESP32-S3-WROOM-1-N16R8 board at Espressif's supported performance limits before implementing the camera streaming application.

## Hardware Profile

- CPU: dual-core ESP32-S3 at 240 MHz
- Flash: 16 MB Quad SPI flash at 80 MHz in QIO mode
- PSRAM: 8 MB Octal SPI PSRAM at 80 MHz
- Camera: OV5640 on the board pin map recorded in `ESP32CAM硬件接口.jpg`

Experimental 120 MHz flash or PSRAM modes are excluded because the project requires sustained video operation.

## Performance Profile

- Build with compiler performance optimization (`-O3` in ESP-IDF 5.4.2).
- Use a 32 KB instruction cache and a 64 KB data cache with a 64-byte data cache line.
- Keep PSRAM boot initialization and memory testing enabled.
- Use the heap allocator for PSRAM while reserving internal DMA-capable memory.
- Enable camera PSRAM DMA and pin the camera driver task to core 1.
- Keep Wi-Fi on its normal core and disable Wi-Fi power saving only while station networking is active.
- Increase transmit-oriented Wi-Fi and TCP buffers conservatively, enable the supported IRAM throughput paths, and keep TCP/IP on core 0. Buffers required by DMA remain in internal RAM.

## Runtime Verification

The existing `components/Init` component owns a hardware profile report. At startup it logs CPU frequency, flash capacity, PSRAM capacity, free internal RAM, and free PSRAM. A capacity or initialization mismatch returns an error and prevents the application from silently running with the wrong memory configuration.

## Acceptance Criteria

- Configuration contract checks pass.
- ESP-IDF build completes in `build_codex`.
- Boot log reports a 240 MHz CPU, 16 MB flash, initialized 8 MB PSRAM, and a passing PSRAM memory test.
- No 120 MHz experimental memory mode is enabled.
- Camera/video application work remains paused until board logs confirm the hardware profile.
