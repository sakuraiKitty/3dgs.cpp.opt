#ifndef COUPLING_MANAGER_H
#define COUPLING_MANAGER_H

#include "../mpm/MPMManager.h"
#include "../GSScene.h"
#include "DisplacementMapper.h"
#include "GaussianParticleMapper.h"
#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include <memory>
#include <vector>
#include <glm/glm.hpp>

/**
 * 物理-渲染耦合管理器
 * 协调MPM物理仿真和3D高斯渲染
 *
 * 职责：
 * 1. 管理物理仿真和渲染之间的数据流
 * 2. 协调CPU-GPU数据传输
 * 3. 调用位移映射shader
 * 4. 同步物理和渲染管线
 */
class CouplingManager {
public:
    struct Config {
        bool enable_coupling = true;          // 启用耦合
        float displacement_scale = 1.0f;    // 位移缩放因子
        bool use_rigid_transform = true;     // 使用刚性变换
    };

    explicit CouplingManager(std::shared_ptr<VulkanContext> context);

    ~CouplingManager();

    /**
     * 初始化耦合系统
     *
     * @param scene 高斯场景
     * @param mpm_manager MPM管理器
     * @param config 配置
     */
    void Initialize(
        const std::shared_ptr<GSScene>& scene,
        const std::shared_ptr<MPM::MPMManager>& mpm_manager,
        const Config& config = Config()
    );

    /**
     * 执行耦合（每帧调用）
     *
     * @param cmd Vulkan命令缓冲区
     * @param frame_index 当前帧索引
     * @param compute_displacements_from_mpm 是否从MPM粒子计算位移（默认true）
     *        false时跳过ComputeParticleDisplacementsGPU，使用已有的drive_displacement_buffer_
     */
    void Couple(VkCommandBuffer cmd, uint32_t frame_index, bool compute_displacements_from_mpm = true);

    /**
     * 直接施加位移到粒子（GPU端）
     * 用于鼠标交互：直接设置位移而非通过MPM力驱动
     *
     * @param cmd Vulkan命令缓冲区
     * @param target_particle 目标粒子索引
     * @param displacement 位移向量（归一化坐标空间）
     * @param influence_radius 影响半径（归一化坐标空间）
     * @param num_particles 粒子总数
     * @param zero_all true=清零所有位移（恢复原始位置）
     */
    void ApplyDirectDisplacement(
        VkCommandBuffer cmd,
        uint32_t target_particle,
        const glm::vec3& displacement,
        float influence_radius,
        uint32_t num_particles,
        bool zero_all = false
    );

    /**
     * 在GPU端计算粒子位移（避免CPU round-trip）
     *
     * @param cmd Vulkan命令缓冲区
     */
    void ComputeParticleDisplacementsGPU(VkCommandBuffer cmd);

    /**
     * 获取耦合统计信息
     */
    struct Statistics {
        uint32_t num_deformable_gaussians = 0;
        uint32_t num_drive_particles = 0;
        float avg_displacement = 0.0f;
    };

    const Statistics& GetStatistics() const { return stats_; }

    /**
     * 启用/禁用耦合
     */
    void Enable() { enabled_ = true; }
    void Disable() { enabled_ = false; }
    bool IsEnabled() const { return enabled_; }

    /**
     * 获取资源（用于绑定到渲染管线）
     */
    std::shared_ptr<Buffer> GetGaussianPositionBuffer() const { return gaussian_position_buffer_; }
    std::shared_ptr<Buffer> GetGaussianRotationBuffer() const { return gaussian_rotation_buffer_; }
    std::shared_ptr<Buffer> GetDeformableIndexBuffer() const { return deformable_index_buffer_; }

    /**
     * 设置坐标变换参数（从Renderer传入）
     */
    void SetCoordTransform(float scale, const glm::vec3& shift) {
        coord_scale_ = scale;
        coord_shift_ = shift;
    }

private:
    /**
     * 创建原始数据缓冲区
     */
    void CreateOriginalDataBuffers(
        const std::shared_ptr<GSScene>& scene,
        const std::vector<bool>& sim_mask
    );

    /**
     * 创建输出缓冲区
     */
    void CreateOutputBuffers(const std::shared_ptr<GSScene>& scene);

    /**
     * 预计算Top-K映射
     */
    void PrecomputeTopKMapping(
        const std::shared_ptr<GSScene>& scene,
        const std::shared_ptr<MPM::MPMManager>& mpm_manager
    );

    /**
     * 创建Top-K权重缓冲区
     */
    void CreateTopKWeightBuffer();

    /**
     * 上传粒子位移到GPU
     */
    void UploadParticleDisplacements();

    /**
     * 下载粒子位移（从MPM GPU到CPU）
     */
    void DownloadParticleDisplacements();

private:
    std::shared_ptr<VulkanContext> context_;

    // 配置和状态
    Config config_;
    bool initialized_ = false;
    bool enabled_ = true;

    // 组件
    std::unique_ptr<DisplacementMapper> displacement_mapper_;
    std::unique_ptr<GaussianParticleMapper> particle_mapper_;

    // 粒子位移计算 pipeline（GPU端）
    std::shared_ptr<ComputePipeline> displacement_compute_pipeline_;
    std::shared_ptr<DescriptorSet> displacement_compute_descriptor_set_;
    bool displacement_pipeline_created_ = false;

    // 直接位移施加 pipeline（交互用）
    std::shared_ptr<ComputePipeline> displacement_application_pipeline_;
    std::shared_ptr<DescriptorSet> displacement_application_descriptor_set_;
    bool displacement_application_pipeline_created_ = false;

    // 数据
    std::shared_ptr<MPM::MPMManager> mpm_manager_;
    std::vector<uint32_t> deformable_indices_;
    std::vector<glm::vec3> particle_displacements_;  // CPU端粒子位移

    // 坐标变换参数（用于shader中的归一化→世界空间转换）
    float coord_scale_ = 1.0f;
    glm::vec3 coord_shift_ = glm::vec3(0.0f);

    // GPU缓冲区 - 输入
    std::shared_ptr<Buffer> deformable_index_buffer_;       // 可变形高斯索引
    std::shared_ptr<Buffer> drive_position_buffer_;         // 驱动粒子原始位置
    std::shared_ptr<Buffer> drive_displacement_buffer_;    // 驱动粒子位移
    std::shared_ptr<Buffer> top_k_index_buffer_;           // Top-K索引
    std::shared_ptr<Buffer> top_k_weight_buffer_;          // Top-K权重

    // GPU缓冲区 - 原始数据
    std::shared_ptr<Buffer> original_position_buffer_;     // 高斯原始位置
    std::shared_ptr<Buffer> original_rotation_buffer_;     // 高斯原始旋转

    // GPU缓冲区 - 输出
    std::shared_ptr<Buffer> gaussian_position_buffer_;     // 高斯新位置
    std::shared_ptr<Buffer> gaussian_rotation_buffer_;     // 高斯新旋转

    // 统计信息
    Statistics stats_;
};

#endif // COUPLING_MANAGER_H
