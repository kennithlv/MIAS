# MIAS — Linux 多相机视频采集

基于 [SunLabWestlake/MIAS](https://github.com/SunLabWestlake/MIAS) 的 Linux 视频采集版本，面向海康机器人 USB 相机和 Teledyne FLIR USB 相机。每台相机使用独立的采集线程和写入线程，输出分段视频、逐帧时间戳和一次实验的汇总信息。

原项目由 Jing Ning 开发。原有 Windows 源码和构建说明保存在 `legacy/`；新的 CMake 构建使用 `src/`，不再编译旧的 `MIAS.cpp`。Basler 和大恒的旧实现保留在历史源码中，**本次 Linux 版本没有实现这两个相机后端**。

## 当前功能与验证范围

- 海康 MVS、FLIR Spinnaker 相机后端可按需编译；每台相机按序列号选择。
- 每台相机保存独立视频，默认使用 **FFV1 无损编码、AVI 容器**，按主机接收的单调时钟约每 30 秒分段。
- 保存设备帧号、原始设备时间戳及其单位、主机接收时间。视频播放帧率不能代替采集时间戳。
- 同时限制每台相机缓冲区的帧数和内存；缓冲区满或发生采集错误时停止此次采集并报告错误，不静默丢弃后继续。
- 正常停止时，先结束采集，再排空已经入队的帧并关闭视频文件。
- 自带模拟相机，用于验证多路采集、视频保存、时间戳和停止流程，无需安装厂商 SDK。

**模拟相机测试验证的是程序的通用保存流程。真实相机后端仍需在安装对应 SDK 的 Linux 电脑上编译，并连接设备验证；本仓库不保证任意电脑都能同时达到五台相机的最高帧率。**

## 1. 安装基础依赖

目标环境为 Ubuntu 22.04 / 24.04，64 位。先在终端安装：

```bash
sudo apt update
sudo apt install build-essential cmake libopencv-dev nlohmann-json3-dev python3-opencv
```

相机厂商 SDK 不随仓库分发：

- 海康：使用 **MVS SDK 4.3 或更新版本**（需提供 `MV_CC_Initialize` / `MV_CC_Finalize`）；[MVS Linux 下载中心](https://www.hikrobotics.com/en/machinevision/service/download/)
- FLIR：[Spinnaker SDK](https://www.teledynevisionsolutions.com/products/spinnaker-sdk/)

安装与操作系统及处理器架构匹配的版本，并按 SDK 自带说明安装 USB 权限规则和驱动。先用厂商客户端确认每台相机可以单独采集，再运行 MIAS。

## 2. 先测试不接相机的视频保存

在仓库根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/MIAS --simulate 5 --duration 5 --output video
ctest --test-dir build --output-on-failure
```

程序生成五路 320×240、30 fps 模拟视频。每次运行创建新的实验目录，避免覆盖先前的实验。

也可以使用仓库自带的模拟配置：

```bash
./build/MIAS --config config.json
```

`config.json` 的默认设备是模拟相机，不会自动打开接在电脑上的真实相机。完整命令选项：

```bash
./build/MIAS --help
```

## 3. 编译真实相机支持

安装 MVS 和 Spinnaker 后，在新的构建目录启用两个后端：

```bash
cmake -S . -B build-camera -DCMAKE_BUILD_TYPE=Release \
  -DMIAS_WITH_HIKROBOT=ON \
  -DMIAS_WITH_FLIR=ON
cmake --build build-camera -j
```

只使用某一家相机时，只开启对应选项即可。默认安装路径为 `/opt/MVS` 和 `/opt/spinnaker`，可用 `-DMVS_ROOT=路径`、`-DSPINNAKER_ROOT=路径` 改写。构建系统找不到 SDK 头文件或库时会给出错误，不会假装已经启用真实相机支持。

列出相机及序列号：

```bash
./build-camera/MIAS --list --backend hikrobot
./build-camera/MIAS --list --backend flir
```

## 4. 分别录制你的三组相机

| 示例配置 | 相机 | 默认设置 |
|---|---|---|
| `configs/hikrobot-five.json` | 5 台 MV-CH050-60UC | 2448×2048，30 fps，BayerGR8 |
| `configs/blackfly-five.json` | 5 台 BFS-U3-50S4M-CS | 2448×2048，30 fps，Mono8 |
| `configs/chameleon-five.json` | 5 台 CM3-U3-13Y3M-CS | 1280×1024，30 fps，Mono8 |

先把相应模板复制为 `config.local.json`，再把五个 `REPLACE_WITH_CAMERA_…_SERIAL` 替换为 `--list` 查到的真实序列号。例如：

```bash
cp configs/hikrobot-five.json config.local.json
# 用文本编辑器填写五台相机的序列号后运行
./build-camera/MIAS --config config.local.json
```

三个硬件模板默认持续录像，按 **Ctrl+C** 正常结束。短时间试录可以用：

```bash
./build-camera/MIAS --config config.local.json --duration 10
```

模板的 30 fps 是便于开始验证的设置，**不是已经保证的五机帧率**。先验证一台，再增加至五台，之后逐步提高 `fps`。相机不支持的参数应根据报错和厂商客户端调整。

海康模板的 `BayerGR8` 按单通道原始 Bayer 数据保存，因此普通播放器显示的是灰度纹理，尚未进行彩色重建。需要直接观看彩色视频时，可在设备支持的情况下使用 `BGR8` / `RGB8`，同时重新评估传输、编码和存储负荷。Blackfly 和 Chameleon 模板是单色配置。

## 5. 输出文件和停止行为

```text
video/
  session-YYYYMMDDTHHMMSS-microseconds/
    manifest.json
    cam01/
      segment_000001.avi
      segment_000002.avi
      frames.csv
    cam02/
      ...
```

- **视频文件**：一台相机一个目录，按帧接收的主机单调时钟切分；短时间采集只有第一段。分段不使用设备时钟，外部触发或不均匀采集时不保证每段帧数相同。
- **`frames.csv`**：对应视频中的每一帧，含文件名、片段内帧索引、采集索引、设备帧号和时间戳。
- **`manifest.json`**：记录配置、实际设备信息、采集状态、已接收帧数、提交给编码器的帧数、设备帧号缺口、超时及错误。

`frames_submitted` 表示已经提交给视频编码器的帧数，不能独立证明文件最终可完整解码。正式实验前应完整解码试录的视频，并核对解码帧数、CSV 行数和设备帧号。

可用随附工具检查一次录制，路径替换为终端打印的实际实验目录：

```bash
python3 tools/verify_recording.py video/session-YYYYMMDDTHHMMSS-microseconds
```

工具会实际解码视频，核对视频、CSV 和汇总信息中的帧数及片段列表，并检查已报告的帧号完整性警告。存在错误或完整性警告时返回非零退出码；即使检查通过，也不能证明 USB 接收绝无丢帧或各相机曝光同步。

`manifest.json` 的 `status:"completed"` 仅表示程序流程正常结束。请同时检查 `integrity_warnings`、每台相机的 `frame_id_gaps`（向前跳号）和 `frame_id_discontinuities`（帧号重复或回退）。发现这些情况会输出警告并记录到汇总文件，但不会因此中断录像；它们也可能与设备帧号回绕或重置有关，需要结合 CSV 和设备行为判断。

Ctrl+C、达到指定时长或帧数时，程序会停止接收并保存完已经入队的帧；关闭窗口、强制结束进程或断电可能使当前视频片段无法完整关闭。退出后请查看终端错误和实验汇总，再开始下一次采集。

## 6. 同步与 USB 带宽测试

默认 `trigger:"off"` 为各台相机独立连续采集。相近时间启动线程**不等于曝光同步**。需要同步时，配置 `trigger:"external"`、正确的触发源及边沿，并接入与各相机电气规格匹配的共同硬件触发信号。程序不生成外部触发脉冲。

只测试 USB 接收、不保存视频时：

```bash
./build-camera/MIAS --config config.local.json --no-record --duration 30
```

这项测试能帮助定位接收能力，但不能替代实际视频录制测试。详细参数、时间戳含义、预览和常见问题见 [Linux 使用说明](docs/linux.md)。
