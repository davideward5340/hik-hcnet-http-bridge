# 信创 Linux 便携包交付说明

本交付物面向 **x86_64、glibc** 的统信 UOS、Deepin、银河麒麟等 Linux。它不是 Docker/Podman 镜像；客户机解压后执行 `start.sh` 即可运行，无需安装 HCNetSDK、FFmpeg 或 OpenSSL 系统包。

## 支持边界

- 支持目标：x86_64，glibc 版本不低于包内 `release-manifest.env` 的 `REQUIRED_GLIBC`；当前 HCNetSDK 全部组件的最高要求为 glibc 2.27，正式构建基线设为 glibc 2.28。
- 覆盖策略：应在 glibc 2.28 的 x86_64 基线系统构建，例如统信 UOS V20 或银河麒麟 V10 开发环境，再在目标客户系统实测。
- 不支持：Alpine/musl、ARM64、龙芯、MIPS、申威等非 x86_64 架构。它们需要对应架构的海康 HCNetSDK、FFmpeg 和桥接程序重新构建，不能复用本包。
- 不承诺任意版本均可运行；系统安全策略（SELinux、挂载 `noexec`、防火墙）仍需由部署方放行。

## 客户机使用

```sh
tar -xzf hik-sdk-http-bridge-linux-x86_64-1.0.0.tar.gz
cd hik-sdk-http-bridge-linux-x86_64-1.0.0
chmod +x start.sh
./start.sh
```

默认仅监听 `127.0.0.1:28080`，与 Windows C# 版本一致。使用 `curl http://127.0.0.1:28080/healthz` 确认服务状态。

若需改端口或超时，复制并编辑 `config/config.json`，然后执行：

```sh
HIK_BRIDGE_CONFIG="$PWD/config/config.json" ./start.sh
```

若需写入文件日志：

```sh
HIK_BRIDGE_LOG_FILE="$PWD/logs/bridge.log" ./start.sh
```

NVR 用户名和密码只在 `/video` 请求中传递，不应写入 `config.json`、日志或发布包。

## 发布前验收

1. 执行 `./start.sh --version`，确认程序可加载。
2. 执行 `./start.sh`，再调用 `/healthz`。
3. 用真实 NVR 验证 `option=realplay`。
4. 用真实录像时间段验证 `option=playback`、倍速和前端重新请求定位。
5. 关闭浏览器请求，确认日志出现会话结束，NVR 不残留预览/回放会话。
6. 至少在统信、Deepin、麒麟各一台目标版本机器上完成上述验证后再发布。

## 构建机要求

发布二进制必须在 glibc 2.28 的 x86_64 基线构建机生成。建议使用与客户环境隔离的统信 UOS V20、银河麒麟 V10 或 Debian 10 构建环境；Docker/Podman 仅可作为构建机工具，客户机不需要也不使用容器运行时。

不要在 Ubuntu 24.04、Deepin 23 等较新系统中直接编译后交付给旧版统信或麒麟。`scripts/build-portable.sh` 和 `scripts/package-portable.sh` 会识别二进制的 GLIBC 符号版本并拒绝此类产物。
