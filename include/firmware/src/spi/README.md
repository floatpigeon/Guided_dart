# spi/ — ICM-42622-P / IIM-42652 底层 C 封装

本目录只放**底层 C 封装（硬件驱动）**，不含 C++：

- `iim42622.h` — 稳定 C ABI。不透明句柄 `dart_iim42622_t`、配置/样本结构体和
  `dart_iim42622_status_t`，只有固定宽度整数、指针和显式 POD，无厂商头文件。
- `iim42622.c` — 驱动实现，是本仓库**唯一**包含 K230 SDK 厂商头
  （`drv_spi.h`）的翻译单元。负责 SPI 实例生命周期与传输、受支持 IMU 的寄存器配置
  （WHO_AM_I / PWR_MGMT0 / GYRO_CONFIG0 / ACCEL_CONFIG0 / TEMP_DATA1 突发读）、量程灵敏度换算、饱和标志和单调时钟
  时间戳；厂商返回码与总线错误在这里被归一成自有状态码。

使用 **SPI mode 3**；片选由 K230 SPI HAL 管理。保留历史 `iim42622` 文件名与接口名，
按 WHO_AM_I 区分 `0x46`（ICM-42622-P）和 `0x6f`（IIM-42652），其他 ID 仍拒绝初始化。
`dart_iim42622_device_name()` 返回实际识别的型号。新板重新连接后实测 ID 为 `0x6f`。

IIM-42652 的寄存器和换算依据 TDK 的 [DS-000440 v1.0](https://cdn.sparkfun.com/assets/4/2/e/d/c/IIM-42652_Datasheet.pdf)：
软复位后等待 2 ms，在休眠状态设置量程/ODR，再开启低噪声模式并等待 50 ms；
读回电源和量程/ODR 寄存器确认配置生效，再允许采样。该顺序满足开启后 200 µs 内
不写寄存器的要求，也覆盖陀螺仪启动时间。温度按 `raw / 132.48 + 25` 换算。

C++ 侧（`IImu` 抽象、`Estimator` 估计器、`k230::Iim42622` 适配器）都在 `../imu/`，
适配器是唯一同时包含 `imu/interface.cpp` 与 `spi/iim42622.h` 的地方。

## 构建

`DART_IMU_K230`（在 `include/CMakeLists.txt` 的 spi 一节声明）打开时才把驱动编成
`dart_iim42622`，并且要求 RISC-V 交叉工具链存在，否则直接 FATAL_ERROR。

```bash
cmake --preset cross   # DART_IMU_K230=ON，编出 dart_iim42622 + dart_imu_k230
cmake --preset host    # K230 关闭：只有纯 C++ 的 dart_imu（estimator）
```
