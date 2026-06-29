#ifndef DISPLACEMENT_MAPPER_H
#define DISPLACEMENT_MAPPER_H

#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include "../vulkan/DescriptorSet.h"
#include "../vulkan/pipelines/ComputePipeline.h"
#include "../mpm/MPMStructs.h"
#include "GaussianParticleMapper.h"
#include <memory>
#include <vector>
#include <glm/glm.hpp>

/**
 * 位移映射器
 * 将MPM粒子的位移通过刚性变换映射到3D高斯
 */
class DisplacementMapper {
public:
    /**
     * 映射配置
     */
    struct Config {
        float displacement_scale = 1.0f;  // 位移缩放因子
        bool use_rigid_transform = true;   // 是否使用刚性变换
    };

    explicit DisplacementMapper(std::shared_ptr<VulkanContext> context);
    ~DisplacementMapper();

    /**
     * 初始化（创建descriptor sets和pipeline）
     */
    void Initialize();

    /**
     * 设置资源绑定
     */
    void SetResources(
        const std::shared_ptr<Buffer>& deformable_index_buffer,
        const std::shared_ptr<Buffer>& drive_position_buffer,
        const std::shared_ptr<Buffer>& drive_displacement_buffer,
        const std::shared_ptr<Buffer>& top_k_index_buffer,
        const std::shared_ptr<Buffer>& top_k_weight_buffer,
        const std::shared_ptr<Buffer>& original_position_buffer,
        const std::shared_ptr<Buffer>& original_rotation_buffer,
        const std::shared_ptr<Buffer>& gaussian_position_buffer,
        const std::shared_ptr<Buffer>& gaussian_rotation_buffer
    );

    /**
     * 映射位移到高斯
     *
     * @param cmd Vulkan命令缓冲区
     * @param num_deformable 可变形高斯数量
     * @param num_particles 粒子总数
     * @param coord_scale MPM坐标变换的scale因子
     * @param coord_shift MPM坐标变换的shift向量
     * @param config 配置
     */
    void MapDisplacements(
        VkCommandBuffer cmd,
        uint32_t num_deformable,
        uint32_t num_particles,
        float coord_scale,
        const glm::vec3& coord_shift,
        const Config& config = Config()
    );

    /**
     * 获取位移缓冲区
     */
    std::shared_ptr<Buffer> GetDisplacementBuffer() const {
        return drive_displacement_buffer_;
    }

private:
    std::shared_ptr<VulkanContext> context_;

    // 资源缓冲区
    std::shared_ptr<Buffer> deformable_index_buffer_;
    std::shared_ptr<Buffer> drive_position_buffer_;
    std::shared_ptr<Buffer> drive_displacement_buffer_;
    std::shared_ptr<Buffer> top_k_index_buffer_;
    std::shared_ptr<Buffer> top_k_weight_buffer_;
    std::shared_ptr<Buffer> original_position_buffer_;
    std::shared_ptr<Buffer> original_rotation_buffer_;
    std::shared_ptr<Buffer> gaussian_position_buffer_;
    std::shared_ptr<Buffer> gaussian_rotation_buffer_;

    // Pipeline和Descriptor
    std::shared_ptr<ComputePipeline> map_pipeline_;
    std::shared_ptr<DescriptorSet> descriptor_set_;

    bool initialized_ = false;
};

#endif // DISPLACEMENT_MAPPER_H
