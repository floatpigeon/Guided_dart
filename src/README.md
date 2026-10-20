# 业务源码

`src/` 全部属于 L4，按功能划分调用、编排与策略。设备初始化、线程、文件系统、
编码和日志写入的实现放在 `include/firmware/src/`，通过公开接口供 L4 调用。

| 路径 | 职责 |
|---|---|
| `dart_entry.cpp` | 唯一业务进程入口，将板级工厂注入启动流程 |
| `startup/` | 参数解析、服务组装、故障隔离和运行顺序 |
| `vision/` | 相机采集生命周期、逐帧识别和帧分发 |
| `recording/` | 录像参数映射、启停决策和采集线程中的租约排空 |
| `diagnostics/` | 相机错误的业务日志字段和检测 CSV 字段映射 |
| `detector/` | 灯/装甲板检测与 ROI，只消费帧数据 |

`startup/` 通过注入的设备工厂调用下层服务，不处理硬件配置和 POSIX 资源。
`vision/` 决定帧如何交给识别和录像；`recording/` 决定何时启停录像，
不拥有写线程或编码器；`diagnostics/` 定义业务记录内容，不管理文件缓冲与同步。
检测器只包含 `camera/frame_view.cpp`，构建目标只依赖 `dart_camera_types`；
不持有相机、不创建设备、不操作帧租约。
所有 L4 模块均不包含 K230/MPP 或 POSIX 实现头文件。

`src/CMakeLists.txt` 统一声明领域库与业务应用目标；`dart_detection_checks`
和 `dart_application_checks` 分别独立编译所属模块，领域库不继承应用的设备或
运行时依赖。板级、日志、存储和录像目标由 `include/CMakeLists.txt` 声明；
`runtime/` 中现有 IMU 采样服务本轮保留，launcher 负责进程监督。

应用参数、数据流和退出契约见 [`startup/README.md`](startup/README.md)。
