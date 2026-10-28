"""FastAPI app for the browser dashboard."""

from __future__ import annotations

from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, StrictInt, StrictStr, field_validator

from .dashboard_service import DashboardService
from .dashboard_state import Thresholds


STATIC_DIR = Path(__file__).with_name("static")
FALLBACK_INDEX = "<!doctype html><html><body><h1>Lab-Environment-Monitoring</h1></body></html>"


class ThresholdPayload(BaseModel):
    max_people: StrictInt | None = None
    temperature_max: float = 35.0
    humidity_max: float = 70.0
    mq2_max: StrictInt = 1800
    medium_smoking: StrictInt | None = None
    high_smoking: StrictInt | None = None


class Esp32Payload(BaseModel):
    host: StrictStr
    port: StrictInt = 81

    @field_validator("host")
    @classmethod
    def validate_host(cls, value: str) -> str:
        value = value.strip()
        if (not value or len(value) > 253 or any(char.isspace() for char in value)
                or ":" in value or "/" in value):
            raise ValueError("请输入有效的 ESP32 IP 地址或主机名")
        return value

    @field_validator("port")
    @classmethod
    def validate_port(cls, value: int) -> int:
        if not 1 <= value <= 65535:
            raise ValueError("ESP32 端口必须在 1 到 65535 之间")
        return value


class BrowserDiagnosticPayload(BaseModel):
    event: StrictStr
    frame_sequence: StrictInt
    width: StrictInt
    height: StrictInt
    risk: StrictStr
    frame_url: StrictStr

    @field_validator("event")
    @classmethod
    def validate_event(cls, value: str) -> str:
        if value not in {"load", "error"}:
            raise ValueError("unsupported browser diagnostic event")
        return value

    @field_validator("frame_sequence", "width", "height")
    @classmethod
    def validate_nonnegative(cls, value: int) -> int:
        if value < 0:
            raise ValueError("browser diagnostic values must be non-negative")
        return value


def create_app(
    host: str,
    port: int = 81,
    model: str = "yolo11n.pt",
    confidence: float = 0.4,
    device: str | None = None,
    capture_dir: Path | str = "captures",
) -> FastAPI:
    service = DashboardService(
        host=host,
        port=port,
        model=model,
        confidence=confidence,
        device=device,
        capture_dir=capture_dir,
    )

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        try:
            yield
        finally:
            await service.stop()

    app = FastAPI(title="Lab-Environment-Monitoring", lifespan=lifespan)
    app.state.dashboard_service = service
    if STATIC_DIR.is_dir():
        app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    def state_payload() -> dict[str, object]:
        payload = service.state.snapshot()
        payload.update({"esp32_host": service.host, "esp32_port": service.port})
        return payload

    @app.get("/", response_class=HTMLResponse)
    async def index() -> str:
        dashboard = STATIC_DIR / "dashboard.html"
        if dashboard.is_file():
            return dashboard.read_text(encoding="utf-8")
        return FALLBACK_INDEX

    @app.get("/api/state")
    async def state() -> dict[str, object]:
        return state_payload()

    @app.post("/api/start")
    async def start() -> dict[str, object]:
        await service.start()
        return state_payload()

    @app.post("/api/stop")
    async def stop() -> dict[str, object]:
        await service.stop()
        return state_payload()

    @app.post("/api/esp32")
    async def esp32(payload: Esp32Payload) -> dict[str, object]:
        try:
            await service.update_connection(payload.host, payload.port)
        except (RuntimeError, ValueError) as error:
            raise HTTPException(status_code=409, detail=str(error)) from error
        return state_payload()

    @app.post("/api/thresholds")
    async def thresholds(payload: ThresholdPayload) -> dict[str, object]:
        try:
            if payload.max_people is not None and (
                payload.medium_smoking is not None or payload.high_smoking is not None
            ):
                raise ValueError("provide max_people only")
            if payload.max_people is not None:
                validated = Thresholds(
                    max_people=payload.max_people,
                    temperature_max=payload.temperature_max,
                    humidity_max=payload.humidity_max,
                    mq2_max=payload.mq2_max,
                )
            elif payload.medium_smoking is not None or payload.high_smoking is not None:
                validated = Thresholds(
                    medium_smoking=payload.medium_smoking,
                    high_smoking=payload.high_smoking,
                    temperature_max=payload.temperature_max,
                    humidity_max=payload.humidity_max,
                    mq2_max=payload.mq2_max,
                )
            else:
                raise ValueError("max_people is required")
        except ValueError as error:
            raise HTTPException(status_code=400, detail=str(error)) from error
        if payload.max_people is not None:
            await service.update_thresholds(
                validated.max_people,
                temperature_max=payload.temperature_max,
                humidity_max=payload.humidity_max,
                mq2_max=payload.mq2_max,
            )
        else:
            await service.update_thresholds(
                medium_smoking=validated._legacy_medium,
                high_smoking=validated._legacy_high,
                temperature_max=payload.temperature_max,
                humidity_max=payload.humidity_max,
                mq2_max=payload.mq2_max,
            )
        return state_payload()

    async def save_frame(prefix: str) -> dict[str, str]:
        try:
            result = await service.save_current_frame(prefix)
        except RuntimeError as error:
            raise HTTPException(status_code=409, detail=str(error)) from error
        return {
            "image_path": str(result.image_path),
            "metadata_path": str(result.metadata_path),
        }

    @app.post("/api/screenshot")
    async def screenshot() -> dict[str, str]:
        return await save_frame("screenshot")

    @app.post("/api/save")
    async def save() -> dict[str, str]:
        return await save_frame("save")

    @app.get("/frame/latest.jpg")
    async def latest_frame(f: int = 0) -> Response:
        frame = service.get_preview_frame(f)
        if frame is None:
            raise HTTPException(status_code=404, detail="no frame available")
        service.log_browser_frame_request(frame, requested_sequence=f)
        return Response(content=frame, media_type="image/jpeg")

    @app.post("/api/diagnostics/browser", status_code=204)
    async def browser_diagnostics(payload: BrowserDiagnosticPayload) -> Response:
        service.log_browser_event(
            payload.event,
            payload.frame_sequence,
            payload.width,
            payload.height,
            payload.risk,
            payload.frame_url,
        )
        return Response(status_code=204)

    @app.websocket("/ws/state")
    async def state_socket(websocket: WebSocket) -> None:
        await websocket.accept()
        try:
            if not service.add_browser(websocket):
                await websocket.close(code=1013)
                return
            await service.broadcast()
            while True:
                await websocket.receive_text()
        except WebSocketDisconnect:
            pass
        finally:
            service.remove_browser(websocket)

    return app
