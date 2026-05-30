#include "DisplacementMapper.h"
#include "vulkan/pipelines/ComputePipeline.h"
#include "vulkan/Shader.h"
#include <spdlog/spdlog.h>

// Shader编译结果 (将在编译时生成)
extern uint32_t SPV_MAP_DISPLACEMENT[];
extern size_t SPV_MAP_DISPLACEMENT_len;

DisplacementMapper::DisplacementMapper(std::shared_ptr<VulkanContext> context)
    : context_(context) {
    spdlog::info("[DisplacementMapper] DisplacementMapper created");
}

void DisplacementMapper::CreatePipeline(const std::shared_ptr<VulkanContext>& context) {
    spdlog::info("[DisplacementMapper] Creating compute pipeline");

    // TODO: 下一阶段集成，需要：
    // 1. 创建descriptor sets
    // 2. 创建compute pipeline
    // 3. 关联资源
}

void DisplacementMapper::MapDisplacements(
    VkCommandBuffer cmd,
    const std::vector<glm::vec3>& particle_displacements,
    uint32_t particle_count,
    const GaussianParticleMapper& mapper,
    const Config& config
) {
    spdlog::trace("[DisplacementMapper] Mapping {} particle displacements to {} gaussians",
                 particle_displacements.size(), mapper.GetDeformableIndices().size());

    // 1. 上传粒子位移到GPU
    if (!displacement_buffer_) {
        displacement_buffer_ = std::make_shared<Buffer>(
            context_,
            particle_count * sizeof(glm::vec3),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY
        );
    }

    displacement_buffer_->uploadData(particle_displacements);

    // 2. 调用GPU版本的映射
    MapDisplacementsGPU(
        cmd,
        static_cast<uint32_t>(mapper.GetDeformableIndices().size()),
        config
    );

    spdlog::trace("[DisplacementMapper] Mapping complete");
}

void DisplacementMapper::MapDisplacementsGPU(
    VkCommandBuffer cmd,
    uint32_t num_deformable,
    const Config& config
) {
    // TODO: 下一阶段实现：
    // 1. 绑定pipeline和descriptor sets
    // 2. 设置push constants (num_deformable, displacement_scale)
    // 3. dispatch shader

    spdlog::trace("[DisplacementMapper] GPU mapping for {} gaussians", num_deformable);
}
