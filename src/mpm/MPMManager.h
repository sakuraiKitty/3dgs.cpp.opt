#ifndef MPM_MANAGER_H
#define MPM_MANAGER_H

#include "MPMStructs.h"
#include "ParticleGenerator.h"
#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include "../vulkan/pipelines/ComputePipeline.h"
#include "../vulkan/DescriptorSet.h"
#include "../vulkan/Shader.h"
#include <memory>
#include <vector>

// 前向声明
class GSScene;

namespace MPM {

/**
 * MPM管理器
 * 管理MPM物理模拟的核心类
 *
 * 功能：
 * - 初始化MPM系统（粒子、网格）
 * - 执行物理步进
 * - 管理GPU缓冲区
 * - 提供状态查询
 */
class MPMManager {
public:
    /**
     * MPM配置
     */
    struct Config {
        // 网格配置
        uint32_t grid_size = 64;              // 64x64x64网格
        float grid_lim = 1.0f;                // 网格域 [0, grid_lim]
        float grid_spacing = 1.0f / 64.0f;    // 单元格大小
        float inv_dx = 64.0f;                 // 1/dx

        // 时间步配置
        float dt = 1.0f / 30.0f;              // 时间步长 [s]
        uint32_t substeps = 32;               // 子步数 (从128降低，减少累积阻尼)

        // 物理参数
        glm::vec3 gravity = {0.0f, -9.8f, 0.0f}; // 重力加速度 [m/s^2]
        float damping = 0.999f;              // 网格速度阻尼 (从0.99→0.999，减少97%衰减)

        // 材料参数（默认jelly）
        MaterialConfig material;

        // 粒子生成配置
        ParticleGenerator::Config generator_config;
    };

    /**
     * 构造函数
     */
    explicit MPMManager(std::shared_ptr<VulkanContext> context);

    ~MPMManager();

    /**
     * 初始化MPM系统
     *
     * @param config 配置参数
     */
    void Initialize(const Config& config);

    /**
     * 从场景加载粒子
     *
     * @param scene 高斯场景
     * @param sim_mask 可变形区域掩码
     */
    void LoadParticlesFromScene(
        const std::shared_ptr<GSScene>& scene,
        const std::vector<bool>& sim_mask
    );

    /**
     * 从预生成的粒子数据加载
     *
     * @param particles 粒子数据
     */
    void LoadParticles(const std::vector<ParticleData>& particles);

    /**
     * 执行一步物理模拟
     *
     * @param cmd Vulkan命令缓冲区
     * @param dt 时间步长
     * @param override_substeps 可选的子步数覆盖（0=使用config_.substeps）
     */
    void Step(VkCommandBuffer cmd, float dt, uint32_t override_substeps = 0);

    /**
     * 重置模拟到初始状态
     */
    void Reset();

    /**
     * 轻量按需诊断：同步回读粒子缓冲(2.3MB)，扫描 max|disp|/max|vel|/max|F-I|/moved 计数
     * 每 diag_interval_ 帧执行一次（默认60帧≈2x/sec@30fps），用于定位"不回弹/不衰减"类问题
     * 关键指标 max_strain=max|F-I|：若≈0 → F未偏离I → 应力≈0 → 无弹性恢复力
     */
    void Diagnose();

    /**
     * 启用/禁用物理模拟
     */
    void Enable() { enabled_ = true; }
    void Disable() { enabled_ = false; }
    bool IsEnabled() const { return enabled_; }


    /**
     * 状态查询
     */
    uint32_t GetParticleCount() const { return num_particles_; }
    uint32_t GetGridSize() const { return config_.grid_size; }
    const Config& GetConfig() const { return config_; }
    const DeformableRegion& GetRegion() const { return region_; }

    /**
     * 初始粒子 AABB（归一化空间），用于交互抓取半径自适应
     * 对标 PhysDreamer gui_demo.py: grab_radius = aabb_diag * 0.02
     */
    float GetInitialAABBDiag() const { return aabb_diag_; }
    bool HasAABB() const { return aabb_computed_; }

    /**
     * 运行时设置网格速度阻尼（对标 PhysDreamer 释放阻尼）
     * 拖拽中=1.0(无阻尼纯跟随)，释放=0.95^(1/substeps)/子步(衰减振荡)
     * Substep 每4阶段 grid_update 读 config_.damping，故运行时改立即生效
     */
    void SetDamping(float d) { config_.damping = d; }

    /**
     * 位置 home-spring 开关 + 刚度（为刚体旋转/平移模态提供恢复力，FCR 客观材料无法）
     * k≈15 → 周期 T=2π/√k≈1.6s；配合释放阻尼 0.95/帧，松手后 1-2 次振荡归位。
     * enable=0 关闭（纯 FCR，会卡在刚体旋转态）。
     */
    void SetHomeSpring(float k, bool enable) {
        home_spring_.k_spring = k;
        home_spring_.enable = enable ? 1u : 0u;
    }

    /**
     * 拖拽速度 Dirichlet BC（每子步 SET 半径内非冻结粒子速度，对标 PhysDreamer enforce_particle_velocity_by_mask）
     * 由 Renderer 每帧（拖拽中）调用，MPMManager 在 Substep 的 zero_grid 后、P2G 前派发。
     * 释放时调 ClearDragVelocityBC() 停止驱动，自由震荡。
     * center/radius/velocity 均为归一化空间，maxVelocity=CFL 上限(0=不限)。
     */
    void SetDragVelocityBC(const glm::vec3& center, float radius,
                           const glm::vec3& velocity, float maxVelocity) {
        drag_bc_.center = center;
        drag_bc_.radius = radius;
        drag_bc_.velocity = velocity;
        drag_bc_.alpha = 0.0f;  // SET 模式未用
        drag_bc_.isDragging = 1;
        drag_bc_.maxVelocity = maxVelocity;
        drag_bc_.numParticles = num_particles_;
    }
    void ClearDragVelocityBC() { drag_bc_.isDragging = 0; }

    /**
     * 获取粒子缓冲区（用于交互系统）
     */
    std::shared_ptr<Buffer> GetParticleBuffer() const { return particle_buffer_; }
    std::shared_ptr<Buffer> GetGridBuffer() const { return grid_buffer_; }
    float GetInvDx() const { return config_.inv_dx; }

    /**
     * 获取初始位置缓冲区（用于GPU位移计算）
     */
    std::shared_ptr<Buffer> GetInitialPosBuffer() const { return initial_pos_buffer_; }

    /**
     * 获取粒子位移缓冲区（用于GPU位移计算）
     */
    std::shared_ptr<Buffer> GetDisplacementBuffer() const { return particle_displacement_buffer_; }

    /**
     * 获取粒子位移（用于高斯耦合）
     */
    std::vector<glm::vec3> GetParticleDisplacements() const;

    /**
     * 设置可变形区域
     */
    void SetRegion(const DeformableRegion& region);

    /**
     * 自动分割区域（基于距离阈值）
     */
    void AutoSegmentRegion(const std::vector<glm::vec3>& all_positions);

    /**
     * 应用外力到粒子（用于交互）
     *
     * @param particle_indices 粒子索引列表
     * @param forces 对应的力向量
     */
    void ApplyExternalForces(const std::vector<uint32_t>& particle_indices, const std::vector<glm::vec3>& forces);

    /**
     * 获取粒子位置（用于射线检测）
     */
    std::vector<glm::vec3> GetParticlePositions() const;

private:
    /**
     * 创建GPU缓冲区
     */
    void CreateParticleBuffer();
    void CreateInitialPosBuffer();
    void CreateGridBuffer();
    void CreateDisplacementBuffer();

    /**
     * 创建Compute Pipeline和Descriptor Sets
     */
    void CreatePipelines();
    void CreateDescriptorSets();

    /**
     * 构建Descriptor Sets（在缓冲区创建后调用）
     */
    void BuildDescriptorSets();

    /**
     * 创建单个MPM阶段的Pipeline
     */
    std::shared_ptr<ComputePipeline> CreateMPMPipeline(
        const std::string& shaderName,
        const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
        vk::PushConstantRange pushConstantRange
    );

    /**
     * 记录命令缓冲区
     */
    void RecordPhysicsCommandBuffer(VkCommandBuffer cmd, float dt);

    /**
     * 物理子步进
     */
    void Substep(VkCommandBuffer cmd, float dt);

private:
    // Vulkan上下文
    std::shared_ptr<VulkanContext> context_;

    // 配置
    Config config_;

    // 状态标志
    bool enabled_ = false;
    bool initialized_ = false;
    bool descriptor_sets_built_ = false;

    // 诊断节流
    uint32_t diag_frame_counter_ = 0;
    uint32_t diag_interval_ = 60;  // 每60帧回读一次粒子缓冲(2.3MB)做诊断

    // 粒子数据
    std::vector<ParticleData> cpu_particles_;          // CPU端粒子数据
    std::vector<glm::vec3> cpu_particle_initial_pos_;   // 初始位置（用于计算位移）
    uint32_t num_particles_ = 0;

    // 初始粒子 AABB（归一化空间，交互抓取半径自适应用）
    glm::vec3 aabb_min_ = glm::vec3(0.0f);
    glm::vec3 aabb_max_ = glm::vec3(0.0f);
    float aabb_diag_ = 0.0f;
    bool aabb_computed_ = false;

    // 网格数据
    uint32_t grid_total_nodes_ = 0; // grid_size^3

    // 可变形区域
    DeformableRegion region_;

    // GPU缓冲区
    std::shared_ptr<Buffer> particle_buffer_;           // 粒子数据
    std::shared_ptr<Buffer> initial_pos_buffer_;        // 粒子初始位置（用于GPU位移计算）
    std::shared_ptr<Buffer> particle_displacement_buffer_; // 粒子位移
    std::shared_ptr<Buffer> grid_buffer_;                // 网格节点
    std::shared_ptr<Buffer> staging_buffer_;             // 用于下载结果

    // Compute pipelines
    std::shared_ptr<ComputePipeline> zero_grid_pipeline_;
    // compute_stress_pipeline_ 已移除 — 应力计算合并到 P2G shader inline
    std::shared_ptr<ComputePipeline> p2g_pipeline_;
    std::shared_ptr<ComputePipeline> grid_update_pipeline_;
    std::shared_ptr<ComputePipeline> grid_freeze_pipeline_;   // 网格冻结pipeline（冻结区域速度归零）
    std::shared_ptr<ComputePipeline> g2p_pipeline_;
    std::shared_ptr<ComputePipeline> drag_bc_pipeline_;       // 拖拽速度 Dirichlet BC（每子步）
    std::shared_ptr<ComputePipeline> pin_frozen_pipeline_;    // 粒子级硬冻结（每子步 G2P 后，对标 PhysDreamer gui_demo.py:313）
    std::shared_ptr<ComputePipeline> home_spring_pipeline_;   // 位置 home-spring（每子步 ZeroGrid 后，为刚体模态提供恢复力）

    // 位置 home-spring 参数（16 bytes，与 apply_home_spring.comp push constant 布局一致）
    // FCR 客观材料对纯旋转零应力→花头刚体旋转不回弹；弹簧 F=-k(x-x0) 专治该刚体模态。
    struct HomeSpringParams {
        float    k_spring;       // offset 0  — 弹簧刚度 ω² (1/s²), 周期 T=2π/√k
        float    dt;             // offset 4  — 子步时间步长（每子步由 Substep 注入）
        uint32_t num_particles;  // offset 8
        uint32_t enable;         // offset 12 — 0=禁用 1=启用
    };
    static_assert(sizeof(HomeSpringParams) == 16, "HomeSpringParams must be 16 bytes (GLSL push constant)");
    HomeSpringParams home_spring_{0.0f, 0.0f, 0u, 0u};  // 默认禁用

    // 拖拽速度 BC 参数（48 bytes，与 apply_drag_velocity_bc.comp push constant 布局一致）
    struct DragBCParams {
        glm::vec3 center;        // offset 0
        float     radius;        // offset 12
        glm::vec3 velocity;      // offset 16
        float     alpha;         // offset 28 (SET 模式未用)
        int32_t   isDragging;    // offset 32
        float     maxVelocity;   // offset 36
        uint32_t  numParticles;  // offset 40
        int32_t   _pad;          // offset 44
    };
    static_assert(sizeof(DragBCParams) == 48, "DragBCParams must be 48 bytes (GLSL push constant)");
    DragBCParams drag_bc_{};  // isDragging=0 默认（不驱动）

    // Descriptor sets
    std::shared_ptr<DescriptorSet> particle_grid_descriptor_; // 粒子+网格绑定（P2G: 0=Particle,1=Grid）
    std::shared_ptr<DescriptorSet> grid_only_descriptor_;      // 仅网格绑定（ZeroGrid, GridUpdate）
    std::shared_ptr<DescriptorSet> g2p_descriptor_;            // G2P专用（0=Grid readonly,1=Particle write）
    std::shared_ptr<DescriptorSet> particle_init_descriptor_; // 粒子+初始位置（PinFrozen: 0=Particle,1=InitPos）


    // 粒子生成器
    ParticleGenerator generator_;
};

} // namespace MPM

#endif // MPM_MANAGER_H
