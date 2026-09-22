#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

SERVICE_NAME=hikbridge
INSTALL_DIR=/usr/local/hikbridge
STATE_DIR=/var/lib/hikbridge
UNIT_FILE=/etc/systemd/system/hikbridge.service
INSTALLED_IMAGE="$INSTALL_DIR/hikbridge.AppImage"

fail() {
    printf '%s\n' "错误：$*" >&2
    exit 1
}

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' "错误：install.sh 必须使用 sudo/root 权限运行。" >&2
    printf '请执行：sudo sh "%s"\n' "$0" >&2
    exit 1
fi

command -v systemctl >/dev/null 2>&1 || fail "系统未安装 systemctl。"
[ -d /run/systemd/system ] || fail "当前系统未使用 systemd 作为服务管理器。"
command -v install >/dev/null 2>&1 || fail "系统缺少 install 命令。"

case "$(uname -m)" in
    x86_64|amd64)
        ARCH=x86_64
        IMAGE_NAME=hik-sdk-http-bridge-0.9.1.260922-x86_64-glibc2.23.AppImage
        ;;
    aarch64|arm64)
        ARCH=aarch64
        IMAGE_NAME=hik-sdk-http-bridge-0.9.1.260922-aarch64-glibc2.23.AppImage
        ;;
    *)
        fail "不支持的 CPU 架构：$(uname -m)。当前仅支持 x86_64 和 aarch64。"
        ;;
esac

SOURCE_IMAGE="$SCRIPT_DIR/$IMAGE_NAME"
[ -f "$SOURCE_IMAGE" ] || fail "缺少当前架构的 AppImage：$SOURCE_IMAGE"
[ -f "$SCRIPT_DIR/uninstall.sh" ] || fail "缺少卸载脚本：$SCRIPT_DIR/uninstall.sh"


printf '当前架构：%s\n' "$ARCH"
printf '安装目录：%s\n' "$INSTALL_DIR"

systemctl disable --now "$SERVICE_NAME.service" >/dev/null 2>&1 || true
install -d -m 0755 "$INSTALL_DIR" "$STATE_DIR"
install -m 0755 "$SOURCE_IMAGE" "$INSTALLED_IMAGE"
install -m 0755 "$SCRIPT_DIR/install.sh" "$INSTALL_DIR/install.sh"
install -m 0755 "$SCRIPT_DIR/uninstall.sh" "$INSTALL_DIR/uninstall.sh"

TEMP_UNIT=$(mktemp "${TMPDIR:-/tmp}/hikbridge.service.XXXXXX") || fail "无法创建临时服务文件。"
cleanup() {
    rm -f "$TEMP_UNIT"
}
trap cleanup EXIT HUP INT TERM

cat > "$TEMP_UNIT" <<EOF
[Unit]
Description=Hik HCNetSDK HTTP Bridge
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=/usr/local/hikbridge
Environment=HOME=/root
Environment=XDG_STATE_HOME=/var/lib/hikbridge
Environment=APPIMAGE_EXTRACT_AND_RUN=1
ExecStart=/usr/local/hikbridge/hikbridge.AppImage
Restart=on-failure
RestartSec=5s
TimeoutStopSec=30s
KillMode=control-group
UMask=0022

[Install]
WantedBy=multi-user.target
EOF

install -m 0644 "$TEMP_UNIT" "$UNIT_FILE" || fail "无法写入服务文件：$UNIT_FILE"
systemctl daemon-reload || fail "systemctl daemon-reload 失败。"
systemctl enable "$SERVICE_NAME.service" || fail "无法设置开机自动启动。"

if ! systemctl restart "$SERVICE_NAME.service"; then
    printf '%s\n' "错误：$SERVICE_NAME 服务启动失败，相关日志如下：" >&2
    journalctl -u "$SERVICE_NAME.service" -n 30 --no-pager >&2 || true
    exit 1
fi

if ! systemctl is-active --quiet "$SERVICE_NAME.service"; then
    printf '%s\n' "错误：$SERVICE_NAME 服务未进入 active 状态。" >&2
    journalctl -u "$SERVICE_NAME.service" -n 30 --no-pager >&2 || true
    exit 1
fi

printf '%s\n' "安装完成：$SERVICE_NAME 服务已启动，并设置为开机自动启动。"
printf '已安装文件：%s、%s/install.sh、%s/uninstall.sh\n' "$INSTALLED_IMAGE" "$INSTALL_DIR" "$INSTALL_DIR"
printf '状态查看：sudo systemctl status %s\n' "$SERVICE_NAME"
printf '日志查看：sudo journalctl -u %s -f\n' "$SERVICE_NAME"
printf '卸载命令：sudo sh %s/uninstall.sh\n' "$INSTALL_DIR"
