#ifndef DISPLACEMENT_MAPPER_H
#define DISPLACEMENT_MAPPER_H

#include "vulkan/VulkanContext.h"
#include <vulkan/Buffer.h>
#include "mpm/MPMStructs.h"
#include "coupling/GaussianParticleMapper.h"
#include <memory>
#include <vector>
#include <glm/glm.hpp>

// 前向声明
class VkCommandBuffer;

/**
 * 位移映射器
 * 将MPM粒子的位移映射到3D高斯
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

    DisplacementMapper(std::shared_ptr<VulkanContext> context);

    /**
     * 创建compute pipeline
     */
    void CreatePipeline(const std::shared_ptr<VulkanContext>& context);

    /**
     * 映射位移到高斯
     *
     * @param cmd Vulkan命令缓冲区
     * @param particle_displacements 粒子位移
     * @param particle_count 粒子数量
     * @param mapper K近邻映射器
     * @param config 配置
     */
    void MapDisplacements(
        VkCommandBuffer cmd,
        const std::vector<glm::vec3>& particle_displacements,
        uint32_t particle_count,
        const GaussianParticleMapper& mapper,
        const Config& config = Config()
    );

    /**
     * GPU版本的位移映射
     * 位移数据已在GPU上，直接调用shader
     */
    void MapDisplacementsGPU(
        VkCommandBuffer cmd,
        uint32_t num_deformable,
        const Config& config = Config()
    );

    /**
     * 获取位移缓冲区
     */
    std::shared_ptr<Buffer> GetDisplacementBuffer() const {
        return displacement_buffer_;
    }

private:
    std::shared_ptr<VulkanContext> context_;
    std::shared_ptr<Buffer> displacement_buffer_;      // 粒子位移

    // Compute pipeline (将在下一阶段集成)
    // std::shared_ptr<ComputePipeline> map_displacement_pipeline_;
};

#endif // DISPLACEMENT_MAPPER_H
