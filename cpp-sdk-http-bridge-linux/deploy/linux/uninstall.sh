#!/usr/bin/env sh
set -eu

SERVICE_NAME=hikbridge
UNIT_FILE=/etc/systemd/system/hikbridge.service
INSTALL_DIR=/usr/local/hikbridge
STATE_DIR=/var/lib/hikbridge
FOUND=0

fail() {
    printf '%s\n' "错误：$*" >&2
    exit 1
}

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' "错误：uninstall.sh 必须使用 sudo/root 权限运行。" >&2
    printf '请执行：sudo sh "%s"\n' "$0" >&2
    exit 1
fi

if command -v systemctl >/dev/null 2>&1; then
    if systemctl is-active --quiet "$SERVICE_NAME.service" || systemctl is-enabled --quiet "$SERVICE_NAME.service"; then
        FOUND=1
        printf '正在停止并禁用 %s.service...\n' "$SERVICE_NAME"
    fi
    systemctl disable --now "$SERVICE_NAME.service" >/dev/null 2>&1 || true
fi

if [ -f "$UNIT_FILE" ]; then
    FOUND=1
    rm -f -- "$UNIT_FILE" || fail "无法删除服务文件：$UNIT_FILE"
    printf '已删除：%s\n' "$UNIT_FILE"
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || fail "systemctl daemon-reload 失败。"
    systemctl reset-failed "$SERVICE_NAME.service" >/dev/null 2>&1 || true
fi

safe_remove_tree() {
    target=$1
    case "$target" in
        /usr/local/hikbridge|/var/lib/hikbridge) ;;
        *) fail "拒绝删除非预期目录：$target" ;;
    esac
    if [ -e "$target" ]; then
        FOUND=1
        rm -rf -- "$target" || fail "无法删除目录：$target"
        printf '已删除：%s\n' "$target"
    fi
}

safe_remove_tree "$INSTALL_DIR"
safe_remove_tree "$STATE_DIR"

if [ "$FOUND" -eq 0 ]; then
    printf '%s\n' "未检测到已安装的 hikbridge，无需卸载。"
else
    printf '%s\n' "hikbridge 已完全卸载。"
fi

exit 0
