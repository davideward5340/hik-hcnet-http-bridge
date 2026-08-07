# Linux HCNetSDK HTTP Bridge（C++17）

该项目使用 HCNetSDK 直接登录 NVR：实时调用 `NET_DVR_RealPlay_V40`，回放调用 `NET_DVR_PlayBackByTime_V40`；随后把 SDK 回调的 PS 流交给 FFmpeg。Bridge 登录后优先读取设备压缩参数以直接判断主/子码流编码；老设备不支持时才探测 fMP4 初始化段。H.264 使用 `-c:v copy` 保持零视频转码，H.265 或未知编码才解码并转为 H.264 的 HTTP chunked fragmented MP4（fMP4）。默认仅输出视频；只有将 `media.enableAudio` 显式设为 `true` 时，1 倍速才附带 AAC。前端必须通过 MSE（`MediaSource` + `SourceBuffer`）追加分片，不应直接将 `/video` 赋给 `<video>.src`。

## 实时首画面优化配置

`sdk.readVideoCodecFromDevice=true` 会优先使用 `NET_DVR_GET_COMPRESSCFG_V30` 决定 H.264 直通或 H.265 转码。`media.codecCacheFile` 持久保存已识别通道的编码和过期时间，不保存认证信息。设备不返回压缩参数时，探测 FFmpeg 与正式转码会复用同一 HCNetSDK 登录、实时句柄和队列，不再第二次登录 NVR。

`sdk.realPlayKeyFrameIntervalFrames` 默认 `25`，仅在设备当前 I 帧间隔更长或为自动时尝试缩短其持久编码设置；设为 `0` 禁止改写设备配置。`sdk.forceKeyFrameOnRealPlay=true` 时，实时取流成功后请求 NVR 立即发送当前主/子码流 I 帧。设备或权限不支持时只记录日志并继续播放。浏览器通过 `/session-rendered` 回报首帧实际渲染时间；实时页面默认使用子码流，首次建流未渲染首帧即失败时仅自动回退一次主码流。

`sdk.connectProbeTimeoutMs` 默认 `1500`。Bridge 在登录前先建立一次到 NVR SDK TCP 端口的短连接；IP、端口、路由或防火墙不可达时，返回 `NVR_UNREACHABLE`，使前端准确提示网络问题而不切换码流或发起无效重连。

## 面向信创 Linux 的交付方式

默认交付物是**免容器便携目录包**，而不是 Docker/Podman 镜像。客户机只需解压并执行 `start.sh`；包中自带桥接程序、HCNetSDK、完整 `HCNetSDKCom` 和静态 FFmpeg。

```text
hik-sdk-http-bridge-linux-x86_64-1.0.0/
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
dist/hik-sdk-http-bridge-linux-x86_64-1.0.0.tar.gz
dist/hik-sdk-http-bridge-linux-x86_64-1.0.0.tar.gz.sha256
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

保留的 `Dockerfile`、`scripts/package-oci.sh` 和 `scripts/run-oci.sh` 只供允许容器运行时的环境使用；它们不是本项目对客户机的默认交付方式。

## AppImage 单文件交付（可选）

可将同一套已验证的便携包内容封装为单个 `.AppImage`，具体见 [APPIMAGE_PACKAGE.md](APPIMAGE_PACKAGE.md)。

```sh
./scripts/package-appimage.sh
```

AppImage 仅是额外的单文件分发格式；正式产物仍须通过 glibc 2.28 和静态 FFmpeg 校验。无 FUSE 的系统可使用 `--appimage-extract-and-run` 降级运行。

## 许可证

- 海康 SDK 许可证位于 `vendor/licenses/`，再分发前必须确认海康授权范围。
- 静态 FFmpeg 的许可证、构建配置和源代码获取方式必须由发布方随包提供，具体见 `runtime/ffmpeg/README.md`。
