#!/usr/bin/env bash
# ============================================================================
# .script/ 构建、部署和清理脚本共用的
# 定位、环境加载与板端访问逻辑。**不直接执行，只被 source。**
#
# 提供:
#   REPO_ROOT                 仓库根目录（由调用脚本的路径反推，与当前 cwd 无关）
#   BOARD_USER / BOARD_DIR    板端用户 / 部署目录（默认 root / /sdcard/app）
#   RTOS_MODE / RTOS_TARGET   传输方式（scp|card）与其目标（板子 IP 或卡/挂载点）
#   RTOS_MTP                  1 = 目标是板子的 MTP/gphoto2 挂载（不是真读卡器）
#   RTOS_DIR                  实际使用的部署目录（card 模式下解析成卡上的 app 目录）
#
#   rtos_resolve_transport    解析传输方式；带连通性/目录预检，失败即 die
#   rtos_resolve_local_path   gphoto2://… / mtp://… / file://… -> 本地路径
#   rtos_find_mtp_sdcard      找出板子 MTP 挂载里对应 /sdcard 的 store（判据：根目录有 revision.txt）
#   rtos_ssh <cmd...>         在板子上执行命令（card 模式下拒绝）
#   rtos_remove <path...>     删除板端/卡上的路径（两种模式通用）
#   rtos_list_children <dir>  列出目录下条目名（不含 . 与 ..；兼容多列输出）
#   rtos_artifact_path <目标> 根据构建目录配置返回产物绝对路径
#   info / warn / die         统一输出（支持 NO_COLOR）
#
# 环境变量（都没设置时请看 .script/board.env.example）:
#   BOARD_IP    板子 IP；设置后走 ssh/scp 模式
#   SDCARD      卡挂载点、app 目录，或板子 MTP 的 URI/挂载路径；BOARD_IP 未设置时用它
#   BOARD_USER  ssh 用户（默认 root）
#   BOARD_DIR   板端部署目录（默认 /sdcard/app）
# 也可把上面几个写进 .script/board.env（本文件会自动读取，且不覆盖已有环境变量）。
# ============================================================================

RTOS_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[1]}")" && pwd)"
REPO_ROOT="$(cd "${RTOS_SCRIPT_DIR}/.." && pwd)"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    _c_info=$'\033[32m' _c_warn=$'\033[33m' _c_err=$'\033[31m' _c_off=$'\033[0m'
else
    _c_info='' _c_warn='' _c_err='' _c_off=''
fi

info() { printf '%s[rtos]%s %s\n' "${_c_info}" "${_c_off}" "$*"; }
warn() { printf '%s[rtos]%s %s\n' "${_c_warn}" "${_c_off}" "$*" >&2; }
die() {
    printf '%s[rtos]%s %s\n' "${_c_err}" "${_c_off}" "$*" >&2
    exit 1
}

# --- .script/board.env：只填未设置的变量，命令行/环境里的值优先 -----------------
_rtos_board_env="${RTOS_SCRIPT_DIR}/board.env"
if [[ -f "${_rtos_board_env}" ]]; then
    while IFS= read -r _line || [[ -n "${_line}" ]]; do
        _line="${_line%%#*}"
        [[ "${_line}" == *=* ]] || continue
        _key="${_line%%=*}"
        _val="${_line#*=}"
        _key="${_key//[[:space:]]/}"
        _val="${_val#"${_val%%[![:space:]]*}"}"
        _val="${_val%"${_val##*[![:space:]]}"}"
        _val="${_val%\"}"; _val="${_val#\"}"
        _val="${_val%\'}"; _val="${_val#\'}"
        [[ "${_key}" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        if [[ -z "${!_key:-}" ]]; then
            export "${_key}=${_val}"
        fi
    done < "${_rtos_board_env}"
    unset _line _key _val
fi

BOARD_USER="${BOARD_USER:-root}"
BOARD_DIR="${BOARD_DIR:-/sdcard/app}"

rtos_usage_env() {
    cat <<'EOF'
需要先告诉脚本板子在哪（三选一）:

  export BOARD_IP=192.168.1.100     # 网络模式：ssh/scp
  export SDCARD=/media/$USER/<卡>   # 读卡器模式：直接写已挂载的 SD 卡
  export SDCARD='gphoto2://Kendryte_CanMV_001000000/store_ffff0001'
                                    # 板子用 USB 挂着（MTP/gphoto2），store_ffff0001 = 板端 /sdcard

MTP 挂载点也可以直接填本地路径：
  export SDCARD=/run/user/1000/gvfs/gphoto2:host=Kendryte_CanMV_001000000/store_ffff0001

也可以把上面几行写进 .script/board.env（见 .script/board.env.example）。
可选：BOARD_USER（默认 root）、BOARD_DIR（默认 /sdcard/app）。
EOF
}

# 把 gphoto2://<host>/<store> / mtp://<host>/<store> / file:///path 解析成本地路径。
# 注意：gvfs 的目录名里带冒号（gphoto2:host=...），所以这里必须整段当字符串处理。
rtos_resolve_local_path() {
    local p="$1" scheme host rest sub runtime
    case "${p}" in
    gphoto2://* | mtp://*)
        scheme="${p%%://*}"
        rest="${p#*://}"
        host="${rest%%/*}"
        sub=""
        [[ "${rest}" == */* ]] && sub="${rest#*/}"
        runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
        p="${runtime}/gvfs/${scheme}:host=${host}"
        [[ -n "${sub}" ]] && p="${p}/${sub}"
        ;;
    file://*) p="${p#file://}" ;;
    esac
    printf '%s' "${p}"
}

# 板子以 MTP 挂着时，帮用户把 /sdcard 对应的 store 找出来：
# 判据是根目录有 revision.txt —— 固件构建只把 revision.txt 写进 SDCARD 分区
# （kRTOSSDK tools/gen_image.sh / boards/*/genimage-sdcard.cfg），
# 且 MTP 里 /sdcard 先于 /data 注册，所以它就是 store_ffff0001。
rtos_find_mtp_sdcard() {
    local runtime base host store
    runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    base="${runtime}/gvfs"
    [[ -d "${base}" ]] || return 0
    for host in "${base}"/*:host=*; do
        [[ -d "${host}" ]] || continue
        for store in "${host}"/*; do
            [[ -d "${store}" ]] || continue
            if [[ -f "${store}/revision.txt" ]]; then
                printf '%s' "${store}"
                return 0
            fi
        done
    done
}

# 解析出 RTOS_MODE / RTOS_TARGET / RTOS_DIR，并做预检。
rtos_resolve_transport() {
    if [[ -n "${BOARD_IP:-}" ]]; then
        RTOS_MODE=scp
        RTOS_TARGET="${BOARD_IP}"
        RTOS_DIR="${BOARD_DIR}"
        [[ "${RTOS_DIR}" == /* ]] || die "BOARD_DIR 必须是绝对路径，当前为 '${RTOS_DIR}'"
        info "板子: ${BOARD_USER}@${BOARD_IP} 目录: ${RTOS_DIR}  (ssh/scp)"
        command -v ssh >/dev/null 2>&1 || die "找不到 ssh；改用读卡器模式（export SDCARD=...）"
        if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${BOARD_USER}@${RTOS_TARGET}" true 2>/dev/null; then
            die "连不上 ${BOARD_USER}@${BOARD_IP}（需要免密 ssh）。检查 IP/网络，或改用 export SDCARD=<卡挂载点>"
        fi
        return 0
    fi

    if [[ -n "${SDCARD:-}" ]]; then
        local given="${SDCARD}"
        SDCARD="$(rtos_resolve_local_path "${SDCARD}")"
        RTOS_MODE=card
        RTOS_TARGET="${SDCARD}"
        RTOS_MTP=0
        [[ "${SDCARD}" == */gvfs/* ]] && RTOS_MTP=1
        if [[ -d "${SDCARD}/app" ]]; then
            RTOS_DIR="${SDCARD}/app"
        elif [[ "$(basename "${SDCARD}")" == "app" ]]; then
            RTOS_DIR="${SDCARD}"
        else
            RTOS_DIR="${SDCARD}/app"
        fi
        [[ "${SDCARD}" == /* ]] || die "SDCARD 必须是绝对路径，当前为 '${SDCARD}'"
        [[ "${SDCARD}" != "/" ]] || die "SDCARD 不能是 /（那会把 /app 当成板端目录）"
        if [[ ! -d "${SDCARD}" ]]; then
            warn "SDCARD 目录不存在: ${SDCARD}"
            if [[ "${given}" != "${SDCARD}" ]]; then
                warn "（由 ${given} 解析而来；MTP 挂着时先用 gio mount -l 看实际 URI）"
            fi
            die "先挂载：udisksctl mount -b /dev/sdX2，或确认 gphoto2 挂载点还在"
        fi
        if [[ "${RTOS_MTP}" == "1" ]]; then
            info "板端挂载(MTP/gphoto2): ${SDCARD} -> ${RTOS_DIR}"
            # 板子同时暴露 /sdcard(store_ffff0001) 与 /data(store_ffff0002)，
            # 只有 /sdcard 会在启动时被读到。判据：SDCARD 分区根目录有 revision.txt
            # （见 SDK tools/gen_image.sh 与 boards/*/genimage-sdcard.cfg）。
            local store_root="${SDCARD}"
            if [[ "$(basename "${store_root}")" == "app" ]]; then
                store_root="$(dirname "${store_root}")"
            fi
            if [[ -f "${store_root}/revision.txt" ]]; then
                info "已确认它是板端 /sdcard（分区根目录有 revision.txt）"
            else
                warn "注意：${store_root} 根目录没有 revision.txt，它很可能**不是**板端 /sdcard（而是 /data）！"
                warn "      板子只会从 /sdcard/app/ 读 launcher 与清单，写错地方上电不会生效。"
                warn "      用 gio mount -l 看挂载 URI，选根目录带 revision.txt 的那个 store。"
            fi
            info "MTP 部署使用 GIO URI 上传、SHA-256 回读校验，并保留旧文件备份"
        else
            info "SD 卡: ${SDCARD} -> app 目录: ${RTOS_DIR}  (读卡器)"
        fi
        return 0
    fi

    local mtp_hint
    mtp_hint="$(rtos_find_mtp_sdcard || true)"
    if [[ -n "${mtp_hint}" ]]; then
        warn "检测到板子已用 MTP 挂着，/sdcard 应该是: ${mtp_hint}"
        warn "  export SDCARD='${mtp_hint}'"
    fi
    warn "既没有设置 BOARD_IP，也没有设置 SDCARD。"
    rtos_usage_env >&2
    exit 2
}

# 删除前的安全护栏：只允许动绝对路径、拒绝根目录、拒绝 ".."。
rtos_assert_safe_path() {
    local path="$1"
    if [[ -z "${path}" ]]; then
        die "内部错误：空路径"
    fi
    if [[ "${path}" != /* ]]; then
        die "拒绝删除非绝对路径: ${path}"
    fi
    if [[ "${path}" == "/" ]]; then
        die "拒绝删除根目录"
    fi
    if [[ "${path}" == *".."* ]]; then
        die "拒绝删除含 .. 的路径: ${path}"
    fi
    return 0
}

rtos_ssh() {
    [[ "${RTOS_MODE}" == "scp" ]] || die "当前是读卡器模式，不能执行远程命令"
    ssh -o ConnectTimeout=10 "${BOARD_USER}@${RTOS_TARGET}" "$@"
}

# 删除路径（文件或目录）。两种模式通用；远程只依赖板端 ssh 会话里已有的 rm。
rtos_remove() {
    local path="$1"
    rtos_assert_safe_path "${path}"
    if [[ "${RTOS_MODE}" == "scp" ]]; then
        rtos_ssh "rm -rf -- '${path}'"
    else
        rm -rf -- "${path}"
    fi
}

# 列出目录下的条目名。板端 msh 的 ls 可能是多列输出，这里统一拆成一行一个；
# 目录不存在时返回空而不是报错。
rtos_list_children() {
    local dir="$1" out=''
    rtos_assert_safe_path "${dir}"
    if [[ "${RTOS_MODE}" == "scp" ]]; then
        out="$(rtos_ssh "ls -a '${dir}'" 2>/dev/null || true)"
    else
        [[ -d "${dir}" ]] || return 0
        out="$(ls -A "${dir}" 2>/dev/null || true)"
    fi
    printf '%s\n' "${out}" \
        | tr -s ' \t' '\n' \
        | grep -vE '^(\.|\.\.)?$' || true
}

# 构建与各部署方式使用相同的产物目录；BUILD_DIR 兼容旧部署配置。
rtos_artifact_path() {
    local target="$1" build_dir="${DART_BUILD_DIR:-${BUILD_DIR:-build-cross}}"
    [[ "${build_dir}" == /* ]] || build_dir="${REPO_ROOT}/${build_dir}"
    printf '%s/%s/%s' "${build_dir}" "${target}" "${target}"
}
