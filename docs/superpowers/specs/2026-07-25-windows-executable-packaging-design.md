# Windows 可执行程序打包设计

## 目标

将 Python 仪表盘制作成 Windows x64 绿色目录版。客户拿到文件夹后，直接双击 `video-ai.exe` 即可启动，无需安装 Python。

## 交付形式

- 使用 PyInstaller 的单目录模式打包。
- 输出目录为 `dist/video-ai/`。
- 使用无控制台模式，启动时不显示命令行窗口。
- 将前端静态资源和 `yolo11n.pt` 一起打包。
- 交付目录包含 PyTorch、Ultralytics、OpenCV、FastAPI、Uvicorn 及其运行依赖。
- 测试代码、构建缓存、截图和开发用虚拟环境不进入交付目录。

本项目不采用单文件模式。机器学习运行库体积较大，单文件程序每次启动都要先解压，启动时间更长，也更容易触发杀毒软件的启发式检查。单目录模式仍然只需要客户双击一个 EXE，运行更稳定。

## 启动参数

打包入口使用以下默认值：

- ESP32 地址：`192.168.1.3`
- ESP32 WebSocket 端口：`81`
- 推理设备：`cpu`
- 模型：打包内置的 `yolo11n.pt`
- 仪表盘监听地址：`127.0.0.1`
- 仪表盘端口：`8000`

Uvicorn 成功监听端口后，程序自动打开默认浏览器。命令行参数仍然保留，售后调试时可以使用 `--no-browser` 等选项。

## 文件路径

模型和前端文件属于只读资源，从 PyInstaller 的资源目录读取。日志和截图写在 EXE 所在目录：

- 日志：`logs/video-ai.log`
- 截图：`captures/`

这样不会向 PyInstaller 内部资源目录写文件，客户也可以直接找到运行日志和截图。

## 日志和异常

程序没有控制台窗口，因此打包入口必须先创建 `logs/`，再导入应用模块。标准输出、标准错误、Python 日志和未捕获异常统一写入 `logs/video-ai.log`。

浏览器打开失败、ESP32 断线或推理异常时，本地仪表盘应尽量继续运行，并把错误写入日志。如果启动阶段发生错误，无法运行服务，也要在进程退出前留下日志。

## 工程文件

- `python_client/packaged_launcher.py`：打包入口，负责默认参数、运行路径和日志初始化。
- `video-ai.spec`：PyInstaller 配置，声明静态资源、模型、隐藏导入和 EXE 属性。
- `tools/build_windows_exe.ps1`：可重复执行的 Windows 打包脚本，只清理打包输出，不碰其他工程文件。
- `python_client/requirements-build.txt`：仅用于构建的 PyInstaller 依赖。

原有的源码启动命令保持不变。

## 测试

自动化测试覆盖以下行为：

- 开发环境和打包环境的资源路径解析。
- EXE 所在目录下的日志、截图路径。
- ESP32 地址、CPU 推理和内置模型等默认参数。
- 售后参数的透传。
- 日志初始化。

原有 Python 测试必须继续通过。

## 打包后验证

1. 检查 `dist/video-ai/video-ai.exe`、前端静态资源和模型是否存在。
2. 使用 `--no-browser` 和非默认端口启动 EXE。
3. 请求 `/`、`/static/dashboard.css` 和 `/api/state`。
4. 启动仪表盘处理流程，确认模型能够加载；如果 ESP32 未连接，错误应写入日志。
5. 停止 EXE，确认 `logs/video-ai.log` 已生成。

本次只处理 Windows 程序打包，不重新编译 ESP32 固件。
