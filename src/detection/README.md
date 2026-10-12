# 检测与 ROI

纯 C++23 模块，入口为 `roi.cpp`，CMake 接口目标为 `dart_detection`。输入为相机
ACL 的只读 `dart::camera::FrameView`；检测同步借用像素，不取帧、不释放租约、不保留帧指针。

`detector.cpp` 与 `roi.cpp` 均在类内定义方法，带重复包含保护；包含 `roi.cpp` 即可使用完整模块。
应用链接 `dart_detection` 获取 C++23 和相机接口依赖，两份实现不再独立编译。

初始化时构造 `dart::detection::roi` 并调用 `prepare(max_width, max_height)`。在现有
采集线程拿到 `AcquireStatus::kFrame` 后调用：

```cpp
#include "src/detection/roi.cpp"

auto result = detector.detect(acquired.lease.view());
if (result && result->target.valid) {
    const auto center = result->target.center;  // 全图像素坐标，允许半像素
    // 将 center 和 result->source_frame_id / received 交给下一层。
}
// 其他同步消费者结束后，现有流水线在同一线程释放 acquired.lease。
// kNoFrame / kLeaseExhausted 不调用 detect()，不构造假帧推进 ROI 状态。
```

`prepare()` 和 `detect()` 返回 `std::expected`。无目标是成功结果，表现为
`kind == TargetKind::kNone`、`target.valid == false`；工作区不足、非法输入和乱序帧
返回 `DetectionError`。启动分配失败由调用方处理，热路径没有动态分配。一个实例用于
一条按序调用的流水线；相机停止重启后调用 `reset()`，再接受新的帧号。

`detector.cpp` 实现 `light_detect`、`armor_detect` 和共用的游程测量：

- 直接在 ROI 的 Y8 上用可配置阈值提取 8 邻域连通域，按游程计算面积、质心、
  二阶矩、圆度和主轴；常规 step=1 扫描中两个检测类读同一组候选。首次全图获取时灯用 GREEN_STEP 采样，装甲只在灯锚定的小窗 step=1 精扫。
- 灯检测沿用 v1 的面积、填充率、宽高比和面积乘圆度排序。
- 装甲板沿用 v1 的本帧灯锚、灯尺度门限、灯条形状、平行/长度/厚度/间距门限和
  刚性距离先验。板心取灯条端点所构成四边形的两条对角线交点。
  配对沿主轴投影判断重叠，端点统一方向，避免旋转后依赖水平包围盒或正负倾角翻转。
- 仍有合格的上一对时优先关联；每帧保留形状检查，不把旧中心当本帧测量。

`roi.cpp` 用上一帧灯与板的位置和包围盒规划下一帧窗口，并包含灯上方的装甲板搜索
区域。首次、长时间间隔、尺寸变化、连续丢失或显式开启周期刷新时使用全图（默认关闭周期刷新）。ROI 未找到灯或目标
贴 ROI 边缘时，同帧再尝试全图；短时丢失扩大下一帧窗口，最终清除旧目标。
窗口靠图像边缘时整体平移，保持尺寸。只用 `received` 的单调接收时间判断间隔，
原始 `source_pts` 和其有效标记原样传递。

统一输出始终为本帧装甲板中心优先，否则为本帧灯心；两者都无效时不输出旧坐标。
同时提供灯、装甲板、窗口、贴边质量、全图/重扫标记、候选数和连续丢失帧数。

现有相机只暴露 Y8，不包含色度，因此“绿色灯”按亮度与形状识别，无法区分绿色与
其他颜色的同形亮物。装甲板沿用 v1 的灯锚先验：未识别到本帧灯时不独立猜测装甲板。
阈值、曝光分离度、窗口尺寸和几何默认参数均为 **UNVALIDATED**，需要真实板端样本标定。
远档灯候选跳过形状判据，按 FAR_CONFIRM=3 连续帧确认并用 FAR_JUMP_K=2.5、FAR_JUMP_MIN=10 做位移跳变重数；装甲板使用 PLATE_UP_LO_PCT=20、PLATE_UP_HI_PCT=400、PLATE_OFFX_PCT=200 锚定板心。已补上全图获取时 GREEN_STEP=2 的隔点扫描、灯上方装甲搜索窗 step=1 精扫、锁定后 step=1 的 ROI 扫描及贴边同帧全图复扫；未移植 v1 的 MMZ/VB 帧池、Kalman 状态树、保持旧坐标和录像逻辑。
标量实现保留了确定性参考口径，未宣称获得 v1 RVV 的板端速度。

工作区最大宽高为 65535，默认最多 16384 个游程和 256 个有效连通域；超过上限明确
返回 `kWorkspaceExhausted`，不静默截断。较小帧可以使用原有工作区，超过准备尺寸则拒收。
默认工作区约 1.3 MiB；每帧只清理实际使用的组件槽，不申请整图二值副本。
两个单独检测类消费 `BlobWorkspace::scan()` 产生的候选，输入须遵守其几何和数值约定。

算法来源：[Guided_dart / k230_v1](https://github.com/hyperle/Guided_dart/tree/k230_v1)，
参考版本 `a7390262203f2565876aef1d2a8b628b2722d79f`，
主要参考 `src/detection/measure/roi_measure.cpp`、
`src/detection/armor/armor_{rules,geometry,windows}.cpp` 和
`include/detection/math/shape_metrics.hpp`。

合并后应在包含 `roi.cpp` 的应用上进行主机与 K230 交叉构建验证；多个编译单元包含
这两个文件不会产生重复定义。实际识别质量、耗时和 DMA/流水线行为仍待板端验证。
