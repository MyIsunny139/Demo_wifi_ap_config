# Lab-Environment-Monitoring

基于 ESP32-S3 摄像头和 Python 上位机的实验室环境监测工程。系统使用 `yolo11n.pt` 检测画面中的 `person` 数量，同时采集 DHT11 温湿度和 MQ-2 烟雾传感器数据。上位机实时显示视频、温湿度曲线和 MQ-2 曲线，并可修改报警阈值；超过阈值时，ESP32 驱动 LED 和蜂鸣器报警，OLED 显示状态。

## 功能

- OV2640 摄像头通过 WebSocket 向上位机发送 JPEG 视频帧。
- 上位机使用 YOLO11n COCO 模型统计 `person` 类目标数量。
- DHT11 和 MQ-2 每 2 秒采样一次，数据通过同一条视频 WebSocket 发送。
- OLED 显示 Wi-Fi 状态、温度、湿度、MQ-2 ADC 值和风险等级。
- 网页仪表盘提供实时视频、温湿度动态折线图、MQ-2 动态折线图、截图和保存功能。
- 仪表盘支持修改人数、温度、湿度和 MQ-2 阈值，修改后立即用于后续报警判定。

## 硬件接线

### ESP32-S3 与摄像头

当前代码使用的 OV2640 引脚如下，定义在 `components/camera_service/camera_service.c`：

| 信号 | GPIO |
| --- | ---: |
| XCLK | 15 |
| SIOD / SCCB SDA | 4 |
| SIOC / SCCB SCL | 5 |
| D0 | 11 |
| D1 | 9 |
| D2 | 8 |
| D3 | 10 |
| D4 | 12 |
| D5 | 18 |
| D6 | 17 |
| D7 | 16 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |

摄像头配置为 SVGA（800x600）、JPEG、质量参数 24、2 个 PSRAM 帧缓冲，XCLK 为 10 MHz。

### 传感器、OLED 和报警输出

| 设备 | 接口 | GPIO/参数 |
| --- | --- | --- |
| DHT11 数据 | 单总线 | GPIO42 |
| MQ-2 AO | ADC 单次采样 | GPIO14，ADC2_CHANNEL_3 |
| SSD1306 OLED SDA | I2C0 | GPIO1 |
| SSD1306 OLED SCL | I2C0 | GPIO2 |
| SSD1306 地址 | I2C | `0x3C` |
| 报警 LED | 数字输出 | GPIO21 |
| 有源蜂鸣器 | 数字输出 | GPIO47 |

接线注意：

- DHT11 数据线需要外接约 4.7K~10K 上拉电阻。
- MQ-2 模块的 AO 必须经过确认或分压，保证输入 ESP32-S3 的电压不超过 3.3 V。
- MQ-2 的读数是 12 位 ADC 原始值，范围为 0~4095，不是经过标定的 ppm 浓度。
- MQ-2 使用 ADC2；当前采用 ESP-IDF `adc_oneshot` 驱动，ESP-IDF 会处理 ADC2 与 Wi-Fi 的访问保护。若更换硬件或驱动，需重新确认 ADC2 与 Wi-Fi 的兼容性。
- LED 和蜂鸣器应使用适合负载的驱动电路；较大功率蜂鸣器不能直接由 GPIO 供电。

## 报警规则

仪表盘默认阈值为：

| 项目 | 默认值 | 判定条件 |
| --- | ---: | --- |
| 最大允许人数 | 3 人 | 人数大于 3 进入中风险；人数大于 4 进入高风险 |
| 最高温度 | 35 °C | 温度大于阈值进入高风险 |
| 最高湿度 | 70 % | 湿度大于阈值进入高风险 |
| MQ-2 ADC | 1800 | ADC 值大于阈值进入高风险 |

风险状态需要连续确认 3 帧后发送到 ESP32；恢复到低风险时立即清除报警。中风险蜂鸣器约 1 Hz，高风险蜂鸣器约 4 Hz。ESP32 超过 5 秒没有收到风险指令时会自动关闭报警输出。

## ESP-IDF 固件

工程要求 ESP-IDF 5.4.x，目标芯片为 ESP32-S3。当前硬件校验要求：240 MHz CPU、16 MB Flash、8 MB PSRAM。

在 ESP-IDF 环境中执行：

```powershell
idf.py set-target esp32s3
idf.py build
```

烧录固件、分区表和网页资源：

```powershell
idf.py -p COMx flash monitor
```

也可以使用构建目录中的烧录参数。由于 `flash_args` 内部使用相对路径，需要先进入构建目录：

```powershell
Push-Location build_replica
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"
Pop-Location
```

构建产物示例：

```text
build_replica\Lab-Environment-Monitoring.bin
build_replica\html.bin
```

首次启动或没有可用的已保存 Wi-Fi 凭据时，ESP32 会进入 AP 配网模式：

```text
SSID: ESP32-xiaoyun
密码: 12345678
网关: 192.168.100.1
```

连接 AP 后打开 `http://192.168.100.1/`，通过配置页面扫描并保存 STA 网络。保存后设备会重启连接已保存的 Wi-Fi；连接成功后日志会输出 `STA got IP`，OLED 也会显示设备 IP。

视频 WebSocket 地址为：

```text
ws://<ESP32_IP>:81/video
```

## Python 上位机

建议使用 Python 3.12 64 位环境。首次安装依赖：

```powershell
py -3.12 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r python_client\requirements.txt
```

启动开发版上位机：

```powershell
python -m python_client.dashboard --esp32-host <ESP32_IP> --device cpu
```

默认访问地址：

```text
http://127.0.0.1:8000/
```

常用参数：

```text
--host 127.0.0.1       本地网页监听地址
--port 8000             本地网页端口
--esp32-host <IP>       ESP32 的 STA 地址，必填
--esp32-port 81         ESP32 WebSocket 端口
--model yolo11n.pt      YOLO 模型路径
--confidence 0.4        YOLO 置信度阈值
--device cpu            推理设备，也可以使用 CUDA 设备编号
--capture-dir captures  截图保存目录
--no-browser            不自动打开浏览器
```

在仪表盘中可以设置：

- 最大允许人数；
- 最高温度；
- 最高湿度；
- MQ-2 ADC 报警值；
- ESP32 地址和端口。

## Windows 打包

项目已经包含 `.venv`、PyInstaller 配置和打包脚本。使用 PowerShell 执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_windows_exe.ps1
```

脚本会检查 64 位 Python、`yolo11n.pt`、PyInstaller、MSVC 运行库，并运行打包程序冒烟测试。文件夹版输出为：

```text
dist\Lab-Environment-Monitoring\Lab-Environment-Monitoring.exe
```

运行打包版：

```powershell
cd dist\Lab-Environment-Monitoring
.\Lab-Environment-Monitoring.exe --esp32-host <ESP32_IP>
```

打包版默认使用 CPU 推理，把模型放在 `_internal\yolo11n.pt`，日志和截图分别写入发布目录下的 `logs`、`captures`。如果需要覆盖默认参数，可在 EXE 后追加命令行参数，例如：

```powershell
.\Lab-Environment-Monitoring.exe --esp32-host 192.168.1.20 --port 8010 --confidence 0.5 --no-browser
```

## 日志和测试

开发版详细日志默认写入：

```text
logs\stream-diagnostics.log
```

打包版启动日志默认写入：

```text
logs\Lab-Environment-Monitoring.log
```

运行 Python 测试：

```powershell
python -m pytest python_client\tests -q
python -m py_compile python_client\risk_policy.py python_client\video_ai.py python_client\dashboard.py python_client\dashboard_app.py python_client\packaged_launcher.py
```

当前工程已验证 Python 测试通过，Windows 打包程序冒烟测试通过。真实传感器读数、摄像头图像和报警输出仍需要在接入对应硬件后现场验证。

## 主要目录

```text
components/camera_service/    OV2640 初始化和采集
components/environment_sensor/ DHT11、MQ-2 采集和遥测
components/oled_display/      SSD1306 OLED 显示
components/status_led/        LED、蜂鸣器和风险输出
components/video_stream/      JPEG/WebSocket 视频和环境数据
python_client/                 FastAPI 仪表盘、YOLO 推理和测试
html/                          ESP32 AP 配网页面
tools/                         Windows 打包和冒烟测试脚本
```
