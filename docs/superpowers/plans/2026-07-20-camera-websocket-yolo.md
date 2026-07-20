# Camera WebSocket YOLO Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream OV5640 JPEG frames over WebSocket, analyze people with YOLO on a PC, and return risk commands that drive the onboard WS2812.

**Architecture:** Focused ESP-IDF components own camera capture, video transport, and LED state; the existing `Init` component coordinates them with Wi-Fi. A standalone Python client separates network receive, YOLO inference, risk policy, and display.

**Tech Stack:** ESP-IDF 5.4.2, esp32-camera 2.1.7, esp_http_server WebSocket, led_strip 3.0.3, FreeRTOS, Python 3.11, websockets, OpenCV, Ultralytics YOLO, pytest.

---

### Task 1: OV5640 Camera Service

**Files:** Create `components/camera_service/CMakeLists.txt`, `components/camera_service/camera_service.c`, and `components/camera_service/include/camera_service.h`; modify `components/Init/CMakeLists.txt` and `components/Init/Init.c`.

- [x] Define the verified ESP32-S3-WROOM camera pin map and VGA JPEG double-buffer configuration.
- [x] Initialize the sensor, verify the OV5640 PID, and expose frame get/return APIs.
- [x] Build the firmware and report compile/link results.

### Task 2: WS2812 Status Service

**Files:** Create `components/status_led/CMakeLists.txt`, `components/status_led/status_led.c`, and `components/status_led/include/status_led.h`.

- [x] Initialize one WS2812 on GPIO48 with the RMT backend.
- [x] Implement low/off, medium/yellow blink, high/red fast blink, and five-second timeout in one FreeRTOS task.
- [x] Expose a non-blocking risk update API.

### Task 3: Video WebSocket Service

**Files:** Create `components/video_stream/CMakeLists.txt`, `components/video_stream/video_stream.c`, and `components/video_stream/include/video_stream.h`; modify `components/Init/Init.c` and `components/Init/CMakeLists.txt`.

- [x] Start an independent HTTP server on port 81 with control port 32769 and `/video` WebSocket URI.
- [x] Capture on core 1 at a ten FPS ceiling only while a client is connected.
- [x] Send JPEG binary frames synchronously before returning camera buffers.
- [x] Parse risk JSON, update the LED, reject invalid commands, suppress duplicate command IDs, and send acknowledgements.
- [x] Start/stop the service from Wi-Fi connected/disconnected callbacks.
- [x] Build and verify component integration.

### Task 4: Python Risk Policy TDD

**Files:** Create `python_client/risk_policy.py`, `python_client/tests/test_risk_policy.py`, and `python_client/requirements.txt`.

- [x] Write pytest cases for thresholds and three-frame debounce before implementing the policy.
- [x] Run tests and confirm the expected import failure.
- [x] Implement the minimal policy and run tests to green.

### Task 5: Python Video AI Client

**Files:** Create `python_client/video_ai.py` and `python_client/README.md`.

- [x] Implement reconnecting WebSocket receive with a one-slot newest-frame queue.
- [x] Decode JPEG, run a configurable Ultralytics nano model for person class only, annotate, and display.
- [x] Send immediate changed risk states, one-second refreshes, and consume acknowledgement messages.
- [x] Run Python syntax checks and pytest.

### Task 6: Final Verification

**Files:** Verify all modified and created files.

- [x] Run the performance configuration contract.
- [x] Run Python tests and syntax checks.
- [x] Run the full ESP-IDF build in `build_codex`.
- [x] Review task ownership, buffer lifetime, disconnect paths, comments, and user-visible run instructions.
