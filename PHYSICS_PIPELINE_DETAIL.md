# 3dgs.cpp.opt — 算法与管线详细文档

> Vulkan 3D 高斯溅射渲染器 + MPM 物理仿真集成 demo
> 分支：`phys-sim` ｜ C++20 / Vulkan 1.4 / MSVC v143

本文档详细介绍本 demo 的渲染管线、MPM 物理仿真、高斯-物理耦合、交互系统四大子系统的算法实现与数据流。文档面向已熟悉 3DGS 与 MPM 基础概念的读者，重点说明**工程实现细节**与**踩坑修复历程**。

---

## 1. 项目架构总览

```
src/
├── Renderer.{h,cpp}              # 主渲染器：调度渲染/物理/耦合/交互
├── GSScene.{h,cpp}              # 高斯场景管理（PLY 数据）
├── SceneLoader.{h,cpp}          # PLY 解析（含 stride 修复）
├── GUIManager.{h,cpp}           # ImGui 界面
├── vulkan/                       # Vulkan 封装层
│   ├── VulkanContext.{h,cpp}    # 设备/队列/命令池
│   ├── Buffer.{h,cpp}           # 缓冲区（含 download 回读）
│   ├── Swapchain.{h,cpp}        # 交换链
│   └── pipelines/ComputePipeline.{h,cpp}  # 计算管线 + DescriptorSet
├── mpm/                          # MPM 物理仿真
│   ├── MPMManager.{h,cpp}       # 仿真管理器（子步调度）
│   ├── MPMStructs.h             # ParticleData/GridNode 数据结构
│   ├── ParticleGenerator.{h,cpp}# 粒子生成（从 PLY）
│   └── shaders/mpm/             # MPM compute shaders
├── coupling/                     # 高斯-物理耦合
│   ├── GaussianParticleMapper.{h,cpp}  # KNN 高斯→粒子映射
│   └── DisplacementMapper.{h,cpp}      # GPU 位移计算
└── interaction/                  # 用户交互
    ├── RayCaster.{h,cpp}        # CPU 射线拾取
    └── DragHandler.{h,cpp}      # 拖拽处理（速度 BC + 位置反馈）
```

### 帧主循环（每帧）

```
1. 输入处理（P+左键拖拽事件）
2. 物理交互：射线拾取 → 位置回读 → DragHandler 计算 drag BC
3. MPM Step：substeps 个子步（ZeroGrid→HomeSpring→DragBC→P2G→GridUpdate→GridFreeze→G2P→PinFrozen）
4. 耦合：GPU 计算粒子位移 → 映射到高斯 → override 旋转/cov3D
5. 渲染：Preprocess(排序/cov3D) → Splat(高斯溅射) → GUI(ImGui)
6. 诊断：每 60 帧回读粒子缓冲，打印 max_disp/max_strain/moved
```

三缓冲（`FRAMES_IN_FLIGHT=3`）：每帧独立的 command buffer + fence，CPU/GPU 并行。

---

## 2. 数据结构

### 2.1 ParticleData（176 字节，std430）

MPM 粒子是物理仿真的最小单元。C++ 与 GLSL **必须**严格对齐，否则粒子错位→NaN/爆炸。

| 字段 | 偏移 | 类型 | 说明 |
|------|------|------|------|
| `position` | 0 | vec3 | 位置（归一化空间 [0,1]） |
| `mass` | 12 | float | 质量 = density × volume |
| `velocity` | 16 | vec3 | 速度（归一化空间/s） |
| `freeze_flag` | 28 | uint | 0=可动，1=冻结 |
| `deformation_gradient_cols[3]` | 32 | vec4[3] | 变形梯度 F（每列 vec4，.w=0 填充，48B） |
| `volume` | 80 | float | 粒子体积 |
| `material_id` | 84 | uint | 材料类型 |
| `youngs_modulus` | 88 | float | E [Pa] |
| `poisson_ratio` | 92 | float | ν |
| `density` | 96 | float | ρ [kg/m³] |
| `_apic_padding[3]` | 100 | float[3] | **12B 显式填充**（匹配 std430 mat3 的 16 字节对齐） |
| `apic_matrix_cols[3]` | 112 | vec4[3] | APIC 动量矩阵 C（48B） |
| `is_filled_point` | 160 | uint | 0=原始高斯，1=内部填充点 |
| `_final_padding[3]` | 164 | float[3] | 尾部填充 → **stride=176** |

**关键对齐坑**：GLSL std430 中 `mat3` 的列间 stride=16（非 12），导致 `apic_matrix` 必须 16 字节对齐→offset 104→112，数组 ArrayStride 从 168 上调到 **176**。C++ `glm::mat3` 默认 stride=12，故用 `vec4[3]` 手动存储每列，并显式填充 `_apic_padding[3]`（100→112）与 `_final_padding[3]`（164→176）。`static_assert(sizeof(ParticleData)==176)` 编译期保证。

### 2.2 GridNode（32 字节）

| 字段 | 偏移 | 类型 | 说明 |
|------|------|------|------|
| `velocity` | 0 | vec3 | 网格节点速度 |
| `mass` | 12 | float | 累积质量 |
| `force` | 16 | vec3 | 累积力 |
| `active_count` | 28 | uint | 活跃粒子计数 |

网格分辨率 `grid_size=64`（64³=262144 节点），网格缓冲 8MB。

### 2.3 坐标变换 CoordinateTransform

MPM 在归一化空间 [0,1] 仿真，渲染在世界空间。`CoordinateTransform` 提供 `ToNormalized`/`ToOriginal`/`scale`，由场景 AABB 算出。所有交互参数（dragCenter、dragRadius、dragVelocity）在归一化空间传递，避免 scale 混淆。

---

## 3. 渲染管线（3DGS Vulkan）

### 3.1 高斯溅射渲染流程

1. **Preprocess（Compute）**：对每个高斯计算协方差 cov3D、球谐（SH）展开颜色、排序键（深度）。**可变形高斯在此动态用 override 旋转 + 位移重算 cov3D**。
2. **Splat（Compute + Rasterizer）**：按排序键 tile 分组，alpha 混合溅射到屏幕。
3. **GUI（ImGui）**：叠加控制面板（FPS、Foreground Only、相机保存/加载）。

### 3.2 前景/背景分割

加载时 `SceneLoader` 自动推断三个 PLY：
- `point_cloud.ply` — 全部高斯
- `clean_object_points.ply` — 清洁物体（前景分割基准）
- `moving_part_points.ply` — 可变形部分

通过空间哈希网格将 `clean_object_points` 与完整点云匹配，得到**可变形区域掩码**（前景）。carnation 场景：32,703 前景高斯 / 1,037,279 总高斯。`Foreground Only` 按钮通过 visibility mask 切换渲染。

### 3.3 可变形高斯的 override 机制

物理位移后的高斯不能复用 PLY 静态旋转/cov3D。耦合系统为每个可变形高斯准备：
- `overrideRotationBuffer` — PLY 真实旋转（用于位移叠加基准）
- `overrideDisplacementBuffer` — 物理位移（来自 MPM 粒子）
- Preprocess shader 读取 override，动态计算 `new_rot = ply_rot`（旋转保持）+ `new_pos = orig_pos + displacement`，重算 cov3D。

---

## 4. MPM 物理仿真

### 4.1 默认配置

```cpp
grid_size   = 64;            // 64³ 网格
dt          = 1.0f / 30.0f;  // 帧时间步长
substeps    = 128;           // 子步数 → sub_dt = dt/128 ≈ 0.00026s
gravity     = {0, 0, 0};     // carnation 无重力（原版 carnation.py 同）
damping     = 1.0（拖拽中）/ 0.95^(1/substeps)（释放后）
E           = 2.14e6 Pa;     // 杨氏模量（carnation 实测）
nu          = 0.3;           // 泊松比
rho         = 2000 kg/m³;
home_spring k = 15.0;        // 位置弹簧刚度（见 4.8）
```

### 4.2 子步循环（每 substep 执行一次）

```
1.  ZeroGrid       — 清零网格节点 mass/velocity/force/active
1a. HomeSpring     — 位置弹簧 v += -k(x-x0)·dt        【新，见 4.8】
1b. DragBC         — 拖拽中：SET 半径内非冻结粒子速度   【仅 isDragging】
2.  P2G            — 粒子→网格（质量/动量/力 + APIC C + inline FCR 应力）
3.  GridUpdate     — 网格速度更新（重力 + 阻尼 + CFL 限幅）
4.  GridFreeze     — 冻结粒子所在网格节点速度归零
5.  G2P            — 网格→粒子（APIC 速度 + C 矩阵 + F 更新）
6.  PinFrozen      — 冻结粒子硬钉回初始位 + 速度清零
```

每个阶段后插 `VkMemoryBarrier`（SHADER_WRITE→READ）保证可见性。

### 4.3 P2G（Particle to Grid）

**三次 B 样条插值**（3×3×3=27 节点 stencil），对标 PhysDreamer quadratic B-spline：
```
w[0] = 0.5 * (1.5 - fx)²
w[1] = 0.75 - (fx - 1)²
w[2] = 0.5 * (fx - 0.5)²     // fx = 粒子网格坐标小数部
```

对每个粒子：
1. **inline 应力计算**（原独立 shader，合并避免覆盖 APIC C）：
   ```glsl
   mat3 stress = compute_stress(F, E, nu, material_id, yield_stress=10000);
   ```
2. 遍历 27 节点，APIC 传递：
   - `node.mass += w · p.mass`
   - `node.velocity += w · (p.mass·v + p.apic_C·dpos)` （APIC 动量，dpos=节点-粒子）
   - `node.force += -V · τ · ∇ₓw` （应力力，Kirchhoff τ 配世界梯度 ∇ₓw）

**APIC 一阶矩约束**：stencil 必须用 `base+i` + `dp = i - fx`（非 `base+(i-1)`），保证 Σw·dpos=0，否则 C 矩阵被反号放大→速度正反馈爆炸。

### 4.4 材料模型：FCR（Fixed Corotated）

**Kirchhoff 应力**：
```
τ = 2·μ·(F − R)·Fᵀ + λ·J·(J−1)·I
```
其中：
- `μ = E / (2(1+ν))`，`λ = E·ν / ((1+ν)(1−2ν))`
- `R = extract_rotation_polar(F)` — 极分解旋转
- `J = det(F)` — 体积比
- 体积项用 `I` 而非 `F⁻ᵀ`：det(F)→0 时不发散，消除压缩应力爆炸

**极分解 R**（Newton 迭代，对标 PhysDreamer SVD）：
```glsl
mat3 R = F;
for (int i=0; i<5; i++)
    R = 0.5 * (R + transpose(inverse(R)));   // R ← (R + R⁻ᵀ)/2
```
det(F)≈0 时回退 Gram-Schmidt 避免奇异。

**对称化**（安全网）：
```glsl
tau = 0.5 * (tau + transpose(tau));
```
非对称应力配 ∇w 会做非保守功→持续注入能量→速度不衰减。SVD R 下 (F−R)Fᵀ 本就对称，对称化是 no-op；近似 R 下强制能量守恒。

**客观性陷阱**：FCR 对纯旋转 F=R 给 τ=0（正确，客观材料）。这意味着**纯刚体旋转模态零应力零恢复力**——这是花头拖拽不回弹的根本原因（见第 8 节）。

### 4.5 GridUpdate

```glsl
node.velocity = (node.velocity + gravity·dt + node.force·dt/node.mass) · damping;
// CFL 限幅：|v|·dt <= 0.5·dx → max_v = 0.5/(inv_dx·dt)
node.velocity = clamp(|v|, max_velocity);
```
阻尼 `damping` 由 Renderer 运行时注入：拖拽中=1.0（无阻尼纯跟随），释放后=0.95^(1/substeps)/子步（衰减振荡）。

### 4.6 GridFreeze

遍历粒子，对 `freeze_flag!=0` 的粒子，零化其所在 27 节点的网格速度（软锚点）。对标 PhysDreamer `apply_grid_bc_w_freeze_pts`。

### 4.7 G2P + PinFrozen

**G2P**：APIC 回写
- `v_new = Σ w·node.velocity`（B 样条插值）
- `C_new = 4·Σ w·node.velocity·dposᵀ`（APIC 动量矩阵）
- `x_new = x + v_new·dt`
- `F_new = (I + dt·∇v)·F`，其中 `∇v = 4·Σ w·node.velocity·dposᵀ·inv_dx`
- NaN 归零保护

**PinFrozen**（G2P 后）：把冻结粒子硬钉回初始位 + 速度清零：
```glsl
if (freeze_flag != 0) {
    position = init_pos[idx].xyz;
    velocity = vec3(0.0);
}
```
G2P 会更新所有粒子位置（含冻结粒子的漂移），PinFrozen 修正之，形成刚性锚点。对标 PhysDreamer `gui_demo.py:313-318`。

### 4.8 位置 Home-Spring（刚体模态恢复力）【关键修复】

**动机**：FCR 客观材料对纯旋转零应力。花头被横向拖拽时绕花茎锚点**刚体旋转**（诊断 `ratio=max|F−R|/max|F−I|≈0.057`，F 94% 为旋转），无任何恢复力→卡死在旋转态。任何应力公式都修不了（客观性数学性质）。

**解法**：加弱位置弹簧，专治刚体模态（平移+旋转），FCR 仍管局部变形：
```glsl
// apply_home_spring.comp（每子步，ZeroGrid 后、DragBC 前）
vec3 disp = position - init_pos[idx].xyz;
velocity -= k_spring · disp · dt;     // v += -k(x-x0)·dt
```
- `k=15` → 周期 T=2π/√k≈1.6s
- 配合释放阻尼 0.95/帧 → 松手后 1-2 次振荡归位
- **放在 DragBC 前**：拖拽中 DragBC 的 SET 覆盖被抓粒子速度→弹簧不影响拖拽 batch；非抓取粒子保留弹簧冲量
- 冻结粒子跳过

弹簧作用在总位移（含变形），但 k 弱，FCR 主导快速弹性振荡，弹簧主导慢速刚体漂移，二者分工。

---

## 5. 高斯-物理耦合

### 5.1 GaussianParticleMapper（KNN 映射）

将每个可变形高斯映射到最近的 K 个 MPM 粒子，用**反距离加权**聚合位移：
```
disp_gaussian = Σ (1/d_i) · disp_particle_i  /  Σ (1/d_i)
```
KNN 用 std::less（最小堆，**不是** std::greater——曾因用反导致留 K 个最远粒子，仅 17% 高斯能动）。

### 5.2 DisplacementMapper（GPU 位移计算）

`compute_particle_displacements.spv`：每个高斯读其 K 个邻居粒子的 `(current_pos - init_pos)`，加权求和得高斯位移，写入 `overrideDisplacementBuffer`。53 workgroups / 13356 粒子。

### 5.3 渲染集成

Preprocess shader 读 override：
- `displacement = overrideDisplacement[gaussian_id]`
- `new_position = original_position + displacement`（世界空间）
- 旋转保持 PLY 原值（`overrideRotationBuffer`）
- 动态重算 cov3D

---

## 6. 交互系统

### 6.1 RayCaster（CPU 射线拾取）

P+左键按下时：
1. 屏幕坐标→NDC→世界射线
2. 射线转归一化空间
3. `GetParticlePositions()` 回读粒子缓冲（~1ms）
4. CPU 遍历粒子做 ray-sphere 相交，返回最近粒子 index + clickWorldPos + clickDepth

### 6.2 DragHandler（位置反馈速度 BC）

**对标 PhysDreamer `gui_demo.py:335-340`**：
```python
cur_pick = particle_x[grab_idx]              # 当前粒子位置
world_v  = (target - cur_pick) / frame_dt    # 位置反馈速度
```

C++ 实现（`ComputeDragPushConstants`）：
1. 鼠标像素增量 dx,dy → 世界增量（pixelToWorld 由 clickDepth/FOV/分辨率算）
2. `target_world = clickWorldPos + camRight·dx + (-camUp)·dy·pixelToWorld`
3. **位置反馈**（cur_pick_set_ 时）：
   - `dragCenter_world = current_pick_world_`（球心=当前粒子位置，跟随 batch）
   - `dragVel_world = (target - current_pick) / dt`（P 控制器，到位 v=0）
4. 转归一化空间，注入 MPMManager `SetDragVelocityBC`

**grab radius 自适应**：`grab_radius = AABB_diag · 0.02`（对标 PhysDreamer，~200 粒子局部抓取）。旧固定 0.2→抓 ~1700 粒子→整体刚体旋转→不回弹。

### 6.3 拖拽速度 Dirichlet BC（每子步 SET）

`apply_drag_velocity_bc.spv`（每子步，P2G 前）：半径内非冻结粒子 **SET** velocity = dragVelocity（CFL 限幅）。对标 PhysDreamer `enforce_particle_velocity_by_mask`。

**位置反馈的意义**：旧实现用鼠标速度（delta/dt），batch 被强制到鼠标速度，弹性抵抗时边界持续撕裂（strain 2.46）。位置反馈让 dragVel=(target−cur)/dt，batch 被拉向鼠标目标，到位 v=0，**变形被鼠标位移界住**，防撕裂。

### 6.4 CFL 限幅

`SetCFLParams(inv_dx, sub_dt, cfl=0.05)`：`max_velocity = cfl·dx/sub_dt`，使单子步位移 ≤ cfl·dx。防止拖拽注入速度过大→粒子射出网格→应力爆炸→永久冻结。

---

## 7. 关键 Bug 修复历程

本 demo 经历多次失败模式迭代，以下按时间顺序记录关键修复（详见 `memory/` 目录）：

| 症状 | 根因 | 修复 |
|------|------|------|
| MPM 完全冻结 | `VK_EXT_shader_atomic_float` 未启用→`atomicAdd(float)` 静默失败→grid mass=0 | 启用扩展 |
| 运行时 .spv 过期 | Renderer 用嵌入式 shaders.h，但 MPM shader 从文件系统加载 | CMake `copy_spv_to_source` 自动拷贝 |
| KNN 只 17% 高斯能动 | `std::greater`（最小堆）pop 弹最小→留 K 个最远粒子 | 改 `std::less` |
| PLY stride 错位 | `element face` 的 list property 误算成顶点属性 | 按 element 块只数 vertex property |
| ParticleData 160 vs 176 | mat3 16 对齐使 ArrayStride=176，C++ 按 160 上传→错位溢出 | vec4[3] + 显式填充，static_assert 176 |
| 应力非对称能量注入 | Gram-Schmidt R 近似→(F−R)Fᵀ 非对称→非保守功 | 对称化 + 极分解 R |
| 体积项 det→0 发散 | 用 F⁻ᵀ 配体积项 | 改用 I |
| G2P binding 颠倒 | G2P 与 P2G 共用 descriptor→binding 0/1 颠倒 | 专用 g2p_descriptor_ |
| APIC C 被覆盖 | 独立 compute_stress shader 写 apic_matrix | 应力合并到 P2G inline |
| B-spline kernel 不连续 | 旧 support[-2,2] 权重≠1 | 改 PhysDreamer quadratic B-spline |
| 重力+CFL 爆炸散点 | substeps=32+gravity 偏离原版 | gravity=0, substeps=128 |
| 拖拽炸花茎 | 冻结掩码反向+active/冻结交界应力爆炸+F 退化 | safe_normalize + F 投影 + CFL clamp + NaN 归零 |
| APIC 一阶矩爆炸 | stencil base+(i-1)→Σw·dpos≠0→C 反号放大 | stencil base+i + dp=i-fx |
| 拖拽不回弹（刚体旋转） | FCR 客观性→纯旋转零应力 | **位置 home-spring**（本文 §4.8）|
| 闪退于 BuildDescriptorSets | CreateGridBuffer 触发 BuildDescriptorSets 时 initial_pos_buffer_ 未建→空缓冲绑定 | 触发条件加 `&& initial_pos_buffer_` + CreateInitialPosBuffer 末尾补触发 |
| DescriptorPool reset 冻结 | end-of-init reset 废了 MPM persistent set | 删除 reset |

---

## 8. 核心设计决策：为什么需要 Home-Spring

这是本 demo 最深刻的一课，单独说明。

**现象**：carnation 花头拖拽释放后，max_disp 从 0.27 降到 0.14 后**平台停滞**，max_vel→0，max_strain 停在 0.92，**不回原位**。

**诊断**：诊断日志 `ROTvsSTRETCH: ratio = max|F−R| / max|F−I| ≈ 0.057`。ratio≈0 意味着 F≈R（F 94% 是旋转），即花头发生了**刚体旋转**（绕花茎锚点）。

**根因（数学性质，非 bug）**：FCR 是客观材料，应力 τ=2μ(F−R)Fᵀ 对纯旋转 F=R 恒等于零。这是客观性的必然结果——客观材料对刚体运动零应力、零能量、零恢复力。**任何应力公式（PK1、Kirchhoff、Neo-Hookean）都修不了**，因为它们都是客观的。

**为什么 PhysDreamer 不需要弹簧**：PhysDreamer 的冻结掩码钉住花头顶端壳 6.7% + 花茎，花头**无法刚体旋转**，只能局部变形→F 有拉伸分量→FCR 应力恢复。而我们的冻结粒子若全在花茎，花头自由旋转。

**三条修复路**（memory `rigid-rotation-no-restore-root-cause`）：
1. ✅ **位置 home-spring**（采用）— 弱弹簧拉回初始位，专治刚体模态
2. 非客观应力 — 破坏客观性让旋转产生应力，hacky 且能量不守恒
3. 改拖拽 — 已做（P0-P3），修了撕裂但修不了旋转（旋转是刚体模态，非力施加问题）

最终选择 home-spring：可控、不破坏客观性、FCR 仍正确处理变形。代价是弹簧作用在总位移（含变形），需弱 k 平衡。

---

## 9. 性能基准

| 场景 | 高斯数 | 粒子数 | FPS (RTX 4090) |
|------|--------|--------|----------------|
| carnations | 1,037,279 | 13,356 | ~105（物理关）/ ~60（物理开+128 substep） |

MPM 单帧 128 子步，每子步 6+ 阶段 compute dispatch，是主要开销。诊断每 60 帧回读 2.3MB 粒子缓冲（~1ms）。

---

## 10. 构建与运行

```bash
# CMake 配置（沙箱挡 cmake，实际用 MSBuild 编 vcxproj）
cmake -G "Visual Studio 17 2022" -A x64 -T v143 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="C:/VulkanSDK/1.4.350.0" \
      -S ./ -B ./build_msvc143

# 编译（MSBuild，dangerouslyDisableSandbox）
MSBuild build_msvc143/apps/viewer/3dgs_viewer.vcxproj -p:Configuration=Release -p:Platform=x64

# 重编 MPM/interaction shader（改 .comp 后，无需重链 exe）
C:/VulkanSDK/1.4.350.0/Bin/glslangValidator.exe -V -Isrc/shaders/mpm \
    -o shaders/<name>.spv src/shaders/mpm/<name>.comp

# 运行
./build_msvc143/apps/viewer/Release/3dgs_viewer.exe --camera camera.txt <scene.ply>
```

**注意**：
- MPM/interaction shader 从文件系统 `shaders/*.spv` 加载（非嵌入），改 .comp 需 glslangValidator 重编 .spv + 重启 viewer
- C++ 改动需 MSBuild 重链 exe
- `fmt::format` 不可直接用，用 `ostringstream` / `spdlog::fmt` 格式化

---

## 11. 参考与对标

本 demo 的 MPM 实现对标 PhysDreamer（`mpm_utils.py` / `mpm_solver_diff.py` / `local_utils.py` / `gui_demo.py`），关键对应：

| 本 demo | PhysDreamer | 说明 |
|---------|-------------|------|
| P2G inline 应力 | `p2g_apic_with_stress` | 应力合并避免覆盖 C |
| `compute_fcr_stress` | `kirchoff_stress_FCR` | τ=2μ(F−R)Fᵀ+λJ(J−1)I |
| `extract_rotation_polar` | SVD R=U·Vᵀ | Newton 迭代等价 |
| `grid_freeze.comp` | `apply_grid_bc_w_freeze_pts` | 网格级冻结 |
| `pin_frozen_particles.comp` | `gui_demo.py:313-318` | 粒子级硬钉 |
| `apply_drag_velocity_bc.comp` | `enforce_particle_velocity_by_mask` | 每子步 SET |
| 位置反馈 dragVel | `gui_demo.py:340` | grab_v=(target−cur)/dt |
| grab_radius=AABB·0.02 | `gui_demo.py:186` | 2% 局部抓取 |
| 释放阻尼 0.95/帧 | `gui_demo.py:288` | release_damping |
| **home-spring** | （无） | 本 demo 独有，补偿刚体模态 |

**与 PhysDreamer 的根本差异**：PhysDreamer 靠冻结掩码几何布局阻止刚体旋转，纯 FCR 即可回位；本 demo 冻结布局不同（花头可自由旋转），故加 home-spring 补偿刚体模态恢复力。

---

*文档基于 phys-sim 分支 2026-06-30 状态。算法细节随开发推进更新。*
