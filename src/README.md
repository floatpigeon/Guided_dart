# L4 领域与策略

`src/` 只放检测、跟踪、观测器、控制律和领域状态机等算法及策略。
当前实现为 `detector/`；未来控制模块也放在这里。

L4 可以依赖 L3 的公开数据类型或抽象接口。检测器只包含
`camera/frame_view.hpp`，构建目标只依赖无实现的 `dart_camera_types`；不持有相机、
不创建设备、不操作帧租约，也不包含 K230/MPP 或 POSIX 运行时实现。

应用入口、具体组件选择和生命周期组装放在 `app/`；进程监督、录像线程和文件
写入放在 `runtime/`。这些目录可以组合领域对象，L4 不反向依赖它们。

本目录的领域库统一在 `src/CMakeLists.txt` 中声明；应用和运行时目标分别在
`app/CMakeLists.txt`、`runtime/CMakeLists.txt` 中声明。
