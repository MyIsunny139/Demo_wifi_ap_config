# Dashboard Auto-Open Design

## Goal

When the dashboard Python process starts, automatically open the local dashboard in the user's default browser. Keep a command-line option for environments where automatic browser launch is unwanted.

## Behavior

- Browser auto-open is enabled by default.
- The target URL is derived from the dashboard bind host and port.
- When the service binds to `0.0.0.0` or `::`, the browser uses `127.0.0.1` because wildcard addresses are not useful navigation targets.
- The launcher waits until the local HTTP service accepts a connection before opening the URL.
- `--no-browser` disables browser launch without changing service behavior.
- A browser launch or readiness-check failure must not stop Uvicorn.

## Structure

`python_client.dashboard` remains the executable entry point. A small background launcher owns readiness polling and the `webbrowser.open()` call. `main()` starts that launcher immediately before entering `uvicorn.run()`, so the HTTP server remains the main blocking process.

No changes are required in the FastAPI application, browser assets, model inference, or ESP32 protocol.

## Testing

Entry-point tests will verify URL construction, wildcard-host normalization, the default auto-open behavior, and `--no-browser`. Browser and network calls will be replaced at the module boundary so tests do not open a real browser or bind a real port.

## Documentation

The root README and `python_client/README.md` will state that the dashboard opens automatically and document `--no-browser`.
