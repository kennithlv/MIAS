# Linux 采集与录像说明

## 构建方式

需要 C++17 编译器、CMake、OpenCV 4、nlohmann-json 和线程库。仓库根目录的 CMake 构建新版本；`legacy/` 保留上游的 Windows 代码，仅作历史参考。

| CMake 选项 | 默认值 | 用途 |
|---|---|---|
| `MIAS_WITH_HIKROBOT` | `OFF` | 编译海康 MVS 后端 |
| `MIAS_WITH_FLIR` | `OFF` | 编译 FLIR Spinnaker 后端 |
| `MIAS_WITH_PREVIEW` | `OFF` | 编译 OpenCV 窗口预览；服务器运行通常无需开启 |
| `MVS_ROOT` | `/opt/MVS` | 海康 Linux SDK 安装目录 |
| `SPINNAKER_ROOT` | `/opt/spinnaker` | FLIR Linux SDK 安装目录 |

自定义安装路径示例：

```bash
cmake -S . -B build-camera -DCMAKE_BUILD_TYPE=Release \
  -DMIAS_WITH_HIKROBOT=ON -DMVS_ROOT=/opt/MVS \
  -DMIAS_WITH_FLIR=ON -DSPINNAKER_ROOT=/opt/spinnaker
cmake --build build-camera -j
```

非标准目录布局也可以设置 `MVS_INCLUDE_DIR`、`MVS_LIBRARY`、`SPINNAKER_INCLUDE_DIR`、`SPINNAKER_LIBRARY`。库与头文件必须来自同一 SDK 版本，并且与电脑架构一致。请先按厂商安装说明完成动态库查找配置和 USB 设备访问权限设置。

海康后端要求 **MVS SDK 4.3 或更新版本**，因为代码使用进程级 `MV_CC_Initialize` / `MV_CC_Finalize`。旧 SDK 若缺少这些接口，不能仅更改路径就直接编译此后端；应安装匹配的新版 SDK。

不启用厂商后端时，仅可使用模拟相机。模拟模式能验证代码与编码器，不会模拟 USB 带宽、相机驱动或硬件触发。

## 配置格式

新格式的 `schema_version` 是 `2`。旧的 `need_record`、`fourcc`、以序列号为键的 `cameras` 对象等配置不直接适用，原文件保存在 `legacy/config.json`。

### 全局参数

| 字段 | 示例或默认值 | 含义 |
|---|---|---|
| `schema_version` | `2` | 新配置版本 |
| `output_dir` | `"video"` | 输出根目录；相对路径从启动程序的当前目录计算 |
| `record` | `true` | 是否保存视频；`--no-record` 可临时关闭 |
| `preview` | `false` | 是否显示预览，需启用相应构建选项 |
| `preview_fps` | `10` | 限制预览刷新率，不改变相机采集帧率 |
| `codec` | `"FFV1"` | 支持 FFV1（无损）或 MJPG（有损）；编码器须由 OpenCV 后端支持 |
| `container` | `"avi"` | 支持 AVI；FFV1 也可选择 MKV；示例与自动测试使用 AVI |
| `segment_seconds` | `30` | 按帧接收的主机单调时钟分段；不使用设备时钟，也不保证每段帧数相同 |
| `queue_frames` | `64` | 每台相机缓冲区最多排队帧数 |
| `queue_megabytes` | `256` | 每台相机缓冲区内存上限；与帧数上限同时生效 |
| `duration_seconds` | `0` | 采集时长限制；`0` 表示持续运行，直至 Ctrl+C 或其他停止条件 |
| `frame_limit` | `0` | 每台相机的帧数限制；`0` 表示不按帧数停止 |
| `no_frame_timeout_seconds` | `10` | 持续收不到帧时的错误阈值；低频外部触发需要相应调大 |
| `cameras` | 数组 | 要打开的每台相机的配置 |

缓冲区只能吸收短暂抖动。持续写入速度低于采集速度时，增加缓冲区不能消除长期积压。超过任一缓冲上限时，本次采集应以错误状态结束；错误发生前已经入队的帧会尽量写完，触发溢出的帧不会被算作成功保存。

### 每台相机参数

| 字段 | 示例 | 含义 |
|---|---|---|
| `backend` | `"hikrobot"`、`"flir"`、`"simulated"` | 相机后端 |
| `serial` | `--list` 输出的序列号 | 精确选择设备；不依赖 USB 插入顺序 |
| `name` | `"cam01"` | 输出子目录名；同一次采集中不能重复 |
| `pixel_format` | `"Mono8"` / `"BayerGR8"` | 相机输出格式，必须由设备支持 |
| `width`、`height` | `2448`、`2048` | 请求的图像宽高；`0` 保留设备当前值 |
| `fps` | `30` | 连续采集时请求的帧率，以及保存视频的播放帧率 |
| `exposure_us` | `0` | 曝光时间（微秒）；`0` 保留当前设置 |
| `trigger` | `"off"` / `"external"` | 自由连续采集或外部触发 |
| `trigger_source` | `"Line0"` | 外部触发输入节点，须根据实际相机接线确认 |
| `trigger_activation` | `"RisingEdge"` | 外部触发边沿，须由设备支持 |
| `throughput_limit` | `0` | 请求的设备链路限速（字节/秒）；`0` 保留设备设置 |

真实相机模板必须先填写序列号。`REPLACE_WITH_CAMERA_…_SERIAL` 是占位符，不能作为实际设备使用。

目前录像流程支持每像素 1 字节的 Mono8/Bayer8，或每像素 3 字节的 BGR/RGB 8-bit 图像。RGB 输入会转换为 OpenCV 的 BGR 通道顺序。10/12/16-bit 或打包像素格式不应套用这些模板；需要另行实现并验证完整的位深处理链路。原始 Bayer 录像仅允许无损 FFV1，避免有损编码破坏 Bayer 数据；图像宽高必须为偶数，防止部分视频后端静默裁剪。

海康彩色模板的 BayerGR8 保存的是 Bayer 原始数值，不会自动还原颜色。设备的实际 ROI、图像尺寸和像素格式应结合 `manifest.json` 与第一段视频核对。Chameleon 的 Mono8 模式不应直接套用其 Raw8 模式的最高帧率标称值。

## 命令行

```bash
# 帮助
./build/MIAS --help

# 列出指定厂商的设备
./build-camera/MIAS --list --backend hikrobot
./build-camera/MIAS --list --backend flir

# 从配置开始录像，直到 Ctrl+C
./build-camera/MIAS --config config.local.json

# 限制为 10 秒，或每台最多 300 帧（可以分别使用）
./build-camera/MIAS --config config.local.json --duration 10
./build-camera/MIAS --config config.local.json --frames 300

# 指定输出目录
./build-camera/MIAS --config config.local.json --output recordings

# 不保存视频，只测试接收
./build-camera/MIAS --config config.local.json --no-record --duration 30

# 模拟五台相机并实际保存视频
./build/MIAS --simulate 5 --frames 90 --output video
```

CLI 覆盖项只影响本次运行，不会改写配置文件。新版按启动即采集的方式运行，不使用旧程序的 `c`、`s`、`r` 交互命令。停止使用 Ctrl+C，或配置时长/帧数限制。

## 预览

默认构建和配置均关闭预览，适合无显示器的采集电脑。需要预览时：

```bash
cmake -S . -B build-camera -DCMAKE_BUILD_TYPE=Release \
  -DMIAS_WITH_HIKROBOT=ON -DMIAS_WITH_FLIR=ON -DMIAS_WITH_PREVIEW=ON
cmake --build build-camera -j
./build-camera/MIAS --config config.local.json --preview
```

需要可用的图形桌面或显示服务。减少 `preview_fps` 可以降低界面开销；五机带宽或录像性能测试应先关闭预览。

## 视频时间和 CSV 时间戳

视频是固定播放帧率文件。如果外部触发间隔不均匀或相机实际帧率低于配置值，视频播放时长可能不同于真实实验时长。分段在收到跨过 `segment_seconds` 阈值的下一帧时发生，使用主机单调接收时间；各相机的片段边界不会因此自动同步，外部触发时也不保证每段固定帧数。真实时间分析应使用 CSV，不能仅用“视频第几帧 ÷ 配置帧率”。

`frames.csv` 字段如下：

| 字段 | 说明 |
|---|---|
| `video_file` | 该帧所属的视频片段 |
| `video_frame_index` | 片段内的视频帧索引 |
| `capture_index` | 程序为该相机接收到的帧分配的采集索引 |
| `frame_id` | SDK 提供的设备帧号 |
| `frame_id_valid` | 当前帧号是否可用；无效时不能据此推断丢帧 |
| `camera_timestamp` | SDK 原始设备时间戳 |
| `camera_timestamp_unit` | 设备时间戳单位；`ticks` 不能未经换算当作纳秒 |
| `host_monotonic_ns` | 主机接收时的单调时钟（纳秒），适合计算接收间隔 |
| `host_unix_ns` | 主机接收时的 Unix 时间（纳秒），受主机时钟同步与调整影响 |

主机时间记录的是接收时间，不是传感器曝光时间。不同相机的设备时钟也不一定共用起点或频率。跨相机同步分析需要共同触发、帧号对应及相应的时钟校准。

## 完整性检查与常见问题

1. **录制前**：先在厂商客户端单独打开每台相机，再退出客户端，避免设备被其他程序占用。通过 `--list` 检查实际序列号。
2. **短时间试录**：先一台，再五台；查看 `manifest.json` 中的状态、错误、`integrity_warnings`、帧号缺口、超时与队列峰值。`frame_id_gaps` 记录向前跳号，`frame_id_discontinuities` 记录重复或回退。它们会触发终端及汇总警告，但不主动中断录像；设备帧号回绕或重置也可能引起警告。`status:"completed"` 只代表程序流程正常结束，不表示已经证明 USB 零丢帧。
3. **验证保存结果**：完整解码所有片段，核对解码帧数与 CSV 数据行数。自动模拟测试会检查视频能够解码及数量对应；这不等同于对实际相机链路的验证。
4. **提示无法打开编码器**：确认 OpenCV 包含可用的视频编码后端及 FFV1 支持。程序不会默默换用有损编码。先运行模拟测试验证本机编码链路。
5. **提示缓冲区满**：降低采集帧率、关闭预览或减小 ROI，定位 USB 接收、编码和磁盘写入中哪一环落后，再调整系统；不要只无限增加缓冲区。
6. **外部触发后没有图像**：检查触发输入、边沿、电平和实际触发脉冲。触发间隔超过 `no_frame_timeout_seconds` 时会被判定为采集失败。
7. **提示参数节点不支持或无法写入**：在厂商客户端核对该型号/固件实际提供的节点；不要假定 Blackfly 与 Chameleon 的所有控制项完全相同。

程序无法保证断电、磁盘满、进程被强制结束或编码器内部异常之后的视频仍然完整。错误状态和 `frames_submitted` 都应结合实际解码检查解读；本程序不提供断电后的文件恢复保证。

### 校验已经保存的一次实验

将以下路径换成实际输出的 `session-…` 目录，不要传入整个 `video/` 根目录：

```bash
python3 tools/verify_recording.py video/session-YYYYMMDDTHHMMSS-microseconds
# 需要机器可读结果时
python3 tools/verify_recording.py video/session-YYYYMMDDTHHMMSS-microseconds --json
```

校验工具需要 Python OpenCV（Ubuntu 安装包为 `python3-opencv`）。它会逐段完整解码，不只依赖视频容器报告的帧数，并核对 CSV 索引、片段列表、汇总帧数和文件大小。退出码 `0` 表示没有发现错误或完整性警告，`1` 表示存在文件不一致或完整性警告，`2` 表示校验未能执行。`--json` 结果用 `saved_files_consistent` 表示保存文件本身是否一致，用 `ok` 表示连同完整性警告的总体结果；两者不能混淆。该检查不能证明从未到达主机的曝光帧均被记录，也不能证明 USB 绝无丢帧或跨相机曝光同步。

## 自动测试

```bash
ctest --test-dir build --output-on-failure
```

GitHub Actions 配置在 Ubuntu 22.04 和 24.04 上构建不依赖厂商 SDK 的版本，并执行模拟视频测试。真实海康/FLIR 后端的验证需要对应 SDK 和设备；仓库没有使用伪造 SDK 头文件来冒充真实兼容性测试。
