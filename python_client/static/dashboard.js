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
  maxPeopleInput: document.getElementById("maxPeopleInput"),
  thresholdButton: document.getElementById("thresholdButton"),
  screenshotButton: document.getElementById("screenshotButton"),
  saveButton: document.getElementById("saveButton"),
  savePath: document.getElementById("savePath"),
  peopleValue: document.getElementById("peopleValue"),
  maxPeopleValue: document.getElementById("maxPeopleValue"),
  confidenceValue: document.getElementById("confidenceValue"),
  riskValue: document.getElementById("riskValue"),
  rxFpsValue: document.getElementById("rxFpsValue"),
  aiFpsValue: document.getElementById("aiFpsValue"),
  updatedValue: document.getElementById("updatedValue"),
  toastRegion: document.getElementById("toastRegion"),
  dhtChart: document.getElementById("dhtChart"),
  mq2Chart: document.getElementById("mq2Chart"),
  dhtChartValue: document.getElementById("dhtChartValue"),
  mq2ChartValue: document.getElementById("mq2ChartValue"),
  temperatureMaxInput: document.getElementById("temperatureMaxInput"),
  humidityMaxInput: document.getElementById("humidityMaxInput"),
  mq2MaxInput: document.getElementById("mq2MaxInput"),
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
const addressStorageKey = "lab-environment-monitoring.esp32-host";

let stateSocket = null;
let reconnectTimer = null;
let reconnectDelay = 1000;
let frameRevision = 0;
let requestedFrameSequence = 0;
let retriedFrameSequence = 0;
let toastTimer = null;
let latestState = null;
let addressDirty = false;
const environmentHistory = [];
const MAX_HISTORY = 60;
let lastEnvironmentSample = null;

function drawChart(canvas, series, maximum, colors) {
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const width = Math.max(240, Math.floor(rect.width || canvas.width));
  const height = Math.max(120, Math.floor(rect.height || canvas.height));
  canvas.width = width * dpr;
  canvas.height = height * dpr;
  const context = canvas.getContext("2d");
  context.setTransform(dpr, 0, 0, dpr, 0, 0);
  context.clearRect(0, 0, width, height);
  context.strokeStyle = "#30363d";
  context.lineWidth = 1;
  for (let i = 1; i < 4; i += 1) {
    const y = 10 + ((height - 24) * i) / 4;
    context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke();
  }
  series.forEach((values, index) => {
    if (values.length < 1) return;
    context.strokeStyle = colors[index];
    context.lineWidth = 2;
    context.beginPath();
    let activeSegment = false;
    values.forEach((value, pointIndex) => {
      if (!Number.isFinite(value)) { activeSegment = false; return; }
      const x = values.length === 1 ? width - 4 : (width - 8) * pointIndex / (MAX_HISTORY - 1) + 4;
      const seriesMaximum = Array.isArray(maximum) ? maximum[index] : maximum;
      const y = height - 14 - Math.max(0, Math.min(1, value / seriesMaximum)) * (height - 28);
      if (activeSegment) context.lineTo(x, y); else context.moveTo(x, y);
      activeSegment = true;
    });
    context.stroke();
  });
}

function redrawEnvironmentCharts() {
  drawChart(elements.dhtChart,
    [environmentHistory.map((point) => point.temperature), environmentHistory.map((point) => point.humidity)],
    [50, 100], ["#45a37a", "#78b7e5"]);
  drawChart(elements.mq2Chart,
    [environmentHistory.map((point) => point.mq2)], 4095, ["#d6a13d"]);
}

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
    requestedFrameSequence = 0;
    retriedFrameSequence = 0;
    return;
  }

  const frameSequence = Number(latestState?.frame_sequence) || 0;
  if (frameSequence <= 0 || frameSequence === requestedFrameSequence) return;

  if (frameSequence !== retriedFrameSequence) retriedFrameSequence = 0;
  requestedFrameSequence = frameSequence;
  frameRevision += 1;
  const frameUrl = new URL("/frame/latest.jpg", location.origin);
  frameUrl.searchParams.set("v", `${Date.now()}-${frameRevision}`);
  frameUrl.searchParams.set("f", String(frameSequence));
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
  elements.maxPeopleValue.textContent = String(Number(state.max_people) || 0);
  elements.confidenceValue.textContent = `${Math.round(Number(state.smoking_confidence || 0) * 100)}%`;
  if (Number.isFinite(Number(state.mq2_adc)) && state.environment_sampled_at_ms !== null
      && state.environment_sampled_at_ms !== lastEnvironmentSample) {
    lastEnvironmentSample = state.environment_sampled_at_ms;
    const dhtValid = Boolean(state.dht_valid) && Number.isFinite(Number(state.temperature_c)) && Number.isFinite(Number(state.humidity_pct));
    environmentHistory.push({ temperature: dhtValid ? Number(state.temperature_c) : NaN, humidity: dhtValid ? Number(state.humidity_pct) : NaN, mq2: Number(state.mq2_adc) });
    if (environmentHistory.length > MAX_HISTORY) environmentHistory.shift();
    elements.dhtChartValue.textContent = dhtValid
      ? `T ${Number(state.temperature_c).toFixed(1)}°C / H ${Number(state.humidity_pct).toFixed(1)}%`
      : "DHT11 错误";
    elements.mq2ChartValue.textContent = `ADC ${Math.round(Number(state.mq2_adc))}`;
    redrawEnvironmentCharts();
  }
  elements.riskValue.textContent = riskText[risk];
  elements.rxFpsValue.textContent = Number(state.receive_fps || 0).toFixed(1);
  elements.aiFpsValue.textContent = Number(state.inference_fps || 0).toFixed(1);
  elements.updatedValue.textContent = formatTime(state.updated_at);

  if (Number.isInteger(state.max_people)) elements.maxPeopleInput.value = state.max_people;
  if (Number.isFinite(Number(state.temperature_max))) elements.temperatureMaxInput.value = state.temperature_max;
  if (Number.isFinite(Number(state.humidity_max))) elements.humidityMaxInput.value = state.humidity_max;
  if (Number.isInteger(state.mq2_max)) elements.mq2MaxInput.value = state.mq2_max;
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
  const max = Number(elements.maxPeopleInput.value);
  if (!Number.isInteger(max) || max < 0) {
    throw new Error("最大允许人数必须是大于或等于 0 的整数");
  }
  const temperature = Number(elements.temperatureMaxInput.value);
  const humidity = Number(elements.humidityMaxInput.value);
  const mq2 = Number(elements.mq2MaxInput.value);
  if (!Number.isFinite(temperature) || temperature < 0 || temperature > 125
      || !Number.isFinite(humidity) || humidity < 0 || humidity > 100
      || !Number.isInteger(mq2) || mq2 < 0 || mq2 > 4095) {
    throw new Error("环境阈值范围无效");
  }
  return { max_people: max, temperature_max: temperature, humidity_max: humidity, mq2_max: mq2 };
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
  const failedSequence = requestedFrameSequence;
  if (failedSequence > 0 && retriedFrameSequence !== failedSequence) {
    retriedFrameSequence = failedSequence;
    requestedFrameSequence = 0;
    window.setTimeout(() => {
      if (Number(latestState?.frame_sequence) === failedSequence) updateFrame(true);
    }, 250);
  }
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
window.addEventListener("resize", redrawEnvironmentCharts);
redrawEnvironmentCharts();
