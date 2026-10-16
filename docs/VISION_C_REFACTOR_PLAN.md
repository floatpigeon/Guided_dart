# 识别、帧池与录像的数据面计划

目标不是把 `.cpp` 批量改成 `.c`，而是建立“C 数据面 + C++ 控制面”的稳定边界，在不改变识别语义和帧所有权的前提下减少运行期分配、复制和不可诊断的硬件耦合。

> **修订（2026-10-11，已确认）**：本文档原计划让识别/录像走 C 数据面 + `dart_status_t` C ABI。经确认，本轮改为：
> 1. **识别与录像使用 C++ 接口，但目录按职责划分**：L4 检测在 `src/detector/`，只依赖公开的 `dart::camera::FrameView`；录像含线程与文件写入，属于 `runtime/recordor/`；应用组装在 `app/`。纯 C++ 不等于领域层，具体运行时实现不能放入 L4。对外接口为 `dart::detection::IDetector`、`dart::recorder::Recorder`。§2.1 的 `dart_status_t`/`dart_gray_frame_t` 与 §2.2 的定长 `DartError` **不再采用**：跨层错误沿用相机模块的 `int` 站点码 + `std::expected`；业务模块尚无共享错误类型，录像 `start()` 暂用 `bool` + `last_error()`，该缺口记在下方"未决问题"。
> 2. **本轮 recorder 只做灰度证据落盘**（PGM/PBM + events.csv，有界队列 + 写线程 + 丢帧计数），不碰厂商 API。v1 的录像 100% 依赖 VENC/VB（`kd_mpi_venc_*` ×12、`kd_mpi_vb_*` ×6，无软件编码路径），硬件 H.264 需要一个独立的 `video/` ACL（照相机模块的三层做法），留待后续。
> 3. **帧分发用"拷贝"模型**：采集线程独占 `ICamera`，`acquire → 二值化/识别 → 拷 Y 进 recorder 空闲块 → release`，与 v1 同构；租约所有权不跨线程（release 本身是一次相机调用，必须留在采集线程）。
> 4. **内核语言可后置**：对外纯 C++ 与内核是否为 C 是两件事。v1 的三个 RVV 内核（`binarize.c`、`tile_scanner_c.c`、`roi_measure_c.c`）零厂商依赖，可原样搬入并由 C++ 适配器包裹；`DETECTOR_DESIGN.md` 要求的标量参考路径一并保留。
>
> **v1 调研结论（移植前必读）**：
> - v1 的 `IDetector::detect(const GrayFrame&)` 吃的是**已二值化**的 0/255 图，且 `tile_scanner_c.c`/`roi_measure_c.c` 把亮度门限写死为 `DART_BRIGHT_MIN 128u`。要让模块直接消费 Y8 灰度，必须把二值化并入模块并把该常量改为运行期阈值。
> - `roi_measure_c.c` 坐标用 `uint16`：宽/高 > 65535 直接拒收，runs 上限 65535（640×360/1280×720 无碍，但要写进契约）。
> - `TileScanner::run` 在尺寸变化时会每帧重新 `prepare()` → `new (std::nothrow) uint8_t[]`，违反"运行态不分配"；移植时改为启动期固定 workspace。
> - v1 识别层唯一的厂商依赖是 `vision/frame_pool.{hpp,cpp}`（MMZ/VB），**整块删除**，由 `FrameView` 取代；`src/detection/` 全树零 SDK 依赖、无 `throw`/`dynamic_cast`/`typeid`，与 `-fno-exceptions -fno-rtti` 天然兼容。
> - v1 检测跑在采集线程，无检测专用线程——与第 3 条的拷贝模型一致。
>
> **未决问题**：业务模块（识别/录像）的共享错误类型尚未确定；`VISION_C_REFACTOR_PLAN` 原定的定长 `DartError` 与 `STRUCTURE_DESIGN.md` §2.2 同源，需与相机模块的多态 `ErrorPtr` 一并裁决。

## 1. 目标和非目标

目标：

1. 将像素热循环、固定工作区、帧池、MPP/VENC 句柄和队列状态放到可审计的 C ABI；
2. 运行态不做 `malloc/free/new/delete`，明确每个池槽的所有者和 cache/DMA 同步；
3. 保留 C++ 编排、跟踪、状态机、错误决策和控制层；
4. 在 C++ 控制面用 `std::expected<T, DartError>` 表达跨层失败，在 C ABI 中只使用状态码和 POD；
5. 以主机纯算法、交叉构建和板端量化结果共同验收。

非目标：

- 不以 C 作为天然性能保证；
- 不在这一轮重写 BSP/SDK、固件或整个应用；
- 不为主机测试加入 MPP/VICAP/VENC 的伪兼容路径；
- 不把“通过主机回归”写成视觉硬件已通过。

## 2. 边界规则

### 2.1 C ABI

```c
#ifdef __cplusplus
extern "C" {
#endif

typedef enum dart_status {
    DART_STATUS_OK = 0,
    DART_STATUS_INVALID_ARGUMENT,
    DART_STATUS_NO_MEMORY,
    DART_STATUS_OVERFLOW,
    DART_STATUS_TIMEOUT,
    DART_STATUS_HARDWARE_ERROR,
    DART_STATUS_STALE,
    DART_STATUS_BUSY
} dart_status_t;

typedef struct dart_gray_frame {
    uint32_t struct_size;
    const uint8_t *data;
    uint32_t width, height, stride;
    uint64_t timestamp_us;
    uint32_t flags;
} dart_gray_frame_t;

#ifdef __cplusplus
}
#endif
```

所有公共结构体带尺寸/版本；字段使用固定宽度类型；每个指针都写清所有者、只读/可写、有效期、cache 属性和 DMA 同步。C 函数返回状态码加输出参数，不能跨越 C++ 异常。

### 2.2 C++ 控制面错误

C++23 `Result<T> = std::expected<T, DartError>` 只用于初始化、资源创建、跨层调用和需要保留失败原因的操作。没有目标、ROI 为空、候选被门限拒绝是正常结果，用枚举/`bool` 和计数器表示。

`DartError` 定长、可平凡复制、包含 domain/code/operation/native_code/detail 字段，不持有动态字符串。叶子模块不打印同一错误；可恢复边界记录一次动作（drop/retry/disable），不可恢复边界记录一次 fatal 并停止。

### 2.3 C 回调和线程

C 回调一律 `(void *context, const event *)`，C++ 用 `extern "C" noexcept` trampoline；异常不得跨边界。实时线程不格式化长日志、访问文件系统或无限等待；队列有界，满时按策略丢样本并累计计数。

## 3. 推荐模块边界

| 模块 | 首阶段语言 | 目标位置 | 说明 |
|---|---|---|---|
| 二值化 | C | `src/detector/binarize.c`（规划，当前集成在 `detector.cpp`） | `restrict`、边界和 stride 检查，保留标量参考 |
| 帧池 | C | `include/firmware/app/src/camera/{c_api,k230}/` | 槽状态、VB/MMZ、cache/DMA 和所有权 |
| 全图扫描 | C | `src/detection/scanner/tile_scanner.c` | 瓦片统计和 RVV 快路径，不能认识 tracker |
| ROI 测量 | C | `src/detection/measure/roi_measure.c` | 固定工作区、连通域和形状质量 |
| 几何纯函数 | C | `src/detection/geometry/*.c` | POD 输入输出、无全局状态 |
| 录像/取证 | C++（本轮决定） | `src/recorder/recorder.cpp` | 有界队列、丢帧计数、写线程；灰度证据（PGM/PBM/CSV）。硬件 H.264 归后续 `video/` ACL |
| pipeline/tracker | C++ | `src/detection/*.cpp`（本轮：接口在 `src/detection/`，实现待移植） | 编排、状态机、滤波、诊断和 C/C++ 翻译 |
| 控制/主程序 | C++ | `src/control/`、`app/main.cpp`、`app/application.cpp` | 生命周期、策略、配置和依赖组装 |

先保持 C++ 的模块包括 `DetectionPipeline`、`TargetTracker`、尺度卡尔曼和应用入口；若将来要移除 libstdc++，另开任务，不与第一轮数据面迁移混合。

## 4. 工作区和内存账

所有 C 模块采用：

```c
size_t dart_xxx_context_size(void);
size_t dart_xxx_workspace_size(const dart_xxx_config_t *, uint32_t w, uint32_t h);
dart_status_t dart_xxx_init(void *ctx, size_t ctx_bytes,
                             void *workspace, size_t workspace_bytes,
                             const dart_xxx_config_t *cfg);
```

初始化后处理函数不得分配/释放；工作区不足必须返回错误，不能静默截断或退化。启动期 arena 可统一分配，但每个模块要记录字节数、高水位和对齐要求。配置改变工作区大小必须停机重建。

帧池、录像输入池和 VENC 输出池要分开记账，不能在文档中把“复制到非缓存输入块”写成零拷贝。每个池记录槽数、单槽大小、cache 属性、当前状态和释放方；任何跨模块传递都只传句柄/索引加生命周期契约。

## 5. 分阶段实施

### 阶段 0：冻结基线

- 确认当前 Git 实际目录和 CMake 可配置性；
- 增加 C11/C++23、`std::expected`、`-fno-exceptions` 的最小编译/链接探针；
- 定义固定输入、日志 schema、内存账和板端探针格式；

完成条件：主机纯类型可编译、交叉探针可判定、参数/日志版本稳定。

### 阶段 1：公共 ABI 和错误契约

- 建立 `include/core/status.h`、错误枚举和静态断言；
- 写 frame/ROI/result 的所有权、stride、时间戳和 valid 语义；
- C++ 包装只做 RAII/类型转换，不复制像素；
- 添加失败注入测试，确保错误只在决策边界记录一次。

### 阶段 2：帧池和二值化

- 先以普通 C 标量路径建立正确性和工作区查询；
- 加入槽位状态、cache/DMA 注释和运行态分配计数器；
- 用固定帧验证极性、stride、边界和 invalid 输入；
- 板端探针确认实际带宽后再考虑 RVV。

### 阶段 3：全图扫描

- C 实现提供标量参考和可选 RVV 实现；
- 16×16 瓦片、邻域合并、Top-K、圆度和面积门限均用值类型表达；
- RVV 自检逐候选对比标量参考，工具链/VLEN 作为构建条件；
- 记录扫描 p50/p95/max、图像大小和 cache 状态。

### 阶段 4：ROI 测量与跟踪接线

- C 测量器只接受绝对/明确区间的 ROI 和工作区；
- C++ tracker 处理确认、尺度状态、马氏门限、连续拒收救援和重捕获；
- 输出 `fresh/held/lost/rejected/out_of_roi`，控制层只接收有效结果；
- 用合成输入验证状态机，但真实曝光/ROI 带宽必须板端确认。

### 阶段 5：录像数据面

- 固定输入块和 `free → inflight → free` 状态；
- VENC/MMP 返回码在 ACL/C 适配器翻译，写线程只拿句柄/索引；
- `send/get_stream` 使用有界超时，失败立即回收或进入明确降级；
- 运行期不逐包 mmap/munmap，不把大对象放写线程栈。

### 阶段 6：池数量和零拷贝实验

先建立复制路径的稳定板端基线，再逐项实验共享池、flush/invalidate、槽数和队列长度。每次只改变一个变量，比较帧率、p95、内存高水位、队列丢弃和视觉结果。零拷贝只有在所有者、cache 和 VENC 生命周期可证明时才接受。

### 阶段 7：集中错误传播和日志

删除叶子层“打印后返回”的重复日志；在应用启动、帧循环恢复、录像降级和线程退出边界记录结构化错误。高频相同错误采用首次若干条加时间窗汇总；正常否定结果进入诊断 CSV/计数器。

### 阶段 8：运行时收紧

在初始化错误路径稳定后启用 `-fno-exceptions -fno-rtti`（若工具链探针通过），收紧 C++ 容器和动态分配审计，检查 ELF 段大小、启动堆高水位和所有线程栈上限。

## 6. 验证矩阵

| 层级 | 可验证内容 | 不可替代内容 |
|---|---|---|
| 主机 | C 内核、几何、状态机、ABI、失败注入 | RVV 时序、cache、MPP、曝光 |
| 交叉构建 | C/C++ 标准、符号、链接、段大小 | 实际 DMA/传感器行为 |
| 板端探针 | stride、cache、VB、帧时间、池状态 | 纯算法覆盖率 |
| 板端整链 | 真实识别、延迟、录像、内存和日志 | 不同固件未测配置 |

每轮提交附：变更模块、主机结果（若适用）、交叉构建结果、板端命令/参数、原始日志和失败样本。性能结论写明工具链、频率、分辨率、线程/核、输入和测量日期。

## 7. 主要风险

- C/C++ ABI 的尺寸、对齐或所有权不一致；
- cache/DMA 未同步导致偶发旧帧；
- 池槽重复释放、队列满或 VENC 生命周期错误；
- 把“无目标/拒收”错误化导致日志风暴；
- 以主机性能或历史 v1 记录推断板端实时性；
- 运行期动态分配、隐式复制或未界定循环破坏最坏时延；
- 为测试引入业务层板端模拟，掩盖真实硬件缺陷。

## 8. 提交顺序

建议每个提交只跨一个边界：`status/ABI → frame_pool → binarize → scanner → ROI → tracker → recorder → C++ 错误传播 → 优化`。涉及 cache、VB/MMZ、VENC、线程栈或真实帧时间的提交，必须同时附板端结果；没有板端条件时标为 `BLOCKED: hardware validation pending`，不宣称完成。
