# video-ai

本工程由 ESP32-S3 摄像头固件和本地 Python 服务组成。ESP32 连接 Wi-Fi，通过 WebSocket 发送 OV2640 JPEG 视频帧，接收风险指令并控制 WS2812 状态灯和无源蜂鸣器。Python 服务负责执行 YOLO 抽烟行为识别、统计画面人数，并提供风险可视化浏览器仪表盘。

## 当前硬件配置

- 主控：ESP32-S3，8 MB PSRAM
- 摄像头：OV2640，DVP 接口
- 状态灯：GPIO48 上的 WS2812
- 蜂鸣器：GPIO14 上的三线 MH-FMD 无源蜂鸣器，5V供电
- Wi-Fi STA：由路由器或手机热点通过 DHCP 分配地址
- Wi-Fi 凭据：NVS 永久保存最近配置的 3 个不同 SSID，按最近配置优先自动连接
- 视频 WebSocket：`ws://<DHCP 分配的 IP>:81/video`
- 配网页面：ESP32 AP 模式下提供 `/spiffs/apcfg.html`

摄像头引脚配置位于 `components/camera_service/camera_service.c`：

```c
PWDN -1 (board tied low)
RESET -1 (software reset)
XCLK 15
SIOD 4
SIOC 5
D0   11
D1   9
D2   8
D3   10
D4   12
D5   18
D6   17
D7   16
VSYNC 6
HREF  7
PCLK  13
```

蜂鸣器接线：

```text
MH-FMD VCC   -> 5V
MH-FMD GND   -> ESP32-S3 GND
MH-FMD IO/IN -> GPIO14
```

ESP32-S3 与蜂鸣器必须共地。GPIO14 只输出 0V/3.3V 的 2 kHz PWM，不得接入5V；如果模块会将输入端内部上拉到5V，必须增加三极管、MOS 管或电平转换后再连接 GPIO14。

当前稳定的摄像头参数：

```c
CAMERA_XCLK_HZ           10000000
CAMERA_INIT_FRAME_SIZE   FRAMESIZE_HD (1280x720)
CAMERA_JPEG_QUALITY      20
fb_count                 1
fb_location              CAMERA_FB_IN_PSRAM
grab_mode                CAMERA_GRAB_WHEN_EMPTY
CONFIG_CAMERA_PSRAM_DMA  disabled
JPEG frame buffer        524288 bytes (custom)
VIDEO_FRAME_PERIOD_MS    250 (目标约 4 FPS)
```

## ESP32 固件

在工程根目录使用 ESP-IDF 编译、烧录并打开串口监视器：

```powershell
idf.py build
idf.py flash monitor
```

正常启动后应出现以下关键日志：

```text
status_led: WS2812 ready on GPIO48; passive buzzer ready on GPIO14 at 2000 Hz
wifi_manager: STA got IP: 192.168.166.200, netmask: 255.255.255.0, gateway: 192.168.166.194
camera_service: OV2640 ready: HD (1280x720) JPEG, xclk=10000000 Hz, quality=20, frame_buffers=1, buffer=PSRAM
video_stream: Video WebSocket ready at ws://<device-ip>:81/video
```

串口日志中的 `STA got IP` 是 ESP32 当前地址。手机热点重启后地址可能变化，
此时在上位机停止处理，修改“ESP32 地址”并重新启动处理即可。

设备上电后依次尝试 NVS 中保存的最多 3 组 Wi-Fi。每组使用现有重试策略仍无法连接时切换到下一组；三组全部失败或尚未保存任何凭据时，才启动 `ESP32-xiaoyun` AP 配网。重复配置同一 SSID 会更新其密码并移到第 1 位，不会占用多个槽位；第 4 个不同 SSID 会淘汰最早配置的一组。旧版固件保存的单组 `ssid/password` 可继续读取。凭据在普通固件升级和断电后保留，执行整片擦除（如 `idf.py erase-flash`）会清除。

STA 支持 WPA2/WPA3 手机热点并启用 PMF 协商。连接失败日志会打印 `reason`、原因名称和 RSSI；例如 `ASSOC_FAIL` 表示热点在关联阶段拒绝设备，`4WAY_HANDSHAKE_TIMEOUT`、`AUTH_FAIL` 或 `HANDSHAKE_TIMEOUT` 才通常与密码或安全模式有关。

## Python 仪表盘

创建 Python 环境：

```powershell
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r python_client\requirements.txt
```

工程根目录已包含可直接运行的 `smoking.onnx` 抽烟检测权重，来源为 [Smoking-detection-yolo11](https://github.com/alihassanml/Smoking-detection-yolo11)。该模型固定批量大小为 1，适合本项目逐帧推理。替换模型时至少应包含以下任一类别名：

```text
smoking, smooking, smoke, smoker, cigarette, person smoking, smoking person, 抽烟, 吸烟
```

模型可同时包含 `person` 类用于人数统计。通用 COCO `yolo11n.pt` 没有抽烟类别，程序会拒绝加载，不能用于本项目的抽烟识别。

启动浏览器仪表盘：

```powershell
python -m python_client.dashboard --esp32-host <ESP32 当前 IP> --device cpu
```

上位机会将视频链路诊断写入 `logs\stream-diagnostics.log`，并自动轮转保留最近 3 个历史文件。日志中的 `RX`、`AI`、`TX/ACK`、`HTTP` 和 `BROWSER` 记录可通过帧序号及 SHA-256 前缀对应，用于定位 JPEG 损坏、推理阻塞、风险指令阻塞或浏览器解码失败。

本地服务就绪后会自动使用默认浏览器打开仪表盘。如需禁止自动打开，增加 `--no-browser` 参数，然后手动访问：

```text
http://127.0.0.1:8000
```

常用参数：

```text
--host 127.0.0.1          本地服务监听地址
--port 8000               本地服务端口
--esp32-host <地址>       ESP32 当前 DHCP 地址（也可在仪表盘内修改）
--esp32-port 81           ESP32 视频 WebSocket 端口
--model smoking.onnx      抽烟检测 YOLO 模型路径
--confidence 0.4          检测置信度
--device cpu              强制使用 CPU 推理
--capture-dir captures    截图保存目录
--no-browser              不自动打开浏览器
```

## Windows 绿色目录版

在项目根目录执行打包脚本：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_windows_exe.ps1
```

脚本会安装构建专用的 PyInstaller，检查 x64 和 MSVC 运行库，并自动验证前端、API 与 YOLO 模型加载。全部通过后生成：

```text
dist\video-ai\video-ai.exe
```

交付时复制整个 `dist\video-ai` 文件夹。客户无需安装 Python，双击
`video-ai.exe` 后程序会打开控制台并自动启动浏览器。控制台实时显示运行日志，按
`Ctrl+C` 可结束程序；日志同时写入 `logs\video-ai.log`，截图写入 `captures\`。

## 风险告警

Python 仪表盘根据抽烟人数生成风险等级（默认 `0/1/2` 人对应低/中/高风险）。升级到中/高风险需要连续 3 帧确认；当前帧未检测到抽烟时立即恢复低风险并解除 ESP32 告警。浏览器仅显示风险颜色和文字，不播放告警声音。

ESP32 声光报警行为：

```text
low    WS2812 熄灭，蜂鸣器静音
medium WS2812 琥珀色慢闪，蜂鸣器以 2 kHz 响500ms、停500ms
high   WS2812 红色快闪，蜂鸣器以 2 kHz 响150ms、停150ms
```

启动时蜂鸣器发声100ms用于自检。如果连续5秒未收到有效风险指令，固件会同时关闭状态灯和蜂鸣器。终端停止、网络断开或 ESP32 WebSocket 断开时，终端和 ESP32 两端都会解除告警。

## 工程结构

```text
components/
  camera_service/   OV2640 初始化和视频帧采集
  video_stream/     WebSocket 视频流和风险指令处理
  status_led/       WS2812 与无源蜂鸣器声光报警
  wifi_manager/     STA/AP、DHCP 和 Wi-Fi 凭据管理
  ap_wifi/          AP 配网页面服务
  ws_server/        配网功能使用的 WebSocket 服务
  button/           配网依赖使用的按键组件
html/               写入 SPIFFS 的 AP 配网页面
main/               ESP-IDF 程序入口
python_client/      本地仪表盘、YOLO 客户端、测试和前端资源
tools/              本地检查与打包脚本
```

`managed_components/` 保存 ESP-IDF Component Manager 下载的依赖，不应作为无用文件删除。

## 测试

Python 测试：

```powershell
python -m pytest python_client\tests -q
python -m py_compile python_client\risk_policy.py python_client\video_ai.py python_client\dashboard.py python_client\packaged_launcher.py
```

蜂鸣器源码契约检查：

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_buzzer_config.ps1
```

固件编译检查：

```powershell
idf.py build
```

## 生成文件

编译输出、虚拟环境、YOLO 模型权重、截图、日志、打包缓存和临时工作树均已在 `.gitignore` 中忽略。
