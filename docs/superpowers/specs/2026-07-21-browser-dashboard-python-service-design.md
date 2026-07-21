# Browser Dashboard Python Service Design

## Goal

Build a single-page browser dashboard for the video-ai project. The browser shows live video, connection status, real-time warning state, threshold controls, and action buttons for start, stop, screenshot, and save. The browser does not talk to the ESP32 directly. A local Python service owns the ESP32 connection, YOLO inference, warning logic, and frame capture.

## Architecture

The Python service runs on the PC and exposes one local web app. It keeps three live flows:

- ESP32 video ingest: connect to `ws://<device-ip>:81/video`, receive JPEG frames, keep the newest frame only.
- Analysis pipeline: decode JPEG, run YOLO person detection, compute risk level, and keep the latest annotated frame plus status snapshot.
- Browser push channel: serve the page and push state updates, warning messages, and annotated frames to the browser.

Recommended stack:

- `FastAPI` for the local HTTP and WebSocket server.
- Existing `websockets`, `opencv-python`, and `ultralytics` for ingest and analysis.
- Static HTML, CSS, and vanilla JavaScript for the browser UI so the page stays simple and has no extra build step.

## Page Layout

The page is a single dark dashboard with compact, bordered panels and stable sizing.

- Top bar: connection state, current ESP32 address, current processing state.
- Main video panel: live annotated frame, with a clear warning banner overlaid inside the panel.
- Right-side control panel: threshold inputs, start button, stop button, screenshot button, save button.
- Bottom status strip: current people count, risk level, receive FPS, inference FPS, last update time, and current warning text.

The visual style should stay data-dense and restrained: dark neutral background, status colors only for state, no decorative gradients or floating orbs, and no oversized hero treatment.

## Interaction Model

- `Start` connects the Python service to the ESP32 stream and begins analysis if not already running.
- `Stop` closes the ESP32 session, stops analysis, and leaves the browser page open.
- `Threshold` inputs change the active people-count boundaries without restarting the page.
- `Screenshot` saves the latest annotated frame to disk and exposes the saved file path or download link.
- `Save` persists the current frame with metadata such as timestamp, people count, and risk level.

The browser updates immediately when the latest analyzed frame or warning state changes. If no valid frame is available, the video panel shows a loading state instead of a blank area.

## Backend Responsibilities

The Python service owns all state and side effects:

- Maintain one ESP32 client connection and reconnect when needed.
- Keep a one-frame queue so stale frames are dropped when inference is slower than capture.
- Run YOLO in a worker thread or background task.
- Compute warning state from the detected people count and current thresholds.
- Save screenshots and annotated frames on request.
- Broadcast state snapshots to the browser over a local WebSocket.

The browser only renders UI and sends control commands to the Python service.

## Failure Handling

- If the ESP32 is offline, the dashboard stays open and shows disconnected state plus reconnect progress.
- If a frame cannot be decoded, it is skipped and the latest good state remains visible.
- If YOLO is still loading, the page shows a loading indicator and disables analysis-dependent controls.
- If screenshot or save fails, the UI shows a visible error message and keeps the stream alive.
- If the browser disconnects, the backend keeps the ESP32 session and analysis running unless the user explicitly stops it.

## Testing

Add focused tests around the Python state model and dashboard actions:

- threshold parsing and validation
- start/stop state transitions
- frame replacement behavior
- warning level computation
- screenshot/save command handling
- reconnect behavior after ESP32 disconnect

Keep the UI testable by separating state updates from rendering logic. The browser shell should only consume a normalized state object and a frame source.

## Acceptance Criteria

- The browser opens a single dashboard page with live video and status.
- The page provides start, stop, threshold, screenshot, and save controls.
- Python remains the only component that connects to the ESP32.
- Real-time warning text updates as the detected people count changes.
- Screenshot and save actions work on the latest annotated frame.
- Existing ESP32 streaming behavior remains unchanged.
