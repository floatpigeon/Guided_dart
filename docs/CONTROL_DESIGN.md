# 飞行控制设计基线

适用对象：无动力抛体、X 型翼、约 20 m/s 的滑翔。控制器不是独立可运行程序，后续应作为 `src/control/` 的纯 C++ 领域模块。

## 1. 控制链和边界

控制链只有一个出口：四片舵面的归一化舵偏 `[-1, 1]`。

```
FlightStateMachine  → 当前模式和安全退化
VisualOuterLoop    → 视觉/航向误差到姿态或迎角请求
AttitudeInnerLoop  → 姿态请求到三轴归一化力矩
ControlAllocator   → 三轴力矩到四片舵面
FlightController   → 在一个控制拍内编排上述步骤
```

建议目标文件：

| 层 | 目标接口 | 约束 |
|---|---|---|
| 值类型/参数 | `include/control/control_types.hpp` | 唯一参数入口，禁止散落宏和隐式单位 |
| 状态机 | `include/control/flight_state_machine.hpp` | 不认识传感器和 SDK |
| 外环 | `include/control/visual_outer_loop.hpp` | 只接收带有效位/时间戳的估计量 |
| 内环 | `include/control/attitude_inner_loop.hpp` | 显式报告 `authority_lost/ill_posed` |
| 分配 | `include/control/control_allocator.hpp` | 矩阵为数据，可验证符号、限幅和速率 |
| 编排 | `include/control/flight_controller.hpp` | 注入 `IImuState/ITargetEstimate/IServoOutput` |

没有视觉、IMU、舵机或参数时，控制器必须输出可解释的安全结果，不能猜符号、伪造状态或把 NaN 送入执行器。

## 2. 三模式状态机

| 模式 | 外环 | 行为 |
|---|---|---|
| `Freefall` | 不工作，偏置为 0 | 仅保持基准姿态；发射和视觉锚定条件未满足时停留 |
| `Glide` | 工作 | `theta_cmd = theta_base + delta_theta`，同时受迎角、姿态和速率约束 |
| `Terminal` | 偏置强制清零 | 恢复终端基准姿态；吸收态，不再承诺可修正落点 |

`Freefall → Glide` 必须同时满足“已发射、视觉已锚定、静默段已过”。进入末段可综合目标尺度、高度和剩余时间，但没有视觉不能进入 `Glide`。`Terminal` 的切换应从固定时间阈值逐步升级为可行性判断：剩余轨道误差、姿态捕获时间、舵面余量和状态不确定度都要纳入。

历史仿真表明 `t_go_terminal` 会在撞姿和脱靶之间折中；该量只能作为实验旋钮，不能在未标定时写成默认真值。

## 3. 视觉外环：航向误差优先

定义视线角 `lambda`、航迹角 `gamma`、迎角 `alpha`：

```
sigma = lambda - gamma = epsilon + alpha
s     = sigma_dot + lambda_s * sigma
```

首选误差是航向误差 `sigma`，而不是只让像素中心归零的 `epsilon`。像素误差把机头压到目标上，但若迎角长期不为零，速度方向仍然偏离目标，会留下与射程近似成正比的系统性脱靶。

一个带边界层的实现可写成：

```
delta_theta_c = kd * clamp(s, -phi, +phi)
delta_theta   = delta_theta_c + (gamma_hat - theta_base)
```

但它只有在动力学匹配和权限约束都满足时，才可称为滑模控制。更稳妥的虚拟迎角形式是：

```
alpha_v = (v / A_hat) * (lambda_rate_hat + k_sigma * sigma - a_g_hat / v)
theta_cmd = gamma_hat + clamp(alpha_v, alpha_min, alpha_max)
```

必须把 `v/A`、重力法向项、视觉延迟、内环滞后和舵机速率纳入设计；不可仅通过增大 `kp/kd` 顶住滞后。像素域转换使用 `f*sec^2(angle)` 的局部雅可比，不能永远使用一个 `pixels_per_rad`。

### 3.1 符号和通道

机体 z 轴朝下、像面 y 轴朝下时，默认符号为：

- 偏航：目标在右，像 x 增大，`pixel_sign = +1`；
- 俯仰：目标在上，像 y 减小，`pixel_sign = -1`。

实际方向必须在台架上抬高机头/旋转机体逐通道确认。两个通道不能共用一个未经验证的符号。

### 3.2 能量和权限保护

迎角、姿态偏置、姿态指令速率和舵面速率是四种不同限制：

```
alpha_limit = min(alpha_stall, a_max / A_hat, alpha_cap) * low_speed_scale
```

末段权限会迅速下降；能力与 `t_go^2` 成正比，因此修正必须尽早，末段主要价值是保姿态。预设像素只改变视觉误差的参考点，不会凭空改变弹道。

## 4. 姿态内环：离散滑模

目标模型应明确采用连续系数还是每拍增量。连续系数口径示例：

```
theta_next = theta + tick * omega
omega_next  = omega + tick * (Cm_alpha*alpha + Cm_delta*delta + b_hat)
s           = (omega - omega_cmd) + c*(theta - theta_cmd)
s_next      = s - K*tick*sat(s/phi)
```

由模型反解舵面前，必须满足：

1. `phi >= K*tick`，否则带内一步越过零点形成极限环；报告 `ill_posed`，不静默夹住。
2. `|Cm_delta|` 足够大且方向已标定；舵效丢失时改用压角速度的安全兜底，报告 `authority_lost`。
3. `b_hat` 的单位与模型一致。若观测器给的是按拍角速度增量，送入连续模型前必须除以实际控制周期。
4. 舵面位置、速率和动压限幅反馈给外环；饱和后离散到达律不再成立。
5. `valid=false` 时不报告虚假的角度跟踪误差，不因缺数据继续放大控制量。

## 5. 控制分配

```
delta = B * [p, q, r]^T
```

`B` 是 `AllocatorParams::mix` 的数据，不在代码中散落正负号。对称 X 型尾翼的几何不变量应在单元测试和台架阶跃中验证：滚转列方向一致，俯仰/偏航列与翼面方位一致，三轴列无意外置换，非限幅时可往返重构。

分配器至少报告：限幅前/后舵偏、速率限幅、实际可实现力矩、哪一片舵触发限制。上板前必须做单轴正负阶跃：确认舵面方向、零位、最大行程和 PWM 映射。

## 6. 参数、单位与标定

`ControllerParams` 是唯一入口；参数必须带单位、来源和 `UNVALIDATED`/`CALIBRATED` 状态。

| 参数 | 作用 | 获取方式 |
|---|---|---|
| `A(v)` | 迎角到横向加速度 | 不同速度做受限阶跃，拟合速度调度 |
| `Cm_alpha(v)` | 迎角到稳定力矩 | 姿态阶跃与动压记录 |
| `Cm_delta(v)` | 舵偏到力矩 | 舵偏阶跃看角速度初始斜率 |
| `kp/kd/lambda_s` | 外环误差响应 | 从视觉噪声、延迟和可用迎角反推，不能照搬 v1 |
| `phi/dead_zone` | 抖振与噪声抑制 | 静止/匀速目标测 `LOS_rate` 噪声 |
| `t_go_terminal` | 末段切换旋钮 | 以脱靶、撞姿、饱和率联合评估 |
| `allocator.mix` | 三轴到舵面的矩阵 | 实拍每轴阶跃确认 |

## 7. 验收和接线顺序

当前没有 `FlightController` 实现或主机自检；下面是后续验收契约，不是已通过结果：

1. 先验证值类型、符号、限幅和分配矩阵（主机可重复）；
2. 再用 ACL 接入 IMU/视觉/舵机，检查时间戳、有效位和错误传播；
3. 关闭外环做姿态内环台架阶跃，记录超调、捕获时间、饱和率和舵效；
4. 接入观测器但保持外环受限，检查 `NIS`、延迟和 `b_hat` 单位；
5. 最后按 Freefall → Glide → Terminal 做板端试验，覆盖丢视觉、低速、饱和、NaN 和断电收尾。

必须记录撞点脱靶、撞击姿态/航迹、外环和内环饱和率、舵面总变差、视觉延迟、观测器拒收/救援次数及终端剩余可修正误差。