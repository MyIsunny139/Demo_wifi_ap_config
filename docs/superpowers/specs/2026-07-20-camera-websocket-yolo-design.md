# Camera WebSocket YOLO Design

## Architecture

The ESP32-S3 initializes the OV5640 once, then exposes `ws://<device-ip>:81/video`. A FreeRTOS task pinned to core 1 captures VGA JPEG frames only while one WebSocket client is connected and sends one complete JPEG per binary WebSocket message. The same connection accepts JSON risk commands and returns JSON acknowledgements.

The Python client continuously receives frames, keeps only the newest frame, runs YOLO person detection, displays the annotated image, and sends debounced risk state updates. ESP32 applies the state to the GPIO48 WS2812 without blocking the HTTP server.

## Components

- `Init`: system startup order and Wi-Fi lifecycle integration.
- `camera_service`: OV5640 pin map, JPEG configuration, frame acquisition and return.
- `video_stream`: port 81 HTTP server, `/video` WebSocket, capture task, JSON command parsing, acknowledgements and client lifecycle.
- `status_led`: WS2812 state task, low/medium/high display and five-second command timeout.
- `python_client`: connection, newest-frame queue, YOLO inference, risk policy and user interface.

## Protocol

- ESP32 to Python: one JPEG image in each binary WebSocket message.
- Python to ESP32: `{"type":"risk","command_id":123,"level":"medium","people":4}`.
- ESP32 to Python: `{"type":"ack","command_id":123,"applied":true}`.

Risk thresholds are 0-2 people low, 3-5 medium and 6 or more high. A level must be observed for three consecutive processed frames before it changes. Stable state is refreshed once per second; a changed state is sent immediately.

## Failure Handling

- Camera initialization failure is logged and prevents video service startup without corrupting Wi-Fi provisioning.
- No client means no frame capture.
- Send failure clears the client and pauses capture.
- Invalid JSON or unknown levels receive a negative acknowledgement.
- WebSocket disconnect or five seconds without a valid risk command turns the LED off.
- Python reconnects automatically and drops stale frames when inference is slower than reception.

## Acceptance

- ESP-IDF build passes with the validated N16R8 performance profile.
- Board detects OV5640 and captures JPEG frames.
- Python displays and annotates the live stream.
- Risk commands produce the specified WS2812 states and acknowledgements.
- Wi-Fi disconnect/reconnect and AP provisioning continue to work.
