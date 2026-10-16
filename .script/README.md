# .script/ —— 仓库短命令

三个日常命令，在仓库里任何位置都能跑（脚本自己会定位仓库根）：

| 命令 | 作用 | 参数 |
|---|---|---|
| `build-rtos` | 交叉编译板端可执行文件 | `-r`（清 build-cross 重编）/ `--host`（host 编译检查）/ `launcher` / `dart_app` |
| `deploy-rtos` | 把产物上板到 `/sdcard/app/` | `launcher` / `dart_app` / `list`（只推清单）；可加 `--keep-logs` |
| `clean-rtos` | 清除板端内容（**保留目录本身**；固件自带的 `examples/` 默认不动） | `launcher` / `dart_app` / `list` / `logs` / `records` / `examples` |

要让上面三个名字能**直接敲**（不带 `./`、不带路径），先跑一次 `install-rtos`：

```bash
bash .script/install-rtos --both     # 一次性；详见下面"怎么直接敲 build-rtos -r"
```

`deploy-rtos` 默认调用 `deploy-mtp.sh`，将产物写入当前 USB 板子的
`/sdcard/app`，直接运行即可，无需先设置环境变量；USB 存储尚未挂载时会尝试自动挂载：

```bash
deploy-rtos
deploy-rtos dart_app   # 只部署 dart_app
```

需要指定其它板子或使用 `clean-rtos` 时，可通过环境变量或 `.script/board.env` 配置：

```bash
export BOARD_IP=192.168.1.100     # 网络模式：ssh/scp（需免密），优先级最高
export SDCARD=/media/$USER/SDCARD # 读卡器模式：直接写已挂载的 SD 卡
export SDCARD='gphoto2://Kendryte_CanMV_001000000/store_ffff0001'
                                  # MTP 模式：板子用 USB 挂着，这是板端 /sdcard
```

都写进 `.script/board.env` 更省事（该文件已被 `.gitignore` 忽略）：

```bash
cp .script/board.env.example .script/board.env   # 然后改里面的地址
```

### MTP（板子用 USB 挂着）注意

板子会暴露两个存储，**只有一个是 /sdcard**：

| MTP store | 板端路径 | 说明 |
|---|---|---|
| `store_ffff0001` | `/sdcard` | **要部署到这个**；判据：根目录有 `revision.txt`（SDK 的 genimage 只把它写进 SDCARD 分区） |
| `store_ffff0002` | `/data` | 写进去板子也不会启动它 |

MTP 部署会核对 `revision.txt`；缺少该文件时，必须存在已部署的 `app/launcher` 与
`app/startup_final.list` 才能确认目标，否则拒绝写入。地址也可以直接给
gvfs 的本地路径（含冒号，记得加引号）。

MTP 部署由 `deploy-rtos` 转交 `deploy-mtp.sh`，用 `gio copy` 直接操作
存储 URI。先备份旧文件，再上传临时文件并完整回读校验 SHA-256，改名替换后再次校验。
主机上的 `/tmp/dart-mtp-backup.*` 备份会保留；板上日志和录像也会保留。
此前通过 GVfs FUSE 路径运行 `cp` 出现过 0 字节文件，但原生 GIO 上传和改名已在当前
连接上验证成功，不能将 FUSE 写入失败直接判定为所有 MTP 写入都不可用。

## 怎么直接敲 `build-rtos -r`（一次性，不用 `./`）

`.script/` 默认不在 PATH 里。**在仓库根目录执行一次**这条即可（注意：`bash` 后面跟文件名，
不需要 `./`）：

```bash
bash .script/install-rtos --both      # 把 .script/ 追加进 ~/.bashrc 和 ~/.zshrc
source ~/.zshrc                       # 当前终端生效（bash 用户 source ~/.bashrc）
build-rtos -r                         # 之后直接敲，不再需要任何前缀
```

`install-rtos` 做的事：往 rc 里写一段带标记的 PATH 追加（`# >>> dart-rtos >>>` … `# <<< dart-rtos <<<`），
改动前备份成 `<rc>.rtos.bak`。它不接受位置参数，参数只有：

| 参数 | 作用 |
|---|---|
| 无 | 按 `$SHELL` 猜一个 rc（猜中的 rc 不存在、另一个存在时，按存在的那个写） |
| `--both` | `~/.bashrc` 与 `~/.zshrc` 都写（**不确定自己用哪个 shell 就用这个**） |
| `--bash` / `--zsh` | 只写指定的那个 |
| `-u` | 撤销：删掉本脚本写入的段落（两个 rc 都查），rc 里其它行一行不动 |
| `-h` | 帮助 |

行为特点：重复执行是幂等的（路径没变就跳过），仓库搬家后重跑一次会自动把路径改成新位置。
本脚本是"执行"用的，`source` 它会被拒绝（避免 `set -euo pipefail` 污染你的 shell）。

不想动 rc 文件时，直接用路径调用也一样（任何目录都能用）：
`/path/to/dart/.script/build-rtos -r`。

## 典型流程

```bash
bash .script/install-rtos --both   # 只做一次：之后下面这些命令直接敲，不带 ./
build-rtos                  # 增量编译 launcher + dart_app
build-rtos -r               # 改了 CMake / 换工具链后全量重编
deploy-rtos                 # 上板：两个 ELF + startup_final.list
clean-rtos                  # 清空 /sdcard/app 下全部内容（目录与固件的 examples/ 保留）
clean-rtos records          # 只清录像，保留 ELF 和清单
```

`deploy-rtos` / `clean-rtos` 的地址解析顺序：

1. `BOARD_IP` 有值 → ssh/scp 模式（会先测连通性，连不上立刻报错，不会跑到一半失败）；
2. 否则 `SDCARD` 有值 → 使用指定的卡挂载点、`app/` 目录或 URI。部署时 MTP URI
   直接交给 GIO，gvfs 路径先取得 URI；`clean-rtos` 使用对应的本地挂载路径；
3. 都没有 → `deploy-rtos` 默认部署到当前 USB 板子的 `store_ffff0001`；
   `clean-rtos` 提示配置地址后退出。

## 底下的实现

构建、网络部署和读卡器部署直接在对应命令内实现；MTP 的备份、上传与恢复逻辑单独保留：

| 短命令 | 调用 |
|---|---|
| `build-rtos` | CMake host/cross 预设（`-r` 先清 `build-cross`） |
| `deploy-rtos`（网络） | SSH/scp 上传与 md5 校验 |
| `deploy-rtos`（USB/MTP） | `deploy-mtp.sh`：旧文件备份、SHA-256 回读校验与替换失败恢复 |
| `deploy-rtos`（读卡器） | 临时文件写入、大小与 md5 校验、改名替换后再次校验 |
| `clean-rtos` | 直接用 ssh + `rm -rf`，或本地 `rm -rf`（见 `_rtos-common.sh`） |
| `install-rtos` | 往 `~/.bashrc` / `~/.zshrc` 写一段 PATH 追加 |
| `check-camera-pool.sh` | 用 SDK 头文件和 fake SDK 调用运行相机资源池/生命周期回归检查 |
| `player` | 独立构建并启动 `tools/video_player` 主机录像播放器 |

构建和部署使用 `DART_BUILD_DIR` 指定产物目录（默认 `build-cross`，host 默认 `build`）；
部署同时兼容旧变量 `BUILD_DIR`，优先级低于 `DART_BUILD_DIR`。相对路径以仓库根目录为基准。
例如：

```bash
DART_BUILD_DIR=/tmp/dart-cross build-rtos
DART_BUILD_DIR=/tmp/dart-cross deploy-rtos dart_app
SDCARD="/media/$USER/SDCARD" deploy-rtos --keep-logs
bash .script/check-camera-pool.sh
```

相机检查仍需本机 C 编译器和 `K230_SDK_ROOT`（默认 `/mnt/mydata/kRTOSSDK`）的 MPP
头文件；它运行真实 K230 相机生命周期实现，覆盖范围与 `build-rtos --host` 的编译检查不同。

`_rtos-common.sh` 是共用的定位/环境/板端访问层，不被直接执行。它带的护栏：

- 只允许删绝对路径，拒绝 `/`，拒绝含 `..` 的路径；
- 删除前逐条打印将被删的项；
- 远程模式只使用板端 ssh 会话里必然存在的 `ls` / `rm`，不依赖 `find`、不依赖远程 glob。

## 主机录像播放器

```bash
bash .script/player records/rec_0001
bash .script/player --check records/rec_0001
```

`player` 自动增量构建 Qt 主机工具，参数透传给播放器；录像相对路径以调用时的
工作目录为准。`DART_PLAYER_BUILD_DIR` 可覆盖默认构建目录 `build/video_player`。
已通过 `install-rtos` 将 `.script/` 加入 PATH 时，也可直接执行 `player`。
图片和文字关联规则及完整参数见 [播放器说明](../tools/video_player/README.md)。

## 注意

- 手工调试时不要与 launcher 同时跑 payload：两者都会独占 VICAP/VB。
- `deploy-rtos <目标>` 只动该目标，不清日志；全量读卡器部署默认清 `logs/*` 与 `records/*`，
  用 `deploy-rtos --keep-logs` 可保留。网络和 MTP 部署均保留日志与录像。
- MTP 部署会完整回读校验文件；失败时保留主机备份并报错，请按实际错误排查连接或存储。
