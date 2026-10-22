#!/usr/bin/env bash
# 构建/部署/清理共用的定位、配置和存储访问层；只被 source。
# 程序默认 /sdcard/app，数据默认 /data/app。
# BOARD_IP 选择 SSH；否则 SDCARD / DATA_SDCARD 选择 MTP URI 或读卡器分区。
# MTP 始终通过 GIO 操作；rtos_list_entries 不跟随符号链接。

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
BOARD_DATA_DIR="${BOARD_DATA_DIR:-/data/app}"

rtos_usage_env() {
    cat <<'EOF'
默认使用 USB/MTP 板子，也可在环境变量或 .script/board.env 配置：
  BOARD_IP=192.168.1.100       # SSH/scp（优先）
  SDCARD=/media/$USER/SDCARD   # /sdcard 分区的挂载点或 app 目录
  DATA_SDCARD=/media/$USER/DATA # /data 分区的挂载点或 app 目录
  SDCARD=gphoto2://Kendryte_CanMV_001000000/store_ffff0001
  DATA_SDCARD=gphoto2://Kendryte_CanMV_001000000/store_ffff0002
可选：BOARD_USER=root、BOARD_DIR=/sdcard/app、BOARD_DATA_DIR=/data/app。
读卡器全量部署或清理数据时需要同时指定 DATA_SDCARD。
EOF
}

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
        [[ -z "${sub}" ]] || p="${p}/${sub}"
        ;;
    file://*) p="${p#file://}" ;;
    esac
    printf '%s' "${p}"
}

# 将分区根或应用目录转换为统一位置；URI 始终由 GIO 操作，不写 GVfs FUSE。
# 输出 RTOS_LOCATION / RTOS_STORE。
rtos_storage_location() {
    local given="${1%/}" mount="$2" board_dir="${3%/}" relative rest host store
    [[ "${board_dir}" == "/${mount}/"* ]] || die "板端目录必须位于 /${mount}/ 下：${board_dir}"
    rtos_assert_safe_path "${board_dir}"
    relative="${board_dir#"/${mount}/"}"
    if [[ "${given}" == */gvfs/* ]]; then
        command -v gio >/dev/null || die "找不到 gio"
        given="$(LC_ALL=C gio info "${given}" | sed -n 's/^uri: //p')"
        [[ -n "${given}" ]] || die "无法取得 MTP URI"
        given="${given%/}"
    fi
    case "${given}" in
    gphoto2://* | mtp://*)
        rest="${given#*://}"
        host="${rest%%/*}"
        [[ "${rest}" == */* && -n "${host}" ]] || die "需要存储根 URI：${given}"
        rest="${rest#*/}"
        store="${rest%%/*}"
        [[ "${store}" == store_* ]] || die "需要 MTP store URI：${given}"
        RTOS_STORE="${given%%://*}://${host}/${store}"
        [[ "${given}" == "${RTOS_STORE}" || "${given}" == "${RTOS_STORE}/${relative}" ]] ||
            die "请指定分区根或 ${relative} 目录：${given}"
        RTOS_LOCATION="${RTOS_STORE}/${relative}"
        ;;
    *)
        given="$(rtos_resolve_local_path "${given}")"
        rtos_assert_safe_path "${given}"
        [[ -d "${given}" ]] || die "分区未挂载或目录不存在：${given}"
        given="$(realpath -e -- "${given}")"
        if [[ "${given}" == */"${relative}" ]]; then
            RTOS_STORE="${given%"/${relative}"}"
        else
            RTOS_STORE="${given}"
        fi
        rtos_assert_safe_path "${RTOS_STORE}"
        RTOS_LOCATION="${RTOS_STORE}/${relative}"
        ;;
    esac
}

rtos_resolve_transport() {
    RTOS_DATA_DIR=''
    RTOS_DATA_STORE=''
    if [[ -n "${BOARD_IP:-}" ]]; then
        RTOS_MODE=scp
        RTOS_TARGET="${BOARD_IP}"
        RTOS_DIR="${BOARD_DIR}"
        rtos_assert_safe_path "${RTOS_DIR}"
        command -v ssh >/dev/null || die "找不到 ssh"
        ssh -o ConnectTimeout=5 -o BatchMode=yes "${BOARD_USER}@${RTOS_TARGET}" true ||
            die "连不上 ${BOARD_USER}@${BOARD_IP}（需要免密 SSH）"
        info "板子：${BOARD_USER}@${BOARD_IP}；程序目录：${RTOS_DIR}"
        return
    fi
    rtos_storage_location "${SDCARD:-gphoto2://Kendryte_CanMV_001000000/store_ffff0001}" sdcard "${BOARD_DIR}"
    RTOS_DIR="${RTOS_LOCATION}"
    RTOS_TARGET="${RTOS_STORE}"
    RTOS_MTP=0
    case "${RTOS_TARGET}" in
    gphoto2://* | mtp://*)
        RTOS_MODE=mtp
        RTOS_MTP=1
        command -v gio >/dev/null || die "找不到 gio"
        if ! gio info "${RTOS_STORE}" >/dev/null 2>&1; then
            local device="${RTOS_STORE%/*}/"
            gio mount "${device}" || die "无法挂载 USB 板子"
            gio info "${RTOS_STORE}" >/dev/null || die "无法访问存储：${RTOS_STORE}"
        fi
        if ! gio info "${RTOS_STORE}/revision.txt" >/dev/null 2>&1; then
            gio info "${RTOS_DIR}/launcher" >/dev/null 2>&1 &&
                gio info "${RTOS_DIR}/startup_final.list" >/dev/null 2>&1 ||
                die "没有 revision.txt 或已部署的 launcher 与清单，无法确认 /sdcard 分区"
        fi
        info "USB/MTP 程序目录：${RTOS_DIR}"
        ;;
    *)
        RTOS_MODE=card
        info "读卡器程序目录：${RTOS_DIR}"
        ;;
    esac
}

# 数据分区独立于 ELF 所在分区；仅在需要操作数据时解析。
rtos_resolve_data_transport() {
    [[ -z "${RTOS_DATA_DIR}" ]] || return 0
    if [[ "${RTOS_MODE}" == scp ]]; then
        rtos_assert_safe_path "${BOARD_DATA_DIR}"
        RTOS_DATA_DIR="${BOARD_DATA_DIR}"
        [[ "${RTOS_DATA_DIR}" != "${RTOS_DIR}" ]] || die "程序与数据不能指向同一个目录"
        return
    fi
    local data="${DATA_SDCARD:-}" app_store="${RTOS_TARGET}"
    if [[ -z "${data}" && "${RTOS_MODE}" == mtp ]]; then
        [[ "${RTOS_TARGET}" == */store_ffff0001 ]] ||
            die "无法推断 /data 分区，请设置 DATA_SDCARD"
        data="${RTOS_TARGET%/*}/store_ffff0002"
    fi
    [[ -n "${data}" ]] || die "日志/录像在独立的 /data 分区，请设置 DATA_SDCARD（分区挂载点或 app 目录）"
    rtos_storage_location "${data}" data "${BOARD_DATA_DIR}"
    RTOS_DATA_DIR="${RTOS_LOCATION}"
    RTOS_DATA_STORE="${RTOS_STORE}"
    [[ "${RTOS_DATA_STORE}" != "${app_store}" ]] || die "程序与数据不能指向同一个分区"
    if [[ "${RTOS_MODE}" == mtp ]]; then
        [[ "${RTOS_DATA_STORE%/*}" == "${app_store%/*}" ]] || die "数据分区必须来自同一台 MTP 板子"
        gio info "${RTOS_DATA_STORE}" >/dev/null || die "无法访问 /data 分区，请检查 DATA_SDCARD"
        if gio info "${RTOS_DATA_STORE}/revision.txt" >/dev/null 2>&1; then
            die "数据目标含 revision.txt，疑似 /sdcard 分区，请检查 DATA_SDCARD"
        fi
    else
        [[ "${RTOS_DATA_STORE}" != *://* ]] || die "读卡器模式的 DATA_SDCARD 必须是本地挂载点"
    fi
    info "数据目录：${RTOS_DATA_DIR}"
}

rtos_assert_safe_path() {
    local path="$1" checked="$1"
    case "${checked}" in
    gphoto2://* | mtp://*)
        checked="${checked#*://}"
        checked="/${checked#*/}"
        ;;
    esac
    [[ "${checked}" == /* && "${checked}" != / && "${checked}" != *'..'* &&
       "${checked}" != *'/./'* && "${checked}" != */. && "${checked}" != *'//'* &&
       "${checked}" != *"'"* && "${checked}" != *$'\n'* && "${checked}" != *$'\r'* ]] ||
        die "拒绝不安全的路径：${path}"
}

rtos_ssh() {
    [[ "${RTOS_MODE}" == scp ]] || die "当前不是网络模式"
    ssh -o ConnectTimeout=10 "${BOARD_USER}@${RTOS_TARGET}" "$@"
}

# 输出：d/f/l<TAB>条目名。保留空格、包含隐藏文件，不跟随符号链接。
rtos_list_entries() {
    local dir="$1" listing name size kind path
    rtos_assert_safe_path "${dir}"
    if [[ "${RTOS_MODE}" == mtp ]]; then
        listing="$(LC_ALL=C gio list -h -n -a standard::type "${dir}")" || return 1
        while IFS=$'\t' read -r name size kind; do
            [[ -n "${name}" ]] || continue
            kind="${kind%%$'\t'*}"
            case "${kind}" in
            '(directory)') kind=d ;;
            '(symbolic link)' | '(symlink)') kind=l ;;
            '(regular)') kind=f ;;
            *) die "无法识别 MTP 条目类型：${name} ${kind}" ;;
            esac
            printf '%s\t%s\n' "${kind}" "${name}"
        done <<< "${listing}"
    elif [[ "${RTOS_MODE}" == scp ]]; then
        rtos_ssh "[ -d '${dir}' ] && [ -r '${dir}' ] && [ ! -L '${dir}' ] || exit 1
        for p in '${dir}'/* '${dir}'/.[!.]* '${dir}'/..?*; do
            if [ -L \"\$p\" ]; then k=l
            elif [ -d \"\$p\" ]; then k=d
            elif [ -e \"\$p\" ]; then k=f
            else continue; fi
            printf '%s\\t%s\\n' \"\$k\" \"\${p##*/}\"
        done"
    else
        [[ -d "${dir}" && ! -L "${dir}" ]] || return 1
        for path in "${dir}"/* "${dir}"/.[!.]* "${dir}"/..?*; do
            if [[ -L "${path}" ]]; then kind=l
            elif [[ -d "${path}" ]]; then kind=d
            elif [[ -e "${path}" ]]; then kind=f
            else continue; fi
            name="${path##*/}"
            [[ "${name}" != *$'\n'* && "${name}" != *$'\t'* ]] || die "不支持文件名中的换行或制表符"
            printf '%s\t%s\n' "${kind}" "${name}"
        done
    fi
}

# 从可访问的分区根逐层检查应用目录；不存在返回 1，读取错误返回 2。
rtos_directory_exists() {
    local root="$1" dir="$2" relative component entries kind name found
    relative="${dir#"${root}/"}"
    while [[ -n "${relative}" ]]; do
        component="${relative%%/*}"
        entries="$(rtos_list_entries "${root}")" || return 2
        found=0
        while IFS=$'\t' read -r kind name; do
            [[ "${kind}" == d && "${name}" == "${component}" ]] && found=1
        done <<< "${entries}"
        [[ "${found}" == 1 ]] || return 1
        root="${root}/${component}"
        if [[ "${relative}" == */* ]]; then relative="${relative#*/}"; else relative=''; fi
    done
    return 0
}

rtos_remove() {
    local path="$1" entries kind name
    rtos_assert_safe_path "${path}"
    case "${path}" in
    gphoto2://* | mtp://*)
        local uri_path="${path#*://}"
        uri_path="${uri_path#*/}"
        [[ "${uri_path}" == store_*/* && "${uri_path}" != */ ]] || die "拒绝删除 MTP 分区根：${path}"
        ;;
    esac
    if [[ "${RTOS_MODE}" == scp ]]; then
        rtos_ssh "rm -rf -- '${path}'"
    elif [[ "${RTOS_MODE}" == mtp ]]; then
        kind="$(LC_ALL=C gio info -n -a standard::type "${path}")" || return 1
        if [[ "${kind}" == *'standard::type: 2'* ]]; then
            entries="$(rtos_list_entries "${path}")" || return 1
            while IFS=$'\t' read -r kind name; do
                [[ -z "${name}" ]] || rtos_remove "${path}/${name}" || return 1
            done <<< "${entries}"
        fi
        gio remove "${path}"
    else
        rm -rf -- "${path}"
    fi
}

rtos_list_children() {
    local entries kind name
    entries="$(rtos_list_entries "$1")" || return 1
    while IFS=$'\t' read -r kind name; do
        [[ -z "${name}" ]] || printf '%s\n' "${name}"
    done <<< "${entries}"
}

# 本地/SSH 可用 /proc 时补查写句柄，包括暂时没有追加数据的写入者。
# MTP 不暴露打开句柄，由后续元数据观察检测变化。
rtos_open_write_paths() {
    local script
    [[ "${RTOS_MODE}" != mtp ]] || return 0
    script="$(cat <<'EOF'
command -v readlink >/dev/null 2>&1 || exit 0
[ -d /proc ] || exit 0
for fd in /proc/[0-9]*/fd/[0-9]*; do
    target=$(readlink "$fd" 2>/dev/null) || continue
    case "$target" in "$1"/* | "$2"/*) ;; *) continue ;; esac
    flags=''
    fdinfo="${fd%/fd/*}/fdinfo/${fd##*/}"
    if [ -r "$fdinfo" ]; then
        while read -r key value rest; do
            if [ "$key" = flags: ]; then flags=$value; break; fi
        done < "$fdinfo"
    fi
    # 能看到目标句柄但读不到访问模式时也保留。
    case "$flags" in
    '' | *[!0-7]*) printf '%s\n' "$target" ;;
    *) [ "$((flags & 3))" -eq 0 ] || printf '%s\n' "$target" ;;
    esac
done
exit 0
EOF
)"
    if [[ "${RTOS_MODE}" == scp ]]; then
        rtos_ssh "set -- '$1' '$2'"$'\n'"${script}"
    else
        sh -c "${script}" -- "$1" "$2"
    fi
}

# 清理专用：记录整个目标的元数据；目录必须包括所有后代，不能只看目录 mtime。
# 不读取日志/录像内容，不跟随符号链接。读取失败由调用方保留目标。
rtos_data_snapshot() {
    local path="$1" state
    rtos_assert_safe_path "${path}"
    if [[ "${RTOS_MODE}" == mtp ]]; then
        state="$(rtos_mtp_tree_state "${path}")" || return 1
    else
        local script
        script="$(cat <<'EOF'
_rtos_tree_state() (
    stat -c '%F|%d:%i|%s|%y|%z|%n' -- "$1" || exit 1
    if [ -d "$1" ] && [ ! -L "$1" ]; then
        [ -r "$1" ] && [ -x "$1" ] || exit 1
        for child in "$1"/* "$1"/.[!.]* "$1"/..?*; do
            [ -e "$child" ] || [ -L "$child" ] || continue
            _rtos_tree_state "$child" || exit 1
        done
    fi
)
_rtos_tree_state "$1"
EOF
)"
        if [[ "${RTOS_MODE}" == scp ]]; then
            state="$(rtos_ssh "set -- '${path}'"$'\n'"${script}")" || return 1
        else
            state="$(sh -c "${script}" -- "${path}")" || return 1
        fi
    fi
    printf '%s\n' "${state}" | LC_ALL=C sort | sha256sum | cut -d' ' -f1
}

rtos_mtp_tree_state() {
    local path="$1" metadata listing name size kind rest
    metadata="$(LC_ALL=C gio info -n -a 'standard::type,standard::size,time::modified,time::modified-usec,etag::value' "${path}")" || return 1
    printf '%s\n' "${path}" "${metadata}"
    if [[ "${metadata}" == *'standard::type: 2'* ]]; then
        listing="$(LC_ALL=C gio list -h -n -a 'standard::type,standard::size,time::modified,time::modified-usec,etag::value' "${path}")" || return 1
        printf '%s\n' "${listing}"
        while IFS=$'\t' read -r name size kind rest; do
            [[ -n "${name}" ]] || continue
            rtos_assert_safe_path "${path}/${name}"
            case "${kind}" in
            '(directory)') rtos_mtp_tree_state "${path}/${name}" || return 1 ;;
            '(regular)')
                [[ "${size}" =~ ^[0-9]+$ && "${rest}" == *time::modified=* ]] || return 1
                ;;
            '(symlink)' | '(symbolic link)') ;;
            *) return 1 ;;
            esac
        done <<< "${listing}"
    elif [[ "${metadata}" == *'standard::type: 1'* ]]; then
        [[ "${metadata}" == *'standard::size: '* && "${metadata}" == *'time::modified: '* ]] || return 1
    fi
}

# 构建与各部署方式使用相同的产物目录；BUILD_DIR 兼容旧部署配置。
rtos_artifact_path() {
    local target="$1" build_dir="${DART_BUILD_DIR:-${BUILD_DIR:-build-cross}}"
    [[ "${build_dir}" == /* ]] || build_dir="${REPO_ROOT}/${build_dir}"
    printf '%s/%s/%s' "${build_dir}" "${target}" "${target}"
}
