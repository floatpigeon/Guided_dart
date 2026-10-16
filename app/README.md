# dart_app —— 板端业务应用

`main.cpp` 提供业务进程入口，调用 `application.hpp` 声明的 `dart::app::run()`。
`application.cpp` 是应用的**组合根**：解析配置、创建组件、连接依赖、安排运行和
退出顺序。当前接入相机 ACL（`dart_camera_acl`）、识别（`src/detector`）和录像
（`runtime/recordor`）；后续 IMU、控制与执行器也由这里组装，各自实现留在对应模块。

本目录是应用组合根，可以选择具体设备和运行时实现。`src/` 保持为 L4 领域，
只消费公开数据或抽象接口；`runtime/launcher/` 负责进程启动与监督，
`runtime/recordor/` 负责录像线程与文件写入。应用负责进程内部的组件生命周期。
单进程可以有多个线程或任务：当前相机调用保持串行，
后续 IMU 采样和控制任务应按各自频率运行，不能依赖相机逐帧循环的进度。

## 相机采集、识别与录像为什么放在同一个进程

相机 ACL 在 `create()` 里自建并独占 `dart_media`（全局 VB 初始化 + VICAP/VB 私有
输出池），同一时刻只允许一个消费者。v1 实测把同类程序在清单里放多条，5 个实例
几乎同时抢 VB/VICAP，只有 9/28 次写出日志，其余静默死掉。因此：

- 清单里**只能有一条** `dart_app`；
- "识别"和"录像"在同一进程内完成，不做跨进程帧共享；
- 手工调试时不要与 launcher 同时运行。

## 流水线

```
camera->acquire(timeout) -> FrameLease
  -> detector.Detect(lease.view())      识别同步借用像素；不取帧、不释放租约
  -> recorder.try_submit(std::move(lease))
        kAccepted     -> 所有权转给 Recorder（录像线程拷贝为紧凑 Y8 后写盘）
        其他          -> 采集线程自己 release()
  -> recorder.drain_releases()          每拍调用：录像线程归还的租约在采集线程释放，
                                        保证相机 ACL 的调用始终串行
```

三个取帧结果分别计数，语义不同，都不能当错误：

| `AcquireStatus` | 含义 | 处理 |
|---|---|---|
| `kFrame` | 拿到一帧 | 走上面整条流水线 |
| `kNoFrame` | 超时内没有帧 | 正常否定结果，`no_frame++` |
| `kLeaseExhausted` | 租约槽用满，驱动根本没被触碰 | 容量性丢弃，`lease_exhausted++` |

租约容量是编译期参数（`kLeaseCapacity = 4`，要求 ≤ `channel_buffers`），取 4 是为了
让"识别中/录像拷贝中/写盘中"的帧彼此不互锁。

## 参数

```
dart_app [选项]
  --seconds N      有界运行秒数，0 = 跑到 SIGINT/SIGTERM（默认 0）
  --acq WxH        传感器模式（默认 1920x1080）
  --fps N          传感器帧率（默认 30）
  --out WxH        输出尺寸（默认跟随 --acq；宽 32 对齐、高偶数、不上采样）
  --csi N          CSI 端口 0..2（默认 0）
  --mirror N       0 无 / 1 水平 / 2 垂直 / 3 双向（默认 0）
  --exp US         固定曝光微秒（默认 250）
  --ae             自动曝光（与 --exp 互斥）
  --vision on|off  识别开关（默认 on）
  --record on|off  录像开关（默认 on）
  --rec-keep N     只保留最近 N 轮录像，0 = 不清理（默认 0）
  --rec-fps N      目标录像帧率，按时间采样（默认 30 FPS）
  --rec-format F   pgm（灰度，默认）| pbm（按阈值二值化）
  --rec-dir DIR    录像根目录（默认 /sdcard/app/records）
  --det-csv PATH   识别结果 CSV；off 关闭（默认关闭）
  --threshold N    二值化阈值（默认 50）
  --log PATH       结构化日志（默认 /sdcard/app/logs/dart_app.log）
  --status-ms N    状态行间隔（默认 1000）
  --acquire-ms N   单次取帧超时（默认 200）
  --max-acquire-errors N  连续取帧错误上限，超过即退出（默认 8）
```

**所有默认值都是 UNVALIDATED**：传感器模式必须精确命中（ACL 拒绝静默回退），
曝光/阈值要用真实板端样本标定，见 [`docs/DEBUG_GUIDE.md`](../docs/DEBUG_GUIDE.md) §5。

## 输出

```
/sdcard/app/logs/dart_app.log                      结构化日志（状态与耗时两行共用一次 fsync，写完关闭文件）
/sdcard/app/records/rec_NNNN/img/frame_XXXXXXXX.pgm   灰度取证图（或 .pbm）
/sdcard/app/records/rec_NNNN/logs/frames.csv          每帧一行的放行记录（Recorder 写）
/sdcard/app/records/detection.csv                     每帧一行的识别结果（本程序写）
```

`detection.csv` 列：`sequence,received_us,source_pts,kind,valid,cx,cy,area,radius,quality,clipped,missed_frames,candidates,full_frame,rescanned,win_x0,win_y0,win_x1,win_y1,admission`
（窗口坐标是**闭区间四角像素坐标**，与 `ScanMetadata::search_window` 一致。）

日志是"结构化单行"，正常否定结果（无目标、ROI 无连通域、候选被拒）**不进日志**，
只进 CSV 与计数器：

```
ts_ms=... level=INFO module=app op=status elapsed_s=10 frames=298 fps=29.8 no_frame=2
  lease_exhausted=0 target=37 detect_err=0 rec_input=298 rec_acc=75 rec_down=223 rec_busy=0
  rec_queued=1 rec_pending=0 rec_written=74 rec_event_dropped=0 release_err=0 run_hw=512
```

## 退出契约

有界运行（`--seconds`）或 SIGINT/SIGTERM 后，按固定顺序收尾，每一步都留日志：

```
step=input_stop      主循环出界（reason=signal|deadline|abort）
step=recorder_stopped  停止录像（先停消费者；此时相机仍持有租约，不能先 stop 相机）
step=leases_drained   排空 release 队列，租约全部在采集线程释放
step=camera_stopped   camera.stop()
step=summary          全部计数器 + Recorder 统计 + 工作区高水位
step=camera_destroyed 相机析构（deinit → destroy → media destroy）
step=exit
```

由 launcher 以 `&!` 拉起时，程序清理完相机后写入 launcher 提供的 `.done`
标记。launcher 据此启动下一轮；仅靠板端 `waitpid` 和 PID 存活探测会误判。

顺序不能颠倒：`recorder` 仍持租约时 `camera.stop()` 会返回 `BUSY`。板端卡死时，
最后一条 `step=` 就是定位点。

## 构建与上板

```bash
bash .script/build-rtos dart_app          # -> build-cross/dart_app/dart_app
msh> /sdcard/app/dart_app --seconds 10 --record off     # 先只验证采集+识别
msh> /sdcard/app/dart_app --seconds 10                  # 再验证录像落盘
```

## 验证状态

重命名后的 `dart_app` 需重新部署并验证板端自启；以下板端记录是迁移前流水线的
历史验证结果。

- 已通过（2026-10-15，CanMV 001000000 板端）：CSI2 上电自启、1920×1080 PGM
  完整回读、90 秒分段后的完成标记与自动重新启动、仅保留最近两轮。
  本次复位后上一轮 43 张和当前轮 9 张均完整，零字节文件数为 0。
  黑暗样本平均灰度 0.55；此结果不代表识别质量或目标曝光已标定。
- 已通过（2026-10-13，本机）：交叉构建（`-march=rv64imafdcv -mabi=lp64d`，
  `-fno-exceptions -fno-rtti`，静态链接 RT-Smart ELF，约 1.7 MB）。
- **待板端验证**：目标场景的曝光/阈值标定、RVV 与 cache/DMA 的完整压力测试、
  高帧率下的丢帧与长期稳定性；识别质量与性能尚未验证。
- 本程序**不**包含硬件 H.264/VENC：v2 的录像只有灰度取证（PGM/PBM）。

## 90 fps 配置与耗时统计（2026-10-15）

启动清单请求 `--acq 1280x720 --fps 90 --csi 2`，输出跟随传感器尺寸，
识别与录像继续使用 v2 实现。没有合并 v1 分支或移植其检测器。
`camera_create` 日志的 `sensor` 是精确匹配后回读的模式；它不是实测帧率。
`status fps` 是本轮累计处理帧率，`timing fps` 是当前窗口处理帧率。

`op=timing` 每个状态周期报告取帧、识别和后处理的平均/最大微秒数：
`acquire_us` 包含等待驱动出帧及 cache invalidate；`detect_us` 只覆盖 Detect；
`pipeline_us` 覆盖录像放行、释放租约、CSV 行写入及归还队列；
`telemetry_us` 是上一个周期状态日志写入/同步及 CSV 刷新的耗时。
90 fps 每帧预算约 11111 us，需要结合这些数值定位 CPU 或 I/O 开销，
不能仅凭请求档位断言实际能处理 90 fps。下述数据已完成板端验证。

启动清单通过 `--rec-fps 30` 注入目标录像帧率，Recorder 按单调接收时间采样，
采集档位从 30 fps 改到 90 fps 时仍以每秒 30 张为目标。未指定 `--rec-fps` 时，
程序沿用 `recorder::Config::fps` 的默认值。启动日志的 `rec_fps` 与
`op=recorder_start fps`、录像 CSV 首行的 `fps` 可核对实际注入值。
输入不足或队列忙时实际落盘帧率会降低，队列忙仍会丢弃录像帧并计数。
原 `--rec-every` 参数已移除。

板端验证：仅把采集模式改到 720p90 时，一轮 7624 个处理帧，CSV 接收时间戳
计算为 84.7 fps。分阶段统计显示无目标场景 Detect 平均约 1224 us，
后处理平均约 1466 us，后处理单帧峰值 128068 us。
此前每 60 帧录一张的配置中，为识别 CSV 设置 64 KiB 缓冲后，连续四个 90 秒完整轮次处理
8008 / 8012 / 8011 / 8006 帧，约 89 fps；后处理平均约 63 us，
识别仍约 1214 us。四轮取帧/识别/写入/释放错误及录像 busy 均为 0，
每轮写出 134 张 PGM。状态日志与 CSV 周期刷新仍会产生停顿；
这些结果只验证黑暗无目标场景吞吐，不代表复杂目标场景的最坏耗时。
证据见 `records/board-performance-2026-10-15/`（本地录像目录，不提交 Git）。
上述吞吐数据来自旧抽帧配置。30 FPS 配置已于 2026-10-15 上板：启动日志与
录像 CSV 均确认目标为 30 FPS，连续六个完整轮次实际落盘约 5.4～6.1 FPS。
其中 `rec_0073` 的 CSV 跨度 89.868 秒，共 6528 个输入帧、512 张已写图片，
实际约 5.7 FPS，录像忙丢弃 1710 帧、队列高水位 2；采集、拷贝、写入、释放与
metadata 错误均为 0，已回读完整 1280×720 PGM 样本。参数注入已生效，当前
写盘链路尚不能维持 30 FPS；未追加性能改动。证据见
`records/board-recorder-fps30-2026-10-15-175445/`（本地目录，不提交 Git）。
