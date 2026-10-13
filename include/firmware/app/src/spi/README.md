# spi/ — IIM-42622 底层 C 封装

本目录只放**底层 C 封装（硬件驱动）**，不含 C++：

- `iim42622.h` — 稳定 C ABI。不透明句柄 `dart_iim42622_t`、配置/样本结构体和
  `dart_iim42622_status_t`，只有固定宽度整数、指针和显式 POD，无厂商头文件。
- `iim42622.c` — 驱动实现，是本仓库**唯一**包含 K230 SDK 厂商头
  （`drv_spi.h`）的翻译单元。负责 SPI 实例生命周期与传输、IIM-42622 寄存器配置
  （WHO_AM_I / PWR_MGMT0 / GYRO_CONFIG0 / ACCEL_CONFIG0 / TEMP_DATA1 突发读）、量程灵敏度换算、饱和标志和单调时钟
  时间戳；厂商返回码与总线错误在这里被归一成自有状态码。

IIM-42622 使用 **SPI mode 3**；片选由 K230 SPI HAL 管理。

C++ 侧（`IImu` 抽象、`Estimator` 估计器、`k230::Iim42622` 适配器）都在 `../imu/`，
适配器是唯一同时包含 `imu/imu_sample.hpp` 与 `spi/iim42622.h` 的地方。

## 构建

`DART_IMU_K230`（在 `include/CMakeLists.txt` 的 spi 一节声明）打开时才把驱动编成
`dart_iim42622`，并且要求 RISC-V 交叉工具链存在，否则直接 FATAL_ERROR。

```bash
cmake --preset cross   # DART_IMU_K230=ON，编出 dart_iim42622 + dart_imu_k230
cmake --preset host    # K230 关闭：只有纯 C++ 的 dart_imu（estimator）
```
