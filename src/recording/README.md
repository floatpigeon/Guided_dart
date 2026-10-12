# 录像 / 灰度证据

`dart::recorder::Recorder` 是本轮的录像垂直切片。它只接收相机 ACL 的
`FrameLease`，不包含 K230、MPP 或 VENC 头文件；输入帧在线程中拷贝为紧凑
Y8，再写成 PGM（或阈值打包后的 PBM）。硬件 H.264 需要后续独立的 video ACL，
不会把厂商句柄带入这个上层模块。

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

`sample_every_n` 先于队列检查，因此 240 Hz 输入配 `4` 会先降到约 60 Hz，
不会用加深队列吸收写盘抖动。采样命中后，录像用 `try_to_lock` 和固定槽做放行：
槽满、写线程忙或锁正被写线程占用都立即返回 `kBusy`，不排队、不等待，计数器
`Stats::busy` 增加。

`queue_slots` 是硬上限；槽从“已接收”到“拷贝完成并写完文件、等待采集线程释放
租约”一直占用，因而也包含当前拷贝中的帧和 release 队列中的帧。录像线程拷贝
完成后不直接调用相机 `release()`，而是把租约放入固定的 release 队列；采集线程
每拍调用 `drain_releases()`，保证相机 ACL 的调用仍然串行。停止录像后也要先
drain，随后才能停止/销毁相机。一个 Recorder 成功启动后只允许 stop 一次，不会
通过再次 start 重置未释放的租约。

## 文件和对应记录

每次 `start()` 创建 `output_directory/rec_NNNN/`：

```
img/frame_00000001.pgm   # 或 .pbm
logs/frames.csv
```

CSV 对每次提交写一行，包含输入序号、源帧号、源 PTS、PTS 时间基有效位、单调接收时间、放行决定和
对应图片名；降采样、忙丢弃和非法帧也有行。`Config::event_sink` 在 metadata
写线程中收到同一条 `FrameEvent`，可接入上层更快的文件/数据记录，不会在 VICAP
回调中做 I/O。`event_slots` 也是有界的，满时只丢 metadata 并累计
`Stats::event_dropped`，不会反向阻塞采集。
