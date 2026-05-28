# 自动化验收工具使用指南

## 概述

本工具集用于3DGS.cpp混合管线迁移的自动化验收，支持：
- 建立baseline
- 各阶段验收对比
- 自动触发F12截图和性能数据记录

## 工具列表

### 1. benchmark.py - 主要验收工具

**功能**：
- 自动启动viewer并加载carnations场景
- 等待场景稳定（8秒warmup）
- 触发F12截图（纯3DGS渲染结果）
- 捕获FPS性能数据
- 保存截图和JSON数据

**使用方法**：
```bash
cd "d:/liuyue/physDreamerVulkanDemo/3DGS.cpp"
python scripts/benchmark.py baseline          # 建立baseline
python scripts/benchmark.py stage_1          # 阶段1验收
python scripts/benchmark.py stage_2          # 阶段2验收
python scripts/benchmark.py stage_3          # 阶段3验收

# 自定义运行时长
python scripts/benchmark.py stage_1 --duration 20
```

**输出结构**：
```
verification/
├── baseline/
│   ├── baseline_frame.png      # 纯3DGS渲染截图（F12）
│   └── baseline_results.json   # 性能数据
├── stage_1/
│   ├── stage_1_frame.png
│   └── stage_1_results.json
└── stage_2/
    ├── stage_2_frame.png
    └── stage_2_results.json
```

**数据格式**：
```json
{
  "timestamp": "2026-05-28T13:32:33",
  "stage": "baseline",
  "scene": "carnations",
  "resolution": [1280, 720],
  "screenshot": "baseline_frame.png",
  "fps": {
    "avg": 66.69,
    "min": 4.0,
    "max": 104.0,
    "median": 101.0,
    "samples": 16
  },
  "frame_time": {
    "avg": 15.0
  },
  "stability": {
    "std_dev": 45.55,
    "variance": 100.0
  }
}
```

### 2. compare_stages.py - 阶段对比工具

**功能**：
- 对比两个阶段的截图质量
- 计算PSNR、MSE等指标
- 评估图像质量差异

**使用方法**：
```bash
python scripts/compare_stages.py baseline stage_1
python scripts/compare_stages.py baseline stage_2
python scripts/compare_stages.py stage_1 stage_2
```

**输出示例**：
```
============================================================
              阶段对比: baseline vs stage_1
============================================================

[baseline] 2026-05-28T13:32:33
[stage_1] 2026-05-28T14:00:00

[对比] 图像质量分析...
  PSNR:      35.42 dB
  MSE:       12.85
  最大差异:  45.00
  平均差异:  3.21

[评估] 良好 (轻微差异)

[保存] 对比结果: baseline_vs_stage_1.json
```

## 工作流程

1. **工具启动viewer** - 自动加载carnations场景和camera.txt
2. **等待场景稳定** - 8秒warmup期间忽略性能数据
3. **触发F12截图** - 使用Windows API激活窗口并发送F12
4. **捕获性能数据** - 从verbose日志中解析FPS数据
5. **分析并保存** - 计算统计数据并保存JSON
6. **自动清理** - 终止viewer进程

## 注意事项

1. **运行环境**：
   - Python 3.x
   - 依赖库：pip install pillow numpy
   - Windows系统（使用Windows API发送F12）

2. **截图质量**：
   - F12截图是纯3DGS渲染结果，不包含GUI
   - 分辨率取决于viewer窗口大小
   - 截图保存在项目根目录的screenshot_N.png

3. **FPS数据捕获**：
   - 使用--verbose模式启动viewer
   - 从spdlog::debug输出解析FPS
   - 自动去除warmup期间的数据（前20%）

4. **warmup时间**：
   - 默认8秒，确保场景完全加载
   - warmup期间FPS不稳定，会在分析时去除

## 当前Baseline状态

**已建立**: ✅
- **时间**: 2026-05-28T13:32:33
- **场景**: carnations
- **GPU**: NVIDIA GeForce RTX 4090 Laptop GPU
- **分辨率**: 1280x720
- **平均FPS**: 66.69 fps
- **帧时间**: 15.00 ms

## 下一步

Baseline已建立完成！现在可以开始阶段1的实施工作：

```bash
# 查看baseline截图
d:\liuyue\physDreamerVulkanDemo\3DGS.cpp\verification\baseline\baseline_frame.png

# 查看baseline数据
d:\liuyue\physDreamerVulkanDemo\3DGS.cpp\verification\baseline\baseline_results.json
```

准备就绪后，开始阶段1：基础架构准备
