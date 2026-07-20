# video-ai Python 客户端

该程序连接 ESP32 的 `ws://<设备IP>:81/video`，只保留最新 JPEG 帧，使用
Ultralytics YOLO 检测人员，并将风险等级发回 ESP32 控制板载 WS2812。

## 安装

在工程 `hello_world` 目录执行：

```powershell
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r python_client\requirements.txt
```

默认模型为 `yolo11n.pt`，首次运行时 Ultralytics 会自动下载模型。使用 NVIDIA
GPU 时，应先按照 PyTorch 官方说明安装与本机 CUDA 匹配的 PyTorch，再安装其余依赖。

## 运行

从 ESP32 串口日志获取局域网 IP，然后执行：

```powershell
python -m python_client.video_ai --host 192.168.1.100
```

常用参数：

```text
--model yolo11n.pt     YOLO 模型文件
--confidence 0.4       检测置信度阈值
--device 0             使用第一块 CUDA GPU
--device cpu           强制使用 CPU
--reconnect-delay 2    断线重连间隔（秒）
```

预览窗口显示人员数量、风险等级、接收帧率和推理帧率。按 `q` 退出，也可在终端按
`Ctrl+C`。客户端断线后会自动重连。

风险规则：0-2 人为低风险，3-5 人为中风险，6 人及以上为高风险。新等级连续出现
三帧后才生效；状态变化时立即发送，并每秒刷新一次。ESP32 五秒未收到有效指令时
会自动熄灭风险灯。

## 测试

```powershell
python -m pytest python_client\tests -q
python -m py_compile python_client\risk_policy.py python_client\video_ai.py
```
