# MPM 减子步 & 减 atomic 研究报告

> **生成方式**: deep-research workflow (103 agents / 4.5M tokens / 21 源 / 47 claims / 25 验证 / 18 确认 / 7 否决 / 8 综合结论)
> **生成日期**: 2026-07-02
> **场景背景**: Vulkan + 3DGS + MPM, carnation 场景, FCR/Neo-Hookean 弹性体, ~1M 粒子, RTX 4090, 当前 substeps=128, P2G 用 `VK_EXT_shader_atomic_float` atomicAdd

---

## 一、减子步：数值方法

### ✅ 确认成立的杠杆

#### 1. 隐式/半隐式时间积分（最核心，high confidence）

- **generalized-α 族**（统一 Newmark-β / HHT-α / WBZ，单参数 ρ∞）证明**无条件稳定、解除 CFL 约束**，且带可控高频数值耗散——正好压制那些逼你上 128 子步的虚假振荡。
- **Gao et al. 2018**（ACM TOG）的 GPU MPM solver 同时发布 explicit 和 **fully implicit** 两个变体，证明隐式 MPM 在 GPU 上可实现。
- **结论**：当前 substeps=128 是**显式格式的产物**，不是物理刚需。换积分器是减子步的正路，不是调旋钮。

#### 2. APIC 间接支持大 dt（high confidence）

- APIC 保角动量、比 PIC 耗散小、比 FLIP 稳定。本项目里已有 `apic_matrix`。
- 它**不能解除 CFL**，但能降低 transfer 重采样噪声——这些噪声本来会逼小子步。属于"间接杠杆"。

### ⚠️ 被否决/需警惕的方向

#### 3. XPBD（被部分否决）

- XPBD 通过 compliance α=1/stiffness 把刚度与 dt/迭代数解耦，**理论上**不需要子步就能达到刚度。这一条在 constraint-solver 范式内验证为真。
- **但被否决的关键点**：XPBD **不是 MPM 方法**，没有 PBD 风格的内迭代环。把它套到 FCR/Neo-Hookean carnation 上是**范式替换**（用约束求解替代力驱动 MPM），不是 drop-in。研究明确指出这条路风险高、未在 MPM 弹性体上验证。

#### 4. CFL 自适应子步 / 能量稳定格式

- 搜索角度覆盖了，但源（arXiv 1810.12894、Hu mls-mpm）**被判 unreliable/未提取到 claim**——没有可引用的强证据。能量稳定（variational/discrete-gradient）积分器理论上能保能量、减子步，但缺少 MPM 弹性体的实证落地。

### 🔑 关键 caveat（必须知道）

> 隐式积分的"无条件稳定"**只对线性系统严格证明**。对 FCR/Neo-Hookean 这种非线性材料，**实际 dt 上限是精度（cell-crossing 噪声、转移噪声、材料非线性），不是稳定性**。子步能大幅降，但不是无限降——这是经验工程问题，需要实验。

---

## 二、减 atomic：算法

### ✅ 确认成立

#### 1. atomicAdd 是 MLS-MPM 的内禀特性（high confidence）

- Taichi mpm99 原文 P2G 就是 `grid_v[base+offset] += weight*(...)` 的 scattered +=，GPU 上 lower 成 atomicAdd，**没有 coloring、没有共享内存归约**。当前的 `VK_EXT_shader_atomic_float` 路径与 canonical 一致。降 atomic 必须用非 canonical 算法。

#### 2. Vulkan subgroup 归约（最直接的 GPU-native 杠杆，high confidence）

- `subgroupAdd` / `subgroupInclusiveAdd`（Vulkan 1.1 core via VK_KHR_shader_subgroup）通过 warp-shuffle/ballot 硬件做 lockstep reduce/scan，**不走 atomicAdd**。
- **aggAtomic 模式**：P2G 里 target 同一 grid node 的 lane 先用 subgroup op 归约，再由单 lane 发一次 atomicAdd。
- subgroupSize：NVIDIA 32 / AMD 64。RTX 4090 是 32。

#### 3. block 级共享内存归约（high confidence）

- CUB 的 BlockReduce/BlockScan：先在 `shared` TempStorage 里本地累积，再单线程写一次 global。**模式可移植**，但 CUB 是 CUDA-only，Vulkan 要**手撸 GLSL 版**（shared array + barrier + subgroup op）。

### ❌ 被否决的关键点（重要！）

#### 4. 图着色（coloring）被否决 0-3 / 1-2

- "coloring 消 atomic 优于 atomicAdd" **没有被证实**。来自 arXiv 1804.06023 的三条相关 claim 全部被否决：
  - "coloring 消除 atomic" → 1-2 否决
  - "高密度场景 atomic 成瓶颈、coloring 更优" → 1-2 否决
  - "coloring 与 atomic 直接对比，atomic 常常持平或更快" → 0-3 否决（这条本身是反 color 的）
- **解读**：coloring 不是无条件赢家，**场景密度依赖**。carnation 这种 ~1M 粒子、cell 密度适中的场景，coloring 很可能不划算（要预排序 + 多 pass + load imbalance）。

#### 5. CUB device-wide 原语被否决 0-3

- "CUB 提供 sort/scan/reduction 无原子路径" 这条被否决，原因是 claim 措辞过强（CUB 本身存在，但"无原子 ready-made 路径 for per-cell 累积"未被证实）。Block 级原语是真的（见上条），device-wide 那条是过度引申。

### 🔑 关键 caveat

- subgroup 聚合的收益**条件性**：P2G 的 quadratic B-spline 要散到 8 个 node，同一 subgroup 里 lane 命中同 node 的概率需要 `subgroupPartition`/ballot 分组，复杂度上升。**不是免费的**。
- atomic-reduction 的 mechanics 确定，但 **MPM-specific 的加速幅度没有基准测试**。

---

## 三、carnation 场景落地建议

按 ROI 排序（场景：FCR 弹性体、~1M 粒子、RTX 4090、Vulkan/GLSL、substeps=128）：

### 🥇 优先级 1：subgroup-aggAtomic P2G（减 atomic，低风险）

- 用 `subgroupAdd` 把同 subgroup 内 target 同一 node 的贡献归约，单 lane 发 atomicAdd。
- **不改物理**，纯 P2G kernel 改写。RTX 4090 subgroup=32，carnation cell 密度下命中率应该可观。
- **落地难点**：quadratic B-spline 的 8-node scatter 需要 `subgroupPartitionNV` 或 ballot 分组同 node lane。先做一个 `subgroupBallot` 版本原型，Nsight 数 atomic 次数。
- **预期**：atomic 次数降到 1/N（N=同 node lane 数），但不一定线性提速（要看 contention 程度）。

### 🥈 优先级 2：隐式积分器（减子步，高风险高回报）

- 引入 generalized-α 或 Backward-Euler MPM。理论上 substeps 128 → 个位数。
- **真实成本**：非线性 FCR 隐式求解器要 Newton 迭代 + 线性 solve（网格规模不大，PCG/CG 可行），Vulkan 里要写新的 compute shader 链路。这是**大工程**。
- **建议路径**：先做**混合**——bulk 用显式 MLS-MPM，**只在被拖拽/高应力的花头区域**做隐式或 XPBD 风格约束 solve（见 open question 3）。这能局部降子步、控制工程量。
- **必须做实验**：carnation FCR 在隐式下精度允许的最大 dt 是多少？这是**经验值**，文献给不出。

### 🥉 优先级 3：block 级共享内存归约（减 atomic，中等工程量）

- 把 P2G 改成"particle 排序→block 处理连续 cell 切片→shared 累积→单次 global 写"。
- 需要先按 cell key radix sort 粒子（Taichi/稀疏网格的经典路子）。当前是 dense grid 64³，改 sort-then-scatter 要加排序管线。
- **注意**：research 表明 coloring 不一定赢 atomic，但 sort-then-scatter + shared 归约是另一条路，理论上更稳。

### ⛔ 不推荐

- **XPBD 替代 MPM**：范式替换，未验证，风险极高，放弃。
- **纯 coloring 消 atomic**：被否决，density 依赖，carnation 大概率不划算。
- **CUB device-wide 直接搬**：CUDA-only，Vulkan 要全手撸，且收益未证实。

---

## 四、4 个待回答的开放问题（研究给不出，需要实验）

1. carnation FCR 在隐式 generalized-α 下，精度允许的最大 dt 是多少？128 子步能降到多少才出可见形变/振荡误差？
2. GLSL subgroup-aggAtomic P2G 在 quadratic B-spline 8-node scatter 下能否干净实现？RTX 4090 ~1M 粒子下是否真的打赢当前 `VK_EXT_shader_atomic_float` 路径？
3. 混合方案可行性：bulk 显式 + 花头局部隐式/XPBD 约束，能否局部降子步？
4. 稀疏/block-sparse 网格 + 粒子排序本身能否把 atomic contention 降到 coloring 路径有竞争力的程度？

---

## 五、来源（primary 质量已标注）

| 来源 | 质量 | 用途 |
|------|------|------|
| [Gao 2018 — GPU MPM (implicit+explicit)](https://dl.acm.org/doi/10.1145/3272127.3275044) | primary | 隐式 GPU MPM 可行性 |
| [generalized-α MPM (arXiv 2001.09257)](https://arxiv.org/abs/2001.09257) | primary | 无条件稳定积分器 |
| [APIC (Jiang 2015)](https://dl.acm.org/doi/10.1145/2786789.2786796) | secondary | 间接降子步 |
| [MLS-MPM canonical (Taichi mpm99)](https://github.com/taichi-dev/taichi) | primary | atomicAdd 内禀性证据 |
| [XPBD (Macklin & Muller)](https://mmacklin.com/xpbd.pdf) / [doi](https://doi.org/10.1111/cgf.14019) | primary | 范式替代（被部分否决） |
| [Vulkan subgroup spec](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_shader_subgroup.html) | secondary | subgroup 归约杠杆 |
| [CUB](https://nvlabs.github.io/cub/) | primary | block 级归约模式 |
| [coloring (arXiv 1804.06023)](https://arxiv.org/abs/1804.06023) | primary | 被否决的反例 |

---

## 六、最终建议

**先做优先级 1（subgroup aggAtomic）**，它**不改物理、纯 shader 改写、可逐步验证**，且和已有的 Nsight profiling 工作流契合。隐式积分器是终局解但工程量大，建议先验证优先级 1 的收益上限，再决定是否上隐式。
