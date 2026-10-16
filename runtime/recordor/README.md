# 录像 / 灰度证据

`dart::recorder::Recorder` 是本轮的录像垂直切片。它只接收相机 ACL 的
`FrameLease`，不包含 K230、MPP 或 VENC 头文件；输入帧在线程中拷贝为紧凑
Y8，再写成 PGM（或阈值打包后的 PBM）。硬件 H.264 需要后续独立的 video ACL，
不会把厂商句柄带入这个运行时服务。本模块包含线程管理和具体文件系统操作，
放在 `runtime/`；L4 算法不依赖它。构建目标在 `runtime/CMakeLists.txt` 中声明。

## 类与消费方式

包含 `runtime/recordor/recorder.cpp`，并通过 `dart_recording`（别名 `dart_recorder`）
INTERFACE 目标取得相机 C 数据面和线程依赖。类外方法均为 `inline`，可供多个
翻译单元共同包含。`dart_recording_checks` 逐文件编译全部类。

| 文件 | 职责 |
|---|---|
| `recorder.cpp` | `Recorder`，放行、固定队列、线程、释放调度及嵌套的 `Stats` |
| `session.cpp` | `Session`，轮次编号、目录创建与保留清理 |
| `image_writer.cpp` | `ImageWriter`，接收像素与文件名，负责帧拷贝及 PGM/PBM 编码写入 |
| `event_writer.cpp` | `EventWriter`，事件 CSV、缓冲与刷新 |
| `error.cpp` | `RecorderError`，错误状态、系统错误码与路径 |
| `config.cpp` | 录像配置、事件与事件回调的共享契约 |

图像队列槽位是 `Recorder` 的私有类型；释放队列直接保存租约，事件队列直接
保存事件。队列计数维护容量与有效区间，图像写入器不依赖队列类型。

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

`Config::fps` 是目标录像帧率，默认 `30` 表示每秒选取 30 张，与采集帧率独立。
`dart_app --rec-fps N` 把它注入 Recorder；未指定时沿用 `Config::fps` 的默认值。
原 `sample_every_n` / `--rec-every` 已由这个参数替代。
采样使用 `FrameView::received` 的单调微秒时间，每个 `1/fps` 时间区间最多选一帧，
不依赖未经验证的源 PTS。时间区间先于队列检查，采集停顿或队列忙后不突发补帧，
不会用加深队列吸收写盘抖动。输入帧率不足或写盘忙时，实际落盘帧率会低于目标。
采样命中后，录像用 `try_to_lock` 和固定槽做放行：
槽满、写线程忙或锁正被写线程占用都立即返回 `kBusy`，不排队、不等待，计数器
`Stats::busy` 增加。

`queue_slots` 是硬上限；槽从“已接收”到“拷贝完成并写完文件、等待采集线程释放
租约”一直占用，因而也包含当前拷贝中的帧和 release 队列中的帧。录像线程拷贝
完成后不直接调用相机 `release()`，而是把租约放入固定的 release 队列；采集线程
每拍调用 `drain_releases()`，保证相机 ACL 的调用仍然串行。停止录像后也要先
drain，随后才能停止/销毁相机。一个 Recorder 成功启动后不能再次 start；stop 可重复调用，不会
通过再次 start 重置未释放的租约。

## 文件和对应记录

每次 `start()` 创建 `output_directory/rec_NNNN/`：

`Config::keep_sessions` 为 0 时保留全部轮次；大于 0 时在新建轮次前清理最旧的
`rec_NNNN`，将新轮次计入保留数量。开机清单设置为 2，使用 90 秒分段和
`--rec-fps 30`。1080p PGM 每张约 2 MB；原每 4 帧取图的配置实测 90 秒写入约
725 MB，连续复位容易耗尽存储。清理只处理录像器自己的文件名，遇到未知文件停止
并报错；`last_system_error()` 和 `last_error_path()` 给出系统错误及失败路径。

```
img/frame_00000001.pgm   # 或 .pbm
logs/frames.csv
```

CSV 首行的 `fps` 记录本轮注入的目标录像帧率。
CSV 对每次提交写一行，包含输入序号、源帧号、源 PTS、PTS 时间基有效位、单调接收时间、放行决定和
对应图片名；降采样、忙丢弃和非法帧也有行。`Config::event_sink` 在 metadata
写线程中收到同一条 `FrameEvent`，可接入上层更快的文件/数据记录，不会在 VICAP
回调中做 I/O。`event_slots` 也是有界的，满时只丢 metadata 并累计
`Stats::event_dropped`，不会反向阻塞采集。
