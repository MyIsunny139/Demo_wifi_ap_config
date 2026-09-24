# Python 仪表盘

本目录提供与 `video-ai` ESP32 固件配套的本地浏览器仪表盘。

浏览器只连接本机 Python 服务。Python 服务连接 ESP32 视频 WebSocket，接收 JPEG 帧，执行 YOLO 抽烟行为识别，更新仪表盘，并将风险指令发回 ESP32。

## 安装

在工程根目录执行：

```powershell
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r python_client\requirements.txt
```

默认模型路径为 `smoking.onnx`，工程根目录已提供可直接运行的权重，来源为 [Smoking-detection-yolo11](https://github.com/alihassanml/Smoking-detection-yolo11)。模型固定批量大小为 1，当前程序按视频帧逐张推理，兼容该限制。模型类别名为作者拼写的 `Smooking`，程序已兼容。替换模型时应包含 `smoking`、`smooking`、`smoke`、`smoker`、`cigarette`、`person smoking`、`smoking person`、`抽烟` 或 `吸烟` 类别之一；可选的 `person` 类用于人数统计。通用 COCO `yolo11n.pt` 不包含抽烟类别，会被程序拒绝。需要使用 NVIDIA GPU 时，请先安装与本机 CUDA 匹配的 PyTorch，再安装其余依赖。

## 启动

从 ESP32 串口日志的 `STA got IP` 行读取 DHCP 地址，再启动上位机：

```powershell
python -m python_client.dashboard --esp32-host <ESP32 当前 IP> --device cpu
```

详细视频诊断日志保存在 `logs\stream-diagnostics.log`。复现花屏后保留问题前后约 10 秒的 `RX`、`AI`、`TX/ACK`、`HTTP`、`BROWSER` 日志，即可区分 ESP32 JPEG、模型推理、风险指令和浏览器显示问题。

本地服务就绪后会自动使用默认浏览器打开仪表盘。如需禁止自动打开，增加 `--no-browser` 参数，然后手动访问：

```text
http://127.0.0.1:8000
```

常用参数：

```text
--host 127.0.0.1          本地服务监听地址
--port 8000               本地服务端口
--esp32-host <地址>       ESP32 当前 DHCP 地址
--esp32-port 81           ESP32 视频 WebSocket 端口
--model smoking.onnx      抽烟检测 YOLO 模型路径
--confidence 0.4          检测置信度
--device cpu              强制使用 CPU 推理
--capture-dir captures    截图保存目录
--no-browser              不自动打开浏览器
```

## Windows 绿色目录版

从工程根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_windows_exe.ps1
```

脚本会自动检查 x64、MSVC 运行库、前端、API 和 YOLO 模型加载。构建结果位于 `dist\video-ai\`。交付整个文件夹，客户双击
`video-ai.exe` 即可启动。程序会显示命令行窗口，实时输出日志；按 `Ctrl+C` 可结束进程，日志同时保存在
`logs\video-ai.log`。

## 旧版命令行客户端

仍可使用旧版 OpenCV 预览客户端：

```powershell
python -m python_client.video_ai --host <ESP32 当前 IP> --device cpu
```

日常使用建议选择浏览器仪表盘。

## 测试

```powershell
python -m pytest python_client\tests -q
python -m py_compile python_client\risk_policy.py python_client\video_ai.py python_client\dashboard.py
```
