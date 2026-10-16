#!/usr/bin/env bash
# 将交叉编译产物经 GIO/MTP 写入板端 /sdcard/app，保留日志、录像和旧文件备份。
# 用法：bash .script/deploy-mtp.sh <存储根 URI 或 app URI> [--only launcher|dart_app|list]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.script/_rtos-common.sh
source "${SCRIPT_DIR}/_rtos-common.sh"
DEST="${1:-}"
[[ -n "${DEST}" ]] || { echo '需要 gphoto2:// 或 mtp:// 存储 URI' >&2; exit 2; }
shift
ONLY=''
while [[ $# -gt 0 ]]; do
    case "$1" in
        --only) shift; ONLY="${1:-}" ;;
        --only=*) ONLY="${1#--only=}" ;;
        --keep-logs) ;; # MTP 部署始终保留日志和录像。
        *) echo "未知选项：$1" >&2; exit 2 ;;
    esac
    shift
done
case "${ONLY}" in ''|launcher|dart_app|list) ;; *) echo '无效的 --only 目标' >&2; exit 2 ;; esac
case "${DEST}" in gphoto2://*|mtp://*) ;; *) echo '需要 gphoto2:// 或 mtp:// 存储 URI' >&2; exit 2 ;; esac
DEST="${DEST%/}"
if [[ "${DEST##*/}" == app ]]; then
    STORE="${DEST%/app}"
    APP="${DEST}"
else
    STORE="${DEST}"
    APP="${DEST}/app"
fi

die() { printf 'deploy-mtp: %s\n' "$*" >&2; exit 1; }
command -v gio >/dev/null || die '找不到 gio（Ubuntu 由 libglib2.0-bin 提供）'
command -v sha256sum >/dev/null || die '找不到 sha256sum'
if ! gio info "${STORE}/revision.txt" >/dev/null 2>&1; then
    gio info "${APP}/launcher" >/dev/null 2>&1 &&
        gio info "${APP}/startup_final.list" >/dev/null 2>&1 ||
        die '既没有 revision.txt，也没有已部署的 app/launcher 与 startup_final.list，无法确认目标存储'
    printf '根目录没有 revision.txt；已通过现有 app/launcher 和 startup_final.list 确认目标。\n'
fi

NAMES=()
SOURCES=()
for name in launcher dart_app list; do
    [[ -z "${ONLY}" || "${ONLY}" == "${name}" ]] || continue
    if [[ "${name}" == list ]]; then
        NAMES+=(startup_final.list)
        SOURCES+=("${REPO_ROOT}/runtime/launcher/startup.list")
    else
        NAMES+=("${name}")
        SOURCES+=("$(rtos_artifact_path "${name}")")
    fi
done
for src in "${SOURCES[@]}"; do
    [[ -s "${src}" ]] || die "缺少非空产物：${src}"
done

# 不经 /run/user/.../gvfs 的 FUSE 写入，直接使用 URI 交给 GIO。
# gphoto2 后端不支持 chmod，上传成功后通过完整回读校验内容。
remote_hash() { gio cat "$1" | sha256sum | cut -d' ' -f1; }
local_hash() { sha256sum "$1" | cut -d' ' -f1; }
verify() { [[ "$(local_hash "$1")" == "$(remote_hash "$2")" ]]; }
ensure_dir() { gio info "$1" >/dev/null 2>&1 || gio mkdir "$1"; }
ensure_dir "${APP}"

BACKUP_DIR="$(mktemp -d /tmp/dart-mtp-backup.XXXXXX)"
TOKEN="${BACKUP_DIR##*.}-$$"
STAGED=()
HAD_OLD=()
cleanup() {
    local uri
    for uri in "${STAGED[@]}"; do gio remove "${uri}" >/dev/null 2>&1 || true; done
}
trap cleanup EXIT
printf '目标：%s（板端 /sdcard/app）\n旧文件备份：%s\n' "${APP}" "${BACKUP_DIR}"

# 所有旧文件先备份，所有新文件先上传并校验；任何失败都在替换前停止。
for i in "${!NAMES[@]}"; do
    dst="${APP}/${NAMES[i]}"
    if gio info "${dst}" >/dev/null 2>&1; then
        gio copy -T "${dst}" "${BACKUP_DIR}/${NAMES[i]}"
        verify "${BACKUP_DIR}/${NAMES[i]}" "${dst}" || die "旧文件备份校验失败：${NAMES[i]}"
        HAD_OLD+=(1)
    else
        HAD_OLD+=(0)
    fi
    stage="${dst}.upload-${TOKEN}"
    STAGED+=("${stage}")
    gio copy -T "${SOURCES[i]}" "${stage}"
    verify "${SOURCES[i]}" "${stage}" || die "上传回读校验失败：${NAMES[i]}，旧文件未替换"
    printf '  已上传并校验 %s\n' "${NAMES[i]}"
done

for i in "${!NAMES[@]}"; do
    dst="${APP}/${NAMES[i]}"
    old="${dst}.previous-${TOKEN}"
    if [[ "${HAD_OLD[i]}" == 1 ]]; then
        gio move -T --no-copy-fallback "${dst}" "${old}"
    fi
    if ! gio move -T --no-copy-fallback "${STAGED[i]}" "${dst}"; then
        if [[ "${HAD_OLD[i]}" == 1 ]]; then
            gio move -T --no-copy-fallback "${old}" "${dst}" \
                || die "替换和恢复均失败；旧文件位于 ${old}，主机备份位于 ${BACKUP_DIR}"
        fi
        die "替换失败：${NAMES[i]}；主机备份位于 ${BACKUP_DIR}"
    fi
    if ! verify "${SOURCES[i]}" "${dst}"; then
        # 将失败的新文件移开，恢复原名；本地主机备份始终保留。
        gio move -T --no-copy-fallback "${dst}" "${STAGED[i]}" \
            || die "最终校验失败且无法移开新文件：${dst}；旧文件/备份保留"
        if [[ "${HAD_OLD[i]}" == 1 ]]; then
            gio move -T --no-copy-fallback "${old}" "${dst}" \
                || die "恢复失败；旧文件位于 ${old}，主机备份位于 ${BACKUP_DIR}"
        fi
        die "最终校验失败：${NAMES[i]}；主机备份位于 ${BACKUP_DIR}"
    fi
    if [[ "${HAD_OLD[i]}" == 1 ]]; then gio remove "${old}"; fi
    printf '  %s：%s bytes，SHA-256 %s\n' "${NAMES[i]}" \
        "$(stat -c %s "${SOURCES[i]}")" "$(local_hash "${SOURCES[i]}")"
done

if [[ -z "${ONLY}" ]]; then
    ensure_dir "${APP}/logs"
    ensure_dir "${APP}/records"
fi
printf '\n部署完成；日志和录像已保留。旧文件备份：%s\n' "${BACKUP_DIR}"
printf '板端验证：/sdcard/app/launcher -t\n'
printf '注意：写入不会重启正在运行的进程；请对板子执行一次完整复位/断电重启后，最新 ELF 和 startup_final.list 才会由开机自启动器加载。\n'
