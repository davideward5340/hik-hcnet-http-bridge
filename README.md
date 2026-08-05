# hik-hcnet-http-bridge

基于海康威视 HCNetSDK 的本地 HTTP 视频桥接项目。Bridge 直接登录 NVR，读取实时预览或按时间回放的码流，经 FFmpeg 规范化为浏览器兼容的 H.264 fragmented MP4（fMP4），由前端通过 MSE（`MediaSource`）写入 `<video>` 播放。

项目面向局域网、客户端与 NVR 点对点部署场景；Bridge 默认只监听 `127.0.0.1`，前端页面通过本机 HTTP 服务访问视频，不需要浏览器插件。

## 子项目

| 目录 | 平台与技术栈 | 说明 |
| --- | --- | --- |
| `csharp-sdk-http-bridge/` | Windows、C#、.NET Framework 4.8 | Windows 服务/CLI 版本。支持 HCNetSDK 取流、H.264 直通、H.265 转 H.264；可自动探测可用的 FFmpeg 硬件转码后端并安全回退到软件转码。 |
| `cpp-sdk-http-bridge-linux/` | Linux、C++17 | Linux CLI/便携包版本。功能与 C# 版本保持一致，当前固定使用软件 FFmpeg 转码。支持 portable package 与 AppImage 打包流程。 |

## 核心能力

- 使用 HCNetSDK 登录 NVR，并支持实时预览与指定时间段回放。
- 支持模拟通道、数字真实通道和从零开始的数字通道序号（`digitalIndex`）。
- 优先 H.264 直通；H.265、未知编码或变速回放时转为 H.264，保证主流浏览器兼容性。
- 输出 HTTP chunked fragmented MP4；前端使用 MSE 追加分片，而不是直接把视频 URL 设置为 `<video src>`。
- 支持回放定位、倍速/慢速、会话主动释放、断线恢复、有界缓存与关键日志。
- 建流前检测 NVR SDK 端口；网络不可达时返回 `NVR_UNREACHABLE`，便于前端明确提示设备 IP、端口、网络或防火墙问题。

## 运行前提

两个子项目均依赖以下供应商/第三方组件，二进制文件不纳入本源码仓库：

- 与目标平台和架构匹配的 HCNetSDK 及其运行库。
- 可执行的 FFmpeg；Windows 版本可按配置启用硬件转码，Linux 版本默认软件转码。
- 合法可访问的 NVR、SDK 端口、账号及授权。

详细的配置、构建和部署说明请分别参阅两个子项目内的 `README.md`。

