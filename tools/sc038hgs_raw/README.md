# SC038HGS 不经 ISP 的 RAW 采集验证

目标路径：`SC038HGS（黑白 RAW10）→ CSI2 / VI → MCM 写 DDR → CPU`。
MCM 属于 ISP 子系统，但此路径不启动 ISP 图像处理流水线，不加载 XML/JSON 标定库。
这是针对本机 `/mnt/mydata/kRTOSSDK` 的实验程序；`width10` 接收修正和
`packed10-lsb` 布局已在本机固件实测，详见文末验证状态。

## 为什么不能直接使用旧示例

[官方 VICAP 文档](https://www.kendryte.com/k230_rtos/zh/main/api_reference/mpp/vicap.html#k-vicap-work-mode)
规定 `ONLY_MCM` 的 dump 参数只能使用 YUV444。本机实际 `libvicap.a` 也检查了这个限制：
`VICAP_DUMP_RAW` 会被拒绝。SDK 内旧的 `sample_sc038hgs/main.c` 使用了这个不可用的组合。

仅把 dump 参数改成 `VICAP_DUMP_YUV444` 仍不足以取得可信的 RAW10：
本机用户库的 `kd_mpi_vicap_set_chn_attr` 在 MCM 模式下将底层格式写死为 0，
stride 写死为 `width * 3`。内核 MCM 中断构造的帧信息也仍标记为 YUV444，
不能用它证明字节已经是 RAW10，也不能信任其 stride 字段。

本程序通过链接器 `--wrap=ioctl`，只在自身进程内适配 MCM 配置请求 `0x40204315`：
校验原配置后，显式请求 `VICAP_MCM_RAW10`、1280 字节 DMA 行跨度及独立 VB 池。
仅这些配置仍会首帧超时；还需下述 `width10` 输入宽度修正。
随后通过 SDK 的 YUV444 dump 分支取得 MCM 缓冲地址；该分支直接调用 MCM dump ioctl，
不执行 YUV 转换。适配没有修改 SDK 静态库，但不是官方承诺稳定的 RAW API。
构建脚本锁定已核查的用户库、内核库 SHA256；库变更时会停止构建，
需要重新检查 ioctl、结构体和硬件格式语义，再更新相机后端持有的
[`mcm_abi.json`](../../include/firmware/src/camera/k230/mcm_abi.json)。正式应用与探针共用这份校验清单。
板端固件需要与核查的 SDK 匹配；程序不能自动验证正在运行的内核库版本。

## 构建和采集

旧 OFFLINE ISP 对照的 XML/JSON、来源哈希及生成说明放在 [`isp/`](isp/README.md)。
它们只供探针对照，生成脚本为 `tools/sc038hgs_raw/generate_isp.py`，
可用 `--output DIR` 写入独立目录进行比对。

在仓库根目录运行，默认读取现有 `build-cross/CMakeCache.txt` 中的 SDK、工具链和链接脚本路径：

```sh
python3 tools/sc038hgs_raw/build.py
```

产物是 `build/sc038hgs_raw/sc038hgs_raw.elf`。这只编译独立探针，不构建业务应用。
将它以独立文件名复制到板上；运行前退出占用 VICAP/VB 的相机应用和自动重启服务。
该探针独占相机及全局 VB 初始化，不能与 `dart_app` 同时运行。

```sh
/sdcard/app/sc038hgs_raw.elf gradient /sdcard/gradient.raw
/sdcard/app/sc038hgs_raw.elf image /sdcard/image.raw
# 原应用绕过 ISP 之前的离线 ISP 路径对照，保存灰度 PGM：
/sdcard/app/sc038hgs_raw.elf isp /data/app/records/isp-reference.pgm
```

先启用传感器渐变测试图验证接收和打包，再获取实际画面。每次丢弃前 7 帧并保存第 8 帧。
ONLY_MCM 初始化会关闭传感器；当前 SDK 的 `start_stream` 负责启动接收器、给传感器上电、
初始化并启流。程序随后设置测试图，避免被 SDK 再次初始化清除；退出时停流和释放资源。
缓冲数量固定为内核 MCM 环的 4 项。控制台逐步记录调用结果、errno 和传感器接口寄存器，
在初始化后、启流后及首帧超时/成功后只读采样 CSI2/VI 和 MCM 状态寄存器，并逐项记录资源清理的开始与结果。
外设寄存器通过 RT-Smart 的 `/dev/mem` 和 `O_RDONLY | O_SYNC`、`PROT_READ` 映射；
MPI 的 mmap 在本固件上仅适用于 VB 内存，不能用于外设地址。映射失败会明确记录，不能据此判断接收器状态。
退出前同步日志，再将 PID 写入启动器提供的 `DART_EXIT_MARKER`，便于通过 USB 文件接口
区分首帧超时、资源清理卡住与 RT-Smart 进程回收异常。
输入固定为本仓库 CSI2 的 640×480 RAW10 黑白 SC038HGS。使用传感器初始化默认曝光和增益，
没有套用业务应用默认的 400 行曝光，不能直接与其画面作同曝光对照。

`isp` 对照模式使用旧应用的 `VICAP_WORK_OFFLINE_MODE`、NV12 通道和固定曝光，
通过公开的场景 API 加载 `/sdcard/app/isp/` 中已有的 XML/JSON。此模式不拦截 MCM 配置，
输出第 8 帧的 Y 平面 PGM 及 JSON；它用于定位路径回归，不会改变业务应用的 RAW 路径。
两种模式均输出传感器和接收器状态、帧元数据及到达帧率。可按 RAW → ISP → RAW 顺序运行，
对比冷启动与 ISP 初始化后 RAW 接收的差异。

ONLY_MCM 还支持可选的接收配置对照参数，仅用于这个独立探针：

| 参数 | 接收宽度 | MCM 输入格式 | DMA 格式 | WR_CTRL 位 17 |
| --- | --- | --- | --- | --- |
| `sdk`（默认） | 1280，SDK 使用了字节跨度 | RAW10 | RAW10 | 保留 |
| `width10` | 640 像素 | RAW10 | RAW10 | 保留 |
| `width16` | 640 像素 | RAW16 | RAW16 | 保留 |
| `width16-input` | 640 像素 | RAW16 | RAW10 | 保留 |
| `width16-ctrl` | 640 像素 | RAW16 | RAW16 | 清零 |

```sh
/sdcard/app/sc038hgs_raw.elf gradient /data/app/records/width16.raw width16
```

上述对照来自已核查内核 `vicap_set_mcm_conf` 与用户库 `libisp.a` 的
`isp_mcm_cfg_wr`、`VideoInSetMcmWrCfg` 的差异：前者将 `cfg.stride` 写入
`0x1204`，后者使用输入像素宽度。正常 ISP 的实板快照是 `0x1204=0x01e00280`、
`0x1200=0x80`、`0x1604=0x40`。实板确认 `width10` 只修正输入宽度即可收帧，
生产后端不需要其余格式或控制位实验修改。
探针先检查实际寄存器符合已核查 SDK 的预期，再在启流前通过 `/dev/mem` 修改
`0x1204` 的输入宽度和 `0x1200` 的对应字段，并回读；格式选择由原配置 ioctl 完成。
所有对照保持 DMA 行跨度 1280 字节、VB 容量和中断配置不变，未修改业务应用或 SDK 库。
RAW16 代表 MCM 传输格式，传感器仍发送 RAW10，DDR 中的有效位位置仍须检查。

每次生成两个文件：原始数据 `*.raw` 和参数 `*.raw.json`。RAW10 模式已验证为
每行 800 字节，保存 384000 字节；JSON 的 `data_stride` 表示数据行长，
`requested_stride` 仍记录请求的保守 DMA 容量跨度 1280。
早期探针保存过完整 614400 字节容量，后 230400 字节不是该帧像素。
RAW16 实验仍保存 614400 字节，尚未逐像素确认其全部内容可靠。
独立池的 VB 块预留了三倍保守 DMA 容量，兼容当前 MCM 的缓冲分配方式；
程序核对返回地址属于这个池，且保持原帧信息用于 release。
输出文件使用独占创建，避免覆盖已有采集结果；失败时删除本次输出。

## 查看和验证

把 RAW 及 JSON 一起复制回主机。转换器要求显式选择打包假设，不会自动把未知字节解释为灰度：

```sh
python3 tools/sc038hgs_raw/convert.py gradient.raw gradient-lsb.pgm --layout le16-lsb
python3 tools/sc038hgs_raw/convert.py gradient.raw gradient-msb.pgm --layout le16-msb
python3 tools/sc038hgs_raw/convert.py gradient.raw gradient-packed.pgm --layout mipi10
# 本机已验证的 MCM RAW10：
python3 tools/sc038hgs_raw/convert.py gradient.raw gradient-mcm.pgm --layout packed10-lsb
# 早期探针的 614400 字节文件：显式只取 800×480 字节有效帧，报告忽略的尾部：
python3 tools/sc038hgs_raw/convert.py old-gradient.raw gradient-mcm-old.pgm --layout packed10-lsb --stride 800
```

前三个选项是候选格式：小端 16 位低 10 位有效、小端 16 位高 10 位有效、MIPI RAW10。
`packed10-lsb` 是本机已验证的 MCM 排列：每组 `p0 | p1<<10 | p2<<20 | p3<<30`
存成 5 个小端字节；CSI 线上的打包方式不等于 DDR 中的布局。
转换器按指定 DMA stride 跳行，16 位布局会检查未使用位是否为零。
需要核对传感器测试图的已知渐变方向、行连续性、数值范围及相邻像素变化，
再用同一布局转换真实图像。若均不匹配，应继续核查 MCM 的打包/对齐配置，而不是强行掩码出图。

确认解包后的像素是 0…1023 后，8 位预览为 `gray8 = raw10 >> 2`。
也可在预览时指定 `--gain 4`，只改变显示亮度，不修改原始 RAW：

```sh
python3 tools/sc038hgs_raw/convert.py image.raw image.pgm --layout packed10-lsb --gain 4
```

黑白传感器每个有效像素就是一个灰度采样，不需要去马赛克。后续可以直接在 RAW10 上做
`raw10 >= threshold10` 二值化。传感器自身的黑电平偏置、坏点、暗角和噪声仍可能影响阈值；
绕过 ISP 不会增加光子数，也不会消除曝光期间产生的运动模糊。

## 验证状态

- 本机 SDK 的 MPI 分支、MCM 配置 ioctl 及内核 DMA 参数已通过静态库反汇编核查。
- 交叉编译结果见 `build/sc038hgs_raw/manifest.json`。
- 2026-10-20 实板：初始化、启流、渐变图设置和传感器字段读回成功；首帧 dump 在
  3000 ms 超时，尚未取得有效 RAW 数据。停流、断电、VICAP/VB 释放均返回成功，
  启动器已通过完成标记继续启动正常应用；前一版使用 MPI 读取外设快照被拒绝，未取得寄存器值。
- 2026-10-20 同轮 RAW → 原 ISP → RAW 对照：原离线 ISP 连续收到 8 帧
  640×480 NV12，短时到达帧率约 96.010 FPS，已保存第 8 帧 Y 平面 PGM；
  实际画面很暗（8 位像素范围 10…14），未据此确认识别有效。
  ISP 前后的 RAW 渐变和 RAW 实景均收不到首帧，资源清理成功。
  `/dev/mem` 快照成功，CSI2/VI 配置一致，MCM 宽度和格式字段存在上述差异。
- 2026-10-20 宽度/格式对照：`sdk` 仍首帧超时；`width10` 只将接收宽度
  从 1280 改为 640 后，渐变、实景均连续收到 8 帧，短时约 96.006 FPS。
  渐变的 307,200 像素在 800 字节紧凑行、低位连续 10 位解包下全部符合
  `max(0, 384 - (x ^ (y & 1)))`，零像素差异；实景像素范围 42…60，较暗。
  重新以 USB 分段读取与整文件读取一致。RAW16 其它对照能收帧，但未确认所有像素可靠。
- 相应宽度修正和 `packed10-lsb` 解包已接入应用；主机回归、相机内存检查通过。
  完整应用已验证 640×480 PBM 持续录像 55.881 FPS、13,017 帧、499,995,987 字节停录，
  队列忙丢帧为 0；复位新建目录并保留旧录像。停录后采集/识别继续，无取帧或识别错误。
  首、中、末二值帧均全黑、目标命中为 0，实际目标识别效果仍需曝光/阈值标定。
