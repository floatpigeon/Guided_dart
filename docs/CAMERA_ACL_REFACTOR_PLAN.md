# 相机底层重构计划：C 数据面 → C++ 虚拟接口（防腐层）

> 本轮目标：把 v1 的相机实现（`src/camera/*.c` + `include/camera/*.hpp`）重构为 v2 形态——C 只返回 `int` 错误码且不做任何其他处理，C++ 层把数字码"翻译"成带丰富上下文的多态错误对象，并用虚接口把上层与厂商 C API 隔开。
>
> 本文档是执行契约：§1 的边界约定与 §1.1 的三处遗留处置均已确认冻结，T1 之后的任务不得再改；有异议在开工前提出。
> 注意：`.gitignore` 含 `docs/*.md`，本文档以 `git add -f` 方式纳入版本控制。
>
> **契约修订（2026-10-11，已确认）**：§1.2 / §1.3 / §1.4 中"单未决租约 + `optional<FrameLease>`"的约定作废，改为**上层注入的有界租约容量**：
> - **C ABI 升到 v3**：`dart_camera_config_t` 新增 `max_outstanding_frames`（1..`channel_buffers`，上限 `DART_CAMERA_MAX_OUTSTANDING_FRAMES`），`DART_CAMERA_ABI_VERSION = 3`；令牌全局唯一，**释放顺序自由**；新增站点码 `DART_CAMERA_E_LEASE_EXHAUSTED(141)`（正常否定结果，不是故障）与只读查询 `dart_camera_get_lease_usage`。
> - **C 侧按槽位持有 dump**：`frame_held[]` / `frame_info[]` / `dart_frame_lease_pool_t`；`dart_camera_release_dump(camera, slot)`。stop/deinit/destroy 仍在任一租约未释放时返回 BUSY。失败的帧回收会释放其租约槽位（失败帧不占容量），未能释放的 dump 留给 stop/deinit 重试并置 FAULTED。
> - **C++ 侧容量以编译期参数注入**：`create_k230_camera<N>`，槽位是 `std::array<detail::FrameLeaseSlot, N>`，成功路径零分配（原逐帧 `new` 一并消除），`capacity()`/`outstanding()` 供上层实施自己的丢弃策略；`acquire` 返回三态 `AcquireResult{kFrame, kNoFrame, kLeaseExhausted}`，**耗尽即丢帧**：不触碰驱动、不进错误通道。
> - **析构安全**：相机销毁时仍被持有的租约被脱钩为 `kDetached`，其后 `release()` 返回 `StaleLeaseError`，不再解引用已销毁对象；SDK 句柄保留待显式重试（上层违反"先释放再销毁"的代价是泄漏而非崩溃）。
> - **验证状态**：主机构建、`gcc -fsyntax-only`（真实 SDK 头）、clangd 静态检查均通过；**交叉构建与交叉链接均已通过**（见 §3，工具链在 `SelfGuidingDart/third_party/toolchain/`）；
>
> **布局与命名修订（2026-10-11，第二次，已确认）**：按 librmcs 的约定重排文件层次与命名，取代 §1.1 与 §1.5 的原树：
> - **平台进目录/命名空间，不进类名**：`k230_camera.hpp/.cpp` → `k230/camera.hpp/.cpp`；类 `K230Camera<N>` → `dart::camera::k230::Camera<N>`；工厂 `create_k230_camera<N>` → `dart::camera::k230::create<N>`（对应 librmcs 的 `librmcs::firmware::gpio::Gpio`）。
> - **按层分目录**：平台无关的接口与错误层留在模块根（`camera.hpp`、`error.hpp/.cpp`、`error_codes.h`）；C ABI 与平台无关实现进 `c_api/`（`camera.h`、`config.c`、`lease.h/.c`）；一切碰 SDK 的东西进 `k230/`（4 个 `.c` + 两个私有头 + C++ 适配器）。"谁能碰厂商头"从"靠自觉"变成"看目录"。
> - **模块内 include 改为模块限定**（`#include "camera/k230/internal.h"`），不再使用 `../`。
> - **验证**：重排后主机构建、SDK 头 `gcc -fsyntax-only`、交叉构建、交叉链接探针、clangd 全量扫描、两个 `/tmp` harness（C 池 63 项 / C++ 适配器 87 项）全部复跑通过；本次只改名字与位置，未改语义。
> - §2/§3 里 T1..T7 的文件名是当时的执行记录，保持原样不再回改。
租约状态机与 C++ 适配器各有一个一次性 `/tmp` harness（**未入库**，本轮决定不引入 `tests/`）：C 池 63 项、C++ 适配器 87 项检查全部通过。板端仍为 `BLOCKED: hardware validation pending`。

---

## 0. 本轮目标与非目标

**目标分层**

```
L3  设备抽象（纯 C++）      ICamera 虚接口 + FrameLease + 类型化状态/模式/帧视图
L2  防腐层 ACL（C++）       K230Camera 适配器：调用 C ABI，把 int 码"翻译"成多态错误对象
L1  C 数据面（纯 C）        camera.c / media / frame / exposure / config：只返回 int，不做其他任何事
```

**非目标（本轮明确不做）**

- 不做主机 fake、不做 C/C++ 单元测试；本轮验证 = 编译 + 静态检查 + 交叉构建，板端标记 BLOCKED；
- 不改 SDK/BSP，不引入新的第三方依赖，不做 IMU/录像/控制模块；
- 不为了"统一风格"把厂商 SDK 改成 C++：厂商头文件只允许出现在 `.c` 里；
- 不宣称板端已验证（无硬件证据时只能写 `BLOCKED: hardware validation pending`）；
- 不做文档重写收尾（原 T8 已按决定移出本轮）：`README.md` 与 `docs/STRUCTURE_DESIGN.md` 的正文更新留到下一轮，本轮只做**路径修复**（指到新目录的文件确实存在）。

---

## 1. 冻结的边界契约

### 1.1 目录布局（新树）

新文件全部落在 `include/firmware/app/src/camera/`；`src/` 整体删除。

```
include/firmware/app/include/status.h          # 保留但本轮不接线/不修改：跨模块通用状态码（本模块不依赖它）
include/firmware/app/src/camera/               # 模块根只放平台无关的接口与错误层
├── camera.hpp              # L3 虚拟接口：ICamera / FrameLease / FrameView / AcquireResult / 类型化枚举
├── error.hpp / error.cpp   # L2 多态错误对象层次 + 翻译器 + 固定缓冲文本出口
├── error_codes.h           # C/C++ 共用：码空间划分宏 + dart 站点码表 + K230 SDK 码镜像表
├── c_api/                  # L1 C ABI 与平台无关实现（只返回 int）
│   ├── camera.h            # 公开 C ABI：POD 结构 + int 返回；无 error 出参
│   ├── config.c            # config init/validate（纯 C，可主机编译）
│   └── lease.h / lease.c   # 有界租约池状态机（纯 C，可主机编译）
├── k230/                   # 平台特化：唯一允许碰 SDK 头与 camera.h 的地方
│   ├── internal.h          # struct dart_camera 与 TU 间私有契约（含 k_video_frame_info）
│   ├── media.h             # 输出池描述 + attach/detach 契约
│   ├── lifecycle.c         # 生命周期：create/init/start/stop/deinit/destroy/state/caps/usage
│   ├── media.c             # media(VB) 所有权 + 私有输出池 + 块映射
│   ├── frame.c             # acquire/release/dump/缓存 invalidate/时间戳
│   ├── exposure.c          # 曝光范围查询 + 秒写入（clamp/换算在 C++）
│   └── camera.hpp/.cpp     # ACL 适配器 dart::camera::k230::Camera<N> + create<N>()
├── README.md               # 模块契约
└── CMakeLists.txt          # 模块构建 + include 导出

命名约定（对齐 librmcs）：**平台进目录与命名空间，不进类名**——适配器是
`dart::camera::k230::Camera`，不是 `K230Camera`（对应 librmcs 的
`librmcs::firmware::gpio::Gpio`）；文件名小写下划线、限定交给所在目录
（`c_api/camera.h` 与 `k230/frame.c` 不再背 `camera_` 前缀）；同名文件出现在不同层
是可接受的（librmcs 的 `spec/*/gpio.hpp` 与 `app/src/gpio/gpio.hpp` 同理），由 include
路径区分。
```

**删除清单**

- `src/**`（含 `src/CMakeLists.txt`、`src/camera/README.md` 及全部 `.c/.h`）；
- `include/camera/exposure_control.hpp`、`include/camera/exposure_adapter.hpp`（`IExposureControl` 并入 `ICamera`）；
- `include/firmware/app/src/logger/error.h` 与 `logger/error.c`（后者为空文件），该目录一并移除。

**三处遗留处置（已确认，T1 执行）**

| 项 | 结论 | 理由 |
|---|---|---|
| `include/firmware/app/src/logger/error.{h,c}` | **删除**（见上删除清单） | `Error` 结构体 + `error_handler` 是"C 层构造错误对象"的 v1 思路；`CameraError` 的 `module`/`code`/`native`/`detail` 字段已由 §1.3 的多态错误对象承载，日志出口留在调用边界 |
| `include/firmware/app/include/status.h` | **保留但本轮不接线**：不删除、不修改、不被 `camera.h` include，也不进入任何相机目标 | `dart_error_t`/`DART_OP_*` 不再是相机 ABI 的一部分；先不动，避免跨模块牵连，后续模块迁移时再统一处理 |
| C 侧状态/模式/帧标志枚举（你已从 `camera.h` 草稿中删掉） | **恢复到 `camera.h`**：`DART_CAMERA_STATE_*`、`DART_CAMERA_EXPOSURE_*`、`DART_CAMERA_FRAME_*` 作为 ABI 数值 | C 拥有状态机（`init/start/stop/deinit` 的合法性依赖它），C++ 不能凭空推断；C++ 侧建 `enum class State`/`ExposureMode` 镜像，用 `static_assert` 把两侧数值一一钉死（T3、T7） |

**当前树的破损点，T1 必须一并修掉**

- `include/camera/exposure_control.hpp` 的 `#include "core/status.h"` 指向不存在的文件（随该文件删除而消失）；
- 顶层 `CMakeLists.txt`（`tests/expected_probe.cpp`）与 `src/CMakeLists.txt`（4 个测试目标）引用不存在的 `tests/` 目录，`cmake` 配置阶段直接失败——本轮不做测试，应整段移除，**不得留空壳文件**。

### 1.2 C ABI：只返回 int，码空间三段划分

K230 SDK 的码是 `K_DEF_ERR = K_ERR_APPID(0xA0000000) | module<<16 | level<<13 | errid`，作为 `int` 恒为负且绝对值 ≥ `0x20000000`；POSIX `errno` 是小正数。据此定义**单 int 承载全部信息**的编码：

```
       0                      → 成功
   1 .. 999                   → dart 站点码（camera_error_codes.h，每个失败点一个稳定码）
 -1 .. -4095                  → -errno（POSIX 平台失败，保留原始 errno）
 <= -0x20000000               → SDK 原生码原样透传（bit31 置位段）
```

`camera_error_codes.h` 给出划分宏，**C 与 C++ 共用同一份**，避免两边各写一套阈值：

```c
enum {
    DART_CAMERA_OK            = 0,
    DART_CAMERA_E_NO_FRAME    = 8,   /* 非错误：这次没等到帧 */
    DART_CAMERA_E_SITE_CODE_MAX = 999,
};
#define DART_CAMERA_CODE_ERRNO_MAX   4095
#define DART_CAMERA_CODE_VENDOR_MAX  (-0x20000000)

/* SDK 码镜像表：C++ 侧可读，不必 include 厂商头文件 */
enum {
    DART_K230_E_BUF_EMPTY = -1609629682,   /* = K_ERR_VICAP_BUF_EMPTY, 见 k_errno.h */
    /* ... */
};
```

镜像表的正确性由 **`.c` 侧的编译期断言**证明（"同一事实声明两次、让编译器证明一致"）：

```c
/* camera.c —— 唯一能 include k_errno.h / k_vicap_comm.h 的地方 */
_Static_assert(K_ERR_VICAP_BUSY <= DART_CAMERA_CODE_VENDOR_MAX, "vendor code space changed");
_Static_assert(K_ERR_VICAP_BUSY == DART_K230_E_VICAP_BUSY, "mirror table drifted from k_errno.h");
```

站点码分段（每个失败点一个码；`404` 这类数字就是站点码本体，语义由表定义）：

| 段 | 含义 | 例 |
|---|---|---|
| 1–19 | 参数/状态 | `E_INVALID_ARGUMENT`, `E_INVALID_STATE`, `E_BUSY`, `E_NOT_SUPPORTED`, `E_OUT_OF_RANGE`, `E_STALE_LEASE`, `E_NO_MEMORY`, `E_NO_FRAME`(=8) |
| 20–39 | 配置 | `E_CONFIG_VERSION`, `E_CONFIG_INVALID` |
| 40–59 | 传感器 | `E_SENSOR_PROBE_FAILED`, `E_SENSOR_MODE_MISMATCH` |
| 60–79 | 池/块/映射 | `E_POOL_CREATE_FAILED`, `E_BLOCK_EXHAUSTED`, `E_MAP_FAILED`, `E_BLOCK_RELEASE_FAILED` |
| 80–99 | 帧几何/所有权 | `E_FRAME_GEOMETRY_MISMATCH`, `E_FRAME_POOL_MISMATCH`, `E_FRAME_NOT_MAPPED` |
| 100–119 | cache/时钟 | `E_CACHE_INVALIDATE_FAILED`, `E_CLOCK_FAILED` |
| 120–139 | 曝光 | `E_EXPOSURE_RANGE_FAILED`, `E_EXPOSURE_WRITE_FAILED` |
| 140–159 | 租约 | `E_LEASE_ABI_MISMATCH` |

**C 层的义务**：只返回码。不 `printf`/不写日志、不做断言打印、不修改错误结构体、不返回 `dart_error_t`、不决定是否重试。

资源回滚（release dump / deinit / munmap / 释放块）仍留在 C——那是所有权，不是错误处理；但 C **只返回主错误码**，回滚是否成功通过**状态可观测**表达（回滚失败置 `FAULTED`，由 C++ 在失败后调用 `dart_camera_get_state` 写进错误对象上下文）。

`camera.h` 因此全部是 `int` 返回 + 数据出参（出参只传数据，不传错误）：

```c
int dart_media_create(dart_media_t **media);
int dart_media_destroy(dart_media_t *media);
int dart_camera_create(dart_camera_t **camera);
int dart_camera_init(dart_camera_t *camera, dart_media_t *media,
                     const dart_camera_config_t *config);
int dart_camera_start(dart_camera_t *camera);
/* 正常否定结果有两个：DART_CAMERA_E_NO_FRAME（这次没等到帧）与
 * DART_CAMERA_E_LEASE_EXHAUSTED（槽位占满，本帧不取、由驱动回收）。见契约修订。 */
int dart_camera_acquire_frame(dart_camera_t *camera, uint32_t timeout_ms,
                              dart_camera_frame_t *frame);
int dart_camera_release_frame(dart_camera_t *camera, const dart_camera_frame_t *frame);
int dart_camera_stop(dart_camera_t *camera);
int dart_camera_deinit(dart_camera_t *camera);
int dart_camera_destroy(dart_camera_t *camera);
int dart_camera_get_state(const dart_camera_t *camera, uint32_t *state);
int dart_camera_get_lease_usage(const dart_camera_t *camera, uint32_t *outstanding,
                                uint32_t *capacity);
int dart_camera_get_caps(const dart_camera_t *camera, dart_camera_caps_t *caps);
int dart_camera_get_exposure_range(const dart_camera_t *camera, float *min_us, float *max_us);
int dart_camera_set_exposure_seconds(dart_camera_t *camera, float seconds);
void dart_camera_config_init(dart_camera_config_t *config);
int dart_camera_config_validate(const dart_camera_config_t *config);
```

> 与 `camera.h` 草稿的差异：曝光只暴露"查范围 / 写秒"，**clamp、向内取整、requested/applied 对比下沉到 C++**（`exposure_plan.c` 的纯计算变成 C++ 纯函数）。这样"27.375..999.75 us 只允许 28..999"这段丰富上下文才存在于错误对象里，而不是 C 里一个 `dart_exposure_result_t`。`dart_camera_get_error` / `dart_exposure_result_t` 一并删除。

### 1.3 C++ 侧：多态错误对象 + 翻译器

存放方式：`std::expected<T, std::unique_ptr<const CameraError>>`，真虚函数多态，仅在失败路径分配；**超时/无帧/租约耗尽都不进错误通道**（见契约修订：`AcquireResult` 三态，成功路径零分配）。

```cpp
namespace dart::camera {

enum class ErrorKind { kArgument, kState, kBusy, kUnsupported, kOutOfRange, kNoMemory,
                       kStaleLease, kHardware, kCacheSync, kRelease, kPlatform, kExposure };

class CameraError {                       // 不可变、多态、经 ErrorPtr 持有
public:
    virtual ~CameraError() = default;
    virtual ErrorKind kind() const noexcept = 0;
    virtual std::string_view category() const noexcept = 0;   // 稳定日志 token："camera.hardware"
    virtual std::int32_t code() const noexcept = 0;           // C ABI 原样返回的数字码
    virtual Operation operation() const noexcept = 0;         // 调用点
    virtual std::string_view message() const noexcept = 0;    // 静态文本，不分配
    virtual void append_context(ErrorTextSink &) const noexcept = 0;   // key=value 细节
};

using ErrorPtr = std::unique_ptr<const CameraError>;
}
```

派生类各带自己的上下文（"丰富上下文"的落点）：

| 类 | 附加上下文 |
|---|---|
| `ArgumentError` | 参数名、实际值 |
| `StateError` | 当前状态、要求状态 |
| `BusyError` | 占用者（未释放的租约 / 未 destroy 的 media） |
| `SensorModeMismatchError` | 请求 w/h/fps/csi/hdr vs SDK 实际回报（对应 `E_SENSOR_MODE_MISMATCH`） |
| `FrameGeometryError` | 期望/实际 w/h/stride/format/pool_id、phys_addr、length、block_bytes |
| `CacheSyncError` | phys_addr、映射地址、length |
| `ExposureRangeError` | requested_us、min_us、max_us、内向取整后的 applied_us |
| `HardwareError` | native_code + 解出的 module/level/errid |
| `PlatformError` | errno 值 |
| `NoMemoryError` | 请求字节数 |
| `StaleLeaseError` | 帧 token vs 租约 generation、owner 指针 |

翻译入口（**唯一的码→对象映射点**）：

```cpp
struct CallContext {                       // 由调用点填写，是上下文的来源
    Operation operation;
    std::int32_t native_hint = 0;          // 可选：调用点已知的附加码
    std::uint32_t requested = 0, observed = 0;
    /* 更细的上下文用带类型的重载补充 */
};
ErrorPtr translate_camera_code(std::int32_t code, const CallContext &ctx) noexcept;
```

分类算法固定：`code > 0` → 查站点码表；`-4095 <= code < 0` → `PlatformError{-code}`；`code <= -0x20000000` → `HardwareError`（解 module/level/errid）+ 站点码表的 operation；`code == DART_CAMERA_E_NO_FRAME` → **不生成错误对象**。

失败路径额外观察一次状态：`init/start` 失败后调 `dart_camera_get_state`，若为 `FAULTED` 则给错误对象追加 `rollback_failed=true`——这是 int-only C ABI 下找回"主错误 + 回滚错误"两个码信息的正规手段（状态是可观测数据，不是错误处理）。

### 1.4 C++ 侧：`ICamera` 虚接口

```cpp
enum class State { kEmpty, kInitialized, kStarted, kFaulted };   // 与 C 侧 ABI 值 static_assert 对应
enum class ExposureMode { kFixed, kAuto };

struct FrameView {                         // 不泄漏 C 结构体
    std::span<const std::byte> luma;       // Y8，只读，DMA 写入；仅在该 lease 生命周期内有效
    std::uint32_t width, height, stride;
    std::uint64_t source_pts;              // SDK 原始值
    bool source_pts_timebase_validated;    // 本轮恒 false
    std::chrono::microseconds received;    // CLOCK_MONOTONIC 接收时间
};

class FrameLease {                         // move-only RAII，指向所属相机的槽位（无堆分配）
public:
    FrameLease(FrameLease &&) noexcept;
    FrameLease &operator=(FrameLease &&) noexcept;
    ~FrameLease();                          // 尽力释放：失败不吞掉，登记进槽位并让下次调用显式报错
    const FrameView &view() const noexcept;
    std::expected<void, ErrorPtr> release() noexcept;   // 显式释放优先
};

struct AcquireResult {                     // 三态：有帧 / 没帧 / 槽位占满（丢帧）
    AcquireStatus status;                  // kFrame | kNoFrame | kLeaseExhausted
    FrameLease lease;                      // 仅当 status == kFrame
};

class ICamera {
public:
    virtual ~ICamera() = default;
    virtual std::expected<void, ErrorPtr> start() noexcept = 0;
    virtual std::expected<AcquireResult, ErrorPtr>
        acquire(std::chrono::milliseconds timeout) noexcept = 0;
    virtual std::expected<void, ErrorPtr> stop() noexcept = 0;
    virtual State state() const noexcept = 0;
    virtual const Caps &caps() const noexcept = 0;
    virtual std::expected<void, ErrorPtr> set_fixed_exposure_us(std::uint32_t us) noexcept = 0;
    virtual std::uint32_t capacity() const noexcept = 0;      // 创建时注入的槽位数
    virtual std::uint32_t outstanding() const noexcept = 0;   // 当前未释放的租约数
};

/* 工厂：create → init → start 三分离，初始化失败不抛异常、不打印。
 * 容量由上层以编译期参数注入；C ABI 允许的每个容量都在 k230_camera.cpp 显式实例化。 */
template <std::uint32_t kCapacity = 1>
std::expected<std::unique_ptr<ICamera>, ErrorPtr>
create_k230_camera(const Config &config) noexcept;
```

约束：`ICamera`/`FrameLease`/`CameraError` 这些头文件**不 include 厂商头文件，也不 include `camera.h`**；只有 `k230_camera.hpp/.cpp` 碰 C ABI，只有 `.c` 碰 MPP/VICAP/VB。

### 1.5 include 路径与 CMake

- 导出 `include/firmware/app/include`（`#include "status.h"`）与 `include/firmware/app/src`（`#include "camera/camera.hpp"`）；
- 模块内文件之间用引号**模块限定**包含（`#include "camera/k230/internal.h"`），根就是已导出的 `include/firmware/app/src`；与 librmcs 的仓库相对 include 同构，不再出现 `../`；
- 目标划分（名字暂定，T1 定稿）：

| 目标 | 源 | 何时构建 |
|---|---|---|
| `dart_camera_core` | `c_api/config.c`、`c_api/lease.c` | 任意环境（纯 C） |
| `dart_camera_k230` | `k230/lifecycle.c`、`k230/media.c`、`k230/frame.c`、`k230/exposure.c` | `DART_CAMERA_K230=ON`（需 SDK + 交叉工具链） |
| `dart_camera_acl` | `error.cpp`（+ 交叉时 `k230/camera.cpp`） | 任意环境；适配器部分随 `DART_CAMERA_K230` |
| `dart_camera_k230_compile_commands` | 同 `dart_camera_k230` 的 4 个源 + `k230/internal.h` | 仅 `DART_CAMERA_K230=OFF`；EXCLUDE_FROM_ALL，只为 clangd 产出编译命令 |
| 测试目标 | — | **全部移除**（本轮不做测试） |

风格基线沿用仓库现有配置：`.clang-format`（LLVM、100 列、4 空格、指针左对齐）、`.clang-tidy`、C11 + C++23、`-Wall -Wextra -Wpedantic`、代码 ASCII（注释可中文）。两个配置文件在 2026-10-11 从 `.clangd-format` / `.clangd-tidy` 改名为标准名（clangd/clang-format/clang-tidy 只识别 `.clang-format` / `.clang-tidy`，旧名等于没人加载）。`.clang-format` 的 `Standard` 仍是 `c++20`，与 C++23 基线不一致，待单独确认后再改。

---

## 2. 任务拆分（每个任务 = 一个上下文窗口能收尾的量）

约定：每个任务只读"必读清单"里列出的文件；不许顺手改别的模块；每个任务一个提交（英文提交信息、单一关注点）。

| # | 任务 | 产出 | 规模 | 依赖 |
|---|---|---|---|---|
| T0 | 计划落盘（本文档） | `docs/CAMERA_ACL_REFACTOR_PLAN.md` | 0 行代码 | — |
| T1 | 边界契约冻结 + 目录搬迁 + 构建骨架 | `camera_error_codes.h`、`camera.h` 定稿、`camera/CMakeLists.txt`、顶层 CMake 修正、删除 `src/**` 与 `include/camera/**`、`camera/README.md` 平移 | ~250 行 + 删除 | T0 |
| T2 | C：媒体与输出池 | `camera_media.c` | ~150 行 | T1 |
| T3 | C：相机生命周期 | `camera.c`（init/start/stop/deinit/destroy/state/caps + 传感器模式精确校验 + 回滚） | ~250 行 | T2 |
| T4 | C：帧路径与租约 | `camera_frame.c`、`frame_lease.{h,c}` | ~220 行 | T3 |
| T5 | C：配置与曝光 | `camera_config.c`、`camera_exposure.c` | ~140 行 | T1 |
| T6 | C++：多态错误层 | `camera_error.hpp/.cpp`（层次 + 翻译器 + 码表一致性 `static_assert` + 固定缓冲 `ErrorTextSink`） | ~300 行 | T1 |
| T7 | C++：虚接口 + K230 适配器 | `camera.hpp`、`k230_camera.hpp/.cpp`、C++ 侧曝光规划 | ~300 行 | T4、T5、T6 |

### T1 边界契约冻结 + 目录搬迁 + 构建骨架

- **做什么**：把 §1.1–1.5 落成文件——`camera_error_codes.h`（码表 + 段宏 + SDK 镜像表）、`camera.h` v2（int-only；**恢复** `DART_CAMERA_STATE_*` / `DART_CAMERA_EXPOSURE_*` / `DART_CAMERA_FRAME_*` ABI 数值；去掉 `dart_error_t`、`dart_camera_get_error`、`dart_exposure_result_t`）、`camera/CMakeLists.txt`、顶层 `CMakeLists.txt`（去掉 `tests/expected_probe.cpp`，`add_subdirectory` 指向新模块目录，去掉 `src` 引用）、删除 `src/**`、`include/camera/*.hpp` 与 `include/firmware/app/src/logger/`、把 `src/camera/README.md` 平移到 `include/firmware/app/src/camera/README.md`（只改路径与失效引用，正文重写留下轮）。
- **必读**：`include/firmware/app/src/camera/camera.h`、`include/firmware/app/include/status.h`（只读，不修改）、`CMakeLists.txt`、`src/CMakeLists.txt`、`src/camera/README.md`。
- **完成判据**：`cmake -S . -B /tmp/dart-host -DDART_CAMERA_K230=OFF` 配置成功；`--build` 通过；`grep -rn "dart_error_t" include src` 为空；`src/`、`include/camera/`、`include/firmware/app/src/logger/` 均不存在；`camera.h` 里状态/模式/帧标志枚举齐备；`camera/README.md` 存在且无 `src/camera` 引用；`git status` 中 `status.h` 无改动。
- **禁止**：不写空壳 `.c` 占位文件；不保留任何 `tests/` 引用；不修改、不删除、不让 `camera.h` include `status.h`。

### T2 C：媒体与输出池（`camera_media.c`）

- **做什么**：`dart_media_create/destroy`（VB 全局初始化/退出、单 owner 约束）、私有输出池创建、按 `channel_buffers` 取块→物理地址→`kd_mpi_sys_mmap_cached`→释放块句柄、映射表维护；失败回滚（munmap / 释放块 / 销毁池）**内部完成**，只返回主站点码；回滚失败置 `FAULTED`。
- **必读**：v1 `src/camera/camera_k230.c` 的 `prepare_output_pool` / `dart_media_*` 两段、`camera_error_codes.h`。
- **完成判据**：带 SDK include 参数的 `gcc -fsyntax-only` 通过；文件内无 `printf`/`fprintf`/`perror`/错误结构体写入；每个 `return` 要么 `0`、要么 `camera_error_codes.h` 里的站点码、要么 SDK 原值。
- **禁止**：不在 `.c` 里做 clamp/日志/翻译；除 `camera.h` 外不引入对外头文件。

### T3 C：相机生命周期（`camera.c`）

- **做什么**：`create`（magic、初值、映射表清空）、`init`（配置校验 → probe → 精确模式匹配（SDK 回退即拒）→ `dart_camera_get_sensor_info` → dev/chn 属性 → `kd_mpi_vicap_init` → caps）、`start`（起流、取 sensor fd、auto 模式判定、初始固定曝光）、`stop`、`deinit`（含 stop、deinit、munmap、释放块、毁池）、`destroy`、`get_state`、`get_caps`；`_Static_assert` 校验 SDK 码镜像表与 `k_errno.h` 一致。
- **必读**：`camera_media.c`（已写好的接口）、`camera.h`、`camera_error_codes.h`、v1 `camera_k230.c` 的 init/start/stop/deinit 段。
- **完成判据**：所有失败返回站点码；SDK 失败返回原生码；`init/start` 回滚失败时 `state == FAULTED`；固定曝光应用逻辑保留但改调 `camera_exposure.c`。
- **禁止**：不在生命周期函数里打印/退出；不留"打印后返回"。

### T4 C：帧路径与租约（`camera_frame.c` + `frame_lease.{h,c}`）

- **做什么**：`acquire_frame`（状态校验 → `dump_frame` → 无帧判定 → 几何/格式/压缩/pool_id 校验 → 映射查找 → `mmz_invalidate_cache` → 单调时间戳 → 签发租约）、`release_frame`（校验 → 释放 dump → 提交租约；SDK 释放失败保留租约可重试）、`frame_lease.c`（generation/token/owner 校验，逐字升级为 int 码）。
- **关键约定**：`K_ERR_VICAP_BUF_EMPTY` / `K_ERR_VICAP_NOTREADY` 或等待超时 → 返回 `DART_CAMERA_E_NO_FRAME`；C++ 把它翻成"空 optional"，**不是错误对象**。
- **必读**：`camera.h`、`camera_error_codes.h`、`camera.c`、v1 的 acquire/release 段与 `frame_lease.c`。
- **完成判据**：每个失败点返回**不同**站点码（几何不匹配 / 池不匹配 / 未映射 / 缓存失败 / 时钟失败各自可区分）；无 `DART_STATUS_*` 残留。
- **禁止**：不在 acquire 内打印；不做任何像素复制；不新增 map/unmap。

### T5 C：配置与曝光（`camera_config.c` + `camera_exposure.c`）

- **做什么**：config 的 `init/validate` 逐条改成站点码（版本/尺寸/缓冲/模式约束原样保留）；`get_exposure_range`（`kd_mpi_sensor_get_exposure_time_range`）、`set_exposure_seconds`（`kd_mpi_sensor_intg_time_set`；`fd < 0` → `E_INVALID_STATE`；AUTO 模式 → `E_NOT_SUPPORTED`）。
- **必读**：v1 `config.c`、`exposure_plan.c`、`camera_k230.c` 的 `apply_exposure` 段。
- **完成判据**：`camera_config.c` 可用主机 `gcc -std=c11 -Wall -Wextra -Wpedantic -c` 通过；C 里不再有 `dart_exposure_result_t`；clamp/取整/微秒→秒换算全部不在 C 里。
- **禁止**：不在 C 里做范围合法性"策略"判断（属 C++ 规划函数）。

### T6 C++：多态错误层（`camera_error.hpp/.cpp`）

- **做什么**：`CameraError` 基类 + §1.3 的派生类集合 + `translate_camera_code(code, ctx)` + `ErrorTextSink`（写入调用者提供的固定缓冲，供 `DEBUG_GUIDE` 的 512 字节单行日志用）+ 码表一致性断言（`static_assert(DART_CAMERA_E_SITE_CODE_MAX < DART_CAMERA_CODE_ERRNO_MAX)`、站点码唯一性用 `constexpr` 检查）+ SDK 码解码（module/level/errid）。
- **必读**：`camera_error_codes.h`、`docs/DEBUG_GUIDE.md` §3（日志字段 `ts level module op code native detail action count msg`）。
- **完成判据**：`g++ -std=c++23 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -fsyntax-only` 通过；每个 `ErrorKind` 至少有一个派生类；`translate_camera_code` 是唯一码→对象映射点（`grep` 可验证）；`message()` 返回 `string_view`、无动态字符串。
- **禁止**：不用 `dynamic_cast`/`typeid`（`-fno-rtti`）；不抛异常；不在错误类里打印。

### T7 C++：虚接口 + K230 适配器（`camera.hpp`、`k230_camera.hpp/.cpp`）

- **做什么**：`ICamera` / `FrameLease` / `FrameView` / `State` / `Caps` / `Config`；`create_k230_camera(config)` 工厂；适配器把每个 C 调用点的 `CallContext`（operation + requested/observed）与码交给翻译器；曝光规划纯函数（范围 → 内向取整 → 微秒/秒换算 → `ExposureRangeError` 携带 requested/applied/min/max）；`FrameLease` 析构失败的"待重试"槽位（下次 acquire 返回 `BusyError` 并说明上次释放失败），不静默丢帧。
- **必读**：`camera.h`、`camera_error.hpp`、`docs/STRUCTURE_DESIGN.md` §2、`camera/README.md`（所有权/租约/时间戳语义段）。
- **完成判据**：使用者只需 `#include "camera/camera.hpp"`，不接触任何 C 类型/厂商类型；`grep -n "k_" k230_camera.cpp` 为空（不泄漏 SDK 类型）；超时路径不分配（`optional` 空值）；所有失败路径都经 `translate_camera_code`。
- **禁止**：不在接口里放 C 结构体；不在适配器里 include 厂商头文件；不做主机 fake。

---

## 3. 验证矩阵（本轮无测试，如实标注）

| 层级 | 手段 | 判定 |
|---|---|---|
| 主机（纯 C 模块） | `gcc -std=c11 -Wall -Wextra -Wpedantic -c camera_config.c frame_lease.c` | 通过/失败 |
| 主机（C++ 接口与错误层） | `g++ -std=c++23 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -fsyntax-only`（`-fno-exceptions` 下 `std::expected` 路径必须编译） | 通过/失败 |
| 主机（SDK 相关 `.c` 语法） | `gcc -fsyntax-only`，include 参数复用顶层 CMake 的 `k230_rtos_app` 列表（`$K230_SDK_ROOT/include`、`mpp/include{,/comm,/ioctl}`、`mpp/userapps/api{,/framework}`、`-include autoconf.h`） | 通过/失败 |
| 交叉构建 | `K230_TOOLCHAIN_DIR=/mnt/mydata/SelfGuidingDart/third_party/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin cmake --preset cross` + `cmake --build --preset cross --target dart_camera_k230 dart_camera_acl`；链接用 `/tmp/link_probe.cpp` 之类的探针把 `libdart_camera_{acl,k230,core}.a` + SDK 静态库 + `link.lds` 链成一个 ELF | **已通过（2026-10-11）**：三个目标库编译成功，探针链出 1.6 MB 静态 RISC-V ELF。这条同时证明目标 GCC 12.0.1 上 `std::expected`/`std::span` 与 `-fno-exceptions -fno-rtti` 可用（原"未验证前不要假设"的风险已消除）。**工具链实际位于 `SelfGuidingDart/third_party/toolchain/`，此前"本机未安装工具链"的记录有误** |
| 板端 | 诊断程序（本轮不建） | **BLOCKED: hardware validation pending**——私有池兼容性、DMA 后 cache 新鲜度、真实帧时序、曝光行量化、热重启与清理失败都必须板端证据 |

诚实结论：本轮可验证的承诺只有"主机编译 + 静态检查 + 边界自洽（`static_assert`）"；"相机能用"必须等交叉构建与板端证据，文档与提交信息中不得写成已通过。

---

## 4. 主要风险与对策

1. **int-only 丢失 native 诊断信息** → 三段码空间 + SDK 码镜像表 + `.c` 侧 `_Static_assert` 对齐厂商头文件；POSIX 失败用 `-errno`。
2. **回滚失败信息只能有一个返回码** → 主码返回 + `FAULTED` 状态可观测，由 C++ 在错误对象里追加 `rollback_failed`。
3. **错误路径分配** → 只在真实失败时分配；无帧/超时走 `optional` 空值；`BusyError` 这类高频拒绝若实测成热点，再改为共享不可变实例（本轮不预先优化）。
4. **`FrameLease` 析构失败不可上报** → 登记"待重试 + 下次调用显式报错"，禁止静默丢失或伪造成功。
5. **枚举两处声明漂移**（C ABI 值 vs C++ `enum class`）→ 用 `static_assert` 把映射钉死；状态机仍归 C 所有。
6. **文档路径过时**（`src/camera`、`core/status.h`、`tests/`）→ T1 至少消除失效引用；正文重写留下一轮；`docs/*.md` 被 `.gitignore` 忽略，新增文档需 `git add -f`。

---

## 5. 提交顺序（每提交一个边界）

`T1 status/ABI+骨架` → `T2 media/pool` → `T3 lifecycle` → `T4 frame/lease` → `T5 config/exposure` → `T6 error translation` → `T7 ICamera+K230 adapter`。

任何涉及 cache/VB/映射/真实帧时序的提交，若没有板端结果，提交信息与文档都必须写明 `BLOCKED: hardware validation pending`，不得宣称完成。
