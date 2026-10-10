# K230D / K230 RT-Smart 业务代码仓库

本仓库**与固件无关**：它面向 K230D RT-Smart 用户态业务代码的开发和部署。

> 相机 C 数据面、C++ 曝光虚接口和构建探针已落地，见 [`include/firmware/app/src/camera/README.md`](include/firmware/app/src/camera/README.md)。其余业务模块和下文的构建/部署脚本仍是目标工作流。先阅读 [`docs/STRUCTURE_DESIGN.md`](docs/STRUCTURE_DESIGN.md)，不要把目标路径当成现有实现。

工作流（官方文档同款流程，见
[如何运行和新增 HelloWorld 应用](https://www.kendryte.com/k230_rtos/zh/v0.8/userguide/helloworld.html)）：

```
编写业务代码 → 交叉编译出 ELF → scp 拷贝到板子 → 利用开机自启的跳板文件启动
```

---

## 1. 业务代码放在哪里？（最重要）

> 当前分支尚未创建具体业务应用；后续业务代码应放在 `src/`
> 视觉、IMU、控制和录像模块。识别/跟踪的目标工况见 [`docs/DETECTOR_DESIGN.md`](docs/DETECTOR_DESIGN.md)，
> 架构边界见 [`docs/STRUCTURE_DESIGN.md`](docs/STRUCTURE_DESIGN.md)，板端调试模板见
> [`docs/DEBUG_GUIDE.md`](docs/DEBUG_GUIDE.md)。

- 目录结构与官方 SDK 的 `src/applications/<应用名>/` 一一对应：
  本仓库用 CMake 独立编译，产物可直接拷板；需要打进固件镜像时，
  把整个目录拷进 SDK 的 `src/applications/` 再用官方 `make` 流程即可，产物等价。
- **新增应用（目标工作流）**：创建 `apps/my_app/`，补齐 `CMakeLists.txt` 和 `main.cpp`，再在顶层 CMake 注册它；当前分支尚未提供 `app_template`。

> 你现有的/未来的业务代码（如传感器、算法、通信逻辑）都放在 `src/` 下。

## 2. 目录结构

```
SelfGuidingDart/
├── cmake/
│   ├── toolchain-riscv64-musl.cmake   # 交叉编译工具链配置（rv64imafdcv/lp64d）
│   └── link.lds                       # RT-Smart 链接脚本（取自自定义 SDK）
├── scripts/                    # 目标：构建/部署脚本（当前尚未创建）
├── src/                         # 目标：业务实现（当前尚未创建）
├── include/                     # 目标：公共接口
├── docs/                        # 设计和验收契约
├── .vscode/                     # VSCode 配置
└── CMakeLists.txt               # 顶层构建骨架
```

## 3. 首次准备（一次性）

> 本节命令是目标工作流；当前分支的 `scripts/` 尚未创建，首次实现应先补齐脚本并验证工具链。

```bash
# 安装交叉工具链（riscv64-unknown-linux-musl-，约 108MB，装到仓库 third_party/）
bash scripts/setup-toolchain.sh
```

环境要求：`cmake >= 3.20`、`ninja`、`scp`（部署用）。SDK 位于
`/mnt/mydata/kRTOSSDK`（仅用于提供 SDK 头文件与链接脚本；若路径不同，
用环境变量 `K230_SDK_ROOT=/你的路径` 或 CMake 参数 `-DK230_SDK_ROOT=...` 覆盖）。

## 4. 编译

产物：`build/<应用名>/<应用名>` —— riscv64 静态链接 ELF，可直接拷板运行。
同时生成 `build/compile_commands.json`，供 clangd 使用。

## 5. 部署到板子

```bash
bash scripts/deploy.sh my_app 192.168.1.100          # 拷贝到板端 /tmp
bash scripts/deploy.sh my_app 192.168.1.100 /sdcard/app
```

然后在板端 msh 终端运行：

```text
msh> /tmp/my_app                # 或 /sdcard/app/my_app
```

> 说明：官方镜像把应用放在 `/sdcard/app/`；手动拷贝调试时放 `/tmp` 即可。
> 若板子开了 SSH/scp 服务（RT-Smart 默认支持网络），deploy.sh 直接可用；
> 也可通过其他方式（U 盘、串口 xmodem、ADB push 等）把 ELF 拷进板子，本质相同。

## 6. VSCode + clangd 使用

1. VSCode 打开本仓库，按提示安装推荐扩展：
   **clangd**（`llvm-vs-code-extensions.vscode-clangd`）、
   **CMake Tools**（`ms-vscode.cmake-tools`）、**CMake**（`twxs.cmake`）。
2. 先运行一次 `bash scripts/build.sh` 生成 `build/compile_commands.json`
   （或让 CMake Tools 自动配置，`.vscode/settings.json` 已写好参数）。
3. clangd 自动读取编译数据库 → 获得完整补全、跳转、诊断。
   - 关闭了 MS C/C++ 的 IntelliSense，避免冲突。
   - `.clangd` 用 `--target=riscv64-unknown-linux-musl` 做索引，
     不影响真实编译（真实编译仍由 GCC 工具链完成）。
4. 常用命令：`Ctrl+Shift+B` 编译；"Tasks: Run Task" 可部署到板子。

## 7. 写业务代码时常用的 SDK 头文件（已自动加进 include 路径）

| 功能 | 头文件示例 | 路径来源 |
|---|---|---|
| 标准 C | `<stdio.h>` 等 | musl 工具链自带 |
| 外设 HAL | `<drv_gpio.h>` `<drv_i2c.h>` 等 | SDK `output/*/rtsmart/libs/rtsmart_hal/include` |
| 多媒体 MPP | `<vb.h>` `<sys/ioctl.h>` 等 | SDK `src/rtsmart/mpp/include` 等 |
| 板级配置宏 | `CONFIG_*`（autoconf.h 自动 include） | SDK `include/generated/autoconf.h` |

需要 MPP 库/OpenCV/nncase 等链接库时，参照官方 SDK
`src/rtsmart/libs/mk/libmpp.mk` 等文件把 `-L`/`-l` 参数补进应用即可
（本模板刻意保持最小，不预置未用到的库）。

## 8. 常见问题

- **找不到工具链**：先运行 `bash scripts/setup-toolchain.sh`；或已装在其他
  位置时用 `-DK230_TOOLCHAIN_DIR=...` 指定。
- **SDK 路径不同**：`export K230_SDK_ROOT=/你的SDK路径` 后再 `build.sh`。
- **clangd 无补全**：确认 `build/compile_commands.json` 存在
  （运行过一次 `build.sh`），并已安装 clangd 扩展。
- **与固件的关系**：本仓库不编译、不烧录固件；板子上必须是已支持
  RT-Smart 用户态（lwp）的固件，ELF 才能直接运行。
