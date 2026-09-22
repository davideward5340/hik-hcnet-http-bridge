# Windows 与 Linux 全量打包发布规则

本文是本项目正式全量包的发布依据。源码目录为 `cpp-sdk-http-bridge-linux`；以下命令除特别说明外均在该目录执行。不得继续依赖 `temp/` 中某次发布的脚本、旧压缩包或旧构建日志作为唯一发布流程。

## 1. 版本、命名与发布产物

版本来源为 `src/version.h`：

- `HIK_BRIDGE_VERSION`：完整对外版本，规则 `发布版本.YYMMDD`，例如 `0.9.1.260922`。
- `HIK_BRIDGE_RELEASE_VERSION`：发布版本，例如 `0.9.1`。
- `HIK_BRIDGE_BUILD_DATE`：打包日期，例如 `2026-09-22`，必须与完整版本日期一致。
- `HIK_BRIDGE_FILE_VERSION`：Windows 数字版本，每段最多 65535；当前为 `0,9,1,0`。字符串文件/产品版本仍显示完整版本号。

修改版本后先重新构建，再打包，不得仅重命名 EXE、AppImage 或修改 version.json 冒充新版本。纯源码目录整理不需要改变运行版本。同日再次发布行为不同的二进制，必须能通过构建记录、提交及文件哈希区分。

固定的正式归档名：

| 平台 | 归档 | 配套校验文件 |
| --- | --- | --- |
| Windows | `hikbridge.zip` | `hikbridge.zip.sha256` |
| Linux | `hikbridge.tar.gz` | `hikbridge.tar.gz.sha256` |

两个压缩包均只有一个顶层目录 `hikbridge/`，内部包含 `version.json`、使用说明和本次更新说明。版本不追加到外层归档名，方便沿用现有下载/分发入口。

正式全量组装工具为 `scripts/package-full-release.py`，需要 Python 3.11 或以上；这是构建机要求，客户机不需要 Python。默认输出到 `dist/releases`。传入 `--output ..` 可输出到仓库根目录，与既有交付位置一致。覆盖同名产物前，工具将上一版归档及校验文件备份到输出目录的 `.release-backups/<时间-PID>/`。

## 2. Windows 发布规则

### 内容与目录

```text
hikbridge/
├── hik-sdk-http-bridge.exe
├── hik-bridge-upgrade.exe
├── config.json
├── version.json
├── 安装.bat
├── 卸载.bat
├── 升级.bat
├── run-bridge.cmd
├── windows-service.cmd
├── hcnetsdk/                  # 完整 HCNetSDK 运行文件及组件
├── ffmpeg/                    # FFmpeg 与配套运行库
├── logs/                      # 空目录
├── state/                     # 空目录
├── LICENSE
├── NOTICE
├── 使用说明.txt
├── 升级说明.txt
└── 本次更新说明.txt
```

来源约定：

- 主程序、升级工具、SDK/FFmpeg 运行目录来自本次已验证的 Windows 构建目录。
- BAT/CMD 直接来自 `deploy/windows/`，文件名保持不变；CMake 默认构建也同步这些脚本到构建交付目录。
- 正式包配置来自 `config/config.windows.json`。不能把构建机调试端口、设备配置或已运行过的旧 build/config.json 当作发布默认配置。
- `ffmpeglibs/` 提供的全部 DLL 必须包含在 ffmpeg 目录，且逐个哈希一致，禁止只打包 ffmpeg.exe。
- 当前规则仍为 x86 桥接主程序、x86 升级工具、x86 海康 SDK，加 x64 FFmpeg；因此当前全量包要求 64 位 Windows。没有在本次目录整理中引入 FFmpeg 自动选位数。
- 禁止混入测试服务程序、PDB、运行日志、设备缓存、安装备份、升级事务备份和真实设备凭据。
- 安装为普通自动启动；升级为整目录备份后全量替换，原配置也会被新版配置替换。升级工具必须与“升级.bat”同时分发。

### 构建、测试与组装

构建机需要 Visual Studio 2022 C++ Build Tools（含 CMake、Ninja）及获授权的海康 Win32 SDK。SDK 包目录需含 `头文件/HCNetSDK.h` 和 `库文件/HCNetSDK.lib`；Runtime 目录需含 hcnetsdk、ffmpeg。

```powershell
# 示例版本应与 src/version.h 一致；SDK 路径请替换为本机实际路径。
./scripts/build-windows.ps1 -Configuration Release -BuildTests `
  -SdkPackage "<海康 Win32 SDK 根目录>" `
  -Runtime "../csharp-sdk-http-bridge/runtime" `
  -BuildDirectory "./build-win32-0.9.1.260922"

python tests/windows-batch.py `
  --stub build-win32-0.9.1.260922/hik-cmd-tool-stub.exe `
  --source deploy/windows `
  --output dist/verify-windows-batch

python tests/runtime-smoke.py `
  --exe build-win32-0.9.1.260922/hik-sdk-http-bridge.exe `
  --sdk build-win32-0.9.1.260922/hcnetsdk `
  --ffmpeg build-win32-0.9.1.260922/ffmpeg/ffmpeg.exe `
  --config config/config.windows.json `
  --output dist/verify-windows-runtime

python scripts/package-full-release.py --platform windows `
  --windows-build build-win32-0.9.1.260922 --output ..
```

windows-batch 测试的输出目录必须是新目录；重跑时换一个新名称，避免覆盖上一次证据。构建中的 CTest 应包含生命周期和升级单元测试。涉及服务安装、迁移、回滚逻辑变更时，应在管理员环境运行 `tests/windows-upgrade-integration.py`，使用其专门的测试服务名，不得拿真实客户服务做回归测试。

组装工具会核对主程序命令行版本（在 Windows 上运行时）、构建版本清单、PE 架构、脚本编码、DLL 哈希及 ZIP 完整性。还应在隔离目录中验证包内 FFmpeg 启动和 H.264 编码、健康/版本接口，以及安装脚本 `/check`。不得把“文件复制完成”当作“已安装并运行验证通过”。

## 3. Linux 发布规则

### 内容与目录

```text
hikbridge/
├── install.sh
├── uninstall.sh
├── start.sh
├── hik-sdk-http-bridge-<版本>-x86_64-glibc2.23.AppImage
├── hik-sdk-http-bridge-<版本>-x86_64-glibc2.23.AppImage.sha256
├── hik-sdk-http-bridge-<版本>-aarch64-glibc2.23.AppImage
├── hik-sdk-http-bridge-<版本>-aarch64-glibc2.23.AppImage.sha256
├── version.json
├── 使用说明.txt
└── 本次更新说明.txt
```

- Linux 正式全量包同时包含 x86_64 和 aarch64，不能只更新其中一个架构。
- 两个 AppImage 都必须由本次源码构建，内部包含对应架构的桥接程序、HCNetSDK、静态 FFmpeg、配置、运行入口及许可证说明。
- 延续 glibc 2.23 目标和 `-glibc2.23.AppImage` 命名；构建脚本检查实际要求不得超过基线。名称中的目标版本不代表已经在每种旧发行版上实测。
- `deploy/linux/install.sh`、`uninstall.sh`、`start.sh` 复制到包根目录。组装时将 AppImage 文件名版本统一替换为 version.h 的版本，不应手工选择旧 AppImage。
- SH 文件使用 UTF-8、LF；tar 中目录、SH、AppImage 权限为 0755，其他文件 0644；uid/gid 为 0，uname/gname 为 root。
- `install.sh` 不执行 SHA-256 校验，也不依赖 sha256sum 或 .sha256 文件。必要文件存在性、架构、root 和 systemd 检查保留。
- 构建/打包阶段仍必须校验文件完整性，并提供校验文件供发布者或用户人工核验；取消安装时校验不等于取消发布质量检查。
- AppImage 使用解压运行路径以兼容无 FUSE 环境。客户机无需另装 FFmpeg；依然需要兼容的 glibc，不能宣称支持 musl/Alpine。

### 构建、测试与组装

在 Linux 或已配置工具链的 WSL 中执行。需要项目构建脚本所指定的 Zig、readelf、patchelf、strip、AppImage runtime/appimagetool，以及两个架构的 SDK、静态 FFmpeg 和许可证。构建不应从未知网站临时替换厂商二进制。

```sh
sh scripts/package-appimage-x86_64-glibc223.sh
sh scripts/package-appimage-arm64-glibc223.sh

python3 tests/linux-installer.py

python3 scripts/package-full-release.py --platform linux --output ..
```

两条 AppImage 构建命令会进行架构、静态 FFmpeg 和 glibc 基线检查。`VERSION` 环境变量不得与 version.h 不一致。正式组装工具会按 version.h 寻找两个 AppImage，校验架构及其 SHA-256，再生成全量 tar.gz。

发布前应对两个 AppImage 做提取、`--version`、配置验证、SDK 初始化、`/healthz`、`/version`、`/cleanSessions` 和退出释放测试。Linux 生命周期测试必须覆盖监听描述符防继承、并发 posix_spawn、HTTP 连接上限及 FFmpeg 清理。ARM64 可在真实 ARM64 主机测试，或使用配置完整的 QEMU/sysroot；记录使用了哪种方式，不将模拟器测试写成真实 ARM 设备验证。

`tests/linux-installer.py` 在临时目录配合模拟 systemctl 验证缺少校验文件时仍可安装，不会执行真实系统安装。实际安装应在测试机上另行检查开机自启、停止/重启和卸载行为。

## 4. 两个平台一起组装

在 Windows 构建完成且两个 Linux AppImage 已输出到本项目 dist 后，可以在 Windows 构建机一次生成两个全量包：

```powershell
python scripts/package-full-release.py --platform all `
  --windows-build build-win32-0.9.1.260922 `
  --linux-dist dist --output ..
```

工具不重新编译程序，也不安装或重启任何服务。它仅组装和校验已经构建的产物。发布前更新 `deploy/RELEASE_NOTES.txt`，说明模板中的版本和日期由工具替换。

发布记录至少保留：源码提交或工作区变更说明、完整版本、构建日期、SDK/FFmpeg 来源及版本、构建命令、测试日志、两个归档的 SHA-256，以及是否进行了真实 NVR 长稳测试。全量组装成功不代替实机性能和故障恢复验收。

## 5. 可选交付方式

`deploy/linux/portable/start.sh` 只用于 `scripts/package-portable.sh` 生成的解压式便携包；`deploy/linux/oci/run.sh` 只用于 OCI 交付。这两类不是当前 hikbridge.tar.gz 的默认形式，不要将它们的启动脚本混入 AppImage 全量包。

Windows 与 Linux 安装包中包含的第三方组件须按其许可证及海康 SDK 授权范围分发，项目代码的开源许可证不能替代第三方组件授权。
