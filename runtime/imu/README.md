# IMU 采样与日志

`session.cpp` 的 `dart::imu::Session` 接收现有 `IImu&`，持有唯一的 `Estimator`。
独立采样线程调用 `read_sample()` / `Estimator::update()`，把返回的完整 `Estimate`
同时保存为上层快照并送入日志队列。日志线程只写快照，不进行第二次积分。
应用须保证设备已初始化，并且生命周期覆盖 Session。

```cpp
dart::imu::Session session{device};
if (!session.start("/data/app/logs/imu.log"))
    return 1;
const auto seeded = session.seed(sensor_to_reference); // 取当前样本，要求静止。
if (seeded.accepted) {
    const auto estimate = session.current();          // 与日志中的数据一致。
    // 后续控制周期继续读取 session.current()。
}
session.reset();                                      // 清零并失去参考系，等待下一次 seed。
session.stop();                                       // 停采样、排空队列、同步关闭文件。
```

`current()`、`seed()`、`reset()` 可以由上层线程调用，同一把状态锁保证命令与样本
的先后顺序。重新积分应调用这个 Session 的 `seed()`，不要另建估计器。
成功 seed 的事件记录清零后的速度/累计旋转、新姿态和新 `generation`；失败则保留
当前状态，记录 `command_accepted=0` 和 `command_quality_events`。reset 也单独记录，
并递增 generation。上层读取完整质量位和时间戳；读取错误不会伪造新样本。

`start(path, period_us, initial_seed, log_limit_bytes)` 每个实例只允许成功启动一次。默认采样周期
1000 µs，`initial_seed=false`，由上层明确给出初始参考系。`dart_app` 传入 true，
启动阶段反复请求以单位四元数进行静止 seed，首次成功后停止自动 seed；上层任何
显式 seed/reset 都取消这个启动请求，reset 后不会自行重新积分。
start/stop 由应用生命周期线程串行调用；stop 后应停止消费者发出命令。

日志默认以 CSV 列写入 `.log` 文件，追加模式保留每轮内容。每次启动追加
`# imu_log` 元数据行和列名，每次正常结束追加 `# session_end` 汇总行。
元数据包含估计器门限、采样周期和固定队列内存大小。解析时跳过 `#` 注释并识别
每轮列名；generation 和 sequence 在新进程/新 Session 中重新计数。

原始日志默认限制为 **32 MiB**，包含已存在的内容，重启不会重置额度。
达到上限后停止向该文件追加，保留已有内容；采样、估计和上层快照继续更新。
不会自动删除或覆盖日志。此时 `stats().log_limit_reached=true`，未写入事件计入
`dropped`，不计为 I/O 错误；文件可能以最后一条完整事件结束，没有结束汇总行。
应用的独立状态日志仍报告采样计数、文件字节数和限额标志。第四参数可指定其他
正数字节上限；记录新一轮完整原始数据前，应先归档并清理旧文件。

| 列 | 含义 |
|---|---|
| `event` | `sample`、`seed`、`reset`、`read_error` |
| `sequence,recorded_ns,timestamp_ns` | 事件序号、处理时间、采样时间；ns，单调时钟域 |
| `generation` | 积分批次；成功 seed 和 reset 推进 |
| `raw_a*,raw_g*` | 传感器 ADC 原始计数，三轴各一组 |
| `a*_m_s2,g*_rad_s,temperature_c,sample_flags` | 原始样本的 SI 值、温度和标志；传感器轴 |
| `qw,qx,qy,qz,roll_rad,pitch_rad,yaw_rad` | 传感器到参考系的四元数与 RPY 姿态 |
| `w*_rad_s,linear_a*_m_s2,v*_m_s,rotation_*_rad` | 参考系角速度、去重力加速度、积分速度、累计旋转 |
| `quality_events,sample_accepted,integrated,has_reference_frame` | 原样保存 Estimate 的有效性和异常标志 |
| `command_accepted,command_quality_events,seed_q*` | 命令结果、拒绝原因、请求的参考四元数 |
| `read_error,dropped` | IImu 读取错误码（非读取错误为 -1）、累计日志事件丢弃数（队列满或达到文件限额） |

浮点值用 9 位有效数字保存，可还原 float。饱和、非有限和时间异常的样本也记录，
通过质量位和 integrated 区分，不能把未积分的旧状态当成新积分结果。
`read_error` 行的姿态/样本是上一次快照，应结合 event 和 timestamp 判断年龄。

文件只由日志线程写入，每秒刷新 stdio 缓冲并 fsync，退出时再次同步。固定 512 条队列在
启动期分配；队列锁只保护单条快照复制，写盘期间不持锁。队列满即丢弃该事件，
sequence 缺口、后续 dropped 列和结束汇总可定位缺失，命令也遵循此规则。
`stats()` 提供 samples/read_errors/written/dropped/io_errors/log_bytes/log_limit_reached；written 指写入 stdio
成功的事件数，是否全部落盘还须检查 io_errors。异常断电可能丢失未同步内容。

主机 `imu_session_test` 覆盖日志与上层快照逐字段一致、重复时间戳、饱和、NaN、
gap、重积分成功/失败、参考系变换、reset、初始 seed、读错误、文件错误，以及
达到文件限额后仍能采样和 seed、跨会话保留已有内容且不突破限额。
真实 SPI 采样率、调度抖动、SD 卡吞吐和长时间记录仍待板端验证。
