# 部署目录

本目录保存交付给使用者的部署脚本及使用说明。编译、构建依赖和压缩包组装工具保留在 `scripts/`，回归测试保留在 `tests/`。

```text
deploy/
├── README.md
├── RELEASE.md                 # Windows/Linux 正式打包发布规则
├── RELEASE_NOTES.txt          # 发布说明模板
├── windows/
│   ├── 安装.bat
│   ├── 卸载.bat
│   ├── 升级.bat
│   ├── run-bridge.cmd
│   ├── windows-service.cmd    # 安装/卸载共用实现
│   └── USAGE.txt              # Windows 使用说明模板
└── linux/
    ├── install.sh             # 双架构 AppImage 全量包的安装入口
    ├── uninstall.sh
    ├── start.sh
    ├── USAGE.txt
    ├── portable/start.sh      # 可选解压式便携包专用入口
    └── oci/run.sh             # 可选 OCI 容器部署入口
```

源码的分类目录不会原样套入正式压缩包。正式包仍以 `hikbridge/` 为唯一根目录，Windows 的 BAT/CMD 和 Linux 的 install.sh、uninstall.sh、start.sh 都放在根目录。脚本依据自身目录寻找主程序与配置，请勿只移动单个脚本。

Windows 脚本工作区编码为 GBK、CRLF、无 BOM，Git 属性负责仓库存储与检出转换；Linux SH 使用 UTF-8、LF。使用说明模板为 UTF-8，组装时替换 `@VERSION@`、`@BUILD_DATE@`。Linux AppImage 文件名中的版本号也由正式组装工具统一替换为 `src/version.h` 的值。

旧 `scripts/windows-*.cmd`、`scripts/install-linux.sh`、`scripts/uninstall-linux.sh`、`scripts/start-appimage.sh` 等路径已迁移，不保留第二份副本。历史 temp 目录仅作历史记录，不再作为发布入口。

具体命令、包内容、命名、校验和发布顺序见 [RELEASE.md](RELEASE.md)。
