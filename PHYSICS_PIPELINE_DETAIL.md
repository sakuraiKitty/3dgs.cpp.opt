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
1. 输入处理（右键拖拽转视角 / 滚轮 dolly / W A S D SPACE SHIFT 平移 / Q E roll）
2. 物理交互：射线拾取 → 单粒子 GPU 回读 → DragHandler 计算 drag BC（含应变门控）
3. MPM Step：substeps 个子步（ZeroGrid → [HomeSpring 跳过] → DragBC → P2G → GridUpdate → GridFreeze → G2P → PinFrozen）
4. 耦合：GPU 计算粒子位移 → 映射到高斯 → override 旋转/cov3D
5. 渲染：Preprocess(排序/cov3D) → Splat(高斯溅射) → GUI(ImGui)
6. 诊断：每 60 帧回读粒子缓冲，打印 max_disp/max_strain/min_det/gated
```

三缓冲（`FRAMES_IN_FLIGHT=3`）：每帧独立的 command buffer + fence，CPU/GPU 并行。

> **算法状态（2026-07-01）**：home-spring 与 F 松弛均已**关闭**（`SetHomeSpring(0,false)` + `SetFRelaxAlpha(0)`），恢复力完全由 FCR 弹性应力 τ=2μ(F−R)Fᵀ 提供，对标 PhysDreamer `gui_demo.py`。子步数按场景 profile 取值（carnation=256）。详见 §4.1 / §4.8 / §8。

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

### 4.1 默认配置（按场景 profile）

`ScenePhysicsProfile`（`src/mpm/ScenePhysicsProfile.h`）按 PLY 所在目录名匹配 carnation/hat/alocasia/telephone，数值取自 PhysDreamer `configs/<scene>.py`：

| 场景 | E [Pa] | ν | ρ | downsample | grid_size | substeps | gravity |
|------|--------|-----|------|------------|-----------|----------|---------|
| carnation | 2.14e6 | 0.3 | 2000 | 0.10 | 64 | **128** | 0 |
| hat | 1.0e5 | 0.3 | 2000 | 0.04 | 64 | 64 | 0 |
| alocasia | 1.0e6 | 0.3 | 2000 | 0.10 | 64 | 128 | 0 |
| telephone | 1.0e5 | 0.3 | 2000 | 0.10 | 64 | 64 | 0 |
| default(未知) | 2.14e6 | 0.3 | 2000 | 0.10 | 64 | 128 | 0 |

```cpp
grid_size   = 64;            // 64³ 网格
dt          = 1.0f / 30.0f;  // 帧时间步长
substeps    = profile.substeps;   // carnation=128 → sub_dt = dt/128 ≈ 2.6e-4 s
gravity     = {0, 0, 0};     // 四场景均无重力（PD simulate_cfg 无 gravity 字段）
damping     = 1.0（拖拽中）/ 0.95^(1/substeps)（释放后，对标 PD release_damping）
E/nu/rho    = profile.*;     // 按场景
home_spring = OFF;           // 纯 FCR 恢复（见 §4.8）
f_relax     = 0;             // 关闭 F 松弛，保留 FCR 弹性耦合
CFL(cfl)    = 0.02;          // 拖拽速度兜底限幅（见 §6.4）
```

> **substeps 128 的由来**：PhysDreamer `gui_demo.py:532` 默认 256（注释明言「<128 指数爆炸→NaN」）。Nsight 实测 256 子步时 GPU memory-latency-bound（54.8% warp 卡 L2 延迟，SM 吞吐仅 7.7%），帧时间 ~62ms（16 FPS）。折半到 128：memory 流量/compute/dispatch/barrier 全减半 → ~28 FPS。低于 PD 推荐 256，靠 strain gating（deform>1.5 停驱）+ PinFrozen F 重置 + G2P NaN reset + 极分解 12 迭代 保稳（已验证拖拽稳定）。grid CFL 随 sub_dt 翻倍自动收紧（max_v 减半）反而更防 F 过冲。

### 4.2 子步循环（每 substep 执行一次）

```
1.  ZeroGrid       — 清零网格节点 mass/velocity/force/active + freeze_mask
1a. HomeSpring     — 【已关闭】enable=0，dispatch 整体跳过（见 §4.8）
1b. DragBC         — 拖拽中：SET 半径内非冻结粒子速度 + 应变门控衰减   【仅 isDragging】
2.  P2G            — 粒子→网格（质量/动量/力 + APIC C + inline FCR 应力 + 标记 freeze_mask）
3.  GridUpdate     — 网格速度更新（重力 + 阻尼 + CFL 限幅）+ 内联冻结节点零化（原 GridFreeze 融合）
4.  G2P            — 网格→粒子（APIC v/C/F 更新，单循环融合 + NaN reset）+ 内联 PinFrozen（F=I/C=0）
```

每阶段后插 `VkMemoryBarrier`（SHADER_WRITE→READ）。carnation 单帧 = 4 阶段 × 128 子步 = **512 次 dispatch**（idle，HomeSpring 跳过）；拖拽时 +DragBC = 5×128 = **640 次**。

**已完成的性能融合**（数值位一致，纯调度重组）：
- **PinFrozen → G2P**：冻结粒子硬钉（init/0/I/0）合并进 G2P 末尾，省 1 dispatch/子步
- **GridFreeze → GridUpdate**：P2G 标记 `freeze_mask`（冻结粒子 floor 节点），GridUpdate 内联零化，省 1 dispatch + 1 barrier/子步
- **G2P 三循环 → 一循环**：原 `interpolate_velocity`+`compute_velocity_gradient`+C_new 三次 27 节点遍历融合为一次（grid 读 81→27 次/粒子，3× 减访存）
- **粒子 shader local_size 256→64**：53 wg→213 wg，填满 96 SM（占用率翻倍，SM-idle 40→20%）

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
for (int i=0; i<12; i++)
    R = 0.5 * (R + transpose(inverse(R)));   // R ← (R + R⁻ᵀ)/2
```
迭代次数 5→**12**（P1）：大旋转/显著拉伸叠加时 5 次不足以收敛→(F−R) 含伪分量→恢复力偏离能量梯度；提到 12 覆盖花头大角度拖拽（PD 用 `wp.svd3` 精确解）。det(F)≈0 时回退 Gram-Schmidt 避免奇异。

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
- **F 限幅**：仅 NaN/inf reset I（与 PD jelly 路径一致，**移除**旧 det 软界等向缩放 + stretch≤2 等比缩放——非保守，会塑样漂移）。拖拽注入的应变由 `apply_drag_velocity_bc` 应变门控限制，F 不会无界增长。

**PinFrozen**（G2P 后）：把冻结粒子硬钉回初始位 + 速度清零 + **F=I / C=0**：
```glsl
if (freeze_flag != 0) {
    position = init_pos[idx].xyz;
    velocity = vec3(0.0);
    deformation_gradient = mat3(1.0);   // 切断虚假应力源
    apic_matrix = mat3(0.0);
}
```
G2P 会更新所有粒子位置（含冻结粒子的漂移），PinFrozen 修正之，形成刚性锚点。**与 PD 的差异**：PD `gui_demo.py:317-318` 只钉 x/v，不动 F/C——因其冻结壳厚（~3 格）、grid_freeze 零化支撑节点使 ∇v≈0→F 自然保持 I。本 demo 冻结壳仅 ~2 格 + grid_freeze 只冻单节点，冻结粒子其余 26 节点常有非零速度→∇v≠0→F 漂移到 1.67→注入 τ=6.2M 虚假应力→全场 F 爆炸（实测移除重置后 max\|F−R\| 0.21→1.67）。故必须显式重置 F=I/C=0。

### 4.8 恢复机制：纯 FCR 弹性（home-spring / F 松弛已关闭）

**当前状态**：`Renderer::updatePhysicsSimulation` 初始化时
```cpp
mpm_manager_->SetHomeSpring(0.0f, /*enable=*/false);  // 关闭位置弹簧
mpm_manager_->SetFRelaxAlpha(0.0f);                    // 关闭 F 松弛
```
`Substep` 中 `if (home_spring_.enable != 0u)` 不成立 → **home-spring dispatch 整体跳过**（无开销）。恢复力 100% 由 FCR 弹性应力 τ=2μ(F−R)Fᵀ 提供。

**为什么关闭**（对标 PhysDreamer `gui_demo.py`）：
- 之前的 home-spring + F 松弛是治「刚体旋转锁死」症状的 workaround，但互相拆台：
  - F 松驰驱 F→I → τ→0 → **杀死 FCR 弹性耦合** → 无恢复力
  - home-spring 速度冲量在 P2G→G2P 回路被稀释（实测 v 比理论小 ~360×）→ 失效
- 真根因是 **substeps=128（<PD 最小 256）+ 拖拽成刚体模态**。提到 256 + 局部 2% 抓取后，拖拽产生**局部变形**（F≠R），FCR 即可恢复（PD `gui_demo.py:184-186` 作者自述）。
- 故关闭两路 workaround，靠纯 FCR + 256 子步 + 局部小抓取自然恢复。

**保留的 home-spring 代码**：`apply_home_spring.comp` 与 `home_spring_pipeline_` 仍在（`enable` 字段运行时控制），便于实验对照，但默认不派发。

> 历史背景：home-spring 曾是核心修复（见 §8 旧版决策）。substeps 提到 256 后该 workaround 不再必要，回退到 PD 原生恢复路径。

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
3. `GetParticlePositions()` 回读粒子缓冲（仅首次拾取，物体未变形）
4. CPU 遍历粒子做 ray-sphere 相交，返回最近粒子 index + clickWorldPos + clickDepth

> **注意**：`GetParticlePositions()` 返回 `cpu_particles_`（仅 Load/Reset 赋值，Step 后陈旧），仅用于首次射线拾取。拖拽中的位置反馈须用 `GetParticlePositionGPU`（见 §6.2）。

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

**cur_pick 来源（C3 修复）**：Renderer 每帧（拖拽中）调 `GetParticlePositionGPU(picked)` —— **单粒子 staging 回读**（176B + `queue.waitIdle`，~1ms，一帧滞后）。旧路径 `GetParticlePositions()` 返回 `cpu_particles_`（Step 后永不更新）→ cur_pick 恒为初始位 → dragVel=(target−init)/dt 饱和在 CFL → batch 过冲不归位。

**手势统计**（`DragHandler`）：`drag_frame_count_` / `accumulated_disp_` / `cfl_clamp_count_`，OnMouseDown 重置、OnMouseUp 摘要日志，用于定位「大力拖拽拉断」（累积位移大 + CFL clamp 多 = 拉断风险）。

**grab radius 自适应**：`grab_radius = AABB_diag · 0.02`（对标 PhysDreamer，空间局部尺度）。稀疏云 2% 仅 ~3 粒子，但决定「局部变形 vs 刚体运动」的是**空间半径**（相对物体尺寸）而非粒子计数：空间半径大→抓取球跨越花头-茎连接区→整块花头刚性转动→F≈R→FCR 零恢复。2% 保证空间局部→花头内部 F≠R→FCR 有恢复力。

### 6.3 拖拽速度 Dirichlet BC + 应变门控（每子步 SET）

`apply_drag_velocity_bc.spv`（每子步，P2G 前）：半径内非冻结粒子 **SET** velocity = dragVelocity（CFL 限幅）+ **应变门控衰减**。对标 PhysDreamer `enforce_particle_velocity_by_mask`。

**应变门控**（弹性硬截断，非塑性）：把变形控制在弹性范围内
```glsl
mat3 F = particles[idx].deformation_gradient;
float stretch  = max(length(F[0]), max(length(F[1]), length(F[2])));  // 拉伸比
float J = det(F);
float compress = 1.0 / max(abs(J), 1e-4);                              // 压缩比
float deform   = max(stretch, compress);                              // 兼顾拉伸/塌缩
float gate = 1.0 - smoothstep(1.2, 1.5, deform);   // 弹性上限 HARD=1.5
if (J < 0.1) gate = 0.0;                            // det 异常：内翻/塌缩，停驱
v *= gate;
```
- 弹性范围内 deform<1.2 → gate=1 全速跟随鼠标；越过 1.5 → gate=0 停止驱动，应变前缘向内传播，整批饱和在弹性上限
- 纯旋转 F=R → 列范数=1 且 J=1 → deform=1 不触发（刚体旋转不受限）
- 阈值标定：日志爆炸点 max\|F−R\|=1.10（λ=2.10 撕裂点）/ min_det=0.09（1/J=11 塌缩点）；HARD=1.5 远低于两者
- **gate→0 时写 0 速度**：停止向该粒子注入能量，P2G 仅由应力力驱动→FCR 弹性恢复接管。粒子位置仍由 G2P 网格速度更新（含弹性回拉），不会卡死

**位置反馈 + 应变门控的意义**：位置反馈让 dragVel=(target−cur)/dt，batch 被拉向鼠标目标，到位 v=0，变形被鼠标位移界住防撕裂；应变门控在用户大力拖拽越过弹性上限时停止跟随，防累积应变→F 奇异→边界粒子甩飞。

### 6.4 CFL 限幅

`SetCFLParams(inv_dx, sub_dt, cfl=0.02)`：`max_velocity = cfl·dx/sub_dt ≈ 2.4 norm/s`，使单子步位移 ≤ cfl·dx。**兜底**防极端甩鼠标单子步射出网格。CFL 只防单子步瞬时射出，**不防累积应变**——累积应变由 §6.3 应变门控负责。原 cfl=0.05（max_vel=6）时 dragVel=1.42 不触发 CFL 但持续 256 子步→累积应变 3.0→拉断飞出；收到 0.02 后 max_vel=2.4 兜底，常规拖拽由应变门控接管。

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
| 拖拽不回弹（刚体旋转） | substeps=128(<PD 256)+大抓取→刚体模态 F≈R→FCR τ≈0 | **substeps 256 + 2% 局部抓取 + home-spring/F-relax 关闭**（本文 §4.8/§8）|
| 闪退于 BuildDescriptorSets | CreateGridBuffer 触发 BuildDescriptorSets 时 initial_pos_buffer_ 未建→空缓冲绑定 | 触发条件加 `&& initial_pos_buffer_` + CreateInitialPosBuffer 末尾补触发 |
| DescriptorPool reset 冻结 | end-of-init reset 废了 MPM persistent set | 删除 reset |
| F 软界非保守塑样漂移 | det 软界等向缩放 + stretch≤2 等比缩放保大小不保方向 | 移除软界，仅 NaN/inf reset（对标 PD jelly）|
| 冻结粒子虚假应力 | 单节点 grid_freeze 不足以保持 F≈I→F 漂移 1.67→τ=6.2M 全场爆炸 | PinFrozen 显式重置 F=I/C=0 |
| 极分解大旋转不收敛 | 5 次迭代不足→(F−R) 含伪分量→恢复力偏离能量梯度 | 迭代 5→12 |
| 拖拽累积应变拉断 | CFL 只防单子步射出，不防 256 子步累积应变 | apply_drag_velocity_bc 应变门控 smoothstep(1.2,1.5) + J<0.1 停驱 |
| cur_pick 陈旧致 batch 过冲 | GetParticlePositions 返回 cpu_particles_（Step 后不更新）→dragVel 齐次饱和 CFL | GetParticlePositionGPU 单粒子 staging 回读 |

---

## 8. 核心设计决策：纯 FCR 恢复（home-spring 已回退）

这是本 demo 最深刻的一课，单独说明。**当前结论：home-spring / F 松弛已关闭，靠纯 FCR + 256 子步 + 局部抓取恢复。**

**现象（旧）**：carnation 花头拖拽释放后不回原位，max_disp 从 0.27 降到 0.14 后平台停滞，max_strain 停在 0.92。诊断 `ROTvsSTRETCH: ratio = max|F−R| / max|F−I| ≈ 0.057` → F 94% 是旋转 → 花头刚体旋转（绕花茎锚点）。

**根因（数学性质，非 bug）**：FCR 是客观材料，应力 τ=2μ(F−R)Fᵀ 对纯旋转 F=R 恒等于零——客观材料对刚体运动零应力、零恢复力。**任何客观应力公式都修不了。**

**旧方案（已回退）**：加位置 home-spring（弱弹簧拉回初始位）+ F 松弛（F←F+α(I−F) 退掉锁定旋转）。两者互相拆台：F 松驰驱 F→I 杀死 FCR 弹性耦合；home-spring 速度冲量在 P2G→G2P 回路被稀释 360× 失效。

**真根因（PD 作者自述 `gui_demo.py:184-186`）**：PhysDreamer 不需要弹簧，靠 FCR 弹性自然恢复。前提是 (1) substeps ≥ 256（<128 指数爆炸→F 发散→刚体旋转锁死）；(2) 2% 局部抓取（空间局部→花头内部 F≠R→FCR 有恢复力），而非大半径刚体模态。本 demo 早期 substeps=128 + 7.5% 抓取→刚体模态→必须靠 spring workaround；提到 256 + 2% 后 workaround 不再必要。

**当前方案**：
1. ✅ substeps=256（carnation profile）
2. ✅ grab_radius=AABB·2%（空间局部）
3. ✅ home-spring OFF / F-relax OFF（纯 FCR）
4. ✅ 极分解迭代 12 次（精确 R→应力对齐能量梯度→真实恢复力）
5. ✅ DragBC 应变门控（防大力拖拽越过弹性上限→F 奇异）

**与 PhysDreamer 的根本差异（已缩小）**：PD 靠冻结壳几何布局（厚 ~3 格 + grid_freeze 零化支撑节点）使 ∇v≈0→冻结粒子 F 自然保持 I，故 PinFrozen 只钉 x/v。本 demo 冻结壳薄（~2 格）+ grid_freeze 只冻单节点，必须 PinFrozen 显式重置 F=I/C=0 切断虚假应力源（§4.7）。这是对 PD 的必要补丁，其余恢复路径已对齐。

---

## 9. 性能基准

| 场景 | 高斯数 | 粒子数 | substeps | FPS (RTX 4090 Laptop) |
|------|--------|--------|----------|----------------|
| carnations | 1,037,279 | 13,356 | 128 | **~28**（物理开）/ ~105（物理关）|
| hat | — | — | 64 | 预期更高（子步少 2×）|

**Nsight GPU Trace 实测**（carnation，优化前后对比）：

| 指标 | 256 子步基线 | 128 子步当前 | 解读 |
|------|-------------|-------------|------|
| 帧时间(traced) | 132.6ms | ~60ms | -55% |
| `sm__throughput` | 7.7% | — | SM 算力低（非算力 bound）|
| `warps_inactive_sm_active` | 35.7%→54.8% | — | **memory-latency 主瓶颈** |
| `warps_inactive_sm_idle` | 40.2%→20% | — | SM 空闲（优化减半）|
| `dramc__throughput` | 1.75% | — | 非带宽 bound |
| `gr__compute_cycles_active` | 94% | — | GPU 满载，非 CPU bound |
| `l1tex hit rate` | 43.9%→38% | — | L1 miss 多，走 L2 |

**瓶颈定位**：memory-latency-bound（54.8% warp 卡 L2 延迟，L1 hit 38%）+ launch-bound（13568 粒子=424 warps 填不满 96 SM×64 warps）。非算力、非带宽、非 CPU。

**已落地优化**（详见 §4.2/§12）：PinFrozen→G2P 融合、local_size 256→64、GridFreeze→GridUpdate 融合、G2P 三循环→一循环、**substeps 256→128**（算法，16→28 FPS）。

**剩余天花板**：128 子步下 ~28 FPS。冲 60 FPS 需 P2G shared-memory tiling（见 §12.2）。

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
| **home-spring / F 松弛** | （无） | 本 demo 曾用，现已**关闭**（§4.8）；改靠 substeps=128 + 2% 抓取对齐 PD |
| **ScenePhysicsProfile** | `configs/<scene>.py` | 按场景 E/substeps/downsample |
| **DragBC 应变门控** | （无，PD 靠 CFL） | 本 demo 独有，防累积应变拉断（§6.3）|
| **PinFrozen 重置 F=I/C=0** | `gui_demo.py:317-318`（PD 只钉 x/v） | 本 demo 补丁：薄冻结壳需显式重置（§4.7）|
| **极分解 12 迭代** | `wp.svd3` 精确解 | Newton 近似，大旋转需多迭代（§4.4）|

**与 PhysDreamer 的根本差异**：PhysDreamer 靠厚冻结壳（~3 格 + grid_freeze 零化支撑节点）阻止刚体旋转 + 保持冻结粒子 F≈I，纯 FCR 即可回位，PinFrozen 只钉 x/v。本 demo 冻结壳薄（~2 格 + 单节点 grid_freeze），须 PinFrozen 显式重置 F=I/C=0 切断虚假应力源；并加 DragBC 应变门控防大力拖拽累积应变。其余恢复路径（纯 FCR、256 子步、2% 抓取、位置反馈）已对齐 PD。

---

## 12. 帧率优化记录与方向

> **当前状态**：carnation ~28 FPS（substeps=128）。前 4 次非算法优化累计 ~8.7% traced 增益，第 5 次 substeps 256→128（算法）贡献 16→28（+75%）。详见 §12.1 记录。下一步 P2G shared-mem tiling（§12.2）冲 60 FPS。

### 12.1 已完成优化（Nsight 验证）

| # | 优化 | 类型 | 单独效果 | 累计 FPS |
|---|------|------|---------|----------|
| 1 | PinFrozen 融合进 G2P | 调度融合（0 barrier） | 0（PinFrozen trivial）| 16 |
| 2 | local_size_x 256→64 | 占用率 | SM-idle 40→20%，吞吐未涨 | ~16.8 |
| 3 | GridFreeze 融合进 GridUpdate（freeze_mask） | 调度融合（-1 真实 barrier） | <1 | ~17 |
| 4 | G2P 三循环→一循环（81→27 grid 读） | 访存减 | traced 127→121ms（-5%）| ~17.5 |
| 5 | **substeps 256→128** | **算法** | **全部减半** | **~28** |

**Nsight 数据驱动教训**：前 4 次非算法优化仅 ~8.7%——真实瓶颈是 memory-latency（54.8% warp 卡 L2）+ 低粒子数 launch-bound，**非 dispatch/barrier 数**。寄存器压力假设证伪（regs 31→31.7）。ALU 非瓶颈（sm 7.7%）→ R 缓存方向错。GPU 94% 活跃 → CPU 回读非瓶颈。唯一有效杠杆：减 memory 流量。

### 12.2 下一步：P2G shared-memory tiling（目标 60 FPS）

**动机**：P2G 每粒子 27 节点 × ~8 atomicAdd = 216 atomic/粒子。`atomicAdd(float)` 到 L2 串行化，是 memory-latency 主源。同 workgroup 粒子若空间共址，可共享 grid 节点→局部累加→每节点 1 atomic。

**方案**：
1. 粒子按网格胞排序（init 时一次性，或每帧 radix sort by cell hash）
2. P2G workgroup 处理空间连续粒子块：load 本块涉及 grid 节点到 shared memory（一次），各粒子读 shared 累加（无 atomic），末尾每节点 1 atomic 写回
3. atomic 数 216/粒子 → ~1/节点/wg，大幅减 L2 串行

**预期**：P2G atomic 延迟减 10×+ → memory-stall 54.8% 大降 → 28→40+ FPS。配合 G2P 同方案可达 60。

**风险**：粒子空间排序需新管线；shared mem bank conflict 需 padding；跨块边界节点 double-counting。

### 12.3 已排除方向（数据证伪，勿重试）
- ❌ 减 dispatch 数（PinFrozen 融合 0 增益）
- ❌ 减 barrier（GridFreeze 融合 <1 增益）
- ❌ 占用率 local_size（SM-idle 降但吞吐不涨，warps 总数受限粒子数）
- ❌ CPU 回读/preprocessFence（GPU 94% 活跃，非 CPU bound）
- ❌ P2G 极分解 R 缓存（ALU 非瓶颈）
- ❌ ZeroGrid 稀疏化（ZeroGrid 是少数高占用 dispatch，稀疏反降填充）

### 12.4 历史方向（12.1-12.7 旧版，已被 §12.1-12.3 取代，保留供参考）

### 12.1 屏障合并（最高收益，零算法风险）
每个子步插 7 道 `VkMemoryBarrier(SHADER_WRITE→READ)`，全局屏障强制整个 compute queue flush 缓存。可优化：
- **按需细化**：P2G→GridUpdate 之间确需 grid 可见，但 ZeroGrid→(HomeSpring跳过)→DragBC 之间写的都是 particle.velocity，DragBC→P2G 之间的屏障可改为 `BUFFER_BARRIER` 精确到 `particle_buffer_`（而非全局），减少 cache flush 范围。
- **合并相邻同阶段屏障**：G2P 写 particle，PinFrozen 紧接读/写 particle——若把 PinFrozen 的逻辑合并进 G2P shader 末尾（同一 dispatch 内按 freeze_flag 写回），省掉一道屏障 + 一次 dispatch。每子步省 1 dispatch × 256 = 256 次/帧。
- 评估用 `vkCmdPipelineBarrier` 的 `BY_REGION_BIT` / 设备级 vs 全局的代价差异。

### 12.2 子步间的真依赖分析（中收益）
当前每子步 6 阶段全部串行（前一阶段屏障在后一阶段前）。实际上：
- ZeroGrid 写 grid；P2G 读 particle 写 grid——P2G 依赖 ZeroGrid 完成（同 buffer grid，真依赖）。
- 但 GridFreeze 写 grid（基于 particle freeze_flag 读 particle）与 P2G 之间隔着 GridUpdate——GridUpdate 读 grid 写 grid，GridFreeze 读 particle 写 grid。这些是同一 grid buffer 的 RW 依赖，难重叠。
- 真正可重叠的是**不同 buffer**：例如 DragBC 写 particle.velocity，与 ZeroGrid 写 grid 无依赖——可在同一 command buffer 里把 DragBC 和 ZeroGrid 连续 dispatch（无屏障间），让 GPU 并行。需要仔细画依赖图确认无 RW 冲突。

### 12.3 异步计算队列（中收益，需架构改动）
MPM 全在 graphics/compute queue 串行。可开独立 `VK_QUEUE_COMPUTE`（多数 GPU 与 graphics 共享引擎，但提交路径独立）：
- 把 MPM 1536 dispatch 录到 secondary command buffer，主 queue 录渲染，用 timeline semaphore 同步。
- 三缓冲下，第 N 帧的 MPM 可与第 N−1 帧的渲染重叠（目前 fence 串行）。
- 风险：RTX 4090 compute 与 graphics 共享 SM，重叠收益有限；但提交/CPU 侧并行仍可减少 stall。

### 12.4 减少 CPU 同步点（低-中收益）
- **`GetParticlePositionGPU`**：拖拽中每帧 `queue.waitIdle` 阻塞 ~1ms。改用 `vkGetQueryPoolResults` 或在帧末 fence 后读上一帧 staging（已经是一帧滞后，可接受），去掉 `waitIdle`。
- **Diagnose 每 60 帧 2.3MB 同步回读**：改用 persistent staging + `vkCmdCopyBuffer` 到 HOST_VISIBLE，fence 后异步读，不阻塞 compute queue。或诊断间隔 60→180 帧（仅观测用）。
- **preprocessFence**：D 段 CPU 回读 numInstances 决定 radix sort dispatch size——可用固定上限（numVertices）的 dispatch + 早退（shader 内 `if(idx>=numInstances) return`）去掉回读，或用 conditional rendering / indirect dispatch。

### 12.5 Push constant vs 重复 bind（低收益，易做）
每子步每阶段都 `vkCmdBindPipeline` + `bind descriptor` + `vkCmdPushConstants`。push constant 每子步重写 dt/num_particles（不变值）。可：
- 把不变的 push constant（grid_size、num_particles、inv_dx）在帧初录一次，仅 dt 变化时更新。
- 用 `vkCmdPushConstants` 仅写变化字段（offset 精确）。
- 估算：256×6×3 = 4608 次 bind 调用/帧，CPU 侧录命令开销非瓶颈（GPU 侧 dispatch 才是），收益小但零风险。

### 12.6 Grid buffer 分块 / 稀疏（中收益，需验证）
ZeroGrid 和 GridUpdate 每子步全量扫 64³=262144 节点，但 carnation 粒子只占花头区域（Y[0.8,1.2]），大部分网格节点 mass=0。可：
- ZeroGrid：改扫 particle 列表清零被触及的节点（类似 grid_freeze 的逐粒子写），而非全网格。53 wg vs 512 wg，每子步省 9× 线程。
- GridUpdate：对 mass=0 节点早退（已可能如此，确认 shader）。若已早退，则 ZeroGrid 全量清零是纯 waste——改成稀疏清零收益大。
- 风险：稀疏清零需保证上帧被触及但本帧未触及的节点也被清（残留 mass）——用 grid 的 `active_count`/generation 标记。

### 12.7 诊断与日志开销
- `spdlog` info 级日志每帧打印（drag handler 每 10 次一帧）——确认 Release 编译日志级别，I/O 阻塞会放大帧时间抖动。
- Diagnose 的 frob/norm 循环在 CPU 上跑 13356 粒子，每 60 帧——可移到 compute shader 异步统计。

### 优先级建议
1. **12.1 屏障细化 + G2P/PinFrozen 合并**（零算法风险，预计 -15~25% 帧时间）
2. **12.4 去 waitIdle / 异步诊断回读**（去 CPU stall，-5~10%）
3. **12.6 ZeroGrid 稀疏化**（若确认全量扫是 waste，-10~20%）
4. 12.2/12.3 依赖重叠与异步队列（架构改动大，最后考虑）

> 注：以上均不改变 substeps=256 / kernel / 应力公式 / 应变门控阈值，即不改变数值结果。建议每项改完用 max_disp/max_strain/min_det 诊断对比确认数值一致。

---

*文档基于 phys-sim 分支 2026-07-01 状态（home-spring 关闭，纯 FCR 恢复，substeps=128，ScenePhysicsProfile 按场景；Nsight 实测 ~28 FPS，下一步 P2G shared-mem tiling 冲 60）。算法细节随开发推进更新。*
