# AppImage 交付说明

本文件由 `scripts/package-appimage.sh` 生成。它将桥接程序、完整 HCNetSDK 目录、`HCNetSDKCom`、SDK 自带 OpenSSL 和静态 FFmpeg 封装为一个 x86_64 AppImage 文件。

## 构建前提

1. 必须在 glibc 2.28 x86_64 基线系统完成 `./scripts/build-portable.sh`。
2. 必须用 `./scripts/build-static-ffmpeg.sh` 生成 `runtime/ffmpeg/ffmpeg`。
3. 将官方 `appimagetool-x86_64.AppImage` 放入 `tools/`，或用 `APPIMAGETOOL=/path/to/appimagetool` 指定路径。

随后执行：

```sh
./scripts/package-appimage.sh
```

输出：

```text
dist/hik-sdk-http-bridge-1.0.0-x86_64.AppImage
dist/hik-sdk-http-bridge-1.0.0-x86_64.AppImage.sha256
```

## 客户机运行

```sh
chmod +x hik-sdk-http-bridge-1.0.0-x86_64.AppImage
./hik-sdk-http-bridge-1.0.0-x86_64.AppImage
```

如系统不允许 FUSE 挂载，使用内置降级模式，不需要安装 Docker 或 Podman：

```sh
./hik-sdk-http-bridge-1.0.0-x86_64.AppImage --appimage-extract-and-run
```

自定义配置和日志必须放在 AppImage 外部：

```sh
HIK_BRIDGE_CONFIG=/opt/hik-bridge/config.json \
HIK_BRIDGE_LOG_FILE=/var/log/hik-bridge/bridge.log \
./hik-sdk-http-bridge-1.0.0-x86_64.AppImage
```

该 AppImage 仍要求 x86_64 和不低于包内清单指定版本的 glibc；当前 SDK 组件最高要求为 glibc 2.27，交付时仍以 glibc 2.28 作为构建基线。不支持 ARM、龙芯、MIPS、申威或 Alpine/musl。
