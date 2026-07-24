# RK3588 板端 RKNN 验证

本项目已经加入 `ppe_single` 单模型模式，可直接验证以下两个文件：

- `model/best-int8.rknn`：INT8 量化模型
- `model/best-fp.rknn`：非量化模型

程序会读取 `model/RK_anchors.txt` 的 18 个 anchor，以及
`model/construction_ppe_labels_list.txt` 的 11 个类别。原有
`config_user.ini` 三模型融合模式保持不变。

## 1. 同步并运行 INT8

```bash
cd project1/demo_multhread_decode_infer_mulmodel
bash run_ppe_validation.sh int8
```

脚本会用当前普通用户执行 CMake 构建，并在启动 `myDemo` 时自动调用
`sudo`，届时按提示输入密码。不要用 `sudo bash run_ppe_validation.sh`，
否则 `build/` 中会产生 root 所有的构建文件。

首次运行会重新执行 CMake 构建。后续没有修改代码时可以跳过构建：

```bash
NO_BUILD=1 bash run_ppe_validation.sh int8
```

不使用脚本时，等价的手动启动命令是：

```bash
sudo ./build/myDemo config_ppe_int8.ini
```

正常启动时重点检查以下日志：

```text
RKNN model id=0 runtime=... driver=...
RKNN output[0] ... type=INT8 ... qnt=AFFINE ...
RKNN model id=0 postprocess=INT8 affine, heads=...
PPE stream 0 detections=...
```

程序默认加载 `project1/1.mp4`～`4.mp4` 和板载摄像头共五路输入并循环
运行。五个 RKNN 上下文按 NPU 核心 `0/1/2/0/1` 轮转，每路每 30 次推理
输出一次类别和置信度。按 `Ctrl+C` 停止。

## 2. 运行非量化模型作基准

```bash
cd project1/demo_multhread_decode_infer_mulmodel
NO_BUILD=1 bash run_ppe_validation.sh fp
```

对应的手动命令为：

```bash
sudo ./build/myDemo config_ppe_fp.ini
```

非量化模型应显示 `postprocess=FP32`。先用同一视频、同一阈值分别运行
INT8 和 FP，比较检测类别、框位置、置信度以及运行速度。若 FP 正常而
INT8 明显漏检，再回头调整校准集，而不是先改后处理。

## 3. 查看带框视频

两份 PPE 配置默认启用 RTMP，并向 WSL 中的 SRS 推送：

```ini
rtmp_url=rtmp://192.168.2.3/live/livestream
enable_rtmp=true
draw_detections=true
```

必须先在 WSL 启动 SRS，再运行板端程序：

```bash
docker start srs
ffplay -fflags nobuffer -flags low_delay -probesize 32 \
  -analyzeduration 0 -framedrop \
  rtmp://192.168.2.3/live/livestream
```

如果 WSL/宿主机地址变化，需要同时修改两份 PPE 配置。当前项目的 RTSP
发送函数仍是占位实现，板端验证应使用 RTMP。若只想在终端检查检测结果，
可将 `enable_rtmp` 临时改为 `false`。

若要直接测试摄像头，把配置中的 `[stream.0]` 改为：

```ini
[stream.0]
enabled=true
type=camera
device_path=/dev/v4l/by-path/你的摄像头设备
width=1920
height=1080
fps=25
chroma_order=uv
auto_white_balance=true
reconnect_interval_ms=1000
```

## 4. 判定结果

- `rknn_init` 失败：先核对生成模型使用的 Toolkit2 与板端 Runtime/驱动兼容性。
- `Cannot match YOLOv5 ... output`：模型类别数不是 11，或模型不是三检测头的
  YOLOv5 原始输出结构；不要强行继续解码。
- 能推理但始终 `detections=0`：先把 `confidence_threshold` 暂降到 `0.10`
  排查，再核对输入图片、类别顺序和模型本身。
- 框基本正确但类别名称错位：核对标签文件顺序，不能按字母排序。
- INT8 与 FP 差异很大：扩大并重新平衡校准集，尤其增加小目标、遮挡、
  明暗变化和各类负样本。

负类 `none`、`no_helmet`、`no_goggle`、`no_gloves`、`no_boots` 使用红框；
其他 PPE 类使用绿框，`Person` 使用蓝色框。
