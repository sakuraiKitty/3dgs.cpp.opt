# 一帧管线图 (Frame Pipeline Diagram)

> 分支 `phys-sim` ｜ carnation 场景实测：1,037,279 高斯 / 13,356 MPM 粒子 / 窗口默认 1280×720
> frame_dt = 1/30 s ｜ MPM substeps = 128 → sub_dt ≈ 0.00026 s

---

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                        一帧管线 (frame_dt = 1/30 s)                          ║
╚══════════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────────────────────────────────┐
│ A. 物理交互 (CPU)                                                            │
│   ① 射线拾取 RayCaster (CPU, 遍历 13356 粒子)                                │
│   ② 位置回读 GetParticlePositions (download 2.3MB, ~1ms)                     │
│   ③ DragHandler.ComputeDragPushConstants (位置反馈 v=(target-cur)/dt)        │
│   ④ SetDragVelocityBC / SetDamping / SetHomeSpring                           │
└───────────────────────────┬──────────────────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ B. MPM Step  (× 128 substeps, sub_dt ≈ 0.00026 s)                            │
│   每个 substep 顺序执行 8 个 compute pass:                                   │
│                                                                              │
│  ⑤ zero_grid            dispatch (8,8,8)   local 8³   = 512 wg / 262144 thr  │
│      └─ 清零 64³ 网格节点                                                    │
│  ⑥ apply_home_spring     dispatch (53,1,1)  local 256  [enable=1]            │
│      └─ v += -k(x-x0)·dt  (刚体模态恢复力)                                   │
│  ⑦ apply_drag_velocity_bc dispatch (53,1,1)  local 256  [仅 isDragging]      │
│      └─ SET 半径内非冻结粒子速度                                              │
│  ⑧ p2g                  dispatch (53,1,1)  local 256                         │
│      └─ APIC 质量/动量/C 传递 + inline FCR 应力 + 3×3×3 B-spline             │
│  ⑨ grid_update           dispatch (8,8,8)   local 8³   = 512 wg              │
│      └─ 重力 + 阻尼 + CFL 限幅                                                │
│  ⑩ grid_freeze           dispatch (53,1,1)  local 256                         │
│      └─ 冻结粒子所在网格节点速度归零                                          │
│  ⑪ g2p                   dispatch (53,1,1)  local 256                         │
│      └─ APIC 回写 v/C/F + NaN 归零                                            │
│  ⑫ pin_frozen_particles  dispatch (53,1,1)  local 256                         │
│      └─ 冻结粒子硬钉回 init_pos + v=0                                         │
│                                                                              │
│   每阶段后插 VkMemoryBarrier(SHADER_WRITE→READ)                              │
│   单帧总 dispatch = 8 × 128 = 1024 次 compute                                 │
└───────────────────────────┬──────────────────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ C. 耦合 Coupling (compute)                                                   │
│  ⑬ compute_particle_displacements dispatch (53,1,1)  local 256               │
│      └─ 每粒子 disp = cur_pos - init_pos                                      │
│  ⑭ DisplacementMapper.map   dispatch (128,1,1) local 256                     │
│      └─ 每可变形高斯 KNN 反距离加权 → overridePositionBuffer (32703 高斯)     │
└───────────────────────────┬──────────────────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ D. 渲染 — Preprocess Command Buffer (recorded once, 每帧重提交)              │
│  ⑮ preprocess.comp       dispatch (4053,1,1) local 256                       │
│      └─ 动态 cov3D(读 override 位移) + SH 颜色 + tile overlap                │
│  ⑯ prefix_sum.comp       dispatch (4053,1,1) local 256  × 21 iters           │
│      └─ tile 边界 exclusive scan (双缓冲 ping/pong)                          │
│                                                                              │
│   ◇ preprocessFence 同步 → CPU 回读 numInstances (可见高斯数) ◇              │
└───────────────────────────┬──────────────────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ E. 渲染 — Main Command Buffer                                                │
│  ⑰ preprocess_sort.comp   dispatch (4053,1,1) local 256                      │
│      └─ 生成 64-bit 排序键 (depth+tile)                                       │
│  ⑱ radix sort × 8 轮 (4 字节 × 8 bit)                                        │
│       ├ sortHist  dispatch (N,1,1) local 256  (N = ceil(numInstances/256/32))│
│       └ sort      dispatch (N,1,1) local 256                                  │
│      └─ 每轮双缓冲,共 16 次 dispatch                                          │
│  ⑲ tile_boundary.comp     dispatch (numInstances+255)/256, local 256         │
│      └─ 写 tile 边界索引到 tileBoundaryBuffer                                 │
│  ⑳ render.comp (Splat)    dispatch (80,45,1)  local 16×16                    │
│      └─ 高斯溅射 alpha 混合 → swapchain image                                 │
│         ╔══════════════════════════════════════════╗                         │
│         ║ 渲染分辨率 = 1280 × 720 (swapchain extent)║                         │
│         ║ 80×45 = 3600 tiles, 每 tile 16×16 = 256 thr║                        │
│         ╚══════════════════════════════════════════╝                         │
│  ㉑ ImGui 渲染 (rasterizer draw, 独立 pass, 分辨率同 swapchain)              │
│   → present                                                                  │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 数值汇总表

| # | Pass | Shader/阶段 | Dispatch (wg) | Local | 线程数 | 频率 |
|---|------|------------|---------------|-------|--------|------|
| ⑤ | ZeroGrid | zero_grid.comp | 8×8×8=512 | 8³ | 262144 | ×128 |
| ⑥ | HomeSpring | apply_home_spring.comp | 53 | 256 | 13568 | ×128 |
| ⑦ | DragBC | apply_drag_velocity_bc.comp | 53 | 256 | 13568 | ×128 (拖拽时) |
| ⑧ | P2G | p2g.comp | 53 | 256 | 13568 | ×128 |
| ⑨ | GridUpdate | grid_update.comp | 8×8×8=512 | 8³ | 262144 | ×128 |
| ⑩ | GridFreeze | grid_freeze.comp | 53 | 256 | 13568 | ×128 |
| ⑪ | G2P | g2p.comp | 53 | 256 | 13568 | ×128 |
| ⑫ | PinFrozen | pin_frozen_particles.comp | 53 | 256 | 13568 | ×128 |
| ⑬ | ParticleDisp | compute_particle_displacements.spv | 53 | 256 | 13568 | ×1 |
| ⑭ | GaussMap | DisplacementMapper.map | 128 | 256 | 32768 | ×1 |
| ⑮ | Preprocess | preprocess.comp | 4053 | 256 | ~1.04M | ×1 |
| ⑯ | PrefixSum | prefix_sum.comp | 4053 | 256 | ~1.04M | ×21 iters |
| ⑰ | PreprocessSort | preprocess_sort.comp | 4053 | 256 | ~1.04M | ×1 |
| ⑱ | RadixSort | sortHist+sort | N | 256 | — | ×8 轮(16 次) |
| ⑲ | TileBoundary | tile_boundary.comp | ceil(numInst/256) | 256 | — | ×1 |
| ⑳ | **Render(Splat)** | **render.comp** | **80×45=3600** | **16×16** | **921600** | ×1 |
| ㉑ | GUI | ImGui raster | — | — | — | ×1 |

---

## 关键说明

- **MPM 粒子数 13,356** → `(13356+255)/256 = 53` workgroups/阶段；128 substeps × 8 阶段 = **1024 次 compute dispatch/帧**（物理主开销）
- **网格 64³=262144 节点** → ZeroGrid/GridUpdate 用 8³ local + 8×8×8 dispatch 覆盖
- **渲染分辨率 1280×720**（默认窗口，`apps/viewer/main.cpp:90-91`；可 `--width/--height` 改）→ render.comp 80×45 tiles，每 tile 16×16
- **D 段有 CPU 同步点**：preprocessFence 等 prefix_sum 完成后 CPU 回读 `numInstances`，再据此算 radix sort 的 dispatch size——这是帧内唯一 CPU 回读阻塞点
- **Preprocess 命令缓冲录制一次复用**：因读 live override buffer，位移每帧变化也能正确重算 cov3D
- **DragBC ⑦ 仅拖拽时派发**；HomeSpring ⑥ enable=1 时恒派发

---

## dispatch size 推导

| 参数 | 值 | 来源 |
|------|-----|------|
| numVertices (高斯总数) | 1,037,279 | point_cloud.ply |
| num_particles (MPM 粒子) | 13,356 | moving_part_points.ply |
| num_deformable (前景高斯) | 32,703 | clean_object_points 匹配 |
| grid_size | 64 | MPMManager::Config |
| substeps | 128 | MPMManager::Config |
| 窗口默认 | 1280×720 | main.cpp:90-91 |
| TILE_WIDTH/HEIGHT | 16×16 | shaders/common.glsl |
| MPM particle wg | `(13356+255)/256 = 53` | Substep() |
| grid wg | `(64+7)/8 = 8` → 8³ | Substep() |
| preprocess wg | `(1037279+255)/256 = 4053` | recordPreprocessCommandBuffer() |
| render wg | `(1280+15)/16 × (720+15)/16 = 80×45` | draw() |
| prefix_sum iters | `ceil(log2(1037279))+1 = 21` | recordPreprocessCommandBuffer() |
| radix sort 轮数 | 8 (4 bytes × 8 bits) | draw() |

---

*基于 phys-sim 分支 2026-06-30 状态。dispatch size 随场景/窗口尺寸变化。*
