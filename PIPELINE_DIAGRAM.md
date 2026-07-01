# 一帧管线图 (Frame Pipeline Diagram)

> 分支 `phys-sim` ｜ carnation 场景实测：1,037,279 高斯 / 13,356 MPM 粒子 / 窗口默认 1280×720
> frame_dt = 1/30 s ｜ MPM substeps = **128**（ScenePhysicsProfile carnation）→ sub_dt ≈ 2.6e-4 s
> 算法状态：home-spring 关闭（dispatch 跳过）/ 纯 FCR / CFL=0.02 / DragBC 应变门控
> 已融合：PinFrozen→G2P、GridFreeze→GridUpdate（freeze_mask）、G2P 三循环→一、local_size 64
> 实测 FPS：~28（物理开，128 substeps）/ ~105（物理关）

---

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                        一帧管线 (frame_dt = 1/30 s)                          ║
╚══════════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────────────────────────────────┐
│ A. 物理交互 (CPU)                                                            │
│   ① 射线拾取 RayCaster (CPU, 遍历 13356 粒子，仅首次拾取)                     │
│   ② 单粒子 GPU 回读 GetParticlePositionGPU (staging 176B + waitIdle, ~1ms)   │
│   ③ DragHandler.ComputeDragPushConstants (位置反馈 v=(target-cur)/dt)        │
│   ④ SetDragVelocityBC / SetDamping (home-spring/F-relax 已关闭，不注入)      │
└───────────────────────────┬──────────────────────────────────────────────────┘
                            ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ B. MPM Step  (× 128 substeps, sub_dt ≈ 2.6e-4 s)                             │
│   每个 substep 顺序执行 4~5 个 compute pass（已融合 PinFrozen/GridFreeze）:   │
│                                                                              │
│  ⑤ zero_grid            dispatch (8,8,8)   local 8³   = 512 wg / 262144 thr  │
│      └─ 清零 64³ 网格节点 + freeze_mask                                       │
│  ⑥ apply_home_spring     【跳过】enable=0，dispatch 不派发（home-spring 关闭）│
│  ⑦ apply_drag_velocity_bc dispatch (211,1,1) local 64   [仅 isDragging]      │
│      └─ SET 半径内非冻结粒子速度 + 应变门控 smoothstep(1.2,1.5) + J<0.1 停驱  │
│  ⑧ p2g                  dispatch (211,1,1) local 64                          │
│      └─ APIC 质量/动量/C 传递 + inline FCR 应力 + 标记 freeze_mask(冻结粒子) │
│  ⑨ grid_update          dispatch (8,8,8)   local 8³   = 512 wg               │
│      └─ 重力 + 阻尼 + CFL 限幅 + 内联冻结节点零化（原 GridFreeze 融合）       │
│  ⑩ g2p                  dispatch (211,1,1) local 64                          │
│      └─ APIC 回写 v/C/F（单循环融合）+ NaN reset + 内联 PinFrozen(F=I/C=0)    │
│                                                                              │
│   每阶段后插 VkMemoryBarrier(SHADER_WRITE→READ)                              │
│   单帧 dispatch = 4×128 = 512（idle，⑥跳过）/ 5×128 = 640（拖拽 +⑦）          │
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
| ⑥ | HomeSpring | apply_home_spring.comp | 211 | 64 | 13504 | **跳过**（enable=0）|
| ⑦ | DragBC | apply_drag_velocity_bc.comp | 211 | 64 | 13504 | ×128 (拖拽时) |
| ⑧ | P2G | p2g.comp | 211 | 64 | 13504 | ×128 |
| ⑨ | GridUpdate(+Freeze) | grid_update.comp | 8×8×8=512 | 8³ | 262144 | ×128 |
| ⑩ | G2P(+PinFrozen) | g2p.comp | 211 | 64 | 13504 | ×128 |
| ⑪ | ParticleDisp | compute_particle_displacements.spv | 53 | 256 | 13568 | ×1 |
| ⑫ | GaussMap | DisplacementMapper.map | 128 | 256 | 32768 | ×1 |
| ⑬ | Preprocess | preprocess.comp | 4053 | 256 | ~1.04M | ×1 |
| ⑭ | PrefixSum | prefix_sum.comp | 4053 | 256 | ~1.04M | ×21 iters |
| ⑮ | PreprocessSort | preprocess_sort.comp | 4053 | 256 | ~1.04M | ×1 |
| ⑯ | RadixSort | sortHist+sort | N | 256 | — | ×8 轮(16 次) |
| ⑰ | TileBoundary | tile_boundary.comp | ceil(numInst/256) | 256 | — | ×1 |
| ⑱ | **Render(Splat)** | **render.comp** | **80×45=3600** | **16×16** | **921600** | ×1 |
| ⑲ | GUI | ImGui raster | — | — | — | ×1 |

---

## 关键说明

- **MPM 粒子数 13,356** → `(13356+63)/64 = 211` workgroups/阶段（local_size=64，占用率优化）；128 substeps × 4 阶段（⑥HomeSpring 跳过，PinFrozen/GridFreeze 已融合）= **512 次 compute dispatch/帧**（idle）；拖拽时 +⑦DragBC = 5×128 = **640 次/帧**。
- **网格 64³=262144 节点** → ZeroGrid/GridUpdate 用 8³ local + 8×8×8 dispatch 覆盖（全量扫，高占用）。
- **渲染分辨率 1280×720**（默认窗口，`apps/viewer/main.cpp:90-91`；可 `--width/--height` 改）→ render.comp 80×45 tiles，每 tile 16×16
- **D 段有 CPU 同步点**：preprocessFence 等 prefix_sum 完成后 CPU 回读 `numInstances`，再据此算 radix sort 的 dispatch size
- **A 段②拖拽中每帧 waitIdle**：`GetParticlePositionGPU` 单粒子 staging 回读 + `queue.waitIdle`（一帧滞后）
- **Preprocess 命令缓冲录制一次复用**：因读 live override buffer，位移每帧变化也能正确重算 cov3D
- **DragBC ⑦ 仅拖拽时派发**；HomeSpring ⑥ enable=0 恒跳过

---

## dispatch size 推导

| 参数 | 值 | 来源 |
|------|-----|------|
| numVertices (高斯总数) | 1,037,279 | point_cloud.ply |
| num_particles (MPM 粒子) | 13,356 | moving_part_points.ply |
| num_deformable (前景高斯) | 32,703 | clean_object_points 匹配 |
| grid_size | 64 | ScenePhysicsProfile (carnation) |
| substeps | **128** | ScenePhysicsProfile (carnation；hat/telephone=64, alocasia=128) |
| 窗口默认 | 1280×720 | main.cpp:90-91 |
| TILE_WIDTH/HEIGHT | 16×16 | shaders/common.glsl |
| MPM particle wg | `(13356+63)/64 = 211` | Substep()（local_size=64） |
| grid wg | `(64+7)/8 = 8` → 8³ | Substep() |
| preprocess wg | `(1037279+255)/256 = 4053` | recordPreprocessCommandBuffer() |
| render wg | `(1280+15)/16 × (720+15)/16 = 80×45` | draw() |
| prefix_sum iters | `ceil(log2(1037279))+1 = 21` | recordPreprocessCommandBuffer() |
| radix sort 轮数 | 8 (4 bytes × 8 bits) | draw() |

---

*基于 phys-sim 分支 2026-07-01 状态（home-spring 关闭 / 纯 FCR / substeps=128 / PinFrozen+GridFreeze 融合 / local_size=64 / ~28 FPS）。dispatch size 随场景/窗口尺寸变化。*
