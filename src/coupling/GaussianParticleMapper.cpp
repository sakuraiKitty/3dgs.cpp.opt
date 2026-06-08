#include "GaussianParticleMapper.h"
#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <queue>
#include <limits>

#ifdef _OPENMP
#include <omp.h>
#endif

void GaussianParticleMapper::PrecomputeMapping(
    const std::vector<glm::vec3>& gaussian_positions,
    const std::vector<glm::vec3>& particle_positions,
    uint32_t k
) {
    spdlog::info("[GaussianParticleMapper] Precomputing KNN mapping");
    spdlog::info("[GaussianParticleMapper] Gaussians: {}, Particles: {}, K: {}",
                gaussian_positions.size(), particle_positions.size(), k);

    if (gaussian_positions.empty() || particle_positions.empty()) {
        spdlog::error("[GaussianParticleMapper] Empty input");
        return;
    }

    gaussian_count_ = gaussian_positions.size();
    k_ = k;
    knn_mappings_.resize(gaussian_count_);

    // 暴力搜索计算K近邻
    // 注意：对于大规模数据，可以考虑使用KDTree或GPU加速
    #pragma omp parallel for
    for (size_t i = 0; i < gaussian_count_; i++) {
        const glm::vec3& gaussian_pos = gaussian_positions[i];

        // 计算到所有粒子的距离
        std::priority_queue<std::pair<float, uint32_t>,
                           std::vector<std::pair<float, uint32_t>>,
                           std::greater<std::pair<float, uint32_t>>> pq;

        for (size_t j = 0; j < particle_positions.size(); j++) {
            glm::vec3 diff = gaussian_pos - particle_positions[j];
            float dist_sq = glm::dot(diff, diff);
            pq.push({dist_sq, static_cast<uint32_t>(j)});

            if (pq.size() > k) {
                pq.pop();
            }
        }

        // 提取K个最近邻
        std::vector<std::pair<float, uint32_t>> nearest;
        while (!pq.empty()) {
            nearest.push_back(pq.top());
            pq.pop();
        }

        // 按距离排序（从小到大）
        std::sort(nearest.begin(), nearest.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        // 存储到映射中
        for (uint32_t ki = 0; ki < k; ki++) {
            if (ki < nearest.size()) {
                knn_mappings_[i].particle_indices[ki] = nearest[ki].second;
                knn_mappings_[i].weights[ki] = 1.0f / static_cast<float>(k); // 均匀权重
            } else {
                // 粒子数不足K个，用0填充
                knn_mappings_[i].particle_indices[ki] = 0;
                knn_mappings_[i].weights[ki] = 0.0f;
            }
        }
    }

    spdlog::info("[GaussianParticleMapper] KNN mapping completed");
}

void GaussianParticleMapper::UploadMappingToGPU(
    const std::shared_ptr<VulkanContext>& context,
    VkCommandBuffer cmd
) {
    spdlog::info("[GaussianParticleMapper] Uploading mapping to GPU");

    // 1. 准备KNN数据
    // 每个高斯8个uint32索引，存储为2个uvec4
    std::vector<glm::uvec4> knn_data(gaussian_count_ * 2);

    for (size_t i = 0; i < gaussian_count_; i++) {
        const auto& mapping = knn_mappings_[i];

        // 第一个uvec4: 索引0-3
        knn_data[i * 2 + 0] = glm::uvec4(
            mapping.particle_indices[0],
            mapping.particle_indices[1],
            mapping.particle_indices[2],
            mapping.particle_indices[3]
        );

        // 第二个uvec4: 索引4-7
        knn_data[i * 2 + 1] = glm::uvec4(
            mapping.particle_indices[4],
            mapping.particle_indices[5],
            mapping.particle_indices[6],
            mapping.particle_indices[7]
        );
    }

    // 2. 创建KNN缓冲区
    size_t knn_buffer_size = knn_data.size() * sizeof(glm::uvec4);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;

    knn_gpu_buffer_ = std::make_shared<Buffer>(
        context,
        static_cast<uint32_t>(knn_buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 上传数据
    knn_gpu_buffer_->upload(reinterpret_cast<const char*>(knn_data.data()), static_cast<uint32_t>(knn_data.size() * sizeof(glm::uvec4)), 0);

    spdlog::debug("[GaussianParticleMapper] KNN buffer uploaded: {} KB",
                 knn_buffer_size / 1024);

    // 3. 创建可变形索引缓冲区（如果设置了）
    if (!deformable_indices_.empty()) {
        size_t index_buffer_size = deformable_indices_.size() * sizeof(uint32_t);

        vk::BufferUsageFlags usageFlags =
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst;

        deformable_index_buffer_ = std::make_shared<Buffer>(
            context,
            static_cast<uint32_t>(index_buffer_size),
            usageFlags,
            VMA_MEMORY_USAGE_GPU_ONLY,
            static_cast<VmaAllocationCreateFlags>(0)
        );

        deformable_index_buffer_->upload(deformable_indices_.data(), static_cast<uint32_t>(sizeof(uint32_t) * deformable_indices_.size()), 0);

        spdlog::debug("[GaussianParticleMapper] Deformable index buffer uploaded: {} KB",
                     index_buffer_size / 1024);
    }

    spdlog::info("[GaussianParticleMapper] Upload complete");
}
