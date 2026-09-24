"use strict";

const elements = {
  esp32Address: document.getElementById("esp32Address"),
  applyAddressButton: document.getElementById("applyAddressButton"),
  connectionStatus: document.getElementById("connectionStatus"),
  processingStatus: document.getElementById("processingStatus"),
  frameStatus: document.getElementById("frameStatus"),
  videoFrame: document.getElementById("videoFrame"),
  emptyState: document.getElementById("emptyState"),
  warningPanel: document.getElementById("warningPanel"),
  warningMessage: document.getElementById("warningMessage"),
  riskLabel: document.getElementById("riskLabel"),
  startButton: document.getElementById("startButton"),
  stopButton: document.getElementById("stopButton"),
  mediumInput: document.getElementById("mediumInput"),
  highInput: document.getElementById("highInput"),
  thresholdButton: document.getElementById("thresholdButton"),
  screenshotButton: document.getElementById("screenshotButton"),
  saveButton: document.getElementById("saveButton"),
  savePath: document.getElementById("savePath"),
  peopleValue: document.getElementById("peopleValue"),
  smokingValue: document.getElementById("smokingValue"),
  confidenceValue: document.getElementById("confidenceValue"),
  riskValue: document.getElementById("riskValue"),
  rxFpsValue: document.getElementById("rxFpsValue"),
  aiFpsValue: document.getElementById("aiFpsValue"),
  updatedValue: document.getElementById("updatedValue"),
  toastRegion: document.getElementById("toastRegion"),
};

const riskText = { low: "低", medium: "中", high: "高" };
const processingText = {
  idle: "空闲",
  starting: "启动中",
  connecting: "连接中",
  receiving: "接收中",
  inference: "推理中",
  running: "运行中",
  stopping: "停止中",
  error: "异常",
};
const addressStorageKey = "video-ai.esp32-host";

let stateSocket = null;
let reconnectTimer = null;
let reconnectDelay = 1000;
let frameRevision = 0;
let toastTimer = null;
let latestState = null;
let addressDirty = false;

try {
  const savedAddress = window.localStorage.getItem(addressStorageKey);
  if (savedAddress) {
    elements.esp32Address.value = savedAddress;
    addressDirty = true;
  }
} catch (error) {
  // Storage can be unavailable in private browsing; editing still works.
}

function showToast(message, type = "info") {
  window.clearTimeout(toastTimer);
  elements.toastRegion.replaceChildren();
  const toast = document.createElement("div");
  toast.className = `toast${type === "error" ? " toast-error" : ""}`;
  toast.textContent = message;
  elements.toastRegion.appendChild(toast);
  toastTimer = window.setTimeout(() => toast.remove(), 4200);
}

function formatError(error) {
  const message = error instanceof Error ? error.message : String(error || "未知错误");
  const translations = {
    "no frame available": "当前没有可保存的视频帧",
    "Failed to fetch": "无法连接本地仪表盘服务",
    "Load failed": "无法连接本地仪表盘服务",
  };
  return translations[message] || `操作失败：${message}`;
}

function formatTime(timestamp) {
  if (!Number.isFinite(Number(timestamp)) || Number(timestamp) <= 0) return "--:--:--";
  return new Date(Number(timestamp) * 1000).toLocaleTimeString("zh-CN", {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function updateFrame(hasFrame) {
  if (!hasFrame) {
    elements.videoFrame.removeAttribute("src");
    elements.videoFrame.style.display = "none";
    elements.emptyState.style.display = "grid";
    elements.frameStatus.textContent = "等待视频帧";
    return;
  }

  frameRevision += 1;
  const frameUrl = new URL("/frame/latest.jpg", location.origin);
  frameUrl.searchParams.set("v", `${Date.now()}-${frameRevision}`);
  frameUrl.searchParams.set("f", String(Number(latestState?.frame_sequence) || 0));
  elements.videoFrame.src = frameUrl.toString();
  elements.frameStatus.textContent = "正在刷新";
}

function reportFrameEvent(event) {
  const source = elements.videoFrame.currentSrc || elements.videoFrame.src || "";
  let frameSequence = Number(latestState?.frame_sequence) || 0;
  try {
    frameSequence = Number(new URL(source, location.origin).searchParams.get("f")) || frameSequence;
  } catch (error) {
    // Keep the sequence from the latest state when the browser reports an invalid URL.
  }
  fetch("/api/diagnostics/browser", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      event,
      frame_sequence: frameSequence,
      width: elements.videoFrame.naturalWidth || 0,
      height: elements.videoFrame.naturalHeight || 0,
      risk: latestState?.risk || "low",
      frame_url: source,
    }),
    keepalive: true,
  }).catch(() => {});
}

function renderState(state) {
  latestState = state;
  const risk = Object.hasOwn(riskText, state.risk) ? state.risk : "low";
  const connected = Boolean(state.connected);

  if (state.esp32_host && !addressDirty && document.activeElement !== elements.esp32Address) {
    elements.esp32Address.value = state.esp32_host;
  }

  elements.connectionStatus.className = `status-value ${connected ? "status-on" : "status-off"}`;
  elements.connectionStatus.innerHTML = `<span class="status-dot" aria-hidden="true"></span>${connected ? "已连接" : "未连接"}`;
  elements.processingStatus.textContent = processingText[state.processing] || state.processing || "空闲";
  elements.warningPanel.className = `warning-panel risk-${risk}`;
  elements.warningMessage.textContent = state.last_error
    ? `处理异常：${state.last_error}`
    : state.warning || "等待状态更新";
  elements.riskLabel.textContent = `${riskText[risk]}风险`;
  elements.peopleValue.textContent = String(Number(state.people) || 0);
  elements.smokingValue.textContent = String(Number(state.smoking) || 0);
  elements.confidenceValue.textContent = `${Math.round(Number(state.smoking_confidence || 0) * 100)}%`;
  elements.riskValue.textContent = riskText[risk];
  elements.rxFpsValue.textContent = Number(state.receive_fps || 0).toFixed(1);
  elements.aiFpsValue.textContent = Number(state.inference_fps || 0).toFixed(1);
  elements.updatedValue.textContent = formatTime(state.updated_at);

  if (Number.isInteger(state.medium_smoking)) elements.mediumInput.value = state.medium_smoking;
  if (Number.isInteger(state.high_smoking)) elements.highInput.value = state.high_smoking;
  if (state.last_saved) elements.savePath.textContent = state.last_saved;

  elements.startButton.disabled = Boolean(state.running);
  elements.stopButton.disabled = !state.running;
  elements.esp32Address.disabled = Boolean(state.running);
  elements.applyAddressButton.disabled = Boolean(state.running);
  updateFrame(Boolean(state.has_frame));
}

async function readResponse(response) {
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    const detail = Array.isArray(payload.detail)
      ? payload.detail.map((item) => item.msg).filter(Boolean).join("；")
      : payload.detail;
    throw new Error(detail || `本地服务返回 HTTP ${response.status}`);
  }
  return payload;
}

async function postJson(url, body) {
  const options = { method: "POST", headers: { Accept: "application/json" } };
  if (body !== undefined) {
    options.headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(body);
  }
  return readResponse(await fetch(url, options));
}

async function runAction(button, action, successMessage) {
  button.disabled = true;
  button.setAttribute("aria-busy", "true");
  try {
    const result = await action();
    if (result && Object.hasOwn(result, "running")) renderState(result);
    if (result && result.image_path) elements.savePath.textContent = result.image_path;
    if (successMessage) showToast(successMessage);
  } catch (error) {
    showToast(formatError(error), "error");
  } finally {
    button.disabled = false;
    button.removeAttribute("aria-busy");
    if (latestState) renderState(latestState);
  }
}

function readEsp32Address() {
  const host = elements.esp32Address.value.trim();
  if (!host || /\s/.test(host) || host.includes(":") || host.includes("/")) {
    throw new Error("请输入有效的 ESP32 IP 地址或主机名");
  }
  return host;
}

async function applyEsp32Address() {
  const host = readEsp32Address();
  const result = await postJson("/api/esp32", { host, port: 81 });
  addressDirty = false;
  try {
    window.localStorage.setItem(addressStorageKey, host);
  } catch (error) {
    // The server-side address remains applied when storage is unavailable.
  }
  return result;
}

function validateThresholds() {
  const medium = Number(elements.mediumInput.value);
  const high = Number(elements.highInput.value);
  if (!Number.isInteger(medium) || medium < 1) {
    throw new Error("中风险抽烟人数必须是大于或等于 1 的整数");
  }
  if (!Number.isInteger(high) || high <= medium) {
    throw new Error("高风险抽烟人数必须是大于中风险抽烟人数的整数");
  }
  return { medium_smoking: medium, high_smoking: high };
}

function scheduleReconnect() {
  if (reconnectTimer !== null) return;
  elements.frameStatus.textContent = `状态通道断开，${Math.ceil(reconnectDelay / 1000)} 秒后重连`;
  reconnectTimer = window.setTimeout(() => {
    reconnectTimer = null;
    connectStateSocket();
  }, reconnectDelay);
  reconnectDelay = Math.min(reconnectDelay * 2, 15000);
}

function connectStateSocket() {
  if (stateSocket && stateSocket.readyState < WebSocket.CLOSING) return;
  const socketProtocol = location.protocol === "https:" ? "wss:" : "ws:";
  stateSocket = new WebSocket(`${socketProtocol}//${location.host}/ws/state`);

  stateSocket.addEventListener("open", () => {
    reconnectDelay = 1000;
    elements.frameStatus.textContent = "状态通道已连接";
  });
  stateSocket.addEventListener("message", (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (payload.type === "state" && payload.state) renderState(payload.state);
    } catch (error) {
      showToast("收到无法解析的状态数据", "error");
    }
  });
  stateSocket.addEventListener("close", scheduleReconnect);
  stateSocket.addEventListener("error", () => stateSocket.close());
}

elements.videoFrame.addEventListener("load", () => {
  elements.videoFrame.style.display = "block";
  elements.emptyState.style.display = "none";
  elements.frameStatus.textContent = "视频帧已更新";
  reportFrameEvent("load");
});

elements.videoFrame.addEventListener("error", () => {
  elements.videoFrame.style.display = "none";
  elements.emptyState.style.display = "grid";
  elements.frameStatus.textContent = "视频帧加载失败";
  showToast("视频帧加载失败，将在下次状态更新时重试", "error");
  reportFrameEvent("error");
});

elements.esp32Address.addEventListener("input", () => {
  addressDirty = true;
});

elements.startButton.addEventListener("click", () => {
  runAction(elements.startButton, async () => {
    await applyEsp32Address();
    return postJson("/api/start");
  }, "处理已启动");
});

elements.stopButton.addEventListener("click", () => {
  runAction(elements.stopButton, () => postJson("/api/stop"), "处理已停止");
});

elements.applyAddressButton.addEventListener("click", () => {
  runAction(elements.applyAddressButton, applyEsp32Address, "ESP32 地址已应用");
});

elements.thresholdButton.addEventListener("click", () => {
  runAction(elements.thresholdButton, async () => {
    const thresholds = validateThresholds();
    return postJson("/api/thresholds", thresholds);
  }, "阈值已应用");
});

elements.screenshotButton.addEventListener("click", () => {
  runAction(elements.screenshotButton, () => postJson("/api/screenshot"), "截图已保存");
});

elements.saveButton.addEventListener("click", () => {
  runAction(elements.saveButton, () => postJson("/api/save"), "当前帧已保存");
});

fetch("/api/state")
  .then(readResponse)
  .then(renderState)
  .catch((error) => showToast(formatError(error), "error"));

connectStateSocket();
