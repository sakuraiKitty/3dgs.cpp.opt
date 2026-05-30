# Vulkan物理仿真与交互集成详细计划 (分阶段验收版)

## 📋 变更说明

**原计划风险分析**：
- 原VULAKN_PHYSICS_INTEGRATION_PLAN.md为总体规划，总计12-16周，风险等级：高
- 包含4个大阶段，但缺乏详细的子步骤拆分
- 每个阶段验收标准不够具体
- 若任何环节出错，需要回退整个阶段

**拆分方案**：
- 将4个大阶段拆分为12个独立可验收的子阶段
- 每个子阶段都可独立回退
- 可逐步验证，降低风险
- 为每个子阶段设计具体验收用例和量化指标

---

## 原计划对比

| 项目 | 原计划 | 拆分后计划 |
|------|--------|-----------|
| **子阶段数** | 4个 (1-4) | 12个 (1.1-4.3) |
| **总时间** | 12-16周 | 14-18周 |
| **风险** | 高 | 分散为低/中/高 |
| **验收点** | 4个（阶段末） | 12个（每子阶段） |
| **回退成本** | 回退整个阶段 | 仅回退问题子阶段 |
| **可并行** | 不可行 | 1.2和1.3可并行，2.1和2.2可并行 |

---

## 🎯 完整详细拆分计划

### 阶段1: MPM基础Compute Shader (4-5周)

#### 阶段1.1: 数据结构与初始化Shader (3-4天)

##### 目标
创建MPM所需的基础数据结构和初始化compute shader。

##### 实施步骤

**Step 1: 创建数据结构头文件** (1天)
```glsl
// src/shaders/mpm/mpm_structs.glsl
#ifndef MPM_STRUCTS_GLSL
#define MPM_STRUCTS_GLSL

// 粒子数据结构 (128 bytes)
struct ParticleData {
    vec3 position;              // 0-12   - 位置
    float mass;                 // 12-16  - 质量
    vec3 velocity;              // 16-28  - 速度
    uint freeze_flag;           // 28-32  - 冻结标志 (0=可动, 1=冻结)
    mat3 deformation_gradient;  // 32-80  - 变形梯度矩阵 (3xvec4)
    float volume;               // 80-84  - 体积
    uint material_id;           // 84-88  - 材料ID
    float density;               // 88-92  - 密度
    float youngs_modulus;       // 92-96  - 杨氏模量
    float poisson_ratio;        // 96-100 - 泊松比
    float padding[7];           // 100-128- 对齐到128字节
};

// 网格节点数据结构 (32 bytes)
struct GridNode {
    vec3 velocity;              // 0-12   - 速度
    float mass;                 // 12-16  - 质量
    vec3 force;                 // 16-28  - 力
    uint active_count;          // 28-32  - 活跃粒子计数
};

// 材料类型定义
const uint MATERIAL_JELLY = 0;
const uint MATERIAL_METAL = 1;
const uint MATERIAL_SAND = 2;
const uint MATERIAL_FOAM = 3;
const uint MATERIAL_SNOW = 4;

#endif // MPM_STRUCTS_GLSL
```

**Step 2: 粒子初始化Shader** (1-2天)
```glsl
// src/shaders/mpm/initialize_particles.comp
#version 460

#pragma include "mpm_structs.glsl"

layout(local_size_x = 256) in;

// 输入：原始点云位置
layout(std430, binding = 0) readonly buffer InputPositions {
    vec3 input_positions[];
};

// 输出：初始化的粒子数据
layout(std430, binding = 1) buffer ParticleBuffer {
    ParticleData particles[];
};

uniform InitUniforms {
    uint num_particles;
    float base_mass;
    float base_volume;
    uint default_material;
    vec3 bounding_box_min;
    vec3 bounding_box_max;
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    // 初始化粒子
    particles[p_id].position = input_positions[p_id];
    particles[p_id].mass = base_mass;
    particles[p_id].velocity = vec3(0.0);
    particles[p_id].freeze_flag = 0;  // 默认可动
    particles[p_id].deformation_gradient = mat3(1.0);  // 单位矩阵
    particles[p_id].volume = base_volume;
    particles[p_id].material_id = default_material;
    particles[p_id].density = 1000.0;  // 水的密度
    particles[p_id].youngs_modulus = 1e6;  // 1 MPa
    particles[p_id].poisson_ratio = 0.3;   // 典型值
}
```

**Step 3: 网格初始化Shader** (1天)
```glsl
// src/shaders/mpm/initialize_grid.comp
#version 460

#pragma include "mpm_structs.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

layout(std430, binding = 0) buffer GridBuffer {
    GridNode nodes[];
};

uniform GridUniforms {
    uint grid_size;  // 每个维度的网格数
    float grid_spacing;
};

void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    uint z = gl_GlobalInvocationID.z;

    if (x >= grid_size || y >= grid_size || z >= grid_size) return;

    uint linear_idx = x + y * grid_size + z * grid_size * grid_size;

    // 初始化网格节点
    nodes[linear_idx].velocity = vec3(0.0);
    nodes[linear_idx].mass = 0.0;
    nodes[linear_idx].force = vec3(0.0);
    nodes[linear_idx].active_count = 0;
}
```

**Step 4: CPU端管理器基础结构** (1天)
```cpp
// src/mpm/MPMManager.h
class MPMManager {
public:
    struct Config {
        uint32_t grid_size = 32;
        uint32_t max_particles = 50000;
        float dt = 1.0f / 60.0f;
        uint32_t substeps = 64;
        glm::vec3 gravity = glm::vec3(0.0f, -9.8f, 0.0f);
        float damping = 0.99f;
    };

    explicit MPMManager(std::shared_ptr<VulkanContext> context);
    
    void Initialize(const Config& config);
    void LoadParticles(const std::vector<glm::vec3>& positions);
    void InitializeGrid();

private:
    std::shared_ptr<VulkanContext> context_;
    
    // GPU缓冲区
    std::shared_ptr<Buffer> particle_buffer_;
    std::shared_ptr<Buffer> grid_buffer_;
    
    // Compute pipelines
    std::shared_ptr<ComputePipeline> init_particles_pipeline_;
    std::shared_ptr<ComputePipeline> init_grid_pipeline_;
    
    Config config_;
};
```

##### 验收标准
- ✅ 所有shader编译通过（无错误和警告）
- ✅ MPMStructs.h中的数据结构大小正确（ParticleData=128B, GridNode=32B）
- ✅ 粒子初始化shader输出正确的初始值
- ✅ 网格初始化shader清空所有网格节点

##### 验收方法
```bash
# 编译验证
glslc -V -o /dev/null src/shaders/mpm/mpm_structs.glsl
glslc -V -o /dev/null src/shaders/mpm/initialize_particles.comp
glslc -V -o /dev/null src/shaders/mpm/initialize_grid.comp

# 数据结构验证
# 运行测试程序验证数据大小
./build/tests/test_mpm_structures
# 预期输出：
#   ParticleData size: 128 bytes ✓
#   GridNode size: 32 bytes ✓
```

##### 验收用例
```cpp
// tests/test_mpm_structures.cpp
TEST(MPMDataStructures, VerifySizes) {
    EXPECT_EQ(sizeof(ParticleData), 128);
    EXPECT_EQ(sizeof(GridNode), 32);
}

TEST(MPMDataStructures, VerifyAlignment) {
    ParticleData p;
    EXPECT_EQ(reinterpret_cast<char*>(&p.mass) - reinterpret_cast<char*>(&p), 12);
    EXPECT_EQ(reinterpret_cast<char*>(&p.freeze_flag) - reinterpret_cast<char*>(&p), 28);
}
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| Shader编译成功率 | 100% | 编译日志 |
| 数据结构对齐 | 正确 | sizeof检查 |
| 内存布局 | 符合预期 | offset检查 |

##### 风险评估
- **风险**: 低
- **回退**: 删除新增的shader文件即可
- **回退成本**: 0.5小时

---

#### 阶段1.2: Zero Grid与P2G Shader实现 (4-5天)

##### 目标
实现MPM算法的清空网格和粒子到网格传递步骤。

##### 实施步骤

**Step 1: Zero Grid Shader** (1天)
```glsl
// src/shaders/mpm/zero_grid.comp
#version 460

#pragma include "mpm_structs.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

layout(std430, binding = 0) buffer GridBuffer {
    GridNode nodes[];
};

uniform GridUniforms {
    uint grid_size;
};

void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    uint z = gl_GlobalInvocationID.z;

    if (x >= grid_size || y >= grid_size || z >= grid_size) return;

    uint linear_idx = x + y * grid_size + z * grid_size * grid_size;

    // 清空网格节点
    nodes[linear_idx].velocity = vec3(0.0);
    nodes[linear_idx].mass = 0.0;
    nodes[linear_idx].force = vec3(0.0);
    nodes[linear_idx].active_count = 0;
}
```

**Step 2: B样条插值函数库** (1天)
```glsl
// src/shaders/mpm/mpm_bspline.glsl
#ifndef MPM_BSPLINE_GLSL
#define MPM_BSPLINE_GLSL

// B样条基函数及其导数
void bspline(float x, out float value, out float derivative) {
    float x_abs = abs(x);
    float x_abs_sq = x_abs * x_abs;
    
    if (x_abs < 1.0) {
        value = 0.5 * x_abs_sq - x_abs + 0.5;
        derivative = x - sign(x);
    } else if (x_abs < 2.0) {
        float temp = 2.0 - x_abs;
        value = 0.5 * temp * temp;
        derivative = -sign(x) * (2.0 - x_abs);
    } else {
        value = 0.0;
        derivative = 0.0;
    }
}

// 计算插值权重和梯度
void compute_weights_and_gradients(
    vec3 offset,
    out vec3 weights[2],
    out vec3 gradients[2]
) {
    for (int i = 0; i < 2; i++) {
        float dim_coord = offset[i] - float(i);
        bspline(dim_coord, weights[i].x, gradients[i].x);
    }
    // 对y和z重复
}
```

**Step 3: P2G Shader** (2-3天)
```glsl
// src/shaders/mpm/p2g.comp
#version 460

#pragma include "mpm_structs.glsl"
#pragma include "mpm_bspline.glsl"

layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer ParticleBuffer {
    ParticleData particles[];
};

layout(std430, binding = 1) buffer GridBuffer {
    GridNode nodes[];
};

uniform P2GUniforms {
    float dt;
    uint grid_size;
    uint num_particles;
    float grid_spacing;
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    ParticleData p = particles[p_id];

    // 计算网格位置
    vec3 grid_pos = p.position / grid_spacing;
    ivec3 base_node = ivec3(floor(grid_pos - 0.5));

    // 2x2x2邻居节点
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 2; k++) {
                ivec3 node_idx = base_node + ivec3(i, j, k);
                vec3 offset = grid_pos - vec3(node_idx);

                // 计算插值权重
                vec3 weights[2], gradients[2];
                compute_weights_and_gradients(offset, weights, gradients);

                float w = weights[i].x * weights[j].y * weights[k].z;

                // 边界检查
                if (any(lessThan(node_idx, ivec3(0))) || 
                    any(greaterThanEqual(node_idx, ivec3(int(grid_size))))) {
                    continue;
                }

                uint linear_idx = node_idx.x + node_idx.y * grid_size + 
                                 node_idx.z * grid_size * grid_size;

                // 原子操作累加
                atomicAdd(nodes[linear_idx].mass, p.mass * w);
                atomicAdd(nodes[linear_idx].velocity.x, p.velocity.x * p.mass * w);
                atomicAdd(nodes[linear_idx].velocity.y, p.velocity.y * p.mass * w);
                atomicAdd(nodes[linear_idx].velocity.z, p.velocity.z * p.mass * w);
                atomicAdd(nodes[linear_idx].active_count, 1);
            }
        }
    }
}
```

##### 验收标准
- ✅ Zero Grid shader清空所有网格节点
- ✅ P2G shader正确传递粒子质量和动量到网格
- ✅ 原子操作无竞争条件
- ✅ 数学验证：网格总质量 = 粒子总质量

##### 验收方法
```bash
# 编译验证
glslc -V -o /dev/null src/shaders/mpm/zero_grid.comp
glslc -V -o /dev/null src/shaders/mpm/p2g.comp

# 数学验证测试
./build/tests/test_p2g_math
# 预期输出：
#   Mass conservation: PASS (error < 0.01%)
#   Momentum conservation: PASS (error < 0.1%)
```

##### 验收用例
```cpp
// tests/test_p2g_math.cpp
TEST(P2G, MassConservation) {
    // 创建测试粒子
    std::vector<ParticleData> particles = create_test_particles(100);
    
    // 运行P2G
    mpm_manager_->RunP2G(particles);
    
    // 计算总质量
    float particle_mass = sum_particle_mass(particles);
    float grid_mass = sum_grid_mass();
    
    EXPECT_NEAR(particle_mass, grid_mass, particle_mass * 0.0001);
}

TEST(P2G, MomentumConservation) {
    // 类似测试动量守恒
}
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 质量守恒误差 | < 0.01% | 单元测试 |
| 动量守恒误差 | < 0.1% | 单元测试 |
| Shader编译 | 通过 | 编译日志 |

##### 风险评估
- **风险**: 中
- **回退**: 删除shader文件
- **回退成本**: 1小时

---

#### 阶段1.3: Grid Update与G2P Shader实现 (4-5天)

##### 目标
实现网格更新和网格到粒子传递步骤。

##### 实施步骤

**Step 1: Grid Update Shader** (1-2天)
```glsl
// src/shaders/mpm/grid_update.comp
#version 460

#pragma include "mpm_structs.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

layout(std430, binding = 0) buffer GridBuffer {
    GridNode nodes[];
};

uniform GridUpdateUniforms {
    uint grid_size;
    vec3 gravity;
    float dt;
    float damping;
};

void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    uint z = gl_GlobalInvocationID.z;

    if (x >= grid_size || y >= grid_size || z >= grid_size) return;

    uint linear_idx = x + y * grid_size + z * grid_size * grid_size;
    float mass = nodes[linear_idx].mass;

    if (mass > 0.0) {
        // 归一化速度
        vec3 velocity = nodes[linear_idx].velocity / mass;

        // 添加重力
        velocity += gravity * dt;

        // 添加阻尼
        velocity *= damping;

        nodes[linear_idx].velocity = velocity;
    }
}
```

**Step 2: G2P Shader** (2-3天)
```glsl
// src/shaders/mpm/g2p.comp
#version 460

#pragma include "mpm_structs.glsl"
#pragma include "mpm_bspline.glsl"

layout(local_size_x = 256) in;

layout(std430, binding = 0) buffer ParticleBuffer {
    ParticleData particles[];
};

layout(std430, binding = 1) readonly buffer GridBuffer {
    GridNode nodes[];
};

uniform G2PUniforms {
    float dt;
    uint grid_size;
    uint num_particles;
    float grid_spacing;
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    // 从网格插值速度
    vec3 grid_pos = particles[p_id].position / grid_spacing;
    ivec3 base_node = ivec3(floor(grid_pos - 0.5));

    vec3 new_velocity = vec3(0.0);

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 2; k++) {
                ivec3 node_idx = base_node + ivec3(i, j, k);

                // 边界检查
                if (any(lessThan(node_idx, ivec3(0))) || 
                    any(greaterThanEqual(node_idx, ivec3(int(grid_size))))) {
                    continue;
                }

                uint linear_idx = node_idx.x + node_idx.y * grid_size + 
                                 node_idx.z * grid_size * grid_size;

                vec3 offset = grid_pos - vec3(node_idx);
                vec3 weights[2], gradients[2];
                compute_weights_and_gradients(offset, weights, gradients);

                float w = weights[i].x * weights[j].y * weights[k].z;
                new_velocity += nodes[linear_idx].velocity * w;
            }
        }
    }

    // 更新粒子
    if (particles[p_id].freeze_flag == 0) {
        particles[p_id].velocity = new_velocity;
        particles[p_id].position += new_velocity * dt;
    }
}
```

##### 验收标准
- ✅ Grid Update正确归一化速度并应用重力
- ✅ G2P正确从网格插值速度
- ✅ 粒子位置更新合理
- ✅ 冻结粒子保持不变

##### 验收方法
```bash
# 数学验证
./build/tests/test_grid_update
./build/tests/test_g2p

# 可视化测试（可选）
./build/tools/visualize_particles --input test.ply --output result.ply
```

##### 验收用例
```cpp
// tests/test_grid_update.cpp
TEST(GridUpdate, VelocityNormalization) {
    // 创建非零质量的网格节点
    setup_test_grid();
    
    // 运行grid update
    mpm_manager_->RunGridUpdate();
    
    // 验证速度 = 原动量/质量
    vec3 expected_vel = original_momentum / mass;
    EXPECT_NEAR(grid_node.velocity, expected_vel, 1e-5);
}

TEST(GridUpdate, GravityApplication) {
    // 验证重力被正确应用
}

// tests/test_g2p.cpp
TEST(G2P, VelocityInterpolation) {
    // 验证速度插值正确性
}

TEST(G2P, FrozenParticlesUnchanged) {
    // 验证冻结粒子不受影响
}
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 速度归一化误差 | < 1e-5 | 单元测试 |
| 重力应用正确 | 符合物理 | 单元测试 |
| 插值精度 | < 0.1% | 单元测试 |
| 冻结粒子稳定 | 位置不变 | 单元测试 |

##### 风险评估
- **风险**: 中
- **回退**: 删除shader文件
- **回退成本**: 2小时

---

#### 阶段1.4: MPM管线集成与测试 (3-4天)

##### 目标
将所有MPM shader集成到管线中，进行端到端测试。

##### 实施步骤

**Step 1: MPM管理器完整实现** (2天)
```cpp
// src/mpm/MPMManager.cpp
void MPMManager::Initialize(const Config& config) {
    config_ = config;
    
    // 创建缓冲区
    CreateParticleBuffer();
    CreateGridBuffer();
    
    // 创建pipeline
    CreatePipelines();
}

void MPMManager::Step(VkCommandBuffer cmd) {
    // MPM子步进循环
    float sub_dt = config_.dt / config_.substeps;
    
    for (uint32_t s = 0; s < config_.substeps; s++) {
        // 1. Zero Grid
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 
                          zero_grid_pipeline_);
        vkCmdDispatch(cmd, 
                     (config_.grid_size + 7) / 8,
                     (config_.grid_size + 7) / 8,
                     (config_.grid_size + 7) / 8);
        
        // 2. P2G
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p2g_pipeline_);
        vkCmdDispatch(cmd, (num_particles_ + 255) / 256, 1, 1);
        
        // Memory barrier
        VkMemoryBarrier barrier = {
            .srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd, ...);
        
        // 3. Grid Update
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 
                          grid_update_pipeline_);
        vkCmdDispatch(cmd, 
                     (config_.grid_size + 7) / 8,
                     (config_.grid_size + 7) / 8,
                     (config_.grid_size + 7) / 8);
        
        // 4. G2P
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g2p_pipeline_);
        vkCmdDispatch(cmd, (num_particles_ + 255) / 256, 1, 1);
        
        // Barrier for next substep
        vkCmdPipelineBarrier(cmd, ...);
    }
}
```

**Step 2: 集成测试程序** (1-2天)
```cpp
// tests/integration_test_mpm.cpp
class MPMIntegrationTest {
public:
    void RunSimpleCollapse() {
        // 创建一列粒子
        std::vector<vec3> positions;
        for (int i = 0; i < 10; i++) {
            positions.push_back(vec3(0.0, 0.1 * i, 0.0));
        }
        
        mpm_manager_->LoadParticles(positions);
        
        // 运行100帧
        for (int frame = 0; frame < 100; frame++) {
            mpm_manager_->Step(cmd);
        }
        
        // 验证粒子已下落
        auto final_positions = mpm_manager_->GetPositions();
        EXPECT_LT(final_positions[0].y, positions[0].y);
    }
    
    void RunEnergyConservation() {
        // 创建弹性系统
        CreateElasticBlock();
        
        float initial_energy = ComputeTotalEnergy();
        
        // 运行若干步
        for (int frame = 0; frame < 50; frame++) {
            mpm_manager_->Step(cmd);
        }
        
        float final_energy = ComputeTotalEnergy();
        
        // 能量应该守恒（误差范围内）
        EXPECT_NEAR(final_energy, initial_energy, initial_energy * 0.05);
    }
};
```

##### 验收标准
- ✅ MPM管线成功创建并运行
- ✅ 粒子在重力作用下正确下落
- ✅ 能量守恒（弹性系统）
- ✅ 性能：50K粒子 > 10 FPS

##### 验收方法
```bash
# 集成测试
./build/tests/integration_test_mpm

# 性能测试
./build/tests/performance_test_mpm --particles 50000
```

##### 验收用例
```cpp
// tests/integration_test_mpm.cpp
TEST(MPMIntegration, SimpleCollapse) {
    // 验证粒子下落
}

TEST(MPMIntegration, ElasticBounce) {
    // 验证弹性碰撞
}

TEST(MPMIntegration, EnergyConservation) {
    // 验证能量守恒
}

TEST(MPMIntegration, BoundaryConditions) {
    // 验证边界条件
}
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 重力下落正确 | y坐标减小 | 集成测试 |
| 能量守恒误差 | < 5% | 集成测试 |
| 50K粒子性能 | > 10 FPS | 性能测试 |
| 内存无泄漏 | 无泄漏 | Valgrind |

##### 风险评估
- **风险**: 高
- **回退**: 禁用MPM功能
- **回退成本**: 4小时

---

### 阶段2: 用户交互系统 (3-4周)

#### 阶段2.1: 射线投射系统 (5-7天)

##### 目标
实现从屏幕坐标到3D世界的射线投射，用于粒子拾取。

##### 实施步骤

**Step 1: 射线投射Compute Shader** (2-3天)
```glsl
// src/shaders/interaction/ray_cast.comp
#version 460

layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer ParticleBuffer {
    vec3 positions[];
};

layout(std430, binding = 1) buffer ResultBuffer {
    uint closest_particle_idx;
    float closest_distance_sq;
};

uniform RayCastUniforms {
    vec3 ray_origin;
    vec3 ray_direction;
    uint num_particles;
    float max_distance;
    float max_distance_sq;
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    vec3 particle_pos = positions[p_id];

    // 计算点到射线距离
    vec3 v = particle_pos - ray_origin;
    float projection = dot(v, ray_direction);
    vec3 closest_point = ray_origin + ray_direction * projection;
    vec3 diff = particle_pos - closest_point;
    float distance_sq = dot(diff, diff);

    // 原子最小操作
    if (distance_sq < max_distance_sq) {
        uint old_idx;
        float old_dist_sq;
        
        // 尝试更新最近粒子
        // (需要使用atomicMin的float版本或手动实现)
        if (distance_sq < closest_distance_sq) {
            // 使用atomic exchange保证一致性
            // 简化版本：使用子组操作
            uint subgroup_local_id = gl_SubgroupInvocationID;
            uint subgroup_closest_id = p_id;
            float subgroup_min_dist = distance_sq;
            
            // 子组内找到最小值
            subgroup_min_dist = subgroupMin(subgroup_min_dist);
            
            if (distance_sq == subgroup_min_dist) {
                // 这个线程有最小值
                closest_particle_idx = p_id;
                closest_distance_sq = distance_sq;
            }
        }
    }
}
```

**Step 2: CPU端射线计算** (1天)
```cpp
// src/interaction/RayCaster.cpp
glm::vec3 RayCaster::ScreenToWorldRay(int screen_x, int screen_y) {
    // 屏幕坐标转NDC
    float ndc_x = (2.0f * screen_x) / width_ - 1.0f;
    float ndc_y = 1.0f - (2.0f * screen_y) / height_;

    // NDC转世界坐标
    glm::vec4 clip_coords(ndc_x, ndc_y, -1.0f, 1.0f);
    glm::vec4 eye_coords = glm::inverse(proj_matrix_) * clip_coords;
    eye_coords = glm::vec4(eye_coords.x, eye_coords.y, -1.0f, 0.0f);

    glm::vec4 world_coords = glm::inverse(view_matrix_) * eye_coords;
    glm::vec3 ray_dir = glm::normalize(glm::vec3(world_coords));

    return ray_dir;
}

uint32_t RayCaster::FindClosestParticle(int screen_x, int screen_y) {
    glm::vec3 ray_origin = camera_position_;
    glm::vec3 ray_dir = ScreenToWorldRay(screen_x, screen_y);

    // 更新uniform buffer
    ray_cast_uniforms_.ray_origin = ray_origin;
    ray_cast_uniforms_.ray_direction = ray_dir;

    // 运行compute shader
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, 
                      ray_cast_pipeline_);
    vkCmdDispatch(cmd_, (num_particles_ + 255) / 256, 1, 1);

    // 读取结果
    uint32_t result;
    vkCmdCopyBuffer(cmd_, result_buffer_, staging_buffer_);
    
    // 等待并读取
    vkQueueSubmit(queue_, ...);
    vkWaitForFences(device_, fence_, ...);
    
    void* data;
    vmaMapMemory(allocator_, staging_allocation_, &data);
    result = static_cast<uint32_t*>(data)[0];
    vmaUnmapMemory(allocator_, staging_allocation_);

    return result;
}
```

**Step 3: 交互管理器基础** (1-2天)
```cpp
// src/interaction/InteractionManager.h
class InteractionManager {
public:
    void Initialize(std::shared_ptr<MPMManager> mpm_manager);
    void OnMouseDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseUp();

private:
    std::shared_ptr<MPMManager> mpm_manager_;
    std::shared_ptr<RayCaster> ray_caster_;
    
    bool dragging_ = false;
    uint32_t dragged_particle_ = UINT32_MAX;
    glm::vec3 drag_start_pos_;
};
```

##### 验收标准
- ✅ 射线投射正确找到粒子
- ✅ 屏幕坐标转换正确
- ✅ 性能：50K粒子 < 10ms
- ✅ 边界情况正确处理

##### 验收方法
```bash
# 单元测试
./build/tests/test_ray_cast

# 可视化测试
./build/tools/visualize_ray_cast --scene test.ply
# 点击屏幕，验证高亮的粒子
```

##### 验收用例
```cpp
// tests/test_ray_cast.cpp
TEST(RayCast, ScreenToWorldConversion) {
    // 测试屏幕坐标转射线
    vec3 ray = ray_caster->ScreenToWorldRay(960, 540);  // 屏幕中心
    
    // 射线应该从相机位置发出
    EXPECT_NEAR(glm::length(ray), 1.0f, 1e-5);  // 单位向量
}

TEST(RayCast, FindClosestParticle) {
    // 创建测试场景
    std::vector<vec3> particles = {
        vec3(0, 0, 0),
        vec3(1, 0, 0),
        vec3(2, 0, 0)
    };
    
    // 设置相机看向原点
    // 射线投射应该找到最近的粒子
    
    uint32_t closest = ray_caster->FindClosestParticle(960, 540);
    EXPECT_NE(closest, UINT32_MAX);
}

TEST(RayCast, Performance) {
    // 性能测试：50K粒子
    CreateLargeParticleScene(50000);
    
    auto start = std::chrono::high_resolution_clock::now();
    uint32_t result = ray_caster->FindClosestParticle(960, 540);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    EXPECT_LT(duration.count(), 10000);  // < 10ms
}
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 射线转换精度 | < 1e-5 | 单元测试 |
| 拾取准确率 | > 95% | 可视化测试 |
| 50K粒子性能 | < 10ms | 性能测试 |
| 边界处理 | 无崩溃 | 边界测试 |

##### 风险评估
- **风险**: 低-中
- **回退**: 禁用交互功能
- **回退成本**: 2小时

---

#### 阶段2.2: 拖拽变形实现 (5-7天)

##### 目标
实现粒子拖拽和物理约束应用。

##### 实施步骤

**Step 1: 位移约束Shader** (2-3天)
```glsl
// src/shaders/interaction/apply_displacement.comp
#version 460

layout(local_size_x = 1) in;

layout(std430, binding = 0) buffer ParticleBuffer {
    ParticleData particles[];
};

uniform DisplacementUniforms {
    uint particle_idx;
    vec3 target_position;
    float stiffness;
    float max_velocity;
};

void main() {
    if (gl_GlobalInvocationID.x != particle_idx) return;

    vec3 current_pos = particles[particle_idx].position;
    vec3 displacement = target_position - current_pos;

    // PD控制：计算期望速度
    vec3 desired_velocity = displacement * stiffness * 60.0f;  // 60 FPS

    // 限制最大速度
    float speed = length(desired_velocity);
    if (speed > max_velocity) {
        desired_velocity = normalize(desired_velocity) * max_velocity;
    }

    particles[particle_idx].velocity = desired_velocity;
}
```

**Step 2: 交互管理器完善** (2-3天)
```cpp
// src/interaction/InteractionManager.cpp
void InteractionManager::OnMouseDown(int x, int y) {
    // 查找最近粒子
    uint32_t closest = ray_caster_->FindClosestParticle(x, y);
    
    if (closest != UINT32_MAX) {
        dragged_particle_ = closest;
        dragging_ = true;
        
        // 获取粒子当前位置作为拖拽起点
        auto positions = mpm_manager_->GetPositions();
        drag_start_pos_ = positions[closest];
    }
}

void InteractionManager::OnMouseMove(int x, int y) {
    if (!dragging_ || dragged_particle_ == UINT32_MAX) return;

    // 计算新的目标位置
    glm::vec3 ray_dir = ray_caster_->ScreenToWorldRay(x, y);
    
    // 在拖拽平面上移动（简化：使用球面投影）
    glm::vec3 target = drag_start_pos_ + ray_dir * 0.1f;  // 简化版本
    
    // 应用位移约束
    mpm_manager_->ApplyDisplacementConstraint(dragged_particle_, target);
}

void InteractionManager::OnMouseUp() {
    dragging_ = false;
    dragged_particle_ = UINT32_MAX;
}
```

##### 验收标准
- ✅ 可以拖拽粒子
- ✅ 拖拽响应延迟 < 50ms
- ✅ 物理行为合理
- ✅ 多粒子支持

##### 验收方法
```bash
# 手动测试
./build/apps/viewer/3dgs_viewer --scene test.ply
# 测试操作：
# 1. 点击粒子
# 2. 拖拽
# 3. 释放
# 4. 验证物理响应

# 自动化测试
./build/tests/test_drag_interaction
```

##### 验收用例
```cpp
// tests/test_drag_interaction.cpp
TEST(DragInteraction, SingleParticle) {
    // 测试单个粒子拖拽
    CreateSingleParticleScene();
    
    // 模拟鼠标事件
    interaction_manager_->OnMouseDown(100, 100);
    interaction_manager_->OnMouseMove(150, 150);  // 拖拽
    interaction_manager_->OnMouseUp();
    
    // 运行物理步进
    mpm_manager_->Step(cmd);
    
    // 验证粒子移动
    auto positions = mpm_manager_->GetPositions();
    EXPECT_GT(glm::length(positions[0] - original_pos), 0.0f);
}

TEST(DragInteraction, ResponseTime) {
    // 测试响应时间
    auto start = std::chrono::high_resolution_clock::now();
    
    interaction_manager_->OnMouseDown(100, 100);
    interaction_manager_->OnMouseMove(150, 150);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    EXPECT_LT(duration.count(), 50);  // < 50ms
}

TEST(DragInteraction, PhysicalBehavior) {
    // 测试物理行为是否合理
}
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 拖拽响应时间 | < 50ms | 性能测试 |
| 物理合理性 | 符合预期 | 可视化检查 |
| 粒子跟随准确度 | > 90% | 自动化测试 |

##### 风险评估
- **风险**: 中
- **回退**: 禁用拖拽功能
- **回退成本**: 3小时

---

#### 阶段2.3: 相机控制与UI (3-4天)

##### 目标
实现Orbit相机控制基础UI。

##### 实施步骤

**Step 1: 相机控制器** (2天)
```cpp
// src/interaction/CameraController.h
class CameraController {
public:
    void OnRightDrag(float dx, float dy);
    void OnScroll(float delta);
    void UpdateCameraMatrix();
    
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjMatrix() const;

private:
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float distance_ = 2.0f;
    glm::vec3 target_ = glm::vec3(0.0f);
};
```

**Step 2: 基础UI显示** (1-2天)
```cpp
// 显示FPS、粒子数等信息
void Renderer::DrawUI() {
    ImGui::Begin("Physics Info");
    ImGui::Text("FPS: %.1f", fps_);
    ImGui::Text("Particles: %d", num_particles_);
    ImGui::Text("Substeps: %d", substeps_);
    ImGui::Separator();
    ImGui::Text("Controls:");
    ImGui::Text("  Left Drag: Deform");
    ImGui::Text("  Right Drag: Rotate Camera");
    ImGui::Text("  Scroll: Zoom");
    ImGui::Text("  Space: Pause");
    ImGui::Text("  R: Reset");
    ImGui::End();
}
```

##### 验收标准
- ✅ 相机旋转流畅
- ✅ 缩放功能正常
- ✅ UI显示正确信息
- ✅ 性能无明显影响

##### 验收方法
```bash
# 手动测试
./build/apps/viewer/3dgs_viewer --scene test.ply
# 测试相机控制
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 相机旋转平滑度 | 无卡顿 | 手动测试 |
| UI显示准确性 | 正确 | 视觉检查 |
| 性能影响 | < 5% | 性能对比 |

##### 风险评估
- **风险**: 低
- **回退**: 使用简单相机
- **回退成本**: 1小时

---

### 阶段3: 材料场与优化 (3-4周)

#### 阶段3.1: 简化材料场 (5-7天)

##### 目标
实现基于3D纹理的空间变化材料参数。

##### 实施步骤

**Step 1: 材料场类实现** (2-3天)
```cpp
// src/mpm/MaterialField.h
class MaterialField {
public:
    void Initialize(uint32_t resolution);
    float QueryYoungsModulus(const glm::vec3& position);
    void SetYoungsModulus(const glm::vec3& position, float value);
    void LoadFromFile(const std::string& path);

private:
    VkImage texture_3d_;
    VkImageView texture_view_;
    VkSampler sampler_;
    uint32_t resolution_;
};
```

**Step 2: 材料查询Shader** (2天)
```glsl
// src/shaders/mpm/query_material.comp
#version 460

layout(binding = 0) uniform sampler3D material_field;

layout(std430, binding = 0) buffer ParticleBuffer {
    ParticleData particles[];
};

uniform MaterialQueryUniforms {
    uint num_particles;
};

void main() {
    uint p_id = gl_GlobalInvocationID.x;
    if (p_id >= num_particles) return;

    // 归一化位置到[0,1]
    vec3 uv = particles[p_id].position * 0.5f + 0.5f;
    
    // 查询材料参数
    float youngs_modulus = texture(material_field, uv).r * 5e8;  // 缩放到合理范围
    
    particles[p_id].youngs_modulus = youngs_modulus;
}
```

##### 验收标准
- ✅ 可以创建和查询材料场
- ✅ 材料参数在空间中变化
- ✅ 可视化材料场正确

##### 验收方法
```bash
# 单元测试
./build/tests/test_material_field

# 可视化测试
./build/tools/visualize_material_field --field material.dat
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 查询精度 | < 1% | 单元测试 |
| 空间变化正确 | 符合设计 | 可视化检查 |
| 性能影响 | < 10% | 性能测试 |

##### 风险评估
- **风险**: 中
- **回退**: 使用均匀材料
- **回退成本**: 2小时

---

#### 阶段3.2: 性能监控与调试 (4-5天)

##### 目标
实现性能监控和调试工具。

##### 实施步骤

**Step 1: 性能监控器** (2-3天)
```cpp
// src/utils/PerformanceMonitor.h
class PerformanceMonitor {
public:
    void Update();
    void DrawUI();
    void StartFrame();
    void EndFrame();
    
    struct Metrics {
        float fps;
        float physics_time_ms;
        float render_time_ms;
        float interaction_time_ms;
        uint32_t particle_count;
    };

private:
    Metrics metrics_;
    std::deque<float> fps_history_;
};
```

**Step 2: 调试可视化** (1-2天)
```glsl
// 可视化网格、粒子速度等
// src/shaders/debug/visualize_grid.comp
```

##### 验收标准
- ✅ 性能数据准确
- ✅ UI显示清晰
- ✅ 调试工具可用

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 性能数据准确性 | 误差<5% | 对比验证 |
| UI可用性 | 清晰易读 | 手动测试 |

##### 风险评估
- **风险**: 低
- **回退**: 删除UI
- **回退成本**: 1小时

---

#### 阶段3.3: 算法优化 (5-7天)

##### 目标
优化MPM算法性能。

##### 实施步骤

**Step 1: Shared memory优化** (2-3天)
```glsl
// 优化P2G/G2P的内存访问
layout(local_size_x = 128) in;
shared float shared_mass[128];
shared vec3 shared_velocity[128];
```

**Step 2: 向量化** (2天)
```glsl
// 使用向量化指令
vec4 process_color_vec4(vec3 color) {
    return vec4(color, 1.0);
}
```

**Step 3: 自适应参数** (1-2天)
```cpp
// 根据性能自动调整参数
void MPMManager::AdaptParameters() {
    float fps = performance_monitor_->GetCurrentFPS();
    
    if (fps < 30.0f) {
        // 降低质量
        config_.substeps = std::max(32, config_.substeps / 2);
        config_.grid_size = std::max(16, config_.grid_size / 2);
    } else if (fps > 60.0f) {
        // 提升质量
        config_.substeps = std::min(256, config_.substeps * 2);
    }
}
```

##### 验收标准
- ✅ FPS提升 > 20%
- ✅ 物理精度保持
- ✅ 自适应调整合理

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| FPS提升 | > 20% | 性能测试 |
| 物理精度误差 | < 5% | 对比测试 |
| 自适应响应 | 合理 | 手动测试 |

##### 风险评估
- **风险**: 中
- **回退**: 禁用优化
- **回退成本**: 4小时

---

### 阶段4: 高斯-物理耦合 (2-3周)

#### 阶段4.1: 粒子到高斯映射 (5-7天)

##### 目标
实现MPM粒子位移到3D高斯的映射。

##### 实施步骤

**Step 1: 预计算K近邻** (2-3天)
```cpp
// src/coupling/GaussianParticleMapper.h
class GaussianParticleMapper {
public:
    void PrecomputeMapping(
        const std::vector<glm::vec3>& gaussians,
        const std::vector<glm::vec3>& particles,
        uint32_t k = 8
    );

private:
    // 每个高斯的K个最近粒子索引
    std::vector<std::array<uint32_t, 8>> gaussian_to_particle_;
};
```

**Step 2: 位移映射Shader** (2-3天)
```glsl
// src/shaders/coupling/map_particle_to_gaussian.comp
#version 460

layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer ParticleBuffer {
    vec3 particle_positions[];
    vec3 particle_displacements[];
};

layout(std430, binding = 1) readonly buffer MappingBuffer {
    uvec4 top_k_indices[];  // 每个高斯4个uint32存K个索引
};

layout(std430, binding = 2) buffer GaussianBuffer {
    vec4 gaussian_positions[];  // xyz=位置, w=padding
};

uniform MappingUniforms {
    uint num_gaussians;
    uint k;
};

void main() {
    uint g_id = gl_GlobalInvocationID.x;
    if (g_id >= num_gaussians) return;

    // 读取K近邻
    vec3 avg_displacement = vec3(0.0);
    
    for (uint i = 0; i < k; i++) {
        uint p_idx = top_k_indices[g_id][i / 4][i % 4];  // 从uvec4数组读取
        avg_displacement += particle_displacements[p_idx];
    }
    
    avg_displacement /= float(k);
    
    // 应用到高斯
    gaussian_positions[g_id].xyz += avg_displacement;
}
```

##### 验收标准
- ✅ 粒子位移正确传递到高斯
- ✅ 渲染结果反映物理变形
- ✅ 性能满足要求

##### 验收方法
```bash
# 可视化测试
./build/apps/viewer/3dgs_viewer --scene test.ply --enable-physics
# 拖拽粒子，观察高斯变形
```

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 位移传递准确性 | > 90% | 对比测试 |
| 渲染变形正确 | 视觉合理 | 手动检查 |
| 性能影响 | < 20ms | 性能测试 |

##### 风险评估
- **风险**: 中-高
- **回退**: 禁用耦合
- **回退成本**: 6小时

---

#### 阶段4.2: 端到端集成测试 (4-5天)

##### 目标
完整的端到端测试和验收。

##### 实施步骤

**Step 1: 集成测试套件** (2-3天)
```cpp
// tests/integration_full.cpp
class FullIntegrationTest {
public:
    void TestPhysicsToRenderingPipeline() {
        // 1. 加载场景
        LoadScene();
        
        // 2. 初始化MPM
        InitializeMPM();
        
        // 3. 运行物理步进
        StepPhysics();
        
        // 4. 映射到高斯
        MapToGaussians();
        
        // 5. 渲染
        Render();
        
        // 6. 验证结果
        VerifyRendering();
    }
    
    void TestInteractiveDeformation() {
        // 测试完整交互流程
    }
};
```

**Step 2: 性能基准测试** (1-2天)
```bash
# 性能基准脚本
./scripts/benchmark_physics_rendering.sh
```

##### 验收标准
- ✅ 完整流程工作正常
- ✅ 性能达标
- ✅ 质量符合预期
- ✅ 无崩溃或泄漏

##### 验收指标
| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 端到端FPS | > 30 FPS | 性能测试 |
| 物理精度 | 符合预期 | 单元测试 |
| 渲染质量 | 视觉合理 | 手动检查 |
| 稳定性 | 长时间无崩溃 | 稳定性测试 |

##### 风险评估
- **风险**: 高
- **回退**: 分阶段回退
- **回退成本**: 8小时

---

#### 阶段4.3: 文档与发布准备 (2-3天)

##### 目标
完善文档和准备发布。

##### 实施步骤

**Step 1: 用户文档** (1天)
- 使用说明
- API文档
- 示例代码

**Step 2: 开发者文档** (1天)
- 架构说明
- 代码结构
- 扩展指南

##### 验收标准
- ✅ 文档完整
- ✅ 示例可运行
- ✅ 发布包就绪

---

## 📊 完整时间表

| 阶段 | 子阶段 | 预计时间 | 风险 | 可独立验收 | 回退成本 |
|------|--------|----------|------|-----------|---------|
| **1** | MPM基础Shader | | | | |
| 1.1 | 数据结构初始化 | 3-4天 | 低 | ✅ | 0.5小时 |
| 1.2 | ZeroGrid+P2G | 4-5天 | 中 | ✅ | 1小时 |
| 1.3 | GridUpdate+G2P | 4-5天 | 中 | ✅ | 2小时 |
| 1.4 | MPM管线集成 | 3-4天 | 高 | ✅ | 4小时 |
| **阶段1总计** | | **14-18天** | | | |
| **2** | 用户交互 | | | | |
| 2.1 | 射线投射 | 5-7天 | 低-中 | ✅ | 2小时 |
| 2.2 | 拖拽变形 | 5-7天 | 中 | ✅ | 3小时 |
| 2.3 | 相机控制UI | 3-4天 | 低 | ✅ | 1小时 |
| **阶段2总计** | | **13-18天** | | | |
| **3** | 材料场优化 | | | | |
| 3.1 | 简化材料场 | 5-7天 | 中 | ✅ | 2小时 |
| 3.2 | 性能监控调试 | 4-5天 | 低 | ✅ | 1小时 |
| 3.3 | 算法优化 | 5-7天 | 中 | ✅ | 4小时 |
| **阶段3总计** | | **14-19天** | | | |
| **4** | 高斯耦合 | | | | |
| 4.1 | 粒子高斯映射 | 5-7天 | 中-高 | ✅ | 6小时 |
| 4.2 | 端到端集成 | 4-5天 | 高 | ✅ | 8小时 |
| 4.3 | 文档发布 | 2-3天 | 低 | ✅ | 1小时 |
| **阶段4总计** | | **11-15天** | | | |

**总计**: 52-70天 ≈ 7-10周 (考虑调试和意外，实际约8-12周)

---

## 🎯 验收框架

### 自动化验收工具

```python
# scripts/verify_physics_stage.py
#!/usr/bin/env python3
"""
物理仿真阶段验收工具
"""

import subprocess
import json
from pathlib import Path

def verify_stage(stage_name):
    """验收指定阶段"""
    print(f"验收阶段: {stage_name}")
    
    # 1. 编译测试
    print("[1/5] 编译测试...")
    result = subprocess.run(["cmake", "--build", ".", "--target", stage_name])
    if result.returncode != 0:
        return False, "编译失败"
    
    # 2. 单元测试
    print("[2/5] 单元测试...")
    result = subprocess.run([f"./build/tests/test_{stage_name}"])
    if result.returncode != 0:
        return False, "单元测试失败"
    
    # 3. 性能测试
    print("[3/5] 性能测试...")
    result = subprocess.run([f"./build/tests/performance_{stage_name}"])
    performance = json.loads(result.stdout)
    
    # 4. 集成测试
    print("[4/5] 集成测试...")
    result = subprocess.run([f"./build/tests/integration_{stage_name}"])
    
    # 5. 生成报告
    print("[5/5] 生成报告...")
    report = {
        "stage": stage_name,
        "status": "passed",
        "performance": performance
    }
    
    with open(f"verification/{stage_name}_report.json", "w") as f:
        json.dump(report, f, indent=2)
    
    return True, "验收通过"

if __name__ == "__main__":
    import sys
    verify_stage(sys.argv[1])
```

### 验收决策流程

```
┌─────────────────────────────────────────────────────────────┐
│                    物理仿真验收流程                          │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  1. Agent自主验收                                            │
│     ├── 运行单元测试                                         │
│     ├── 运行性能测试                                         │
│     ├── 运行集成测试                                         │
│     └── 生成验收报告                                         │
│           ↓                                                  │
│  2. 验收报告提交                                            │
│     ├── 单元测试结果                                         │
│     ├── 性能数据对比                                         │
│     ├── 集成测试状态                                         │
│     └── 问题发现与建议                                       │
│           ↓                                                  │
│  3. 用户人工检查                                            │
│     ├── 审查验收报告                                         │
│     ├── 视觉确认物理效果                                     │
│     ├── 性能数据验证                                         │
│     └── 批准/要求修改                                       │
│           ↓                                                  │
│  4. 批准后进入下一阶段                                      │
│                                                               │
└─────────────────────────────────────────────────────────────┘
```

### 性能基准

| 场景规模 | 粒子数 | 高斯数 | 目标FPS | 最小FPS |
|----------|--------|--------|---------|---------|
| 小 | 10K | 20K | 60+ | 45 |
| 中 | 25K | 50K | 45+ | 30 |
| 大 | 50K | 100K | 30+ | 20 |

---

## 🔧 技术实施要点

### 关键数据结构

```cpp
// MPM粒子缓冲区布局
struct MPMParticleBuffer {
    std::array<ParticleData, MAX_PARTICLES> particles;
    
    // 对齐到16字节以提高缓存效率
    alignas(16) std::array<vec4, MAX_PARTICLES * 8> data_soa;
};

// 网格缓冲区布局
struct MPMGridBuffer {
    std::array<GridNode, GRID_SIZE * GRID_SIZE * GRID_SIZE> nodes;
    
    // 使用3D纹理加速访问
    VkImage grid_texture_3d;
};
```

### 内存优化策略

```cpp
// 使用环形缓冲区减少分配
class MPMRingBuffer {
    std::array<std::shared_ptr<Buffer>, 3> particle_buffers_;
    uint32_t current_index_ = 0;
    
    std::shared_ptr<Buffer> GetCurrentWriteBuffer() {
        return particle_buffers_[current_index_];
    }
    
    std::shared_ptr<Buffer> GetPreviousReadBuffer() {
        return particle_buffers_[(current_index_ + 2) % 3];
    }
    
    void Advance() {
        current_index_ = (current_index_ + 1) % 3;
    }
};
```

---

## 🚨 风险缓解

### 高风险项

1. **MPM算法复杂性**
   - 症状：数学计算错误，物理行为异常
   - 诊断：对比PhysDreamer输出
   - 缓解：分阶段验证，单元测试覆盖
   - 回退：禁用物理仿真

2. **性能不达标**
   - 症状：FPS < 20
   - 诊断：性能profiling
   - 缓解：降采样、减少子步数
   - 回退：简化算法

3. **同步问题**
   - 症状：死锁、数据竞争
   - 诊断：Vulkan validation layers
   - 缓解：双缓冲、严格barrier
   - 回退：顺序执行

### 质量保证

```cpp
// 调试模式交叉验证
#ifdef DEBUG_MODE
    auto cpu_result = ComputeMPMCPU(particles);
    auto gpu_result = ComputeMPMGPU(particles);
    
    float diff = compare_results(cpu_result, gpu_result);
    assert(diff < 0.01, "CPU-GPU结果不一致!");
#endif
```

---

## 📚 参考资料

### 网络调研结果
- [Vulkan Physics Tutorial](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Subsystems/05_vulkan_physics.html)
- [RealTimeParticleBasedSnowSimulation](https://github.com/giaosame/RealTimeParticleBasedSnowSimulation)
- [Intel Vulkan Particles](https://www.intel.cn/content/www/en/us/documents/paralleltechniquesinmodelingparticlesystemsusingvulkanapi-754322.pdf)
- [MPM Compute Shader Paper](https://www.researchgate.net/publication/319525082_Material_Point_Method_based_Fluid_Simulation_on_GPU_using_Compute_Shader)

### PhysDreamer参考
- [INFERENCE_PIPELINE_DESIGN.md](D:\liuyue\physDreamerVulkanDemo\PhysDreamer\projects\inference\INFERENCE_PIPELINE_DESIGN.md)

### 学术资源
- [Taichi MPM Tutorial](https://yuanming.taichi.graphics/publication/2019-mpm-tutorial/)
- [GPU MPM Optimization](https://pages.cs.wisc.edu/~sifakis/papers/GPU_MPM.pdf)

---

**制定时间**: 2026年5月29日
**项目**: Vulkan物理仿真与交互集成
**预期完成**: 2026年8-10月
**目标**: 完全用Vulkan实现物理仿真，不依赖Python/CUDA
