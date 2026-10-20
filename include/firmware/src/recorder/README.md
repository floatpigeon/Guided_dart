# 录像 / 图像证据

`dart::recorder::Recorder` 提供异步录像服务。它只接收相机 ACL 的
`FrameLease`，不包含 K230、MPP 或 VENC 头文件；提交时将输入帧拷贝为紧凑
Y8，后台线程编码并写入独立的 PNG 图片，也兼容旧 PGM/PBM 连续流。硬件 H.264 需要后续独立的 video ACL，
不会把厂商句柄带入录像服务。本模块是 `include/` 中的下层实现库，
包含线程管理、编码和具体文件系统操作；L4 的 `src/recording/` 只决定参数与启停。
构建目标在 `include/CMakeLists.txt` 中声明。

当前 `dart_app` 接收相机层已二值化的 0/255 Y8，默认保存逐帧 1-bit 灰度 PNG（无损 DEFLATE，速度优先）；选择 PGM
也只保存相同二值像素。`--threshold` 只在 RAW10 转换时使用，识别器和 PNG/PBM
打包器固定用中点 128 区分黑白，避免阈值 0 导致录像与识别不一致。

## 类与消费方式

包含 `recorder/recorder.cpp`，并通过 `dart_recording`（别名 `dart_recorder`）
INTERFACE 目标取得相机 C 数据面、线程和 zlib 依赖。类外方法均为 `inline`，可供多个
翻译单元共同包含。`dart_recording_checks` 逐文件编译全部类。

| 文件 | 职责 |
|---|---|
| `recorder.cpp` | `Recorder`，采样放行、图像队列、相机租约释放及图像消费线程 |
| `async/event_worker.cpp` | `EventWorker`，有界元数据队列、消费线程及排空关闭 |
| `storage/session.cpp` | `Session`，轮次编号、目录创建与保留清理 |
| `storage/image_writer.cpp` | `ImageWriter`，帧拷贝、独立 PNG/旧连续流写入和容量检查 |
| `storage/image_files.cpp` | `ImageFiles`，PNG 分组路径、目录创建与逐帧可用空间预算 |
| `storage/storage_space.cpp` | `StorageSpace`，查询分区可用空间及配置的总占用预算 |
| `storage/storage_space.cpp` | `StorageSpace`，分区总占用上限与物理剩余空间的共同预算 |
| `encoding/png_encoder.cpp` | `PngEncoder`，1-bit 灰度打包、快速 DEFLATE 和 PNG 封装 |
| `storage/event_writer.cpp` | `EventWriter`，事件 CSV、缓冲与刷新 |
| `error.cpp` | `RecorderError`，错误状态、系统错误码与路径 |
| `config.cpp` | 录像配置、事件与事件回调的共享契约 |

图像队列槽位是 `Recorder` 的私有类型；释放队列直接保存租约，事件队列直接
保存事件。队列计数维护容量与有效区间，图像写入器不依赖队列类型。
`ImageWriter` 持有逐帧文件管理状态，`Session` 只负责轮次目录、保留策略与启动预算；
`PngEncoder` 只做内存编码，不访问文件系统。
`EventWorker` 独立管理元数据队列和写线程；停止时先排空图像队列，再排空元数据队列，
确保 PNG 完整关闭后产生的事件仍能写出。`Recorder` 保留图像队列与相机租约的共同容量约束，
不再维护元数据线程的内部状态。

## 放行和所有权

```cpp
auto acquired = camera.acquire(std::chrono::milliseconds{0});
if (acquired && acquired->status == dart::camera::AcquireStatus::kFrame) {
    const auto decision = recorder.try_submit(std::move(acquired->lease));
    if (decision != dart::recorder::Admission::kAccepted) {
        // try_submit 未接管 lease；在采集线程释放。
        (void)acquired->lease.release();
    }
    recorder.drain_releases();
}
```

`Config::fps` 是目标录像帧率，默认 `60` 表示每秒选取 60 帧，与采集帧率独立。
`dart_app --rec-fps N` 把它注入 Recorder；未指定时沿用 `Config::fps` 的默认值。
原 `sample_every_n` / `--rec-every` 已由这个参数替代。
采样使用 `FrameView::received` 的单调微秒时间，每个 `1/fps` 时间区间最多选一帧，
不依赖未经验证的源 PTS。时间区间先于队列检查，采集停顿或队列忙后不突发补帧，
已采样的图像进入固定队列吸收短时写盘抖动。输入帧率不足或队列满时，实际落盘帧率会低于目标。
采样命中后，录像用 `try_to_lock` 和固定槽做放行：
槽满或队列锁正被占用都立即返回 `kBusy`，不等待，计数器
`Stats::busy` 增加。

`queue_slots` 分别限制已拷贝图像和待释放租约两个队列。`try_submit()` 只做有界内存
拷贝（640×480 实测约 0.2 ms），不做文件 I/O；拷贝后立即把租约交给 release 队列。
采集线程每拍提交前后调用 `drain_releases()`，保证相机 ACL 的调用仍然串行。
写线程只使用独立像素，不持有相机帧。应用使用固定 16 帧图像队列，640×480 Y8
共 4,915,200 字节，约可缓冲 267 ms 的 60 FPS 输入，覆盖实测 134 ms 的写盘停顿。
停止录像后也要先
drain，随后才能停止/销毁相机。一个 Recorder 成功启动后不能再次 start；stop 可重复调用，不会
通过再次 start 重置未释放的租约。

## 文件和对应记录

每次 `start()` 创建 `output_directory/rec_NNNN/`：

`Config::keep_sessions` 为 0 时保留全部轮次；大于 0 时在新建轮次前清理最旧的
`rec_NNNN`，将新轮次计入保留数量。开机清单使用 `--rec-keep 0 --rec-fps 60`，
每次复位后保留之前的目录；不设 90 秒时限，也不自动重启续录。
清理只处理录像器自己的文件名（兼容旧版逐帧文件），遇到未知文件停止
并报错；`last_system_error()` 和 `last_error_path()` 给出系统错误及失败路径。

```text
img/b0000001/p0000001/00000001.png
img/b0000001/p0000001/00000002.png
...                              # 每组最多 32 张，随后 b0000001/p0000002
logs/frames.csv
```

`dart_app` 默认及启动清单使用 `--rec-format png`。每帧独立、可用普通图片查看器打开。
1-bit 灰度 PNG 保留相机二值化后的全部黑白像素，使用 zlib level 1 DEFLATE，
编码缓冲和压缩器状态一次分配、逐帧复用。编码完成并检查容量后，以 FAT 8.3 短文件名（如 `00000001.png`）
直接写入并独立关闭；写入/同步失败会移除失败文件。首帧和默认每秒显式同步一次，每张图片均独立关闭。
FAT 上每 32 张分一组，限制单目录文件数量和查找开销。
`.script/player records/rec_NNNN` 支持这些 PNG 分组和旧版 PGM/PBM 连续流。
显式 `--rec-format pgm|pbm` 保留旧连续流模式；库 `Config` 的旧默认 PGM 保持兼容。

`Config::max_file_bytes` 默认且最多为 **500 MB = 500,000,000 字节**，PNG 模式按
**本轮所有 PNG 文件实际大小的总和**计算，包括 PNG 头和压缩数据，不包含 CSV/目录分配开销。
写线程先编码，再检查下一张完整图片能否放入预算；放不下时设置 `limit_reached()`，
停止接收并丢弃尚未落盘的队列图片，不生成超限文件或半张 PNG，不自动新建轮次续录。
因此最终总量可能略小于 500 MB。`Admission::kAccepted` 表示成功拷贝进队列；
PNG 的 CSV `accepted` 行只在图片实际成功写完后生成，队列中因停录未写的帧记为 `stopped`。
固定队列与压缩缓冲不会随录像增长。应用只停录像，继续识别和 IMU。
旧连续流模式仍在提交时为固定帧大小预留空间。

`dart_app` 对 `/data` 及其子目录启用 **2 GB = 2,000,000,000 字节**的分区总占用上限，
通过 `Config::max_storage_bytes` 注入；统计 `statvfs` 的已分配块，包含以前所有轮次、日志、
分区其他文件和 FAT 分配开销，不按 PNG 逻辑大小估算。其他输出分区不启用这个总量上限。
库配置为 0 时仅检查物理剩余空间。

启动时查询录像分区空间，预算取每轮上限、物理剩余空间、总量上限剩余空间的较小值，
并在后两者内预留 64 MiB 给日志及文件系统；达到保护线时不新建轮次。
PNG 及启用总量上限的旧 PGM/PBM 流最多每 32 张重新查询空间，期间每张按至少 2 MiB 扣减保守分配预算，
不足时提前重新查询；实际空间不足或查询失败时停录。目录采用两层短名称分组，
每个 p 目录最多 32 张、每个 b 目录最多 32 个 p 目录，限制目录扫描成本。
这避免高压缩图片虽逻辑总量小，仍被 FAT 小文件分配提前占满。以前轮次不自动删除。
总量保护只控制录像写入；停录后识别和 IMU 继续，其日志仍可使用预留空间。

K230 构建必须链接 SDK 的 `libk230_syscalls.a`：该库提供 RT-Smart 原生 `statfs/statvfs`
适配；仅使用当前工具链的 libc 会得到全零容量，导致录像启动报 `storage/EOVERFLOW`。

CSV 首行的 `fps`、`max_file_bytes`、`file_limit_bytes` 记录目标帧率、配置上限和本轮有效上限，格式版本为 2。
CSV 对每次提交尝试写一行（有界队列满时计数丢行），包含输入序号、源帧号、源 PTS、PTS 时间基有效位、单调接收时间、放行决定和
对应文件名、`file_offset`（帧头起始字节偏移）、`file_bytes`（实际文件/帧长度）；PNG 的 `file_offset` 固定为 0，CSV 行可能按写入完成顺序到达；
`image_index` 仍为从 1 开始的输出帧编号。降采样、忙丢弃和非法帧也有行。`Config::event_sink` 在 metadata
写线程中收到同一条 `FrameEvent`，可接入上层更快的文件/数据记录，不会在 VICAP
回调中做 I/O。`event_slots` 也是有界的，满时只丢 metadata 并累计
`Stats::event_dropped`，不会反向阻塞采集。
