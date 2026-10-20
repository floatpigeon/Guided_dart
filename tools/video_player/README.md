# 录像逐帧播放器（主机 Qt 工具）

移植来源：远程 `origin/k230_v1` 的 `tools/video_player`，提交
[`a739026`](https://github.com/hyperle/Guided_dart/tree/a7390262203f2565876aef1d2a8b628b2722d79f/tools/video_player)。
保留 Qt Widgets 的滑块、左右键、图像缩放、像素点击和检测叠加，按当前 v2
的逐帧 PNG、旧版 PGM/PBM 连续流及逐帧文件、录像 CSV 和检测 CSV 重写数据关联。与 v1 一样为手动逐帧浏览，
不按时间自动播放。支持 Qt 5 / Qt 6，不依赖 ROS、SDK 或板端构建。

## 构建与打开

本机已具备 Qt 5 开发环境。其他 Debian/Ubuntu 主机可安装 `cmake`、`g++`、
`qtbase5-dev`；需要图形桌面来显示窗口。在仓库根目录运行：

```bash
# 自动增量构建并打开；也可从其他目录调用 .script/player 的绝对路径。
bash .script/player records/rec_0001

# 默认显示 PNG/PGM/PBM；--png 仅看 PNG，-r 仅看 PGM，-f 仅看 PBM。
bash .script/player -r records/rec_0001

# 拷贝位置不同可显式指定两个 CSV；包含空格的路径加引号。
bash .script/player records/rec_0001/img \
  records/rec_0001/logs/frames.csv records/detection.csv
```

也可以独立构建，不使用任何交叉编译预设：

```bash
cmake -S tools/video_player -B build/video_player
cmake --build build/video_player --parallel
./build/video_player/video_player --help
./build/video_player/video_player records/rec_0001
```

`DART_PLAYER_BUILD_DIR` 可覆盖脚本的默认构建目录 `build/video_player`。
脚本透传参数，数据相对路径以调用时工作目录为准。必须指定一轮 `rec_NNNN`
或其 `img` 目录，避免在多轮录像间隐式挑选。CSV 默认取该轮的 `logs/frames.csv`
及其上一级的 `detection.csv`。显式给出的 CSV 路径不存在时直接报错。

建议一并拷贝以下内容，保持相对目录：

```text
records/
  detection.csv
  rec_0001/
    img/b0000001/p0000001/00000001.png  # 每组 32 张，支持后续 b0000001/p0000002
    img/b0000001/p0000001/00000002.png
    logs/frames.csv
```

**检测日志必须来自同一轮运行。** 当前 `src/diagnostics/detection_csv.cpp` 用 `"w"` 打开
`records/detection.csv`，下一次启动会覆盖它；旧 `rec_NNNN` 目录可能仍在。
每轮结束后应将对应的 `detection.csv` 连同该轮目录一起归档，或启动时通过
`--det-csv` 指定不同路径。已被覆盖的数据无法从图片恢复，播放器会显示未匹配。

## 图片、文字与叠加如何对应

1. 对 `img/recording.pgm` / `.pbm` 扫描帧头并建立字节偏移索引，跳过像素负载；
   复位导致的不完整尾帧会提示并忽略。独立 PNG 支持 `img/bNNNNNNN/pNNNNNNN/00000001.png`，兼容旧版 `img/frame_*.pgm` / `.pbm`；按图片数字编号排序。
   只读取和解码当前选中帧，不把整个录像载入内存。
2. 用 `frames.csv` 的 **`image_file + file_offset`** 精确匹配连续帧，校验 `file_bytes`、
   `decision=accepted` 和 `image_index`。独立图片用包含分组目录的完整 `image_file`；PNG 同时校验 `file_offset=0` 与 `file_bytes` 等于磁盘文件长度，冲突时停用该帧关联。
   `image_index` 是输出帧的编号，不是输入帧号或 CSV 行号。
3. 用录像行的 **`source_sequence + received_us + source_pts`** 精确匹配检测行的
   **`sequence + received_us + source_pts`**，并核对检测的 `admission=accepted`。
4. 右侧显示对应 CSV 的路径、物理行号、原始文本及全部列，便于回查原始记录。

降采样、忙丢弃等输入有 CSV 行但没有图，浏览时不会占据一张图片；图片缺号也不会
使后续文字错位。接收时间和 PTS 一起核对，避免只凭重启后复用的序号串到其他轮次。
重复匹配、放行冲突或没有对应文字时明确提示，停用该帧的检测关联；不按最近时间
或相邻行补齐。metadata 队列本身也可能丢行，所以有图不等于一定有录像 CSV 行。

缺少默认 CSV 时仍可查看图片；缺少检测结果时不画检测叠加。损坏的 CSV、错误的表头
或非法身份字段会报告文件和行号。当前关闭识别/识别出错时写出的短空字段行会按
`kind=none, valid=0` 的格式兼容，保留原文并正确识别最后的 `admission`。
图片读取失败时清空图像区，并显示错误，防止旧图与新文字同屏。

绿色十字仅表示 `valid=1` 且坐标有限、位于图内的目标；支持小数质心。
青色框是检测时实际扫描窗口 `win_x0,win_y0,win_x1,win_y1`，右下边界为开区间。
无目标时也可能有扫描框。非法坐标不绘制。显示使用原始图片坐标，不修改源文件。
`dart_app.log` 的周期状态是汇总信息，没有每图身份，不作为单帧检测结果叠加。

拖动底部滑块或按 **← / →** 切帧，**Home / End** 到首尾；点击图片会在状态栏
和文字面板显示原图像素坐标，黑边点击忽略。右侧文字可选中复制，面板可拖动调宽。

## 无窗口检查与回归

```bash
./build/video_player/video_player --check records/rec_0001
ctest --test-dir build/video_player --output-on-failure
python3 tests/check_cpp_structure.py
```

`--check` 不要求桌面环境，只检查文件列表和文字关联，不解码全部图片。
输出图片数、录像匹配数、检测匹配数和 CSV 有记录但磁盘缺图的数量；还会列出
未匹配/冲突的图片。成功返回 0，关联不完整或解析失败返回 1，命令参数错误返回 2。
若没有检测 CSV，仅以录像关联判断完整性；不代表已经验证了检测结果。

`recording_types.cpp` 定义帧和 CSV 行数据，`csv.cpp` 负责 CSV 解析，
`image_reader.cpp` 负责逐帧解码，`recording.cpp` 扫描文件并关联录像/检测记录；
`image_view.cpp` 管理显示和原图坐标映射，`player.cpp` 组合窗口与命令入口，
`main.cpp` 仅转交入口。可包含 `.cpp` 使用包含保护、
单个具名命名空间及 inline 定义；CMake 分别编译每个组件以验证独立包含。
测试使用 Qt offscreen 平台，不需要实际显示器，覆盖真实 PNG/PGM/PBM 解码、PNG 跨分组目录关联及偏移/长度冲突、非整字节 PBM
行宽、序号/时间身份、缺图与重复记录、关闭识别、叠加边界、切帧和像素点击。

2026-10-16 在本机 Qt 5.15.13 / GCC 13.3 上独立构建和 CTest 通过，
`check_cpp_structure.py` 检查通过；使用既有主机录像样本验证了 `--check`，
并从其他工作目录运行了 `.script/player`。界面通过 offscreen 渲染与事件测试验证，
未进行桌面窗口的人工操作；Qt 6 尚未在本机实测。
