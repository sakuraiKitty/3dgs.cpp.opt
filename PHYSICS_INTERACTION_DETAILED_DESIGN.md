# PhysDreamer MPM 物理仿真 C++ 改造与交互集成详细设计方案

## 📋 文档信息

**项目**: PhysDreamer MPM → C++ Vulkan 改造
**创建时间**: 2025-06-02
**目标用户**: Joe
**验证场景**: `D:\liuyue\physDreamerVulkanDemo\PhysDreamer\data\physics_dreamer\carnations\`
**验证命令**: `build_msvc143/apps/viewer/Release/3dgs_viewer.exe --camera camera.txt carnations/point_cloud.ply`

---

## 📐 目录

1. [架构总览](#架构总览)
2. [MPM算法深度解析](#mpm算法深度解析)
3. [材料参数系统](#材料参数系统)
4. [用户交互系统设计](#用户交互系统设计)
5. [物理-渲染耦合](#物理-渲染耦合)
6. [GUI界面设计](#gui界面设计)
7. [性能监控系统实现](#性能监控系统实现) ⭐ 新增
8. [实施路线图](#实施路线图)
9. [关键技术决策](#关键技术决策)

---

## 🏗️ 架构总览

### 系统整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          3dgs.cpp.opt 系统架构                                │
└─────────────────────────────────────────────────────────────────────────────┘

┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│   用户输入层      │ -> │   交互管理层     │ -> │   物理仿真层     │
│  GLFW/Input      │    │ InteractionMgr   │    │   MPMManager     │
└──────────────────┘    └──────────────────┘    └──────────────────┘
                                │                        │
                                ↓                        ↓
┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│   渲染管线层     │ <- │   高斯耦合层     │ <- │   材料系统层     │
│   Renderer       │    │ GaussianCoupling │    │  MaterialField   │
└──────────────────┘    └──────────────────┘    └──────────────────┘
        │
        ↓
┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│   GPU管线层      │    │   Compute管线    │    │   数据管理层     │
│ Vulkan Render    │    │ MPM Shaders      │    │   GSScene        │
└──────────────────┘    └──────────────────┘    └──────────────────┘
```

### 数据流架构

```
                    主循环 (每帧 ~16ms)
                           │
        ┌──────────────────┴──────────────────┐
        │                                     │
        ↓                                     ↓
┌───────────────┐                   ┌───────────────┐
│ Physics Phase │                   │ Render Phase  │
│  (~8-10ms)    │                   │  (~4-6ms)     │
└───────────────┘                   └───────────────┘
        │                                     │
    ┌───┴───┐                           ┌───┴───┐
    │       │                           │       │
    ↓       ↓                           ↓       ↓
┌──────┐ ┌──────┐                   ┌──────┐ ┌──────┐
│ 1.   │ │ 2.   │                   │ 3.   │ │ 4.   │
│Apply │ │MPM   │                   │Map   │ │Render│
│Mouse │ │Substep│                   │Disp  │ │Gauss │
│Force │ │      │                   │      │ │      │
└──────┘ └──────┘                   └──────┘ └──────┘
    │       │                           │       │
    └───────┴───────────────────────────┴───────┘
                    │
                    ↓
        ┌───────────────────┐
        │  GPU Command      │
        │  Buffers (3)      │
        └───────────────────┘
```

---

## 🔬 MPM算法深度解析

### PhysDreamer MPM完整管线（Python → C++映射）

基于 `physDreamer/warp_mpm/mpm_solver_diff.py` 第169-295行的分析：

#### 单个子步完整流程（7阶段）

```
阶段1: zero_grid
├── 功能: 清空网格动量和质量
├── 输入: grid_nodes[grid_size³]
├── 输出: grid_nodes.velocity = 0, grid_nodes.mass = 0
└── 对应: src/shaders/mpm/zero_grid.comp ✅ (已存在)

阶段2: pre_p2g_operations (可选)
├── 功能: 应用预P2G操作（如外力、脉冲）
├── 用途: 用户交互施加的拖拽力
├── 实现: apply_mouse_force.comp (新建)
└── 对应: mpm_solver_diff.py 第193-199行

阶段3: particle_velocity_modifiers (可选)
├── 功能: 修改粒子速度（边界条件）
├── 用途: 冻结边界、约束粒子运动
├── 实现: apply_velocity_constraints.comp (新建)
└── 对应: mpm_solver_diff.py 第202-212行

阶段4: compute_stress_from_F_trial
├── 功能: 从试变形梯度计算应力
├── 算法: F_trial → Return Mapping → Stress
│   ├── F_trial = F_old + (∇v) * F_old * dt
│   ├── SVD分解: F_trial = U * Σ * V^T
│   ├── 塑性修正（针对塑性材料）
│   └── 计算Piola应力: P = 2μ * (F - R) + λ * (J - 1) * J * F^-T
├── 实现: compute_stress.comp (新建)
└── 对应: mpm_utils.py compute_stress_from_F_trial (第604行)

阶段5: p2g_apic_with_stress (Particle to Grid)
├── 功能: 粒子到网格传递（APIC + 应力）
├── 算法: 3x3×3 B样条插值
│   ├── B样条权重: N(x) = 0.5*|x-0.5|² - |x-0.5| + 0.5
│   ├── 权重矩阵: w[i,j,k] = wx[i] * wy[j] * wz[k]
│   ├── 动量传递: mv += m * (v + C·dp) * w
│   └── 应力力: f = -vol * Stress * ∇w
├── 对应: src/shaders/mpm/p2g.comp ⚠️ (需升级为3x3x3)
└── Python: mpm_utils.py p2g_apic_with_stress (第359行)

阶段6: grid_normalization_and_gravity
├── 功能: 网格速度归一化 + 施加重力
├── 算法:
│   ├── v = mv / m
│   ├── v += g * dt
│   └── v *= damping_scale (可选)
├── 对应: src/shaders/mpm/grid_update.comp ✅ (已存在)
└── Python: mpm_utils.py grid_normalization_and_gravity (第428行)

阶段7: g2p_differentiable (Grid to Particle)
├── 功能: 网格到粒子传递
├── 算法: 3x3×3 插值
│   ├── v_p = Σ v_node * w_node
│   ├── C_p = Σ (v_node ⊗ dp) * w_node * 4/dx
│   ├── F_trial = (I + (∇v) * dt) * F_old
│   └── x_p += v_p * dt
├── 对应: src/shaders/mpm/g2p.comp ⚠️ (需升级为3x3x3)
└── Python: mpm_utils.py g2p_differentiable (第503行)
```

### B样条插值函数详解

PhysDreamer使用的三次B样条（Cubic B-Spline）：

```glsl
// src/shaders/mpm/mpm_bspline.glsl (需完善)
#ifndef MPM_BSPLINE_GLSL
#define MPM_BSPLINE_GLSL

/**
 * 三次B样条基函数及其导数
 * 对应 physDreamer: mpm_utils.py 第372-383行
 *
 * 公式:
 *   N(x) = 0.5 * |x - 0.5|² - |x - 0.5| + 0.5,  for |x| < 1
 *   N(x) = 0.5 * (1.5 - |x|)²,                    for 1 ≤ |x| < 2
 *   N(x) = 0,                                      otherwise
 *
 * 导数:
 *   N'(x) = x - 0.5 * sign(x - 0.5),              for |x| < 1
 *   N'(x) = -sign(x) * (1.5 - |x|),               for 1 ≤ |x| < 2
 *   N'(x) = 0,                                     otherwise
 */
void bspline_cubic(float x, out float value, out float derivative) {
    float x_abs = abs(x);
    float x_abs_sq = x_abs * x_abs;

    if (x_abs < 1.0) {
        // 内部区域: |x| < 1
        value = 0.5 * x_abs_sq - x_abs + 0.5;
        derivative = x - sign(x - 0.5);
    } else if (x_abs < 2.0) {
        // 外部区域: 1 ≤ |x| < 2
        float temp = 1.5 - x_abs;
        value = 0.5 * temp * temp;
        derivative = -sign(x) * temp;
    } else {
        // 超出影响范围
        value = 0.0;
        derivative = 0.0;
    }
}

/**
 * 计算B样条权重矩阵和梯度权重矩阵
 *
 * @param fx 相对位置偏移 (grid_pos - base_node)
 * @param w 输出: 3×3 权重矩阵 w[i,j] = N_i * N_j
 * @param dw 输出: 3×3 梯度权重矩阵 dw[i,j] = N'_i * N_j
 */
void compute_bspine_weights(
    vec3 fx,
    out mat3 w,
    out mat3 dw
) {
    // 对每个维度计算B样条基函数
    vec3 w_values[3], dw_values[3];

    for (int dim = 0; dim < 3; dim++) {
        float offset = fx[dim];
        for (int i = 0; i < 3; i++) {
            float x = offset - float(i - 1);  // 转换到 [-1, 2] 范围
            bspline_cubic(x, w_values[i][dim], dw_values[i][dim]);
        }
    }

    // 构建权重矩阵 (外积)
    // w[i,j,k] = wx[i] * wy[j] * wz[k]
    // 存储为 mat3x3[2] 用于3x3x3的三个切片
    w = mat3(
        vec3(w_values[0].x * w_values[0].y, w_values[0].x * w_values[1].y, w_values[0].x * w_values[2].y),
        vec3(w_values[1].x * w_values[0].y, w_values[1].x * w_values[1].y, w_values[1].x * w_values[2].y),
        vec3(w_values[2].x * w_values[0].y, w_values[2].x * w_values[1].y, w_values[2].x * w_values[2].y)
    );

    dw = mat3(
        vec3(dw_values[0].x * w_values[0].y, dw_values[0].x * w_values[1].y, dw_values[0].x * w_values[2].y),
        vec3(dw_values[1].x * w_values[0].y, dw_values[1].x * w_values[1].y, dw_values[1].x * w_values[2].y),
        vec3(dw_values[2].x * w_values[0].y, dw_values[2].x * w_values[1].y, dw_values[2].x * w_values[2].y)
    );
}

/**
 * 计算插值权重梯度（用于应力力计算）
 * 对应 physDreamer: mpm_utils.py compute_dweight() 函数
 */
vec3 compute_dweight(mat3 w, mat3 dw, int i, int j, int k) {
    // ∇(wx[i] * wy[j] * wz[k])
    float dwx = dw[i].x * w[j].y * w[k].z;
    float dwy = w[i].x * dw[j].y * w[k].z;
    float dwz = w[i].x * w[j].y * dw[k].z;
    return vec3(dwx, dwy, dwz) / grid_spacing;
}

#endif // MPM_BSPLINE_GLSL
```

---

## 🧪 材料参数系统

### PhysDreamer材料参数获取流程

基于 `demo.py` 第480-506行和 `INFERENCE_PIPELINE_DESIGN.md` 第238-270行：

```python
def get_material_params(self, device):
    """
    获取每个粒子的材料参数

    流程:
    1. 查询材料场网络获取杨氏模量偏移量
    2. 添加到基础杨氏模量
    3. 应用钳位约束
    """
    initial_position_time0 = self.particle_init_position.detach()
    query_pts = initial_position_time0

    # 查询材料场 (三平面特征分解)
    sim_params = self.sim_fields(query_pts)  # [N, 1]
    sim_params = sim_params * 1000  # 缩放因子

    youngs_modulus = self.young_modulus.detach().clone()
    youngs_modulus += sim_params[..., 0]

    # 钳位到合理范围
    youngs_modulus = torch.clamp(youngs_modulus, 1.0, 5e8)

    density = self.density.detach().clone()
    poisson_ratio = self.poisson_ratio.detach().clone()

    return density, youngs_modulus, poisson_ratio
```

### C++简化方案：多层级材料参数系统

由于完整神经网络过于复杂，我们采用三级简化方案：

#### 方案A: 均匀材料（Phase 1 - 初始实现）

```cpp
// src/mpm/MaterialSystem.h
class MaterialSystem {
public:
    struct UniformMaterial {
        float youngs_modulus;   // 2.14 MPa (carnation默认值)
        float poisson_ratio;    // 0.3
        float density;          // 2000 kg/m³
        float yield_stress;     // 0 (jelly无需屈服)
    };

    void SetUniformMaterial(const UniformMaterial& material);
    void QueryMaterialParams(
        const std::vector<glm::vec3>& positions,
        std::vector<float>& out_youngs,
        std::vector<float>& out_nu,
        std::vector<float>& out_density
    );
};
```

**优点**: 实现简单，性能最优
**缺点**: 无法模拟材料空间变化

#### 方案B: 基于距离的材料变化（Phase 2 - 增强版）

```cpp
class MaterialSystem {
public:
    struct GradientMaterial {
        float youngs_center;    // 中心杨氏模量
        float youngs_edge;      // 边缘杨氏模量
        float gradient_radius;  // 梯度半径
    };

    void SetGradientMaterial(const GradientMaterial& config);

    // 基于到中心点的距离计算杨氏模量
    float QueryYoungsModulus(const glm::vec3& position) {
        float dist = glm::length(position - center_);
        float t = smoothstep(0.0f, gradient_radius_, dist);
        return mix(youngs_center_, youngs_edge_, t);
    }
};
```

**优点**: 简单的空间变化，易于调试
**缺点**: 材料分布规律受限

#### 方案C: 3D纹理材料场（Phase 3 - 高级版，可选）

```cpp
class MaterialFieldTexture {
public:
    // 从.tga/.png文件加载材料场
    bool LoadFromFile(const std::string& path);

    // 在GPU上通过3D纹理查询
    VkImage GetTextureImage() const { return material_texture_; }
    VkImageView GetTextureView() const { return material_texture_view_; }
    VkSampler GetSampler() const { return sampler_; }

private:
    VkImage material_texture_;
    VkImageView material_texture_view_;
    VkSampler sampler_;
    uint32_t resolution_ = 64;  // 64³ 纹理
};
```

**优点**: 灵活的材料分布，接近PhysDreamer效果
**缺点**: 需要预生成纹理文件，增加存储需求

**推荐实施顺序**: A → B → C (按需升级)

### 材料参数Shader实现

```glsl
// src/shaders/mpm/query_material.comp (方案A: 均匀材料)
#version 460
layout(local_size_x = 256) in;

layout(std430, binding = 0) buffer ParticleBuffer {
    ParticleData particles[];
};

layout(push_constant) uniform MaterialParams {
    float youngs_modulus;   // 统一的杨氏模量
    float poisson_ratio;    // 统一的泊松比
    uint num_particles;
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    particles[p_id].youngs_modulus = youngs_modulus;
    particles[p_id].poisson_ratio = poisson_ratio;
    // density在初始化时已设置
}
```

```glsl
// src/shaders/mpm/query_material_gradient.comp (方案B: 梯度材料)
#version 460
layout(local_size_x = 256) in;

layout(std430, binding = 0) buffer ParticleBuffer {
    ParticleData particles[];
};

layout(push_constant) uniform GradientMaterialParams {
    vec3 center;              // 梯度中心
    float youngs_center;     // 中心杨氏模量
    float youngs_edge;       // 边缘杨氏模量
    float gradient_radius;   // 梯度半径
    float poisson_ratio;     // 统一泊松比
    uint num_particles;
};

float smoothstep(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    vec3 pos = particles[p_id].position;
    float dist = length(pos - center);
    float t = smoothstep(0.0, gradient_radius, dist);

    particles[p_id].youngs_modulus = mix(youngs_center, youngs_edge, t);
    particles[p_id].poisson_ratio = poisson_ratio;
}
```

---

## 🎮 用户交互系统设计

### 交互需求分析

**用户需求**:
1. 鼠标在可变形区域内任意位置施加变形
2. 拖拽时显示绿色箭头，长度表示力的大小
3. 松开鼠标后弹性势能释放，产生震荡
4. 阻尼作用使物体逐渐回到原位
5. 不与现有键位冲突

### 交互架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    用户交互系统架构                            │
└─────────────────────────────────────────────────────────────┘

                    GLFW回调
                       │
        ┌──────────────┴──────────────┐
        │                             │
        ↓                             ↓
┌───────────────┐             ┌───────────────┐
│  Renderer::   │             │  Interaction  │
│  handleInput()│             │  Manager      │
└───────────────┘             └───────────────┘
        │                             │
        │ 相机控制(WASD/鼠标)          │ 物理交互(Shift+拖拽)
        │                             │
        ↓                             ↓
┌───────────────┐             ┌───────────────┐
│ CameraControl │             │  DragHandler  │
│               │             │  + RayCaster  │
└───────────────┘             └───────────────┘
                                       │
                              ┌────────┴────────┐
                              │                 │
                              ↓                 ↓
                      ┌───────────┐     ┌───────────┐
                      │Visual     │     │Force      │
                      │Feedback   │     │Applier    │
                      │(Green     │     │           │
                      │Arrow)     │     │           │
                      └───────────┘     └───────────┘
```

### 键位映射设计（避免冲突）

**现有键位**（来自 GUIManager.cpp 第94-99行）:
- WASD: 移动相机
- Space: 向上
- Shift: 向下
- Left click: 捕获鼠标
- ESC: 释放鼠标
- F12: 截图

**✅ 用户建议的改进键位**（更符合FPS游戏习惯）:
- WASD: 移动相机（保持）
- **Q**: 向上（替代Space，更方便）
- **E**: 向下（替代Shift，释放Shift键）
- **Right click**: 捕获鼠标（替代左键，更符合FPS习惯）
- ESC: 释放鼠标（保持）
- F12: 截图（保持）

**新增物理交互键位**（无冲突）:
- **Shift + 左键拖拽**: 施加物理变形 ✅
- **H键**: 切换物理交互模式（开/关）
- **R键**: 重置物理状态
- **[ / ]键**: 调整拖拽力强度

**模式切换逻辑**:
```
Default模式（默认）:
  - 右键点击: 捕获鼠标用于相机控制
  - WASD: 相机移动
  - Q/E: 上下移动
  - 鼠标移动: 相机旋转

Physics模式（按H切换）:
  - Shift + 左键拖拽: 物理变形 ✅ (Shift已释放)
  - WASD: 相机移动（保持可用）
  - Q/E: 上下移动（保持可用）
  - 鼠标移动（不按Shift）: 相机旋转
  - 右键单独点击: 仍可捕获鼠标（向后兼容）
```

**键位映射对比表**:
| 功能 | 旧键位 | 新键位 | 优势 |
|------|--------|--------|------|
| 向上移动 | Space | **Q** | 更符合FPS游戏习惯 |
| 向下移动 | Shift | **E** | 释放Shift用于物理交互 |
| 捕获鼠标 | Left click | **Right click** | 左键释放用于物理拖拽 |
| 物理变形 | - | **Shift + Left drag** | 无冲突，直观 |

### 拖拽交互流程详解

```
用户按下 Shift + 左键
         │
         ↓
┌──────────────────────────────────────┐
│  Phase 1: 粒子拾取 (Ray Casting)      │
├──────────────────────────────────────┤
│  1. 获取屏幕坐标 (mouse_x, mouse_y)   │
│  2. 计算射线方向                       │
│     ray_dir = ScreenToWorldRay(...)   │
│  3. GPU并行查找最近粒子               │
│     - 遍历所有可变形粒子              │
│     - 计算点到射线距离                │
│     - 原子操作找到最小距离            │
│  4. 返回粒子索引 particle_idx         │
└──────────────────────────────────────┘
         │
         ↓ (拾取成功)
┌──────────────────────────────────────┐
│  Phase 2: 拖拽状态管理               │
├──────────────────────────────────────┤
│  dragging_ = true                     │
│  dragged_particle_ = particle_idx     │
│  drag_start_pos_ = particle.position  │
│  drag_start_screen_ = {mouse_x, y}    │
└──────────────────────────────────────┘
         │
         ↓ (每帧更新)
┌──────────────────────────────────────┐
│  Phase 3: 力的计算与可视化            │
├──────────────────────────────────────┤
│  1. 计算拖拽向量:                     │
│     screen_delta = current - start    │
│  2. 映射到3D空间:                     │
│     world_delta = ProjectToPlane(...) │
│  3. 计算施加的力:                     │
│     force = world_delta * stiffness   │
│  4. 限制最大力:                       │
│     force = clamp(force, max_force)   │
│  5. 更新绿色箭头可视化数据             │
└──────────────────────────────────────┘
         │
         ↓
┌──────────────────────────────────────┐
│  Phase 4: 力应用 (每子步)            │
├──────────────────────────────────────┤
│  在MPM子步开始时调用:                 │
│  apply_mouse_force.comp               │
│  - 对 dragged_particle_ 施加外力      │
│  - force += mouse_force * dt          │
└──────────────────────────────────────┘
         │
         ↓ (用户松开Shift)
┌──────────────────────────────────────┐
│  Phase 5: 释放与震荡                  │
├──────────────────────────────────────┤
│  dragging_ = false                    │
│  mouse_force = {0, 0, 0}              │
│  弹性势能自动释放                     │
│  物理系统自然阻尼衰减                  │
└──────────────────────────────────────┘
```

### Shader实现：射线拾取

```glsl
// src/shaders/interaction/ray_cast_particles.comp
#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable

layout(local_size_x = 256) in;

// 输入：粒子位置
layout(std430, binding = 0) readonly buffer ParticlePositions {
    vec3 positions[];
};

// 输出：最近粒子结果
layout(std430, binding = 1) buffer RayCastResult {
    uint closest_particle_idx;
    float closest_distance_sq;
};

uniform RayCastUniforms {
    vec3 ray_origin;       // 射线起点（相机位置）
    vec3 ray_direction;    // 射线方向（单位向量）
    uint num_particles;    // 粒子总数
    uint sim_mask_count;   // 可变形粒子数（可选优化）
    float max_distance_sq; // 最大拾取距离平方
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    vec3 particle_pos = positions[p_id];

    // 计算点到射线的距离
    // 公式: distance = |(ray_origin + t*ray_dir) - particle_pos|
    //       t = dot(particle_pos - ray_origin, ray_direction)
    vec3 v = particle_pos - ray_origin;
    float projection = dot(v, ray_direction);
    vec3 closest_point = ray_origin + ray_direction * projection;
    vec3 diff = particle_pos - closest_point;
    float distance_sq = dot(diff, diff);

    // 子组内找到最小值（优化性能）
    if (distance_sq < max_distance_sq) {
        // 使用subgroupMin找到全局最小
        float subgroup_min = subgroupMin(distance_sq);

        if (distance_sq == subgroup_min) {
            // 这个线程有最小值，尝试更新
            atomicMin(closest_distance_sq, floatBitsToUint(distance_sq));

            // 再次验证是否仍是最小值
            if (uintBitsToFloat(closest_distance_sq) == distance_sq) {
                closest_particle_idx = p_id;
            }
        }
    }
}
```

### Shader实现：鼠标力应用

```glsl
// src/shaders/interaction/apply_mouse_force.comp
#version 460
layout(local_size_x = 1) in;  // 单线程操作

layout(std430, binding = 0) buffer ParticleBuffer {
    ParticleData particles[];
};

uniform MouseForceUniforms {
    uint target_particle_idx;  // 目标粒子索引
    vec3 force_vector;         // 施加的力向量 [N]
    float max_velocity;        // 最大速度限制 [m/s]
    float dt;                  // 时间步长 [s]
};

void main() {
    if (gl_GlobalInvocationID.x != target_particle_idx) return;

    // PD控制：将力转换为期望速度
    // v = F * dt / m
    float mass = particles[target_particle_idx].mass;
    vec3 impulse = force_vector * dt;
    vec3 delta_v = impulse / mass;

    // 应用速度增量
    vec3 new_velocity = particles[target_particle_idx].velocity + delta_v;

    // 限制最大速度（防止爆炸）
    float speed = length(new_velocity);
    if (speed > max_velocity) {
        new_velocity = normalize(new_velocity) * max_velocity;
    }

    particles[target_particle_idx].velocity = new_velocity;
}
```

### 可视化：绿色箭头绘制

```cpp
// src/interaction/DragVisualizer.h
class DragVisualizer {
public:
    void Initialize(std::shared_ptr<VulkanContext> context);
    void UpdateDragState(
        const glm::vec3& particle_pos,
        const glm::vec3& force_vector
    );
    void Render(VkCommandBuffer cmd, const glm::mat4& view_proj);

private:
    struct ArrowVertex {
        glm::vec3 position;
        glm::vec3 color;  // 绿色: (0, 1, 0)
    };

    std::shared_ptr<Buffer> arrow_vertex_buffer_;
    std::shared_ptr<Pipeline> arrow_pipeline_;

    // 生成箭头几何体
    void GenerateArrowGeometry(
        const glm::vec3& start,
        const glm::vec3& end,
        std::vector<ArrowVertex>& out_vertices
    );
};
```

```glsl
// src/shaders/interaction/visualize_arrow.vert
#version 460
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;

layout(std140, binding = 0) uniform CameraUniforms {
    mat4 view_proj;
};

layout(location = 0) out vec3 frag_color;

void main() {
    gl_Position = view_proj * vec4(in_position, 1.0);
    frag_color = in_color;
}

// src/shaders/interaction/visualize_arrow.frag
#version 460
layout(location = 0) in vec3 frag_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(frag_color, 1.0);  // 不透明绿色
}
```

---

## 🔗 物理-渲染耦合

### PhysDreamer位移插值算法

基于 `INFERENCE_PIPELINE_DESIGN.md` 第356-442行：

```python
def interpolate_points_w_R(
    query_points, query_rotation,
    drive_origin_pts, drive_displacement, top_k_index
):
    """
    使用刚性变换插值位移

    1. 对每个查询点:
       a. 找到k个最近的驱动点
       b. 计算这些点的位移
       c. 拟合刚性变换 (R, t)
       d. 应用变换
    """
    # 获取k近邻的位移
    top_k_disp = drive_displacement[top_k_index]  # [n, k, 3]
    source_points = drive_origin_pts[top_k_index]  # [n, k, 3]

    # 拟合刚性变换
    R, t = get_rigid_transform(source_points, source_points + top_k_disp)

    # 计算平均位移
    avg_offsets = top_k_disp.mean(dim=1)

    # 应用变换
    ret_points = query_points + avg_offsets
    new_rotation = quaternion_multiply(matrix_to_quaternion(R), query_rotation)

    return ret_points, new_rotation
```

### C++实现方案：Top-K刚性变换插值（与Python源码一致）⭐ P0修复

**⚠️ 重要说明**：原始设计使用简单加权平均，但PhysDreamer Python源码使用刚性变换插值（包含旋转）。为保持物理正确性，必须按以下方式实现：

```python
# Python源码参考 (local_utils.py:1074-1103)
def interpolate_points_w_R(query_points, query_rotation, 
                          drive_origin_pts, drive_displacement, top_k_index):
    top_k_disp = drive_displacement[top_k_index]
    source_points = drive_origin_pts[top_k_index]
    
    # 关键：拟合刚性变换 (R, t) 使用SVD
    R, t = get_rigid_transform(source_points, source_points + top_k_disp)
    
    avg_offsets = top_k_disp.mean(dim=1)
    ret_points = query_points + avg_offsets
    
    # 关键：更新旋转四元数
    new_rotation = quaternion_multiply(matrix_to_quaternion(R), query_rotation)
    
    return ret_points, new_rotation
```

**方案A：完整GPU实现（推荐）**

```glsl
// src/shaders/coupling/rigid_transform.glsl (新建)
#ifndef RIGID_TRANSFORM_GLSL
#define RIGID_TRANSFORM_GLSL

// 四元数归一化
vec4 quaternion_normalize(vec4 q) {
    float len = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return q / len;
}

// 四元数乘法 (q = w + xi + yj + zk)
vec4 quaternion_multiply(vec4 a, vec4 b) {
    vec4 result;
    result.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    result.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    result.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    result.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return quaternion_normalize(result);
}

// 旋转矩阵转四元数 (基于pytorch3d算法)
vec4 matrix_to_quaternion(mat3 R) {
    float tr = R[0][0] + R[1][1] + R[2][2];
    
    if (tr > 0.0) {
        float s = sqrt(tr + 1.0) * 2.0;
        return vec4(
            0.25 * s,
            (R[2][1] - R[1][2]) / s,
            (R[0][2] - R[2][0]) / s,
            (R[1][0] - R[0][1]) / s
        );
    } else if ((R[0][0] > R[1][1]) && (R[0][0] > R[2][2])) {
        float s = sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2.0;
        return vec4(
            (R[2][1] - R[1][2]) / s,
            0.25 * s,
            (R[0][1] + R[1][0]) / s,
            (R[0][2] + R[2][0]) / s
        );
    } else if (R[1][1] > R[2][2]) {
        float s = sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2.0;
        return vec4(
            (R[0][2] - R[2][0]) / s,
            (R[0][1] + R[1][0]) / s,
            0.25 * s,
            (R[1][2] + R[2][1]) / s
        );
    } else {
        float s = sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2.0;
        return vec4(
            (R[1][0] - R[0][1]) / s,
            (R[0][2] + R[2][0]) / s,
            (R[1][2] + R[2][1]) / s,
            0.25 * s
        );
    }
}

// 刚性变换结构
struct RigidTransform {
    mat3 R;
    vec3 t;
};

// SVD分解的简化实现（Jacobi迭代，最多10次迭代）
struct SVDResult {
    mat3 U;
    mat3 V;
};

SVDResult svd_jacobi(mat3 A) {
    SVDResult result;
    mat3 V = mat3(1.0);
    mat3 U = A;
    
    for (int iter = 0; iter < 10; iter++) {
        // 找最大非对角元素
        int p = 0, q = 1;
        float max_val = abs(U[0][1]);
        
        if (abs(U[0][2]) > max_val) { max_val = abs(U[0][2]); p = 0; q = 2; }
        if (abs(U[1][2]) > max_val) { max_val = abs(U[1][2]); p = 1; q = 2; }
        
        if (max_val < 1e-6) break;
        
        // Jacobi旋转参数
        float theta = (U[q][q] - U[p][p]) / (2.0 * U[p][q]);
        float t = 1.0 / (abs(theta) + sqrt(theta * theta + 1.0));
        if (theta < 0.0) t = -t;
        
        float c = 1.0 / sqrt(t * t + 1.0);
        float s = t * c;
        
        // 更新U
        for (int i = 0; i < 3; i++) {
            float temp = c * U[i][p] - s * U[i][q];
            U[i][q] = s * U[i][p] + c * U[i][q];
            U[i][p] = temp;
        }
        
        // 更新V
        for (int i = 0; i < 3; i++) {
            float temp = c * V[i][p] - s * V[i][q];
            V[i][q] = s * V[i][p] + c * V[i][q];
            V[i][p] = temp;
        }
    }
    
    result.U = U;
    result.V = V;
    return result;
}

// 拟合刚性变换 (与Python get_rigid_transform一致)
RigidTransform fit_rigid_transform(
    vec3 source_points[8],
    vec3 target_points[8],
    float weights[8]
) {
    // 1. 计算加权质心
    vec3 centroid_A = vec3(0.0);
    vec3 centroid_B = vec3(0.0);
    float weight_sum = 0.0;
    
    for (int i = 0; i < 8; i++) {
        centroid_A += source_points[i] * weights[i];
        centroid_B += target_points[i] * weights[i];
        weight_sum += weights[i];
    }
    
    centroid_A /= weight_sum;
    centroid_B /= weight_sum;
    
    // 2. 计算交叉协方差 H = A^T * B
    mat3 H = mat3(0.0);
    for (int i = 0; i < 8; i++) {
        vec3 a = source_points[i] - centroid_A;
        vec3 b = target_points[i] - centroid_B;
        
        // 外积累加: a * b^T
        H[0] += a * b.x;
        H[1] += a * b.y;
        H[2] += a * b.z;
    }
    
    // 3. SVD: H = U * S * V^T
    SVDResult svd = svd_jacobi(H);
    
    // 4. R = V * U^T
    mat3 R = transpose(svd.V) * transpose(svd.U);
    
    // 5. 确保右手坐标系
    if (determinant(R) < 0.0) {
        svd.V[2] *= -1.0;
        R = transpose(svd.V) * transpose(svd.U);
    }
    
    // 6. t = centroid_B - R * centroid_A
    vec3 t = centroid_B - R * centroid_A;
    
    RigidTransform transform;
    transform.R = R;
    transform.t = t;
    return transform;
}

#endif // RIGID_TRANSFORM_GLSL
```

```glsl
// src/shaders/coupling/map_displacement_with_rotation.comp (替换原map_displacement.comp)
#version 460
#pragma include "rigid_transform.glsl"

layout(local_size_x = 256) in;

// 输入：驱动粒子原始位置（用于计算相对变换）
layout(std430, binding = 0) readonly buffer DriveParticlePositions {
    vec3 drive_positions[];
};

layout(std430, binding = 1) readonly buffer DriveParticleDisplacements {
    vec3 drive_displacements[];
};

// 输入：Top-K映射
layout(std430, binding = 2) readonly buffer TopKMappingBuffer {
    uvec4 top_k_indices[];
    vec4 top_k_weights[];
};

// 输入：高斯原始位置和旋转
layout(std430, binding = 3) readonly buffer OriginalGaussianData {
    vec3 original_positions[];
    vec4 original_rotations[];    // 四元数 (w, x, y, z)
};

// 输出：高斯新位置和旋转
layout(std430, binding = 4) buffer GaussianPositions {
    vec3 positions[];
};

layout(std430, binding = 5) buffer GaussianRotations {
    vec4 rotations[];
};

uniform MappingUniforms {
    uint num_gaussians;
    uint k;  // 固定为8
};

void main() {
    uint g_id = gl_GlobalInvocationID.x;
    if (g_id >= num_gaussians) return;

    // 读取Top-K索引和权重
    uvec4 idx_part1 = top_k_indices[g_id * 2];
    uvec4 idx_part2 = top_k_indices[g_id * 2 + 1];
    uint indices[8] = uint[](
        idx_part1.x, idx_part1.y, idx_part1.z, idx_part1.w,
        idx_part2.x, idx_part2.y, idx_part2.z, idx_part2.w
    );

    vec4 weight_part1 = top_k_weights[g_id * 2];
    vec4 weight_part2 = top_k_weights[g_id * 2 + 1];
    float weights[8] = float[](
        weight_part1.x, weight_part1.y, weight_part1.z, weight_part1.w,
        weight_part2.x, weight_part2.y, weight_part2.z, weight_part2.w
    );

    // 准备K个点对
    vec3 source_points[8];
    vec3 target_points[8];
    float weight_sum = 0.0;
    
    for (int i = 0; i < 8; i++) {
        if (indices[i] != UINT_MAX) {
            source_points[i] = drive_positions[indices[i]];
            target_points[i] = drive_positions[indices[i]] + drive_displacements[indices[i]];
            weight_sum += weights[i];
        }
    }
    
    // 归一化权重
    for (int i = 0; i < 8; i++) {
        if (weight_sum > 0.0) {
            weights[i] /= weight_sum;
        }
    }
    
    // 拟合刚性变换 (R, t)
    RigidTransform transform = fit_rigid_transform(source_points, target_points, weights);
    
    // 应用变换到高斯位置
    vec3 original_pos = original_positions[g_id];
    positions[g_id] = transform.R * original_pos + transform.t;
    
    // 应用旋转变换到高斯四元数
    vec4 original_rot = original_rotations[g_id];
    vec4 R_quat = matrix_to_quaternion(transform.R);
    rotations[g_id] = quaternion_multiply(R_quat, original_rot);
}
```

**方案B：混合实现（备选，如GPU SVD过于复杂）**

如果GPU SVD实现困难，可暂时使用CPU端Eigen库计算变换，GPU仅做应用：

```cpp
// CPU端使用Eigen计算每个高斯的(R,t)
#pragma omp parallel for
for (size_t i = 0; i < num_gaussians; i++) {
    Eigen::Matrix3Xf A(3, k), B(3, k);
    // 收集K个点...
    Eigen::JacobiSVD<Eigen::Matrix3f> svd(H);
    transforms_[i] = {R, t};
}
// 上传到GPU，Shader仅做应用
```

### 高斯位置更新Shader

```glsl
// src/shaders/coupling/update_gaussian_positions.comp (新建)
#version 460
layout(local_size_x = 256) in;

// 输入：渲染粒子原始位置 + 位移
layout(std430, binding = 0) readonly buffer OriginalPositions {
    vec3 original_positions[];
};

layout(std430, binding = 1) readonly buffer Displacements {
    vec3 displacements[];
};

// 输入：可变形区域掩码
layout(std430, binding = 2) readonly buffer SimMask {
    uint sim_mask[];  // bitset或uint数组
};

// 输出：高斯位置
layout(std430, binding = 3) buffer GaussianPositions {
    vec4 gaussian_positions[];  // xyz=位置, w=padding
};

uniform UpdateUniforms {
    uint num_gaussians;
    uint displacement_mode;  // 0=绝对位置, 1=增量位移
};

void main() {
    uint g_id = gl_GlobalInvocationID.x;
    if (g_id >= num_gaussians) return;

    // 检查是否在可变形区域内
    uint mask_word = sim_mask[g_id / 32];
    uint mask_bit = 1u << (g_id % 32);
    bool is_deformable = (mask_word & mask_bit) != 0;

    if (is_deformable) {
        vec3 original_pos = original_positions[g_id];
        vec3 displacement = displacements[g_id];

        if (displacement_mode == 0) {
            // 绝对位置模式（直接设置）
            gaussian_positions[g_id].xyz = original_pos + displacement;
        } else {
            // 增量位移模式（累加）
            gaussian_positions[g_id].xyz += displacement;
        }
    }
    // 静态粒子保持不变
}
```

---

## 🖥️ GUI界面设计

### GUI窗口布局

```
┌─────────────────────────────────────────────────────────────────┐
│  Performance (现有)                                              │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ [FPS图表]                                                 │ │
│  └───────────────────────────────────────────────────────────┘ │
│  History: [========|] 10 s                                     │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Metrics (现有 + 新增MPM计时)                                   │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ Frame Timing                                               │ │
│  │   Total time: X.XX ms (XX.XX FPS)                         │ │
│  │   MPM Physics: X.XX ms (XX%)                              │ │
│  │   Render time: X.XX ms (XX%)                              │ │
│  │   Coupling time: X.XX ms (XX%)                            │ │
│  │   Overhead: X.XX ms (XX%)                                 │ │
│  └───────────────────────────────────────────────────────────┘ │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ MPM Simulation Details                                    │ │
│  │   Substeps: 128                                            │ │
│  │   Substep time: X.XX ms (X.XX µs/step)                    │ │
│  │   Particles: 32,703                                        │ │
│  │   Grid nodes: 262,144 (64³)                                │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Controls (现有)                                               │
│  WASD: move                                                    │
│  Q/E: up/down (改进)                                           │
│  Right click: capture mouse (改进)                             │
│  ESC: release mouse                                            │
│  Mouse captured: true/false                                    │
│  ─────────────────────────────────────────────────────────────│
│  [Foreground Only] 切换                                         │
│  Mode: Foreground Only                                         │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Physics Simulation (新增)                                      │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ Status                                                     │ │
│  │   Enabled: [Enable/Disable] 切换                          │ │
│  │   Particles: 32,703                                       │ │
│  │   Grid: 64³                                               │ │
│  │   Substeps: 128                                           │ │
│  │   FPS: XX.X                                               │ │
│  ├───────────────────────────────────────────────────────────┤ │
│  │ Interaction Mode                                           │ │
│  │   [H] Toggle: Physics/Default                             │ │
│  │   Current: Physics ⚡                                      │ │
│  │   Drag Force: [======|] 1.5x  ([/]调节)                   │ │
│  │   Stiffness: [========|] 0.8                              │ │
│  ├───────────────────────────────────────────────────────────┤ │
│  │ Material Parameters                                       │ │
│  │   Young's Modulus: 2.14 MPa                               │ │
│  │   Poisson Ratio: 0.30                                     │ │
│  │   Density: 2000 kg/m³                                      │ │
│  │   Damping: 0.99                                            │ │
│  ├───────────────────────────────────────────────────────────┤ │
│  │ Actions                                                    │ │
│  │   [R] Reset Simulation                                     │ │
│  │   [S] Save State                                           │ │
│  │   [L] Load State                                           │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Interaction Guide (新增)                                     │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ Camera Controls                                            │ │
│  │   WASD: Move camera                                        │ │
│  │   Q: Move up                                               │ │
│  │   E: Move down                                             │ │
│  │   Right click: Capture mouse for rotation                  │ │
│  │   ESC: Release mouse                                       │ │
│  │                                                            │ │
│  │ Physics Controls                                          │ │
│  │   H: Toggle Physics/Default mode                          │ │
│  │   Shift + Left Drag: Apply deformation force              │ │
│  │   Green arrow: Force magnitude visualization             │ │
│  │   [ / ]: Adjust drag force strength                       │ │
│  │   R: Reset physics state                                  │ │
│  │                                                            │ │
│  │ Tips                                                       │ │
│  │   - Switch to Physics mode (H) before deformation         │ │
│  │   - Drag in deformable region (foreground only)           │ │
│  │   - Longer drag = stronger force                          │ │
│  │   - Release to see elastic oscillation                     │ │
│  │   - Physics runs at 30 FPS, render at monitor refresh    │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
│  │ Keyboard Controls                                         │ │
│  │   H: Toggle interaction mode                              │ │
│  │   R: Reset physics state                                  │ │
│  │   [ / ]: Adjust drag force strength                       │ │
│  │                                                            │ │
│  │ Tips                                                       │ │
│  │   - Drag in deformable region (foreground only)           │ │
│  │   - Longer drag = stronger force                          │ │
│  │   - Release to see elastic oscillation                     │ │
│  │   - Physics runs at 30 FPS, render at monitor refresh    │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### ImGui实现代码

```cpp
// src/GUIManager.cpp (新增部分)

void GUIManager::buildPhysicsGUI() {
    ImGui::SetNextWindowPos(ImVec2(10, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Physics Simulation");

    // Status
    ImGui::SeparatorText("Status");
    ImGui::Checkbox("Enabled", &physics_enabled_);
    ImGui::SameLine();
    if (ImGui::Button("Reset (R)")) {
        physics_reset_requested_ = true;
    }

    if (mpm_manager_) {
        auto stats = mpm_manager_->GetStatistics();
        ImGui::Text("Particles: %zu", stats.num_particles);
        ImGui::Text("Grid: %d³", stats.grid_size);
        ImGui::Text("Substeps: %d", stats.substeps);
        ImGui::Text("Physics FPS: %.1f", stats.physics_fps);
    }

    // Interaction Mode
    ImGui::SeparatorText("Interaction Mode");
    const char* mode_names[] = {"Default", "Physics ⚡"};
    ImGui::Text("Current Mode: %s", mode_names[interaction_mode_]);
    if (ImGui::IsKeyPressed(ImGuiKey_H)) {
        interaction_mode_ = (interaction_mode_ + 1) % 2;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(H to toggle)");

    ImGui::SliderFloat("Drag Force", &drag_force_multiplier_, 0.1f, 5.0f, "%.2fx");
    ImGui::SliderFloat("Stiffness", &interaction_stiffness_, 0.1f, 2.0f, "%.2f");

    if (interaction_mode_ == 1) {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Physics Interaction ACTIVE");
        ImGui::Text("Shift + Drag to deform");
        ImGui::Text("Green arrow shows force");
    } else {
        ImGui::TextDisabled("Camera control mode");
    }

    // Material Parameters
    ImGui::SeparatorText("Material Parameters");
    ImGui::Text("Young's Modulus: %.2f MPa", material_params_.youngs_modulus / 1e6f);
    ImGui::Text("Poisson Ratio: %.2f", material_params_.poisson_ratio);
    ImGui::Text("Density: %.0f kg/m³", material_params_.density);
    ImGui::Text("Damping: %.3f", material_params_.damping);

    ImGui::Separator();
    if (ImGui::Button("Save State (S)")) {
        physics_save_requested_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load State (L)")) {
        physics_load_requested_ = true;
    }

    ImGui::End();

    // Performance Metrics window (更新：添加MPM计时)
    ImGui::SetNextWindowPos(ImVec2(10, 270), ImGuiCond_FirstUseEver);
    ImGui::Begin("Metrics", &popen, ImGuiWindowFlags_AlwaysAutoResize);

    // Frame Timing
    ImGui::SeparatorText("Frame Timing");
    float total_time = mpm_frame_time_ms_ + render_time_ms_ + coupling_time_ms_ + overhead_ms_;
    float fps = 1000.0f / total_time;
    ImGui::Text("Total time: %.2f ms (%.1f FPS)", total_time, fps);

    if (physics_enabled_) {
        float mpm_percent = (mpm_frame_time_ms_ / total_time) * 100.0f;
        ImGui::TextColored(ImVec4(0.0, 0.8, 1.0, 1.0), "MPM Physics: %.2f ms (%.1f%%)",
                          mpm_frame_time_ms_, mpm_percent);
    }

    float render_percent = (render_time_ms_ / total_time) * 100.0f;
    ImGui::Text("Render time: %.2f ms (%.1f%%)", render_time_ms_, render_percent);

    float coupling_percent = (coupling_time_ms_ / total_time) * 100.0f;
    ImGui::Text("Coupling time: %.2f ms (%.1f%%)", coupling_time_ms_, coupling_percent);

    float overhead_percent = (overhead_ms_ / total_time) * 100.0f;
    ImGui::TextDisabled("Overhead: %.2f ms (%.1f%%)", overhead_ms_, overhead_percent);

    // MPM Simulation Details
    if (physics_enabled_ && mpm_manager_) {
        ImGui::Separator();
        ImGui::SeparatorText("MPM Simulation Details");

        auto stats = mpm_manager_->GetStatistics();
        ImGui::Text("Substeps: %d", stats.substeps);
        if (mpm_frame_time_ms_ > 0) {
            float substep_time_us = (mpm_frame_time_ms_ / stats.substeps) * 1000.0f;
            ImGui::Text("Substep time: %.2f µs", substep_time_us);
        }
        ImGui::Text("Particles: %zu", stats.num_particles);
        ImGui::Text("Grid nodes: %d³ (%.0f nodes)",
                   stats.grid_size,
                   std::pow(stats.grid_size, 3));

        // Performance breakdown (可选，详细分析时显示)
        if (show_mpm_breakdown_) {
            ImGui::Separator();
            ImGui::Text("MPM Stage Breakdown:");
            ImGui::Text("  Zero Grid: %.3f ms", stats.zero_grid_time);
            ImGui::Text("  Stress Compute: %.3f ms", stats.stress_time);
            ImGui::Text("  P2G: %.3f ms", stats.p2g_time);
            ImGui::Text("  Grid Update: %.3f ms", stats.grid_update_time);
            ImGui::Text("  G2P: %.3f ms", stats.g2p_time);
        }
    }

    ImGui::End();

    // Interaction Guide (help window)
    ImGui::SetNextWindowPos(ImVec2(380, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("Interaction Guide");
    ImGui::TextWrapped("Camera Controls:");
    ImGui::BulletText("WASD: Move camera");
    ImGui::BulletText("Q: Move up");
    ImGui::BulletText("E: Move down");
    ImGui::BulletText("Right click: Capture mouse for rotation");
    ImGui::BulletText("ESC: Release mouse");
    ImGui::Spacing();
    ImGui::TextWrapped("Physics Controls:");
    ImGui::BulletText("H: Toggle Physics/Default mode");
    ImGui::BulletText("Shift + Left Drag: Apply deformation force");
    ImGui::BulletText("Green arrow: Force magnitude");
    ImGui::BulletText("[ / ]: Adjust drag force strength");
    ImGui::BulletText("R: Reset physics state");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Tips:");
    ImGui::TextWrapped("• Switch to Physics mode (H) before deformation");
    ImGui::TextWrapped("• Drag in deformable region (foreground only)");
    ImGui::TextWrapped("• Longer drag = stronger force");
    ImGui::TextWrapped("• Release to see elastic oscillation");
    ImGui::TextWrapped("• Physics runs at 30 FPS, render at monitor refresh");
    ImGui::End();
}
```

---

## 📅 实施路线图

### 总体时间规划

| 阶段 | 名称 | 预计时间 | 优先级 | 依赖 |
|------|------|---------|--------|------|
| **Phase 0** | 准备与验证 | 2-3天 | P0 | 无 |
| **Phase 1** | MPM核心算法 | 2周 | P0 | Phase 0 |
| **Phase 2** | 材料系统 | 1周 | P1 | Phase 1 |
| **Phase 3** | 用户交互 | 2周 | P0 | Phase 1 |
| **Phase 4** | 物理-渲染耦合 | 1周 | P0 | Phase 1,3 |
| **Phase 5** | GUI与调试 | 1周 | P1 | Phase 2,3,4 |
| **Phase 6** | 集成与优化 | 1周 | P1 | Phase 1-5 |

**总计**: 约8周（考虑调试和调整，预计10周完成）

### Phase 0: 准备与验证（2-3天）

**目标**: 验证现有代码，建立开发环境

**任务清单**:
- [ ] 验证carnations场景加载正确
  ```bash
  build_msvc143/apps/viewer/Release/3dgs_viewer.exe \
    --camera camera.txt \
    D:\liuyue\physDreamerVulkanDemo\PhysDreamer\data\physics_dreamer\carnations\point_cloud.ply
  ```
- [ ] 确认现有MPM shader编译通过
- [ ] 验证MPMInitializer集成正确
- [ ] 建立性能基准测试（FPS监控）

**验收标准**:
- ✅ carnations场景正常渲染
- ✅ FPS显示正常（~105 for 1M gaussians）
- ✅ "Foreground Only" 模式工作正常
- ✅ 无Vulkan验证层错误

### Phase 1: MPM核心算法（2周）

**子阶段 1.1: B样条插值升级（3天）**

**任务**:
- [ ] 升级 `src/shaders/mpm/mpm_bspline.glsl`
  - 实现三次B样条函数
  - 实现3x3×3权重计算
- [ ] 升级 `src/shaders/mpm/p2g.comp` 到3x3×3
- [ ] 升级 `src/shaders/mpm/g2p.comp` 到3x3×3

**验收标准**:
```bash
# 编译测试
glslc -V -o /dev/null src/shaders/mpm/p2g.comp
glslc -V -o /dev/null src/shaders/mpm/g2p.comp

# 数值验证（单元测试）
./build/tests/test_bspine_interpolation
# 预期: 权重和 = 1.0，梯度误差 < 1e-5
```

**子阶段 1.2: 应力计算Shader（4天）**

**任务**:
- [ ] 创建 `src/shaders/mpm/compute_stress.comp`
  - 实现SVD分解（或简化版本）
  - 实现FCR材料模型（Fixed Corotated）
  - 实现Return Mapping
- [ ] 集成到MPM子步管线

**算法细节**:
```glsl
// 简化的FCR应力模型（无需完整SVD）
mat3 compute_fcr_stress(mat3 F, float mu, float lam) {
    // 极分解: F = R * S
    mat3 R = extract_rotation(F);  // 简化: 使用gram-schmidt
    mat3 S = transpose(R) * F;

    // 第一Piola-Kirchhoff应力
    mat3 P = 2.0 * mu * (F - R) + lam * (determinant(F) - 1.0) * determinant(F) * inverse(transpose(F));

    return P;
}
```

**验收标准**:
```bash
./build/tests/test_stress_computation
# 预期: 单位变形产生零应力，10%拉伸产生合理应力
```

**子阶段 1.3: 完整MPM管线（5天）**

**任务**:
- [ ] 实现MPMManager::Step() 完整流程
- [ ] 创建7个阶段的compute pipeline
- [ ] 实现barrier和同步
- [ ] CPU-GPU数据传输优化

**代码框架**:
```cpp
// src/mpm/MPMManager.cpp
void MPMManager::Step(VkCommandBuffer cmd, float dt) {
    float sub_dt = dt / config_.substeps;

    for (uint32_t s = 0; s < config_.substeps; s++) {
        // 1. Zero Grid
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, zero_grid_pipeline_);
        vkCmdDispatch(cmd, (grid_size_ + 7) / 8, (grid_size_ + 7) / 8, (grid_size_ + 7) / 8);

        // 2. Apply Mouse Force (if dragging)
        if (interaction_manager_->IsDragging()) {
            ApplyMouseForce(cmd);
        }

        // 3. Compute Stress
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_stress_pipeline_);
        vkCmdDispatch(cmd, (num_particles_ + 255) / 256, 1, 1);

        VkMemoryBarrier barrier = { ... };
        vkCmdPipelineBarrier(cmd, ...);

        // 4. P2G
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p2g_pipeline_);
        vkCmdDispatch(cmd, (num_particles_ + 255) / 256, 1, 1);

        vkCmdPipelineBarrier(cmd, ...);

        // 5. Grid Update
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, grid_update_pipeline_);
        vkCmdDispatch(cmd, (grid_size_ + 7) / 8, (grid_size_ + 7) / 8, (grid_size_ + 7) / 8);

        // 6. G2P
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g2p_pipeline_);
        vkCmdDispatch(cmd, (num_particles_ + 255) / 256, 1, 1);

        vkCmdPipelineBarrier(cmd, ...);
    }
}
```

**验收标准**:
```bash
./build/tests/test_mpm_integration
# 预期:
# - 重力下落测试: 粒子y坐标减小
# - 能量守恒测试: 误差 < 5%
# - 边界条件: 冻结粒子不动
```

### Phase 2: 材料系统（1周）

**子阶段 2.1: 均匀材料（3天）**

**任务**:
- [ ] 创建 `src/mpm/MaterialSystem.h/cpp`
- [ ] 实现 `query_material.comp`
- [ ] 集成到初始化流程

**验收标准**:
- 所有粒子使用相同材料参数
- carnations使用 E=2.14 MPa

**子阶段 2.2: 梯度材料（4天，可选）**

**任务**:
- [ ] 实现 `query_material_gradient.comp`
- [ ] 添加GUI控制参数

**验收标准**:
- 材料参数随空间位置变化
- 可视化材料分布

### Phase 3: 用户交互（2周）

**子阶段 3.1: 射线投射系统（4天）**

**任务**:
- [ ] 创建 `src/interaction/RayCaster.h/cpp`
- [ ] 实现 `ray_cast_particles.comp`
- [ ] 集成到鼠标事件

**验收标准**:
```bash
./build/tests/test_ray_cast
# 预期: 拾取准确率 > 95%
```

**子阶段 3.2: 拖拽处理（5天）**

**任务**:
- [ ] 创建 `src/interaction/DragHandler.h/cpp`
- [ ] 实现 `apply_mouse_force.comp`
- [ ] 实现拖拽状态机

**验收标准**:
- 拖拽响应延迟 < 50ms
- 施加的力合理（不过大）

**子阶段 3.3: 可视化反馈（5天）**

**任务**:
- [ ] 创建 `src/interaction/DragVisualizer.h/cpp`
- [ ] 实现箭头几何生成
- [ ] 创建 arrow pipeline
- [ ] 集成到渲染管线

**验收标准**:
- 绿色箭头正确显示
- 箭头长度与力成正比

### Phase 4: 物理-渲染耦合（1周）

**子阶段 4.1: 位移映射（3天）**

**任务**:
- [ ] 完善 `map_displacement.comp`
- [ ] 创建 `update_gaussian_positions.comp`
- [ ] 实现CPU端映射逻辑

**验收标准**:
- 位移正确传递到高斯
- 渲染结果反映物理变形

**子阶段 4.2: 完整管线集成（4天）**

**任务**:
- [ ] 集成到主渲染循环
- [ ] 实现双缓冲/三缓冲同步
- [ ] 性能优化

**验收标准**:
```bash
# 运行完整场景
./build_msvc143/apps/viewer/Release/3dgs_viewer.exe \
  --camera camera.txt \
  D:\liuyue\physDreamerVulkanDemo\PhysDreamer\data\physics_dreamer\carnations\point_cloud.ply

# 预期:
# - FPS > 30
# - 拖拽产生可变形效果
# - 释放后弹性振荡
```

### Phase 5: GUI与调试（1周）

**任务**:
- [ ] 实现Physics Simulation窗口
- [ ] 实现Interaction Guide窗口
- [ ] 添加性能监控
- [ ] 添加调试可视化

**验收标准**:
- GUI显示所有关键参数
- 键盘快捷键工作正常
- 性能数据准确

### Phase 6: 集成与优化（1周）

**任务**:
- [ ] 端到端测试
- [ ] 性能优化（shared memory等）
- [ ] 稳定性测试
- [ ] 文档完善

**验收标准**:
- 长时间运行无崩溃
- 内存无泄漏
- FPS达标

---

## ⏱️ 性能监控系统实现

### Vulkan定时查询实现

为了准确测量MPM仿真的每帧用时，我们需要使用Vulkan的定时查询功能。

#### 性能监控类设计

```cpp
// src/utils/PerformanceMonitor.h
class PerformanceMonitor {
public:
    struct Metrics {
        // 帧级计时 (单位: 毫秒)
        float mpm_physics_time = 0.0f;      // MPM物理仿真总时间
        float render_time = 0.0f;           // 高斯渲染时间
        float coupling_time = 0.0f;         // 物理-渲染耦合时间
        float overhead_time = 0.0f;         // 其他开销
        float total_frame_time = 0.0f;      // 总帧时间

        // MPM细分计时
        float zero_grid_time = 0.0f;
        float stress_compute_time = 0.0f;
        float p2g_time = 0.0f;
        float grid_update_time = 0.0f;
        float g2p_time = 0.0f;
        float interaction_time = 0.0f;     // 鼠标交互处理

        // 统计数据
        uint32_t frame_count = 0;
        float fps = 0.0f;
        float mpm_fps = 0.0f;              // MPM物理FPS
    };

    void Initialize(VkDevice device, uint32_t num_frames_in_flight);
    void Shutdown();

    // 开始/结束查询（每帧调用）
    void BeginFrame(VkCommandBuffer cmd, uint32_t frame_index);
    void EndFrame(VkCommandBuffer cmd, uint32_t frame_index);

    // MPM阶段计时
    void BeginMPMPhysics(VkCommandBuffer cmd);
    void EndMPMPhysics(VkCommandBuffer cmd);

    void BeginMPMStage(VkCommandBuffer cmd, const std::string& stage_name);
    void EndMPMStage(VkCommandBuffer cmd, const std::string& stage_name);

    // 获取结果（在CPU端读取查询结果）
    bool GetResults(uint32_t frame_index, Metrics& out_metrics);

private:
    VkDevice device_;

    // Vulkan查询池
    VkQueryPool query_pool_ = VK_NULL_HANDLE;

    // 查询索引映射
    enum QueryIndex {
        FRAME_START = 0,
        MPM_PHYSICS_START,
        MPM_PHYSICS_END,
        RENDER_START,
        RENDER_END,
        COUPLING_START,
        COUPLING_END,
        FRAME_END,

        // MPM阶段查询
        ZERO_GRID_START,
        ZERO_GRID_END,
        STRESS_START,
        STRESS_END,
        P2G_START,
        P2G_END,
        GRID_UPDATE_START,
        GRID_UPDATE_END,
        G2P_START,
        G2P_END,
        INTERACTION_START,
        INTERACTION_END,

        MAX_QUERIES
    };

    // 每帧的查询结果（双缓冲，避免等待）
    std::array<uint64_t, MAX_QUERIES> queries_results_[2];
    uint32_t current_read_frame_ = 0;
};
```

#### 实现代码

```cpp
// src/utils/PerformanceMonitor.cpp
void PerformanceMonitor::Initialize(VkDevice device, uint32_t num_frames_in_flight) {
    device_ = device;

    // 创建定时查询池
    VkQueryPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = MAX_QUERIES * num_frames_in_flight,
        .flags = 0,
    };

    vkCreateQueryPool(device_, &pool_info, nullptr, &query_pool_);
}

void PerformanceMonitor::BeginFrame(VkCommandBuffer cmd, uint32_t frame_index) {
    uint32_t query_offset = frame_index * MAX_QUERIES;

    // 重置查询
    vkCmdResetQueryPool(cmd, query_pool_, query_offset, MAX_QUERIES);

    // 记录帧开始时间戳
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       query_pool_, query_offset + FRAME_START);
}

void PerformanceMonitor::BeginMPMPhysics(VkCommandBuffer cmd) {
    uint32_t frame_index = GetCurrentFrameIndex(); // 来自Renderer
    uint32_t query_offset = frame_index * MAX_QUERIES;

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       query_pool_, query_offset + MPM_PHYSICS_START);
}

void PerformanceMonitor::EndMPMPhysics(VkCommandBuffer cmd) {
    uint32_t frame_index = GetCurrentFrameIndex();
    uint32_t query_offset = frame_index * MAX_QUERIES;

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       query_pool_, query_offset + MPM_PHYSICS_END);
}

void PerformanceMonitor::BeginMPMStage(VkCommandBuffer cmd, const std::string& stage_name) {
    uint32_t frame_index = GetCurrentFrameIndex();
    uint32_t query_offset = frame_index * MAX_QUERIES;

    QueryIndex start_idx, end_idx;
    if (stage_name == "zero_grid") {
        start_idx = ZERO_GRID_START;
        end_idx = ZERO_GRID_END;
    } else if (stage_name == "stress") {
        start_idx = STRESS_START;
        end_idx = STRESS_END;
    }
    // ... 其他阶段

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       query_pool_, query_offset + start_idx);
}

void PerformanceMonitor::EndMPMStage(VkCommandBuffer cmd, const std::string& stage_name) {
    // 类似实现，使用END索引
}

void PerformanceMonitor::EndFrame(VkCommandBuffer cmd, uint32_t frame_index) {
    uint32_t query_offset = frame_index * MAX_QUERIES;

    // 记录帧结束时间戳
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       query_pool_, query_offset + FRAME_END);
}

bool PerformanceMonitor::GetResults(uint32_t frame_index, Metrics& out_metrics) {
    // 从前一帧读取结果（避免同步等待）
    uint32_t read_frame = (frame_index + 1) % 2;  // 双缓冲
    uint32_t query_offset = read_frame * MAX_QUERIES;

    // 获取查询结果
    VkResult result = vkGetQueryPoolResults(
        device_,
        query_pool_,
        query_offset,
        MAX_QUERIES,
        sizeof(queries_results_[read_frame]),
        queries_results_[read_frame].data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_WAIT_BIT  // 或 VK_QUERY_RESULT_WITH_AVAILABILITY_BIT 异步
    );

    if (result != VK_SUCCESS) {
        return false;
    }

    // 获取时间戳周期（纳秒）
    VkPhysicalDeviceProperties device_props;
    vkGetPhysicalDeviceProperties(physical_device, &device_props);
    float timestamp_period = device_props.limits.timestampPeriod;

    // 转换为毫秒
    auto to_ms = [&](uint64_t timestamp) -> float {
        return static_cast<float>(timestamp * timestamp_period) / 1e6f;
    };

    // 计算各阶段耗时
    out_metrics.mpm_physics_time = to_ms(
        queries_results_[read_frame][MPM_PHYSICS_END] -
        queries_results_[read_frame][MPM_PHYSICS_START]
    );

    out_metrics.total_frame_time = to_ms(
        queries_results_[read_frame][FRAME_END] -
        queries_results_[read_frame][FRAME_START]
    );

    // MPM细分阶段
    out_metrics.zero_grid_time = to_ms(
        queries_results_[read_frame][ZERO_GRID_END] -
        queries_results_[read_frame][ZERO_GRID_START]
    );

    out_metrics.stress_compute_time = to_ms(
        queries_results_[read_frame][STRESS_END] -
        queries_results_[read_frame][STRESS_START]
    );

    // ... 其他阶段

    return true;
}
```

#### 在Renderer中集成

```cpp
// src/Renderer.cpp
void Renderer::draw() {
    auto frame_start = std::chrono::high_resolution_clock::now();

    // === Physics Phase ===
    performance_monitor_->BeginFrame(cmd, currentFrameIndex);

    if (mpm_manager_ && mpm_manager_->IsEnabled()) {
        performance_monitor_->BeginMPMPhysics(cmd);

        // MPM子步进循环
        for (uint32_t s = 0; s < substeps; s++) {
            // 1. Zero Grid
            performance_monitor_->BeginMPMStage(cmd, "zero_grid");
            vkCmdBindPipeline(cmd, ...);
            vkCmdDispatch(cmd, ...);
            performance_monitor_->EndMPMStage(cmd, "zero_grid");

            // 2. Compute Stress
            performance_monitor_->BeginMPMStage(cmd, "stress");
            vkCmdBindPipeline(cmd, ...);
            vkCmdDispatch(cmd, ...);
            performance_monitor_->EndMPMStage(cmd, "stress");

            // 3. P2G
            performance_monitor_->BeginMPMStage(cmd, "p2g");
            vkCmdBindPipeline(cmd, ...);
            vkCmdDispatch(cmd, ...);
            performance_monitor_->EndMPMStage(cmd, "p2g");

            // 4. Grid Update
            performance_monitor_->BeginMPMStage(cmd, "grid_update");
            vkCmdBindPipeline(cmd, ...);
            vkCmdDispatch(cmd, ...);
            performance_monitor_->EndMPMStage(cmd, "grid_update");

            // 5. G2P
            performance_monitor_->BeginMPMStage(cmd, "g2p");
            vkCmdBindPipeline(cmd, ...);
            vkCmdDispatch(cmd, ...);
            performance_monitor_->EndMPMStage(cmd, "g2p");
        }

        performance_monitor_->EndMPMPhysics(cmd);
    }

    // === Coupling Phase ===
    performance_monitor_->BeginCoupling(cmd);
    // ... 映射位移到高斯
    performance_monitor_->EndCoupling(cmd);

    // === Render Phase ===
    performance_monitor_->BeginRender(cmd);
    // ... 高斯渲染
    performance_monitor_->EndRender(cmd);

    performance_monitor_->EndFrame(cmd, currentFrameIndex);

    // 获取前一帧的结果（用于GUI显示）
    PerformanceMonitor::Metrics metrics;
    if (performance_monitor_->GetResults(currentFrameIndex, metrics)) {
        // 更新GUI显示
        UpdateGUI(metrics);
    }

    advanceFrame();
}
```

### 简化实现（Phase 1推荐）

如果完整Vulkan查询过于复杂，可以先用CPU计时作为过渡方案：

```cpp
// 简化版本（CPU端计时，适用于Phase 1）
class SimplePerformanceMonitor {
public:
    struct Metrics {
        float mpm_physics_time = 0.0f;
        float render_time = 0.0f;
        float coupling_time = 0.0f;
    };

    void BeginMPMPhysics() {
        mpm_start_ = std::chrono::high_resolution_clock::now();
    }

    void EndMPMPhysics() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - mpm_start_);
        metrics_.mpm_physics_time = duration.count() / 1000.0f;
    }

    const Metrics& GetMetrics() const { return metrics_; }

private:
    std::chrono::high_resolution_clock::time_point mpm_start_;
    Metrics metrics_;
};
```

**优点**: 实现简单，快速验证
**缺点**: 精度较低，包含GPU-CPU同步延迟

### 实施建议

**Phase 1**: 使用简化CPU计时
- 快速集成功能
- 验证性能目标

**Phase 5**: 升级到Vulkan查询
- 获得精确GPU计时
- 支持详细的阶段细分

---

## 🔧 关键技术决策

### 决策1: B样条插值范围

**选项**:
- A. 2x2x2（线性插值，简单）
- B. 3x3x3（三次B样条，PhysDreamer一致）✅

**选择**: B - 3x3×3

**理由**:
- 与PhysDreamer保持一致，确保物理精度
- 性能影响可控（27次操作 vs 8次）
- 更好的数值稳定性

**实施**:
```glsl
// 3x3x3 循环
for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
        for (int k = 0; k < 3; k++) {
            // ... 插值计算
        }
    }
}
```

### 决策2: 应力计算算法

**选项**:
- A. 完整SVD分解（精确，昂贵）
- B. 简化极分解（快速，近似）
- C. Neo-Hookean模型（最简单）✅

**选择**: C - Neo-Hookean for Phase 1, B for Phase 2

**理由**:
- Phase 1优先保证系统集成
- Neo-Hookean无需SVD，性能最优
- Phase 2可升级到更精确模型

**实施**:
```cpp
// Phase 1: Neo-Hookean
mat3 stress = 2.0 * mu * (F - R) + lam * (J - 1.0) * J * inverse(transpose(F));

// Phase 2: FCR with SVD
// 使用Jacobi迭代或gpu-based SVD
```

### 决策3: 材料场实现

**选项**:
- A. 完整神经网络（不现实）
- B. 3D纹理查询（灵活，需要预处理）✅
- C. 程序化生成（简单，有限）

**选择**: 分阶段 - C → B

**理由**:
- Phase 1: 程序化梯度材料足够
- Phase 2: 添加3D纹理支持（可从Python导出）
- 避免运行时神经网络推理

### 决策4: 粒子-高斯映射策略

**选项**:
- A. 1对1映射（最简单，降采样后）
- B. Top-K加权（PhysDreamer一致）✅

**选择**: B - Top-K with K=8

**理由**:
- MPMManager::Initialize()已建立Top-K映射
- 位移插值更平滑
- 与PhysDreamer一致

### 决策5: 交互力施加方式

**选项**:
- A. 直接修改粒子速度（简单）
- B. 速度+位移约束（精确）
- C. 外力积分（物理正确）✅

**选择**: C - 外力积分

**理由**:
- 物理上最正确
- 支持弹性响应
- 与MPM框架一致

**实施**:
```cpp
// 每子步应用外力
for (int s = 0; s < substeps; s++) {
    // ... MPM steps
    ApplyMouseForce(cmd, mouse_force, sub_dt);
}
```

---

## 📊 性能预算

### 目标性能指标

| 场景 | 粒子数 | 高斯数 | 目标FPS | 物理时间预算 |
|------|--------|--------|---------|-------------|
| carnations | 32,703 | 1,037,279 | 30 | 10ms |
| small_scene | 10,000 | 100,000 | 60 | 5ms |
| large_scene | 50,000 | 2,000,000 | 20 | 15ms |

### 时间分解（carnations, 30 FPS目标）

```
总帧时间: 33.3ms
├── Physics Phase: 10ms
│   ├── Zero Grid: 0.1ms (64³ / 512 threads)
│   ├── Apply Mouse Force: 0.05ms (单粒子)
│   ├── Compute Stress: 2ms (32K particles)
│   ├── P2G: 3ms (32K × 27 neighbors)
│   ├── Grid Update: 0.5ms (64³)
│   └── G2P: 2ms (32K × 27 neighbors)
│   └── (×128 substeps，实际并行执行)
├── Render Phase: 18ms
│   ├── Map Displacement: 0.5ms
│   ├── Update Gaussian Positions: 0.3ms
│   └── Gaussian Rendering: 17ms (现有管线)
└── Overhead: 5.3ms
    ├── GPU-CPU sync: 2ms
    ├── barriers: 1ms
    └── driver overhead: 2.3ms
```

### 优化策略

1. **Shared Memory优化** (P2G/G2P)
```glsl
layout(local_size_x = 128) in;
shared ParticleData shared_particles[128];

// 减少全局内存访问
```

2. **Subgroup操作** (Ray Casting)
```glsl
float subgroup_min = subgroupMin(distance_sq);
```

3. **双缓冲/三缓冲** (隐藏延迟)
```cpp
// N帧执行物理，N+1帧渲染
```

---

## 🧪 验收测试用例

### 手动测试用例

**Test 1: 基础物理仿真**
```bash
# 启动carnations场景
./build_msvc143/apps/viewer/Release/3dgs_viewer.exe \
  --camera camera.txt \
  D:\liuyue\physDreamerVulkanDemo\PhysDreamer\data\physics_dreamer\carnations\point_cloud.ply

# 步骤:
1. 等待场景加载
2. 按H键切换到Physics模式
3. 点击"Enable"启用物理仿真
4. 观察: 粒子受重力轻微下落
5. 验证: FPS > 30
```

**Test 2: 交互式变形**
```bash
# 步骤:
1. 确保Physics模式已启用
2. Shift + 左键点击可变形区域
3. 拖拽鼠标，观察绿色箭头
4. 验证: 箭头方向与拖拽方向一致
5. 验证: 箭头长度随拖拽距离增加
6. 松开鼠标
7. 验证: 物体产生弹性振荡
8. 验证: 阻尼使物体逐渐回到原位
```

**Test 3: 边界条件**
```bash
# 步骤:
1. 切换到"Foreground Only"渲染模式
2. 观察背景是否静止
3. 施加变形
4. 验证: 仅前景高斯移动
```

### 自动化测试用例

```cpp
// tests/test_physics_integration.cpp

TEST(MPMPhysics, GravityFalling) {
    // 创建测试粒子（一列）
    std::vector<glm::vec3> positions;
    for (int i = 0; i < 10; i++) {
        positions.push_back(glm::vec3(0.0f, 0.1f * i, 0.0f));
    }

    // 初始化MPM
    mpm_manager_->LoadParticles(positions);
    mpm_manager_->Enable();

    // 运行100帧
    for (int frame = 0; frame < 100; frame++) {
        mpm_manager_->Step(cmd, 1.0f / 30.0f);
    }

    // 验证粒子已下落
    auto final_positions = mpm_manager_->GetPositions();
    EXPECT_LT(final_positions[0].y, positions[0].y);
}

TEST(MPMPhysics, ElasticOscillation) {
    // 创建弹性块
    CreateElasticBlock();

    // 施加脉冲力
    ApplyImpulseForce(glm::vec3(0, 1, 0), 10.0f);

    // 记录初始能量
    float initial_energy = ComputeTotalEnergy();

    // 运行50帧
    for (int frame = 0; frame < 50; frame++) {
        mpm_manager_->Step(cmd, 1.0f / 30.0f);
    }

    // 验证能量守恒（误差范围内）
    float final_energy = ComputeTotalEnergy();
    EXPECT_NEAR(final_energy, initial_energy, initial_energy * 0.05);
}

TEST(DragInteraction, ForceApplication) {
    // 测试拖拽力施加
    InitializeTestScene();

    // 模拟拖拽
    interaction_manager_->OnMouseDown(100, 100);
    interaction_manager_->OnMouseMove(150, 150);  // 拖拽50像素
    interaction_manager_->OnMouseUp();

    // 运行物理步进
    mpm_manager_->Step(cmd, 1.0f / 30.0f);

    // 验证粒子移动
    auto positions = mpm_manager_->GetPositions();
    EXPECT_GT(glm::length(positions[dragged_particle] - original_pos), 0.0f);
}
```

---

## 📝 开发注意事项

### 调试技巧

1. **使用Vulkan Validation Layers**
```bash
set VK_LAYER_PATH=C:/VulkanSDK/1.4.350.0/Bin
set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
```

2. **RenderDoc Capture**
- 捕获Compute Shader执行
- 查看Buffer内容
- 验证数值正确性

3. **CPU-GPU对比验证**
```cpp
#ifdef DEBUG_MODE
    auto cpu_result = ComputeMPMCPU(particles);
    auto gpu_result = ComputeMPMGPU(particles);
    float diff = CompareResults(cpu_result, gpu_result);
    assert(diff < 0.01, "CPU-GPU mismatch!");
#endif
```

### 常见问题与解决

| 问题 | 症状 | 解决方法 |
|------|------|---------|
| 粒子爆炸 | 位置变为NaN | 检查时间步长，减小dt |
| 性能过低 | FPS < 20 | 减少substeps，降低网格分辨率 |
| 渲染不同步 | 高斯位置滞后 | 检查GPU-CPU同步 |
| 内存泄漏 | 内存持续增长 | 使用Valgrind/RenderDoc检查 |

### Git工作流建议

```bash
# 每个Phase一个分支
git checkout -b phase/1-mpm-core
git checkout -b phase/3-user-interaction

# 每个子阶段一个commit
git commit -m "Phase 1.1: Upgrade B-spline to 3x3x3"
git commit -m "Phase 1.2: Implement stress computation shader"

# 定期推送到远程
git push origin phase/1-mpm-core
```

---

## 📚 参考资料

### PhysDreamer源码

- `physDreamer/warp_mpm/mpm_solver_diff.py` - MPM求解器
- `physDreamer/warp_mpm/mpm_utils.py` - MPM工具函数
- `physDreamer/warp_mpm/gaussian_sim_utils.py` - 初始化工具
- `projects/inference/demo.py` - 推理管线
- `projects/inference/local_utils.py` - 局部工具

### 学术资源

- [MPM Tutorial](https://yuanming.taichi.graphics/publication/2019-mpm-tutorial/) - Taichi MPM教程
- [APIC Paper](https://www.math.ucla.edu/~jteran/papers/Jiang2015.pdf) - APIC算法
- [FCR Material](https://doi.org/10.1111/cgf.12604) - Fixed Corotated模型

### Vulkan资源

- [Vulkan Tutorial](https://vulkan-tutorial.com/) - Vulkan教程
- [GPU Open](https://gpuopen.com/) - AMD GPU优化指南

---

## 🎯 总结

本设计方案详细规划了将PhysDreamer的MPM物理仿真改造为C++ Vulkan实现的完整路径：

**核心亮点**:
1. ✅ 完整保留PhysDreamer的3x3×3 B样条插值精度
2. ✅ 分阶段材料系统（均匀→梯度→纹理）
3. ✅ 直观的拖拽交互（绿色箭头可视化）
4. ✅ 弹性振荡+阻尼衰减的物理反馈
5. ✅ 8周可执行的实施路线图

**技术优势**:
- 纯Vulkan实现，无需Python/CUDA依赖
- 实时交互（30 FPS，carnations场景）
- 模块化设计，便于扩展和调试
- 与现有3dgs.cpp.opt无缝集成

**验收保证**:
- carnations场景端到端验证
- 自动化测试覆盖关键功能
- 性能预算确保用户体验

让我们开始实施吧，Joe！ 🚀
