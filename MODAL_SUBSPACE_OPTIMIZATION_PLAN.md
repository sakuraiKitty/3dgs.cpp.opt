# 模态子空间物理优化方案 (Modal Subspace Physics Plan)

> 分支 `phys-sim` ｜ 目标：将实时推理热路径从"全粒子前向 MPM"重构为"模态子空间降阶物理"
> 编写日期：2026-07-01 ｜ 作者：liuyue

---

## 0. 一句话定位

把每帧 **1024 次 compute dispatch（128 substep × 8 pass）** 的 MPM 推理，替换为 **3 次轻量模态派发（力投影 + K 维积分 + 场重建）**，并从坐标系层面消解"刚体模态零应力无恢复力"的死结。

MPM 不被删除，而是**从"每帧实时推理引擎"降级为"离线资产烘焙工具"**——离线跑仿真采集轨迹、提取模态基，实时只查基 + 积分低维 ODE。

---

## 1. 动机与根因对账

### 1.1 当前管线的痛点（来自 memory 实测）

| memory 条目 | 痛点 | 模态方案如何消解 |
|---|---|---|
| `stress-asymmetry-energy-injection` | 花头刚体式运动 → 均匀 F → σ 均匀 → 净力=0 → 无恢复力 | 刚体模态从形变模态分离，挂显式 home-spring |
| `rigid-rotation-no-restore-root-cause` | 客观材料模型对纯旋转零应力 | 不再问材料要刚体恢复力，模态坐标里独立给 |
| `apic-first-moment-stencil-bug` | APIC 一阶矩 stencil 符号错误致 C 反号放大 | 模态空间无 P2G/G2P，无 stencil |
| `physdreamer-gui-demo-recovery-design` | 128 子步 + 大抓取致刚体模态 | K 维 ODE 单步显式，无子步、无 CFL |
| `mpm-gravity-cfl-explosion` | substeps/gravity 偏离原版致爆炸 | 模态阻尼控制刚性，无 CFL 限制 |
| `knn-farthest-not-nearest-bug` / `pin-batch-influence-fix` | KNN + influence_radius 手调 | 拖拽力经 Φᵀ 投影自动扩散，物理正确 |
| `stale-spv-freeze-bug` / `mpm-descriptor-pool-reset-freeze` / `vk-ext-shader-atomic-float-fix` | MPM 运行时基础设施脆弱 | 热路径不再调用这些设施 |

### 1.2 核心洞察

所有痛点本质是**表示选择错误**：全粒子 MPM 把刚体运动与形变运动混在同一个积分空间里，导致刚体模态的恢复力被"客观应力"忠实表达为零。模态降阶的核心动作是**把这个混合空间切开**——刚体 6 自由度显式单独积分，形变自由度在低维子空间积分。

---

## 2. 数学基础

### 2.1 线性模态降阶

对 n=13356 粒子、3n 自由度系统，位移 **u** ∈ ℝ³ⁿ，小形变弹性方程：

$$M\ddot{u} + C\dot{u} + Ku = f_{ext}$$

解广义特征值问题：

$$K\Phi = M\Phi\Lambda$$

- Φ = [φ₁, …, φ_K] ∈ ℝ³ⁿ×K：模态基矩阵
- 前 6 列：刚体模态（λ=0：3 平移 + 3 旋转）
- 之后：形变模态，按 λ 升序

坐标变换 **u = Φq**，q ∈ ℝ^K，代入并左乘 Φᵀ：

$$\tilde{M}\ddot{q} + \tilde{C}\dot{q} + \tilde{K}q = \Phi^T f_{ext}$$

其中 $\tilde{M}=\Phi^T M\Phi = I$，$\tilde{K}=\Phi^T K\Phi=\Lambda=\text{diag}(\lambda_i)$（**本征刚度**）。

→ 3n=40068 维 ODE 降为 K≈30 维 ODE，且 K 个标量方程互相解耦。

### 2.2 本征刚度的意义

- λ_i 是第 i 模态的固有弹性恢复系数，只取决于几何 + 材料分布；
- 低 λ = 软方向（易激发，拖拽主要激发对象）；高 λ = 硬方向（高频小振幅，截断）；
- 刚体模态 λ=0 → **数学上无恢复力**，这正是死结的源头，也是模态方案能"看见并显式处理"它的前提。

### 2.3 非线性模态（carnation 实际所需）

线性模态假设小形变，carnation 花头拖拽是大旋转 + 几何非线性，纯线性基会 locking。两条出路：

1. **Modal warping**：对模态坐标逐个做极分解旋转修正（Choi & Ko）。代价低，仍实时。
2. **数据驱动非线性基**：离线 MPM 采轨迹快照 → PCA / autoencoder 学非线性形变流形。重建为网络前向，实时可行。

→ **本项目选方案 2**：已有 MPM 离线仿真能力，单一 asset 反复交互正适合过拟合到 30 维形变流形。

---

## 3. 架构：MPM 烘焙 → 模态实时

```
┌──────────────────────── 离线（资产烘焙，一次性）────────────────────────┐
│  ① 随机拖拽 MPM 仿真 × N(50~200) 次，存粒子位移轨迹快照               │
│  ② 快照去均值 + 刚体分量剥离（质心 + 主轴对齐）                        │
│  ③ PCA / autoencoder 提取 K 维形变基 Φ ∈ ℝ³ⁿ×K                         │
│  ④ 解析提取刚体模态（6 列：3 平移 + 3 旋转）                          │
│  ⑤ 拟合各模态 λ_i、阻尼 c_i、刚体 home-spring 系数 k_home             │
│  产物：Φ（基矩阵）、Λ（本征刚度）、C（模态阻尼）、k_home               │
│       存为 GPU SSBO + 一个小型 autoencoder decoder 权重                │
└──────────────────────────────────────────────────────────────────────┘
                                ▼
┌──────────────────────── 实时（推理热路径，每帧）────────────────────────┐
│  B'. 模态物理 (3 次 compute dispatch 替代原 1024 次)                    │
│   ⑤' project_forces     dispatch(53,1,1)  ← q_force = Φᵀ(f_drag+g)     │
│   ⑥' integrate_modal    dispatch(1,1,1) local 64  ← K 维 RK4 / 半隐式   │
│        · 形变模态：λ_i·q_i 恢复 + c_i·q̇_i 阻尼                        │
│        · 刚体模态：k_home·q_rigid 恢复 + 角阻尼                         │
│   ⑦' reconstruct_field  dispatch(53,1,1)  ← u = Φ q（或 decoder 前向）  │
│  + barrier(project→integrate→reconstruct)                              │
└──────────────────────────────────────────────────────────────────────┘
                                ▼
              C 段耦合 / D 段渲染管线 不动
```

### 3.1 三个派发的并行粒度

| 阶段 | 数据规模 | 并行粒度 | 派发形态 | 说明 |
|---|---|---|---|---|
| 力投影 | 输入 3n → 输出 K | 粒子级并行 + 归约 | 53 wg | n→K 矩阵向量乘，每粒子贡献 3×K 块 |
| 积分 | K→K 标量 ODE | 单组即可 | 1 wg local 64 | K=30 装得下共享内存 |
| 重建 | 输入 K → 输出 3n | 粒子级并行 | 53 wg | K→n 广播，每粒子查自己 3×K 块 |

⑤′ 与 ⑦′ 都是 53 wg 但方向相反（归约 vs 广播），中间夹 ⑥′ 的 K 维标量积分，存在数据依赖 → 必须分开派发 + barrier。

### 3.2 派发数下界说明

3 是不可压缩下界的代表值。实际可能涨到 4–5：

- 重力体积力 $\Phi^T g$ 是常量，预算一次缓存，实时不算；
- 拖拽力投影与积分之间若加接触检测 → +1；
- 非线性 decoder 重建 → +1 网络前向；
- 刚体模态极分解/旋转恢复拆出 → +1。

即便涨到 5，相对 1024 仍是两个数量级降低。

---

## 4. 拖拽 → 模态力投影

替换当前 `apply_drag_velocity_bc` + influence_radius + KNN 链路：

1. 射线拾取命中粒子 i（CPU RayCaster 不变）；
2. 构造拖拽力 **f** ∈ ℝ³ⁿ：粒子 i 附近高斯核非零，方向 = (target − cur)/dt；
3. **投影**：$q_{drag} = \Phi^T f$（K 维）；
4. 加到 K 维 ODE 外力项。

物理性质：拖拽一个点主要激发低阶刚体平移模态，高频形变模态激发弱——**力传播自动正确**，无需手调 influence_radius 或 KNN 权重，从根上消解 `pin-batch-influence-fix` 的"单粒子拖拽只有光点抖动"问题。

---

## 5. 实施计划（分阶段）

### 阶段 0：离线轨迹采集器（前置，无实时风险）
- [ ] 在现有 MPM 管线上加"快照导出"模式：每子步落盘粒子位置（或稀疏间隔）
- [ ] 跑 50–200 次随机拖拽轨迹（拖拽点、方向、速度随机化）
- [ ] 产出：`traj_*.bin` 快照集，每帧 13356×3 float

验收：快照可重放回放，肉眼比对 MPM 行为正确（此阶段也顺带验证离线 MPM ground truth 可信）

### 阶段 1：模态基提取（离线 Python/C++ 工具）
- [ ] 快照去均值 + 刚体分量剥离（质心对齐 + 主轴对齐）
- [ ] PCA 提取前 K=30 形变模态，记录 λ_i
- [ ] （可选）训 autoencoder decoder 替代线性 PCA（非线性基）
- [ ] 解析提取 6 刚体模态
- [ ] 拟合阻尼 c_i、k_home
- [ ] 导出 `modal_basis.sbo`（Φ）、`modal_params.json`（λ, c, k_home）

验收：用快照重建误差 < 5%（保留前 K 维能量占比 > 95%）

### 阶段 2：实时模态物理 compute shader
- [ ] `project_forces.comp`：Φᵀ f，粒子级归约到 K 维
- [ ] `integrate_modal.comp`：K 维半隐式 Euler / RK4 + 刚体 home-spring
- [ ] `reconstruct_field.comp`：Φ q 重建 3n 位移
- [ ] 接入 Renderer：替换 B 段 MPM Substep() 调用为三个 dispatch + barrier

验收：无拖拽时花头静止稳定（无漂移、无自激振荡）；拖拽后松手**自动回弹到初始位姿**（home-spring 显式恢复）

### 阶段 3：耦合层适配
- [ ] C 段 `compute_particle_displacements` 输入源从 MPM 粒子缓冲改为模态重建的位移场
- [ ] （可选）神经形变场替代 KNN 映射，消除 `knn-farthest-not-nearest-bug` 类隐患

验收：前景高斯随模态位移正确变形，渲染无穿模/抖动

### 阶段 4：非线性增强
- [ ] modal warping 或 autoencoder decoder 上线
- [ ] 大形变场景回归测试（极限拖拽位置回弹正确）

验收：线性基 locking 现象消除，大旋转下重建误差受控

### 阶段 5：性能验收
- [ ] nsight graphics 帧分析：B 段 dispatch 数 1024 → ≤5
- [ ] 物理耗时占比从当前主开销降到 < 10% 帧预算
- [ ] FPS 基准对比（carnation 1,037,279 高斯）

验收：物理层实时开销可忽略，渲染重回主瓶颈

---

## 6. 代价与边界（诚实清单）

1. **模态基是物体特定的**：换 asset（carnation → rose）要重训。本项目单一 asset 反复交互，可接受。
2. **大形变线性基失真**：必须上非线性模态（阶段 4），否则极限位置拉不回、错位。
3. **自接触建模变难**：模态空间做自碰撞检测不如全粒子直观。carnation 若不涉及强自接触，可接受；若需要，加模态空间碰撞约束。
4. **塑形/断裂丢失**：模态是弹性降阶，MPM 的塑性记忆、断裂能力不再有。carnation 不需要，可接受。
5. **离线 MPM 必须可信**：模态基质量上限由离线 MPM 决定。之前修 MPM bug 的工作不白做——它保证离线 ground truth 正确。修 MPM 的目标从"实时跑对"转为"离线烘焙对"，容错更高、可慢跑可人工查验。

---

## 7. 与原版 PhysDreamer 的关系

原版 PhysDreamer 的"推理"是视频扩散模型生成运动，离线、非实时。本方案：

- **实时性**：模态物理保证 30fps 可交互，原版做不到；
- **物理因果**：模态基从 MPM 蒸馏，保持物理可解释，原版扩散是黑盒；
- **材料自适应**：λ_i、c_i 由数据辨识，不靠手调；
- **创新点**：首次将 MPM 物理知识蒸馏进低维模态基用于实时 3DGS 交互，把"全粒子前向仿真"升维为"降阶—数据驱动"架构，刚体恢复力从本构补丁转为坐标系内禀机制。

---

## 8. 关键文件预期改动

| 文件 | 改动 |
|---|---|
| `src/mpm/MPMManager.{h,cpp}` | 增加 snapshot 导出模式；实时路径下被模态物理替代（保留离线烘焙能力） |
| `src/coupling/DisplacementMapper.{h,cpp}` | 输入源切换为模态重建位移场 |
| `shaders/modal/project_forces.comp` | 新增 |
| `shaders/modal/integrate_modal.comp` | 新增 |
| `shaders/modal/reconstruct_field.comp` | 新增 |
| `tools/offline/modal_extract.py`（或 .cpp） | 新增：PCA / autoencoder 基提取 |
| `CMakeLists.txt` | 收录新 shader + 工具 |
| `PIPELINE_DIAGRAM.md` | B 段更新为模态物理 3 派发 |

---

## 9. 里程碑

| M | 内容 | 风险 | 复杂度 |
|---|---|---|---|
| M0 | 离线快照采集器 | 低 | 中 |
| M1 | 模态基提取 + 重建误差验收 | 中（PCA 调参） | 中 |
| M2 | 实时模态物理 shader + 接入 | 中（barrier/缓冲） | 复杂 |
| M3 | 耦合层适配 | 低 | 简单 |
| M4 | 非线性模态增强 | 高（autoencoder） | 复杂 |
| M5 | 性能验收 | 低 | 简单 |

建议先做 M0+M1（离线，零实时风险，且能顺带验证 MPM ground truth），再上 M2。

---

*基于 phys-sim 分支 2026-07-01 状态编写。实施前需先用 `/plan` 或 TodoWrite 细化到 shader 级伪代码。*
