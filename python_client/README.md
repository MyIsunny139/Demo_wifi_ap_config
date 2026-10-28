# Python 上位机

`python_client` 是 Lab-Environment-Monitoring 的 Windows/Python 上位机，负责连接 ESP32-S3、接收 JPEG 和环境遥测、运行 YOLO11n 人数检测，并提供浏览器仪表盘。

## 安装

建议使用 Python 3.12 64 位：

```powershell
py -3.12 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r python_client\requirements.txt
```

模型文件 `yolo11n.pt` 位于工程根目录，默认使用 YOLO11n COCO `person` 类统计人数。

## 启动

在工程根目录执行：

```powershell
python -m python_client.dashboard --esp32-host <ESP32_IP> --device cpu
```

浏览器访问：

```text
http://127.0.0.1:8000/
```

主要参数：

```text
--host 127.0.0.1       本地网页监听地址
--port 8000             本地网页端口
--esp32-host <IP>       ESP32 地址，必填
--esp32-port 81         ESP32 WebSocket 端口
--model yolo11n.pt      YOLO 模型路径
--confidence 0.4        YOLO 置信度阈值
--device cpu            推理设备，可使用 CUDA 设备编号
--capture-dir captures  截图保存目录
--diagnostic-log ...    详细流诊断日志路径
--no-browser            不自动打开浏览器
```

## 仪表盘功能

- 实时显示 ESP32 摄像头画面和人数检测结果；
- 显示 DHT11 温度、湿度动态曲线；
- 显示 MQ-2 ADC 动态曲线；
- 修改最大允许人数、温度、湿度和 MQ-2 阈值；
- 修改 ESP32 地址和 WebSocket 端口；
- 截图并保存 JSON 元数据；
- 通过 WebSocket 向 ESP32 发送风险等级和报警指令。

默认阈值：最大人数 3、温度 35 °C、湿度 70%、MQ-2 ADC 1800。人数大于最大允许人数进入中风险；人数再多 1 人进入高风险。环境传感器任一数值超过阈值时进入高风险。风险状态连续确认 3 帧后发送，恢复低风险时清除报警。

## Windows 打包

依赖安装和打包依赖：

```powershell
python -m pip install -r python_client\requirements-build.txt
```

从工程根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_windows_exe.ps1
```

输出目录：

```text
dist\Lab-Environment-Monitoring\
```

启动打包版：

```powershell
cd dist\Lab-Environment-Monitoring
.\Lab-Environment-Monitoring.exe --esp32-host <ESP32_IP>
```

打包脚本会把 `yolo11n.pt` 和网页静态资源一并收集，并执行 HTTP、模型加载、日志文件检查。打包版默认使用 CPU，运行日志位于 `logs\Lab-Environment-Monitoring.log`，详细流日志位于 `logs\stream-diagnostics.log`。

## 测试

```powershell
python -m pytest python_client\tests -q
python -m py_compile python_client\risk_policy.py python_client\video_ai.py python_client\dashboard.py python_client\dashboard_app.py python_client\packaged_launcher.py
```

`tools\smoke_test_windows_exe.py` 用于验证打包 EXE 能启动网页、加载模型、提供静态资源并创建日志文件：

```powershell
python tools\smoke_test_windows_exe.py dist\Lab-Environment-Monitoring\Lab-Environment-Monitoring.exe
```
