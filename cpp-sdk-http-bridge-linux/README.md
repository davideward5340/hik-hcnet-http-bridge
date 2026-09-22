# HCNetSDK HTTP Bridge（C++17，Linux/Windows）

该项目使用 HCNetSDK 直接登录 NVR：实时调用 `NET_DVR_RealPlay_V40`，回放调用 `NET_DVR_PlayBackByTime_V40`；随后把 SDK 回调的 PS 流交给 FFmpeg。Bridge 登录后优先读取设备压缩参数以直接判断主/子码流编码；老设备不支持时才探测 fMP4 初始化段。Windows 的 1 倍速 H.264 和 Linux 的 H.264 实时预览优先使用 `-c:v copy`；若直通只能生成 `ftyp/moov` 初始化段、未能在超时内生成首个 `moof/mdat` 媒体分片，Bridge 会重新取流并自动降级为 H.264 软件转码。Linux x86_64/ARM64 的 H.264 回放固定使用 libx264 转码，以重建浏览器兼容的时间戳、关键帧和参数集。H.265、未知编码和变速播放同样转为 H.264 的 HTTP chunked fragmented MP4（fMP4）。默认仅输出视频；只有将 `media.enableAudio` 显式设为 `true` 时，1 倍速才附带 AAC。前端必须通过 MSE（`MediaSource` + `SourceBuffer`）追加分片，不应直接将 `/video` 赋给 `<video>.src`。

同一份 `src/main.cpp` 支持 Linux x86_64、Linux ARM64 和 Windows x86。Linux 保持 `libx264` 软件转码，其中 H.264 实时预览仍可直通、H.264 回放固定转码；Windows 仅在确认输入为 H.265 且需要输出 H.264 时启用显卡编码。Windows 启动时按 NVIDIA NVENC、Intel QSV、AMD AMF 顺序执行真实的 64×64 H.264 编码测试；不可用时使用已经过真实编码测试的 `libx264`。Windows 的 1 倍速 H.264 仍直接复制。

Windows x86 构建：

```powershell
.\scripts\build-windows.ps1 -Configuration Release `
  -SdkPackage C:\SDKs\HCNetSDK-Win32 `
  -Runtime C:\SDKs\hik-bridge-runtime
```

`SdkPackage` 必须指向使用者从合法渠道取得的 Win32 HCNetSDK 包，`Runtime` 必须包含 `hcnetsdk/` 和 `ffmpeg/`。厂商 SDK 与 FFmpeg 二进制不随源码仓库分发。输出为 `build-win32/hik-sdk-http-bridge.exe`；Windows 配置使用 `config/config.windows.json`。`ffmpeg.hardwareAcceleration` 可设为 `auto`、`off`、`nvidia`、`qsv` 或 `amf`，`ffmpeg.hardwareProbeTimeoutMs` 控制单次真实编码探测超时。

当前参考运行时的 HCNetSDK 和主程序是 x86，但 `ffmpeg.exe` 是 x64，因此默认交付物运行在 64 位 Windows（允许 x86 主程序启动 x64 FFmpeg）。如需运行在真正的 32 位 Windows 操作系统，须通过 CMake 的 `HIK_WIN32_RUNTIME` 指定包含 Win32 FFmpeg 的运行时目录；该 FFmpeg 至少必须包含 `libx264`、`lavfi/color`、`mpegps` 和 `mp4`，需要显卡加速时还应包含目标厂商的编码器。

## Windows 服务安装与升级

把完整 `build-win32` 交付目录放在固定的本地目录，配置 `config.json` 后，以管理员身份运行 `安装.bat`。安装的是 SCM 原生服务 `hikbridge`（显示名 `Hik SDK HTTP Bridge`），可在 `services.msc` 查看；采用普通自动启动，并在失败后按 5 秒、30 秒、60 秒间隔重启。手动停止不会触发失败恢复。安装后不要移动或删除目录。

安装、卸载均使用 CMD 批处理和 Windows 自带的 `sc.exe`、`schtasks.exe`、`reg.exe` 等命令，不依赖 PowerShell、WMIC 或 curl。`安装.bat`、`卸载.bat` 与公共脚本 `windows-service.cmd` 必须一起保留。中文脚本使用 GBK、CRLF、无 BOM；公共脚本先切换到代码页 936，结束时恢复原代码页。不要直接用 UTF-8 覆盖工作目录中的 GBK 脚本；Git 通过 `.gitattributes` 的 `working-tree-encoding=GBK` 在仓库存储与检出之间转换编码。

服务直接执行 `hik-sdk-http-bridge.exe service --config "绝对配置路径"`。手动调试使用 `run-bridge.cmd` 或 `hik-sdk-http-bridge.exe run --config "配置路径"`。服务入口仅在 Windows 编译；Linux 命令行、SDK 加载和编解码策略保持原有方式。

安装前会把旧服务注册表导出文件和旧任务 XML 保存到 `installation-backups/操作名-随机编号/`，并在执行期间记录原服务路径、启动类型和运行状态，随后停止旧实例、创建或更新服务。安装器先执行 EXE 的配置校验，再等待 SCM 报告 `RUNNING` 并复查；本项目原生服务仅在 SDK 初始化和 HTTP 监听成功后才报告该状态。批处理不解析 JSON、不依赖 HTTP 客户端，实际端口仍由 EXE 读取配置。新服务通过状态检查后才删除 `HikSdkHttpBridge` 旧服务及 `hikbridge`/`HikSdkHttpBridge` 旧计划任务。重复安装更新现有服务，不删除重建。安装失败时尝试恢复原服务启动状态及旧任务定义，并尝试运行恢复的任务；原本禁用的任务仍保留禁用定义。备份不包含旧二进制或用户配置，因此升级前仍应保留旧交付包。

旧 CMD 任务停止后可能留下子进程。纯批处理不按进程名称批量强杀，以免影响其他安装目录的实例；如果提示端口占用或服务启动失败，请先确认旧实例已退出。`安装.bat /check` 仅校验程序和配置，不改动服务或任务；`/nopause` 可用于无人值守执行，退出码 0 表示成功，1 表示失败或未完全完成。

若新服务已通过验证，但失败恢复配置或旧注册清理失败，会保留可用的新服务并返回安装失败，修复提示的问题后重新执行安装即可。卸载使用管理员身份运行 `卸载.bat`；保留配置、日志和缓存。服务被其他程序持有句柄而处于待删除状态时，脚本会明确提示，不会把“待删除”当作已完成卸载。

日志位于配置的 `logging.directory`。服务模式下，文件日志初始化前的启动错误也写入 Windows Application 事件日志，来源 `hikbridge`。正常停止会取消 HTTP/媒体读取，回收所有连接线程，再清理 SDK；如果厂商 SDK 调用阻塞导致 Windows 服务清理超过 60 秒，会记录错误并终止进程，避免永久停留在“正在停止”。

默认构建会同步安装脚本，即使 EXE 无需重新编译；已有 `build-win32/config.json` 不会被构建覆盖。脚本源文件是 `deploy/windows/安装.bat`、`deploy/windows/卸载.bat` 和 `deploy/windows/windows-service.cmd`。

## 服务与退出回归测试

Windows 构建脚本增加 `-BuildTests` 可编译并运行无需 NVR 的生命周期测试，覆盖未完成 HTTP 请求、阻塞发送、HTTP 解析和 FFmpeg 读取取消。Linux 使用 `cmake -S . -B build-test -DHIK_BUILD_TESTS=ON -DHIK_TEST_FFMPEG=/绝对路径/ffmpeg`，构建后在该目录执行 `ctest --output-on-failure`。这些测试使用真实 socket/FFmpeg，但不登录设备。

`tests/windows-batch.py` 使用 `-DHIK_BUILD_TESTS=ON` 生成的 `hik-cmd-tool-stub.exe` 作为系统命令替身，在真实 CMD 中验证安装、重复安装、回滚、卸载、中文提示和特殊字符路径；不会修改本机 SCM 或注册表。例如：`python tests/windows-batch.py --stub build-win32/hik-cmd-tool-stub.exe --source deploy/windows --output build-win32/cmd-tests`。每次指定新的输出目录，以免混入旧测试状态。`tests/runtime-smoke.py --help` 提供真实 SDK 初始化、HTTP 健康检查及 Linux SIGTERM 退出测试。Windows 的普通进程冒烟测试使用强制终止，不能替代 SCM 停止测试。

在没有既有桥接服务/任务的测试机上，以管理员身份运行 `tests/windows-service-integration.ps1`，验证旧任务迁移、中文目录、重复安装、端口冲突后的注册回滚、停止、崩溃恢复和卸载；脚本在 finally 中清理测试注册。该测试不重启系统。发布验收还需实际重启 Windows，并在有设备的环境验证实时预览、回放中停止和恢复；Linux ARM64 的运行验收需要对应架构机器及 SDK。

## 实时首画面优化配置

`sdk.readVideoCodecFromDevice=true` 会优先使用 `NET_DVR_GET_COMPRESSCFG_V30` 决定 H.264 直通或 H.265 转码。`media.codecCacheFile` 持久保存已识别通道的编码和过期时间，不保存认证信息。设备不返回压缩参数时，探测 FFmpeg 与正式转码会复用同一 HCNetSDK 登录、实时句柄和队列，不再第二次登录 NVR。

`sdk.realPlayKeyFrameIntervalFrames` 默认 `25`，仅在设备当前 I 帧间隔更长或为自动时尝试缩短其持久编码设置；设为 `0` 禁止改写设备配置。`sdk.forceKeyFrameOnRealPlay=true` 时，实时取流成功后请求 NVR 立即发送当前主/子码流 I 帧。设备或权限不支持时只记录日志并继续播放。浏览器通过 `/session-rendered` 回报首帧实际渲染时间；实时页面默认使用子码流，首次建流未渲染首帧即失败时仅自动回退一次主码流。

`sdk.connectProbeTimeoutMs` 默认 `1500`。Bridge 在登录前先建立一次到 NVR SDK TCP 端口的短连接；IP、端口、路由或防火墙不可达时，返回 `NVR_UNREACHABLE`，使前端准确提示网络问题而不切换码流或发起无效重连。

## 面向信创 Linux 的交付方式

默认交付物是**免容器便携目录包**，而不是 Docker/Podman 镜像。客户机只需解压并执行 `start.sh`；包中自带桥接程序、HCNetSDK、完整 `HCNetSDKCom` 和静态 FFmpeg。

```text
hik-sdk-http-bridge-linux-x86_64-0.9.1.260922/
├── start.sh
├── bin/hik-sdk-http-bridge
├── sdk/                         # 海康 Linux64 SDK 与 HCNetSDKCom
├── runtime/ffmpeg/ffmpeg        # 静态链接 FFmpeg
├── config/config.json
├── logs/
├── licenses/
└── release-manifest.env
```

详细兼容范围、构建基线和客户机部署方式见 [PORTABLE_PACKAGE.md](PORTABLE_PACKAGE.md)。

> 当前提供的海康 SDK 是 Linux64/x86_64。统信、Deepin、麒麟的 ARM、龙芯、MIPS、申威版本必须更换同架构 HCNetSDK 后重新构建，不能使用此 x86_64 包。

## 生成发布包

1. 在 glibc 2.28 的 x86_64 基线构建机（建议统信 UOS V20 或银河麒麟 V10 开发环境）执行：

   ```sh
   chmod +x scripts/build-portable.sh scripts/package-portable.sh
   ./scripts/build-portable.sh
   ```

2. 从 FFmpeg 官方源码准备源代码，并在同一基线机中构建经许可审核的静态 FFmpeg：

   ```sh
   FFMPEG_SOURCE_DIR=/path/to/ffmpeg-source X264_SOURCE_DIR=/path/to/x264-source ./scripts/build-static-ffmpeg.sh
   ```

   也可以使用自行审核的静态 FFmpeg，但必须放在 `runtime/ffmpeg/ffmpeg`。

3. 打包：

   ```sh
   ./scripts/package-portable.sh
   ```

生成：

```text
dist/hik-sdk-http-bridge-linux-x86_64-0.9.1.260922.tar.gz
dist/hik-sdk-http-bridge-linux-x86_64-0.9.1.260922.tar.gz.sha256
```

脚本会拒绝两类错误产物：高于 glibc 2.28 的桥接二进制，以及依赖客户机动态库的 FFmpeg。这样可避免在开发机“能启动”、交付到统信或麒麟后才因动态库版本失败。

## HTTP 接口

实时（`camera` 为 HCNetSDK 真实通道号，不做固定 `+32`）：

```text
GET /video?option=realplay&ip=192.0.2.10&port=8000&username=admin&password=PASSWORD&camera=33&stream=main&speed=1&sid=live-001
```

回放：

```text
GET /video?option=playback&ip=192.0.2.10&port=8000&username=admin&password=PASSWORD&camera=33&stream=main&speed=1&sid=vod-001&start=1783998000&end=1783998300
```

`start`、`end` 是 Unix 秒。响应包含 `X-Hik-Bridge-Mse-Codecs`、`X-Hik-Bridge-Playback-Start` 和 `X-Hik-Bridge-Playback-End`，供 MSE 前端选择 `SourceBuffer` 并恢复回放。fMP4 不是随机访问文件，回放定位时前端须主动中止旧 fetch，并用新的 `start` 重新请求 `/video`。

为应对短暂网络抖动，回放会话按 `timeouts.playbackKeepAliveMs`（默认 1500ms）调用 HCNetSDK 回放保活；连续三次失败会以 `sdk_keepalive_failed` 结束会话，供前端从最后播放位置重建。`media.queueBytes` 默认 64 MiB，仍为有界队列；队列持续满载会以 `queue_overflow` 明确结束而不会静默丢失码流。

`channelType` 可选：`auto`（默认，`camera` 为真实 SDK 通道号）、`analog`、`digital`（`ip` 为别名）或 `digitalIndex`。`digitalIndex` 用于本项目后台从 0 开始保存的数字通道序号：Bridge 登录后读取 `NET_DVR_DEVICEINFO_V40` 与 `NET_DVR_GET_IPPARACFG_V40`，以 `真实通道 = dwStartDChan + camera` 动态换算，无需固定 `+32`。模拟通道不可用时会返回 `ANALOG_CHANNEL_UNSUPPORTED`、`ANALOG_CHANNEL_OUT_OF_RANGE` 或 `ANALOG_CHANNEL_DISABLED`。

模拟通道 1 示例：

```text
GET /video?option=realplay&ip=192.0.2.10&port=8000&username=admin&password=PASSWORD&camera=1&channelType=analog&stream=main&speed=1&sid=analog-live-001
```

支持速度：`16|8|4|2|1|0.5|0.25|0.125|0.0625`。非 1 倍速不输出音频，避免音视频时间轴失配。

## OCI 交付（可选）

保留的 `Dockerfile`、`scripts/package-oci.sh` 和 `deploy/linux/oci/run.sh` 只供允许容器运行时的环境使用；它们不是本项目对客户机的默认交付方式。

## AppImage 单文件交付（可选）

可将同一套已验证的便携包内容封装为单个 `.AppImage`，具体见 [APPIMAGE_PACKAGE.md](APPIMAGE_PACKAGE.md)。

```sh
./scripts/package-appimage.sh
```

AppImage 仅是额外的单文件分发格式；正式产物仍须通过 glibc 2.28 和静态 FFmpeg 校验。无 FUSE 的系统可使用 `--appimage-extract-and-run` 降级运行。

## 会话容量与异常回收

### 手动清理所有播放会话

新版本的 `GET /healthz` 在原有字段之外返回 `capabilities: {"cleanSessions": true}`、字符串 `generation` 和 `cleaningSessions`。健康查询不会改变会话、清理代次或资源状态，并返回 `Cache-Control: no-store`。

前端只有在有效健康响应中明确检测到该能力时，才调用 `POST /cleanSessions?apply=true`，请求头必须包含 `X-Hik-Cleanup: true`。旧版本健康响应没有该能力字段时提示升级；超时、网络错误和无效健康响应提示检查连接，不能当作旧版本。不存在 `apply=false` 探测模式。

清理接口允许所有网站调用，普通响应与 OPTIONS 预检均返回 `Access-Control-Allow-Origin: *`，无需配置允许来源。已有配置中的 `controlAllowedOrigin` 字段不再使用，可以删除。仍需显式执行参数和 `X-Hik-Cleanup: true` 请求头；OPTIONS 预检不会执行清理。

接口终止调用时已经注册的全部实时预览、回放和启动中会话，先记录 `manual_cleanup` 再断开媒体连接。SDK、FFmpeg 和线程清理完成后才归还名额，服务和健康接口继续运行。返回字段：

```json
{"applied":true,"affectedSessions":2,"cleaningSessions":2,"generation":"service-instance-1"}
```

仍有清理中的会话返回 HTTP 202；已完成返回 200。前端随后轮询 `/healthz`，`cleaningSessions` 为 0 才表示原有会话资源释放完成。`activeSessions` 还可能包含清理后由用户手动建立的新会话，不应据此误报失败。GET 清理请求返回 405；缺少 `apply=true` 或自定义请求头返回 400。

每次清理都会更新 `generation`。支持该协议的前端在用户明确点击播放时获取代次，后续自动重连、码流切换及失败重试一直沿用它，并在 `/video` 查询参数中携带 `generation`。旧代次请求返回 HTTP 409、`MANUAL_RESTART_REQUIRED`。前端收到该错误或 `manual_cleanup` 时取消重试、停止播放器，仅手动播放才能刷新代次。播放/暂停期间还会约每 3 秒检查代次，避免缓存视频迟迟感知不到断连；后台标签页受浏览器定时器节流影响，服务端资源清理不依赖该轮询。

兼容边界：旧前端不携带代次，在新服务第一次清理之前仍可播放；清理之后缺少代次的播放请求也会被拒绝，需使用新版前端。新前端访问旧桥接时可正常播放，但不会发送清理请求。

日志和 `/session-status` 保留主动清理原因，不会被后续断连回调覆盖。清理没有固定的完成秒数，已经进入的同步 SDK 调用需要返回后才能释放资源。

`server.maxSessions` 默认和发布包均为 `16`，同一桥接进程的所有播放页面共享名额。
正在启动、播放及清理中的 `/video` 请求占用名额；`/healthz`、`/version` 不占用。
`/healthz` 返回 `activeSessions`、`maxSessions`、`availableSessions`。
满额时 `/video` 返回 HTTP 503、`code: SESSION_LIMIT` 和相同的容量字段。

浏览器关闭连接后，首帧等待和 FFmpeg 输出等待会在短轮询中取消。
已进入的同步 SDK 调用需先返回，SDK 接收等待设置为 5 秒；不强行终止 SDK 线程。
FFmpeg、SDK 和相关线程清理完成后才归还名额，日志记录清理耗时与剩余占用。
Windows FFmpeg 子进程仅继承自己的标准管道，避免其他会话的 socket/管道被持有。

两个新增超时均可在 `timeouts` 中设置，范围为 1000～600000 毫秒：

- `outputStallMs`：默认 60000，仅计算持续等待 FFmpeg 输出的时间；慢放按速度反比延长，浏览器背压等待不计入。
- `clientWriteStallMs`：默认 60000，持续无法向浏览器发送数据时结束会话；每次发送取得进展即重新计时。

配套前端对 `SESSION_LIMIT` 最多在 1 秒、2 秒后重试，不进行主/子码流切换；停止或替换播放会取消重试。
如果停止所有播放后占用长期不降，可重启桥接服务恢复，并保留日志排查。

## 许可证

- 本项目自有代码采用仓库根目录的 Apache License 2.0。
- 海康 SDK 为厂商专有组件，不随本仓库分发；使用和再分发前必须自行确认厂商授权范围。
- 静态 FFmpeg 的许可证、构建配置和源代码获取方式必须由发布方随包提供，具体见 `runtime/ffmpeg/README.md`。

## 当前发布版本

部署脚本统一存放在 [deploy](deploy/README.md)，Windows/Linux 全量包的目录、命名、版本、构建、测试及校验规则以 [deploy/RELEASE.md](deploy/RELEASE.md) 为准。正式组装入口为 `scripts/package-full-release.py`，不再以 temp 下的历史脚本作为发布入口。

2026-09-22 稳定性修复版本为 `0.9.1.260922`：Windows 独占监听端口；Linux 使用不可继承的监听/连接/管道与 posix_spawn；Windows 升级工具增加跨进程互斥。

`server.maxHttpConnections` 为独立的 HTTP 工作连接上限，默认 64，允许范围 1～4096。达到上限时新连接收到尽力发送的 HTTP 503 后关闭，线程创建失败仅拒绝该连接。此上限包含等待请求头、控制请求及播放请求，区别于仅限制播放的 `maxSessions`；正常配置应为控制请求留出空间。全部 HTTP 名额被占用时控制接口也可能暂时收到忙碌响应。

Linux 全量包的 `install.sh` 已取消 SHA-256 校验，不依赖 sha256sum 或 .sha256 文件；仍保留必要文件、系统及架构检查。随包校验文件仅供人工核验。

Windows 已提供 `升级.bat` 和原生升级工具，支持 C++ 全量升级以及旧 C# 服务迁移。新包需解压到与旧安装目录互不包含的独立目录，以管理员身份运行。完整替换包括新包所有文件和配置，旧目录整体备份，失败时尝试回滚。使用方式及验证范围见 [Windows 升级说明](UPGRADE_WINDOWS.md)。

当前版本为 `0.9.1.260922`，发布版本 `0.9.1`，打包日期 `2026-09-22`。版本定义在 `src/version.h`，用于命令行 `--version`、HTTP `/version`、Windows 文件字符串属性和 AppImage 打包版本。Windows 数字版本为 `0.9.1.0`，满足单段 16 位限制。

全量包沿用 `hikbridge.zip`（Windows）和 `hikbridge.tar.gz`（Linux）命名，根目录均为 `hikbridge/`；Linux 包同时包含 x86_64 与 aarch64 AppImage。每个包内有 `version.json`，包外有 SHA-256 文件。Windows 安装和升级均设置普通自动启动；回滚仍保留旧服务的原启动方式。
