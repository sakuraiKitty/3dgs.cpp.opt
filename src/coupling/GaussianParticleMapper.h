#ifndef GAUSSIAN_PARTICLE_MAPPER_H
#define GAUSSIAN_PARTICLE_MAPPER_H

#include "../mpm/MPMStructs.h"
#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include <vector>
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>

/**
 * 高斯-粒子映射器
 * 计算每个高斯的K近邻粒子，用于位移传递
 */
class GaussianParticleMapper {
public:
    /**
     * 预计算K近邻映射
     *
     * @param gaussian_positions 高斯点云位置
     * @param particle_positions MPM粒子位置
     * @param k 近邻数量 (默认8)
     */
    void PrecomputeMapping(
        const std::vector<glm::vec3>& gaussian_positions,
        const std::vector<glm::vec3>& particle_positions,
        uint32_t k = 8
    );

    /**
     * 上传映射到GPU
     */
    void UploadMappingToGPU(
        const std::shared_ptr<VulkanContext>& context,
        VkCommandBuffer cmd
    );

    /**
     * 获取GPU缓冲区
     */
    std::shared_ptr<Buffer> GetKNNGPUBuffer() const { return knn_gpu_buffer_; }
    std::shared_ptr<Buffer> GetDeformableIndexBuffer() const { return deformable_index_buffer_; }

    /**
     * 获取可变形高斯索引
     */
    const std::vector<uint32_t>& GetDeformableIndices() const {
        return deformable_indices_;
    }

    /**
     * 设置可变形区域
     */
    void SetDeformableIndices(const std::vector<uint32_t>& indices) {
        deformable_indices_ = indices;
    }

    /**
     * 验证映射
     */
    bool IsValid() const {
        return !knn_mappings_.empty() && knn_mappings_.size() == gaussian_count_;
    }

private:
    // CPU端映射结果
    std::vector<MPM::KNNMapping> knn_mappings_;
    size_t gaussian_count_ = 0;
    uint32_t k_ = 8;

    // 可变形高斯索引
    std::vector<uint32_t> deformable_indices_;

    // GPU缓冲区
    std::shared_ptr<Buffer> knn_gpu_buffer_;           // K近邻索引
    std::shared_ptr<Buffer> deformable_index_buffer_; // 可变形索引列表
};

#endif // GAUSSIAN_PARTICLE_MAPPER_H
