# XPBD 弹性体物理改造计划 (XPBD Elastic Body Migration Plan)

> 分支 `phys-sim` ｜ 目标：将 MPM 实时推理替换为 XPBD 约束求解，实现 1-4 substeps/帧
> 编写日期：2026-07-03 ｜ 作者：liuyue（基于 deep-research 报告）

---

## 0. 一句话定位

把每帧 **512 次 compute dispatch（128 substep × 4 pass）** 的 MPM 推理，替换为 **2-4 次 XPBD 约束求解 dispatch**，从波速 CFL 限制中解放，实现 30-60 FPS 实时交互。

MPM 不被删除，而是**降级为离线资产烘焙工具**（提取约束拓扑 + 材料参数），实时只跑 XPBD Jacobi 迭代。

---

## 1. 动机与根因对账

### 1.1 当前 MPM 的痛点（来自 memory 实测）

| memory 条目 | 痛点 | XPBD 如何消解 |
|---|---|---|
| `stress-asymmetry-energy-injection` | 花头刚体式运动 → 均匀 F → σ 均匀 → 净力=0 → 无恢复力 | XPBD 距离约束直接拉回 rest position，无应力计算 |
| `rigid-rotation-no-restore-root-cause` | 客观材料模型对纯旋转零应力 | XPBD 约束是位置投影，旋转不变性自动满足 |
| `apic-first-moment-stencil-bug` | APIC 一阶矩 stencil 符号错误致 C 反号放大 | XPBD 无 P2G/G2P，无 stencil，无 APIC |
| `physdreamer-gui-demo-recovery-design` | 128 子步 + 大抓取致刚体模态 | XPBD compliance α=1/stiffness 解耦刚度与 dt，1-4 substeps 即可 |
| `mpm-gravity-cfl-explosion` | substeps/gravity 偏离原版致爆炸 | XPBD 无 CFL，dt 由精度决定，非稳定性 |
| `knn-farthest-not-nearest-bug` / `pin-batch-influence-fix` | KNN + influence_radius 手调 | XPBD 约束传播自动正确，无需手调 |
| `vk-ext-shader-atomic-float-fix` | atomicAdd(float) 需 VK_EXT_shader_atomic_float | XPBD 纯粒子，无 grid，无 atomic |

### 1.2 核心洞察

MPM 的 128 子步来自 **explicit + 波速 CFL**（dt ≤ CFL·dx/c）。XPBD 是**约束求解**，不是波传播，刚度由 compliance 参数控制，与 dt 解耦。carnation 花头是近刚性可变形体（弹性弯曲，无塑性/断裂），正适合 XPBD。

---

## 2. XPBD 数学基础

### 2.1 核心公式（Macklin & Müller 2016）

对 n 粒子系统，位置 **x** ∈ ℝ³ⁿ，速度 **v** ∈ ℝ³ⁿ，质量 **w** = 1/m ∈ ℝⁿ。

每帧（dt = 1/30 s）：

```
1. 预测位置：x_pred = x + v·dt
2. 约束求解（Jacobi 迭代 M 次）：
   for iter in 1..M:
     for each constraint C_j(x):
       Δx = -α̃_j · ∇C_j · (C_j(x) / |∇C_j|²)
       x += Δx
3. 更新速度：v = (x - x_pred) / dt
```

其中：
- C_j(x) = 0 是第 j 个约束（距离/体积/形状）
- α_j = compliance（柔度，α=1/stiffness）
- α̃_j = α_j / dt²（scaled compliance，让刚度与 dt 解耦）

### 2.2 约束类型（carnation 所需）

| 约束 | 公式 | 用途 |
|------|------|------|
| **距离约束** | C(p₁,p₂) = \|p₁-p₂\| - d₀ | 弹簧刚度，主要弹性来源 |
| **体积约束** | C(tet) = V(tet) - V₀ | 体积守恒，防压缩 |
| **弯曲约束** | C(p₁,p₂,p₃) = angle(p₁,p₂,p₃) - θ₀ | 花头弯曲刚度 |
| **锚点约束** | C(p) = \|p - p₀\| | 花茎根部固定 |

### 2.3 刚度与 compliance 的关系

- α = 0 → 完全刚性（约束严格满足）
- α = 1e-4 → 中等弹性（橡胶）
- α = 1e-2 → 软弹性（花头）
- α = 1 → 几乎无约束（流体）

carnation 花头：α_distance ≈ 1e-3（中等刚度），α_volume ≈ 1e-4（近不可压），α_bend ≈ 1e-2（软弯曲）。

---

## 3. 架构：MPM 烘焙 → XPBD 实时

```
┌──────────────────────── 离线（资产烘焙，一次性）────────────────────────┐
│  ① 从 moving_part_points.ply 提取 13356 粒子位置                        │
│  ② 构建约束拓扑：                                                      │
│     - 距离约束：k-NN 连接（k=6-12），每对粒子一个约束                    │
│     - 体积约束：Delaunay 四面体化，每四面体一个约束                      │
│     - 弯曲约束：表面法向邻接，每三角面片一个约束                         │
│  ③ 计算 rest length / rest volume / rest angle                          │
│  ④ 拟合 compliance 参数（α_distance, α_volume, α_bend）                 │
│  产物：                                                                 │
│     - constraints.sbo（约束索引 + rest 值 + compliance）                 │
│     - particles.sbo（初始位置 + 质量 + 锚点标记）                        │
└───────────────────────────────────────────────────────────────────────┘
                                ▼
┌──────────────────────── 实时（推理热路径，每帧）────────────────────────┐
│  B'. XPBD 物理 (2-4 次 compute dispatch 替代原 512 次)                  │
│   ⑤' predict_positions    dispatch(211,1,1) local 64                   │
│        · x_pred = x + v·dt + g·dt²（重力预测）                          │
│   ⑥' solve_constraints    dispatch(211,1,1) local 64 × M iterations    │
│        · Jacobi 迭代 M=10-20 次（可在单 kernel 内循环）                  │
│        · 每线程处理一个粒子，更新其邻接约束的 Δx                        │
│   ⑦' update_velocities    dispatch(211,1,1) local 64                   │
│        · v = (x - x_pred) / dt                                         │
│   ⑧' apply_drag_bc        dispatch(211,1,1) local 64 [仅拖拽时]        │
│        · 拖拽粒子位置直接设为 target（硬约束）                           │
│                                                                              │
│   每阶段后插 VkMemoryBarrier(SHADER_WRITE→READ)                         │
│   单帧 dispatch = 3×1 = 3（idle，⑧跳过）/ 4×1 = 4（拖拽 +⑧）           │
└───────────────────────────────────────────────────────────────────────┘
                                ▼
              C 段耦合 / D 段渲染管线 不动
```

### 3.1 三个派发的并行粒度

| 阶段 | 数据规模 | 并行粒度 | 派发形态 | 说明 |
|---|---|---|---|---|
| 预测位置 | 13356×3 | 粒子级并行 | 211 wg local 64 | 每粒子独立预测 |
| 约束求解 | ~80000 约束 | 粒子级 Jacobi | 211 wg local 64 × M iter | 每线程处理一个粒子的邻接约束 |
| 更新速度 | 13356×3 | 粒子级并行 | 211 wg local 64 | 每粒子独立更新 |

### 3.2 Jacobi 迭代的并行性

XPBD 的约束求解是 **Jacobi 迭代**（非 Gauss-Seidel），每次迭代可全并行：
- 每线程读当前 x，计算邻接约束的 Δx，累加到 x
- 需要 atomicAdd 累加 Δx（同一粒子被多个约束更新）
- 但 atomic 只在**单粒子邻接约束数**级别（~10-20），远低于 MPM P2G 的全局 scatter

**优化**：用 shared memory 做本地累加，最后 atomicAdd 一次到 global（aggAtomic 模式）。

---

## 4. 约束拓扑设计（carnation 专用）

### 4.1 距离约束（主要弹性来源）

```
对每个粒子 i：
  找 k=8 个最近邻粒子（k-NN，空间哈希或 kd-tree）
  对每个邻接 j：
    添加约束 C(i,j) = |x_i - x_j| - d_rest(i,j)
    compliance α = α_distance
```

约束数：13356 × 8 / 2 ≈ **53424 个距离约束**（去重）。

### 4.2 体积约束（防压缩）

```
对 moving_part_points.ply 做 Delaunay 四面体化（CGAL / TetGen）
对每个四面体 (i,j,k,l)：
  添加约束 C(tet) = V(tet) - V_rest
  compliance α = α_volume
```

约束数：~**40000 个体积约束**（经验值，13k 粒子四面体化）。

### 4.3 弯曲约束（花头弯曲刚度）

```
对表面粒子（法向朝外），找 2-ring 邻接
对每个三角面片 (i,j,k)：
  添加约束 C(tri) = dihedral_angle(i,j,k,l) - θ_rest
  compliance α = α_bend
```

约束数：~**20000 个弯曲约束**（表面粒子约 4000，每粒子 ~5 个三角）。

### 4.4 锚点约束（花茎根部固定）

```
对 Y < 0.3 的粒子（花茎底部）：
  添加约束 C(i) = |x_i - x_rest_i|
  compliance α = 0（完全刚性）
```

约束数：~**500 个锚点约束**。

### 4.5 约束拓扑存储

```glsl
// constraints.sbo
struct Constraint {
  uint type;           // 0=distance, 1=volume, 2=bend, 3=anchor
  uint indices[4];     // 粒子索引（distance=2, volume=4, bend=3, anchor=1）
  float rest_value;    // rest length / volume / angle
  float compliance;    // α
  uint padding[2];
};
// 总约束数 ≈ 53424 + 40000 + 20000 + 500 ≈ 114000
```

---

## 5. Vulkan Compute Shader 设计

### 5.1 predict_positions.comp

```glsl
#version 460
layout(local_size_x=64) in;

layout(set=0, binding=0) buffer Particles {
  vec3 x[];
  vec3 v[];
  float w[];  // 1/mass
};

layout(set=0, binding=1) buffer Predicted {
  vec3 x_pred[];
};

layout(push_constant) uniform PushConstants {
  float dt;
  vec3 gravity;
};

void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= x.length()) return;
  
  x_pred[i] = x[i] + v[i] * dt + gravity * dt * dt;
}
```

### 5.2 solve_constraints.comp（核心）

```glsl
#version 460
layout(local_size_x=64) in;

layout(set=0, binding=0) buffer Particles {
  vec3 x[];
  vec3 v[];
  float w[];
};

layout(set=0, binding=1) buffer Predicted {
  vec3 x_pred[];
};

layout(set=0, binding=2) buffer Constraints {
  Constraint constraints[];
};

layout(set=0, binding=3) buffer DeltaX {
  vec3 delta_x[];  // 累加 Δx
};

layout(push_constant) uniform PushConstants {
  float dt;
  uint num_iterations;  // M=10-20
};

// 距离约束梯度
vec3 grad_distance(vec3 p1, vec3 p2, float d_rest) {
  vec3 d = p1 - p2;
  float d_curr = length(d);
  if (d_curr < 1e-6) return vec3(0);
  return d / d_curr;
}

void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= x.length()) return;
  
  for (uint iter = 0; iter < num_iterations; iter++) {
    delta_x[i] = vec3(0);
    barrier();
    
    // 处理粒子 i 的所有邻接约束
    for (uint c = constraint_start[i]; c < constraint_end[i]; c++) {
      Constraint con = constraints[c];
      if (con.type == 0) {  // distance
        uint j = con.indices[1];
        vec3 grad = grad_distance(x_pred[i], x_pred[j], con.rest_value);
        float C = length(x_pred[i] - x_pred[j]) - con.rest_value;
        float alpha_tilde = con.compliance / (dt * dt);
        float s = 1.0 / (w[i] + w[j] + alpha_tilde);
        vec3 dx = -s * C * grad * w[i];
        atomicAdd(delta_x[i], dx);
      }
      // volume, bend, anchor 类似...
    }
    
    barrier();
    x_pred[i] += delta_x[i];
    barrier();
  }
}
```

### 5.3 update_velocities.comp

```glsl
#version 460
layout(local_size_x=64) in;

layout(set=0, binding=0) buffer Particles {
  vec3 x[];
  vec3 v[];
  float w[];
};

layout(set=0, binding=1) buffer Predicted {
  vec3 x_pred[];
};

layout(push_constant) uniform PushConstants {
  float dt;
};

void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= x.length()) return;
  
  v[i] = (x_pred[i] - x[i]) / dt;
  x[i] = x_pred[i];
}
```

---

## 6. 拖拽交互（apply_drag_bc）

替换当前 `apply_drag_velocity_bc` + influence_radius + KNN：

```glsl
// apply_drag_bc.comp
#version 460
layout(local_size_x=64) in;

layout(set=0, binding=0) buffer Particles {
  vec3 x[];
  vec3 v[];
  float w[];
};

layout(set=0, binding=1) buffer Predicted {
  vec3 x_pred[];
};

layout(push_constant) uniform PushConstants {
  uint drag_particle_id;
  vec3 drag_target;
};

void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i == drag_particle_id) {
    x_pred[i] = drag_target;  // 硬约束，直接设为 target
    v[i] = vec3(0);  // 拖拽时速度清零
  }
}
```

**优势**：无需 influence_radius、KNN、应变门控。拖拽一个粒子，XPBD 约束自动传播到邻接粒子，力扩散物理正确。

---

## 7. 耦合层适配（C 段）

当前 `compute_particle_displacements` 从 MPM 粒子缓冲读位移，改为从 XPBD 粒子缓冲读：

```cpp
// src/coupling/DisplacementMapper.cpp
void DisplacementMapper::Map(VkCommandBuffer cmd) {
  // 输入源从 mpm_particles_buffer_ 改为 xpbd_particles_buffer_
  // 其余 KNN 映射逻辑不变
}
```

**改动极小**：只换 SSBO binding，KNN 反距离加权逻辑不动。

---

## 8. 实施计划（分阶段）

### 阶段 0：约束拓扑提取（离线，无实时风险）

- [ ] 从 `moving_part_points.ply` 读取 13356 粒子位置
- [ ] k-NN 构建距离约束（k=8），存为 `constraints_distance.bin`
- [ ] Delaunay 四面体化构建体积约束，存为 `constraints_volume.bin`
- [ ] 表面三角化构建弯曲约束，存为 `constraints_bend.bin`
- [ ] 标记锚点粒子（Y < 0.3），存为 `constraints_anchor.bin`
- [ ] 合并为 `constraints.sbo`，验证约束数 ~114000

**工具**：Python + NumPy + SciPy（k-NN）+ CGAL/TetGen（四面体化）

**验收**：约束拓扑可视化（Blender / ParaView），无孤立粒子、无退化四面体

### 阶段 1：XPBD 核心 solver（compute shader）

- [ ] `predict_positions.comp`：预测位置 + 重力
- [ ] `solve_constraints.comp`：Jacobi 迭代（先 M=10，后调 10-20）
- [ ] `update_velocities.comp`：速度更新
- [ ] `apply_drag_bc.comp`：拖拽硬约束
- [ ] 接入 Renderer：替换 B 段 MPM Substep() 调用为 4 dispatch + barrier

**验收**：
- 无拖拽时花头静止稳定（无漂移、无自激振荡）
- 拖拽后松手**自动回弹到初始位姿**（距离约束拉回）
- FPS ≥ 30（目标 60）

### 阶段 2：参数调优

- [ ] 调 α_distance（距离刚度）：花头弹性弯曲
- [ ] 调 α_volume（体积刚度）：防压缩
- [ ] 调 α_bend（弯曲刚度）：花头弯曲回弹
- [ ] 调 M（迭代数）：10-20，平衡精度与性能
- [ ] 调 dt（帧时间）：1/30 或 1/60

**验收**：拖拽力反馈自然，松手回弹时间 ~0.5-1s（参考 PhysDreamer）

### 阶段 3：耦合层适配

- [ ] C 段 `compute_particle_displacements` 输入源改为 XPBD 粒子缓冲
- [ ] 验证前景高斯随 XPBD 位移正确变形
- [ ] 渲染无穿模/抖动

**验收**：拖拽花头，3DGS 前景高斯跟随变形，背景不动

### 阶段 4：性能优化

- [ ] Nsight Graphics 帧分析：B 段 dispatch 数 512 → 4
- [ ] 物理耗时占比 < 10% 帧预算
- [ ] FPS 基准对比（carnation 1,037,279 高斯）

**验收**：物理层实时开销可忽略，渲染重回主瓶颈

---

## 9. 代价与边界（诚实清单）

1. **约束拓扑是 asset-specific**：换 asset（carnation → rose）要重提取。本项目单一 asset 反复交互，可接受。
2. **大形变/断裂不支持**：XPBD 是弹性约束，无塑性/断裂。carnation 不需要，可接受。
3. **自接触建模变难**：XPBD 空间做自碰撞检测不如 MPM grid 直观。carnation 若不涉及强自接触，可接受；若需要，加空间哈希 + 碰撞约束。
4. **材料保真度中等**：XPBD compliance 是启发式参数，非真实 FCR/Neohookean。carnation 视觉上等效即可，不需要材料精确。
5. **离线 MPM 仍保留**：用于对比验证 XPBD 行为是否正确，不用于实时推理。

---

## 10. 与模态子空间方案的对比

| 维度 | XPBD | 模态子空间 |
|------|------|-----------|
| 每帧 dispatch | 2-4 | 3 |
| 物理真实性 | 中（compliance 近似） | 高（从 MPM FCR 蒸馏） |
| 大形变 | 好（泛化） | 需非线性增强 |
| 工程量 | 中（2-3 周） | 大（4-6 周，含 autoencoder） |
| 泛化性 | 高（任意弹性体） | 低（asset-specific） |
| 风险 | 低（成熟方案） | 中（非线性增强复杂） |

**结论**：XPBD 适合**快速落地 + 通用性**，模态子空间适合**物理真实性 + asset-specific 优化**。carnation 场景两者都可行，XPBD 工程量更小。

---

## 11. 关键文件预期改动

| 文件 | 改动 |
|---|---|
| `src/xpbd/XPBDManager.{h,cpp}` | 新增：XPBD solver 主类 |
| `src/xpbd/ConstraintBuilder.{h,cpp}` | 新增：离线约束拓扑提取 |
| `shaders/xpbd/predict_positions.comp` | 新增 |
| `shaders/xpbd/solve_constraints.comp` | 新增 |
| `shaders/xpbd/update_velocities.comp` | 新增 |
| `shaders/xpbd/apply_drag_bc.comp` | 新增 |
| `src/coupling/DisplacementMapper.{h,cpp}` | 输入源切换为 XPBD 粒子缓冲 |
| `CMakeLists.txt` | 收录新 shader + 类 |
| `PIPELINE_DIAGRAM.md` | B 段更新为 XPBD 4 派发 |

---

## 12. 里程碑

| M | 内容 | 风险 | 复杂度 |
|---|---|---|---|
| M0 | 离线约束拓扑提取 | 低 | 中（四面体化工具） |
| M1 | XPBD 核心 solver shader + 接入 | 中（Jacobi 并行 + atomic） | 复杂 |
| M2 | 参数调优（α, M, dt） | 低 | 简单 |
| M3 | 耦合层适配 | 低 | 简单 |
| M4 | 性能优化 + 验收 | 低 | 简单 |

**建议先做 M0（离线，零实时风险）+ M1（核心 solver），验证 XPBD 行为正确，再上 M2-M4。**

---

## 13. 参考资料

- Macklin & Müller, "Position Based Dynamics" (2016) — XPBD 原始论文
- https://mmacklin.com/xpbd.pdf
- NVIDIA Flex / PhysX 5 Soft Body — 工业界 XPBD 实现参考
- Taichi XPBD examples — 开源参考实现

---

*基于 phys-sim 分支 2026-07-03 状态编写。实施前需先用 `/plan` 或 TodoWrite 细化到 shader 级伪代码。*
