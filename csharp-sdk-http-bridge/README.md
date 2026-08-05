# HCNetSDK HTTP Bridge（C#）

这是面向 Windows 7 SP1、Windows 10 的 `.NET Framework 4.8`、x86 无界面程序。它只通过 HCNetSDK 登录 NVR 和获取实时/回放码流，再调用独立 FFmpeg 进程转封装为 fragmented MP4，通过本地 HTTP 返回给 Chrome/Edge 的 `<video>` 标签。

## 技术约束

- Visual Studio 2026 可直接打开 `HikSdkHttpBridge.sln`。
- 目标框架为 .NET Framework 4.8，因为 .NET 8/10 不支持 Windows 7。
- 固定构建为 x86，因为当前 HCNetSDK 是 Win32 版本。
- FFmpeg 是转流所需的第三方组件，不参与 NVR 登录；NVR 登录、实时取流和回放取流全部使用 HCNetSDK。
- 输出为非 seekable 的 fragmented MP4，不支持 Range 和进度条拖动。
- 当前随附 FFmpeg 8.1.2；部署到 Windows 7 前必须在目标机验证。若该构建不支持 Win7，替换 `runtime/ffmpeg/ffmpeg.exe` 为支持 Win7、包含 H.264/HEVC demux、MP4 mux 的 x86 构建。只有在显式启用音频时才额外需要 AAC encoder。

## H.265 转 H.264 硬件加速

默认配置 `ffmpeg.hardwareAcceleration` 为 `auto`。服务启动时会依次检查 NVIDIA NVENC、Intel QSV、AMD AMF：不仅查询 FFmpeg 的编译能力，还会执行一次实际 H.264 编码测试。显卡、驱动或会话初始化不可用时会记录原因并自动回退至 `libx264`，不会阻止服务启动。

- 1 倍速 H.264：保持 `copy`，不解码、不转码，也不需要硬件加速。
- H.265：转为浏览器兼容 H.264；若探测成功则优先使用硬件 H.264 编码。NVIDIA 且 FFmpeg 含 CUVID 解码器时，还会启用 H.264/H.265 硬件解码。
- 快放或慢放：需重写 fMP4 时间戳，必然转码；探测成功时仍优先硬件编码。

可将 `hardwareAcceleration` 固定为 `nvidia`、`qsv` 或 `amf`，也可设为 `off` 强制使用软件转码。`/healthz` 和 `/version` 会返回最终选用的 `hardwareAcceleration`；运行日志会包含每个候选后端的探测结果和每个播放会话实际使用的转码后端。

## 构建和运行

```powershell
cd D:\AI_SPACE\hik\csharp-sdk-http-bridge
.\deploy\build.ps1
.\src\HikSdkHttpBridge\bin\x86\Release\net48\hik-sdk-http-bridge.exe validate-config
.\src\HikSdkHttpBridge\bin\x86\Release\net48\hik-sdk-http-bridge.exe run
```

默认输出目录：`src\HikSdkHttpBridge\bin\x86\Release\net48`。运行目录内已经复制 `config.json`、`hcnetsdk` 和 `ffmpeg`。

## 实时首画面优化配置

默认配置会在登录和通道解析后尝试读取设备的压缩参数，优先直接判断主/子码流是 H.264 还是 H.265；仅当老设备不支持该查询时，才以实际码流探测兜底。已识别的编码会写入 `media.codecCacheFile`，其中不包含用户名或密码，服务重启后仍可复用。

`sdk.realPlayKeyFrameIntervalFrames` 默认是 `25`：仅当设备当前 I 帧间隔更长或为自动时，Bridge 才尝试缩短其持久编码设置；设为 `0` 可禁止此项修改。`sdk.forceKeyFrameOnRealPlay=true` 时，实时取流成功后还会请求 NVR 立即发送当前主/子码流的 I 帧。两项依赖设备型号和登录权限，失败只会记录日志，不会中断播放。

`sdk.connectProbeTimeoutMs` 默认 `1500`。每次登录前先检测目标 NVR 的 SDK TCP 端口；IP、端口、路由或防火墙不可达时，接口返回 `NVR_UNREACHABLE`，前端会直接显示网络检查提示，不会误报为通道或码流故障。

浏览器的共用 MSE 播放器会调用 `/session-rendered` 记录实际首帧渲染时间。实时页面默认请求 `stream=sub`；首次建流尚未渲染首帧即失败时，前端只自动尝试一次 `stream=main`。

服务启动后可直接打开输出目录中的 `demo.html` 测试。页面固定连接 `127.0.0.1:28080`；若修改服务端口，请同步修改页面中的两处地址。

健康检查：`GET http://127.0.0.1:28080/healthz`

实时播放（`camera` 为 HCNetSDK 真实通道号，不做固定 `+32`）：

```text
GET /video?option=realplay&ip=192.168.1.64&port=8000&username=admin&password=...&camera=33&stream=main&speed=1&sid=live-1
```

回放需增加 Unix 秒时间戳 `start`、`end`。`speed` 支持 `16,8,4,2,1,0.5,0.25,0.125,0.0625`；实时播放只允许 `speed=1`。通道号必须使用 NVR 的 SDK 通道号：模拟通道通常为 `1...N`，数字/IP 通道通常从 `33` 开始，具体以登录后的设备能力为准。

`channelType` 是可选参数：`auto`（默认，`camera` 为真实 SDK 通道号）、`analog`、`digital`（`ip` 是别名）或 `digitalIndex`。`digitalIndex` 用于本项目后台从 0 开始保存的数字通道序号：Bridge 登录后读取 `NET_DVR_DEVICEINFO_V40` 和 `NET_DVR_GET_IPPARACFG_V40`，以 `真实通道 = dwStartDChan + camera` 动态换算，无需在前端固定 `+32`。模拟通道不存在、超范围或被设备禁用时会返回明确的 `ANALOG_CHANNEL_*` 错误，而不是等待取流超时。

例如，播放模拟通道 1：

```text
GET /video?option=realplay&ip=192.168.1.64&port=8000&username=admin&password=...&camera=1&channelType=analog&stream=main&speed=1&sid=analog-live-1
```

## Windows 服务

以管理员 PowerShell 执行：

```powershell
.\deploy\build.ps1
.\deploy\install-service.ps1
```

卸载执行 `.\deploy\uninstall-service.ps1`。安装脚本会创建 HTTP URL ACL，并以 `LocalService` 账户运行；请确保该账户有权读取部署目录和写入 `logs`。

## 浏览器兼容说明

浏览器能否播放最终取决于 NVR 编码。优先使用 H.264；Chrome/Edge 对 HEVC 的支持依赖系统、硬件和浏览器版本。默认 `media.enableAudio=false`，桥接仅输出视频以降低带宽和 AAC 处理压力；确有声音需求时可改为 `true`，但倍速/慢速始终不输出音频。回放会话会按 `timeouts.playbackKeepAliveMs`（默认 1500ms）发送 SDK 保活。URL 中包含密码会进入浏览器历史和代理日志，只适用于当前内网过渡方案。
