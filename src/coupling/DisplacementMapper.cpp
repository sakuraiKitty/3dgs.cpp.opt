#include "DisplacementMapper.h"
#include "../vulkan/Shader.h"
#include <spdlog/spdlog.h>

// Shader编译结果 (将在编译时生成)
extern uint32_t SPV_MAP_DISPLACEMENT_WITH_ROTATION[];
extern size_t SPV_MAP_DISPLACEMENT_WITH_ROTATION_len;

DisplacementMapper::DisplacementMapper(std::shared_ptr<VulkanContext> context)
    : context_(context) {
    spdlog::info("[DisplacementMapper] DisplacementMapper created");
}

DisplacementMapper::~DisplacementMapper() {
    spdlog::info("[DisplacementMapper] DisplacementMapper destroyed");
}

void DisplacementMapper::Initialize() {
    spdlog::info("[DisplacementMapper] Initializing displacement mapper");

    // 创建 descriptor set
    descriptor_set_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

    // 定义所有绑定
    std::vector<vk::DescriptorSetLayoutBinding> bindings = {
        // binding 0: DeformableIndices (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(0)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 1: DriveParticlePositions (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 2: DriveParticleDisplacements (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(2)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 3: TopKMapping (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(3)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 4: TopKWeights (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(4)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 5: OriginalGaussianPositions (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(5)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 6: OriginalGaussianRotations (readonly)
        vk::DescriptorSetLayoutBinding()
            .setBinding(6)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 7: GaussianPositions (write)
        vk::DescriptorSetLayoutBinding()
            .setBinding(7)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),

        // binding 8: GaussianRotations (write)
        vk::DescriptorSetLayoutBinding()
            .setBinding(8)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute)
    };

    // 创建 pipeline
    auto shader = std::make_shared<Shader>(context_, "map_displacement_with_rotation");
    map_pipeline_ = std::make_shared<ComputePipeline>(context_, shader);

    // 添加 descriptor set layouts
    for (const auto& binding : bindings) {
        map_pipeline_->addDescriptorSetLayoutBinding(binding);
    }

    // 添加 push constants（包含坐标变换参数）
    vk::PushConstantRange pushConstantRange(
        vk::ShaderStageFlagBits::eCompute,
        0,
        sizeof(uint32_t) * 4 + sizeof(float) + sizeof(glm::vec3)  // num_gaussians + k + num_particles + padding + coord_scale + coord_shift
    );
    map_pipeline_->addPushConstant(
        pushConstantRange.stageFlags,
        pushConstantRange.offset,
        pushConstantRange.size
    );

    // 构建 pipeline
    map_pipeline_->build();

    // 添加 descriptor set 到 pipeline
    map_pipeline_->addDescriptorSet(0, descriptor_set_);

    initialized_ = true;

    spdlog::info("[DisplacementMapper] Initialization complete");
}

void DisplacementMapper::SetResources(
    const std::shared_ptr<Buffer>& deformable_index_buffer,
    const std::shared_ptr<Buffer>& drive_position_buffer,
    const std::shared_ptr<Buffer>& drive_displacement_buffer,
    const std::shared_ptr<Buffer>& top_k_index_buffer,
    const std::shared_ptr<Buffer>& top_k_weight_buffer,
    const std::shared_ptr<Buffer>& original_position_buffer,
    const std::shared_ptr<Buffer>& original_rotation_buffer,
    const std::shared_ptr<Buffer>& gaussian_position_buffer,
    const std::shared_ptr<Buffer>& gaussian_rotation_buffer
) {
    if (!initialized_) {
        spdlog::error("[DisplacementMapper] Cannot set resources: not initialized");
        return;
    }

    deformable_index_buffer_ = deformable_index_buffer;
    drive_position_buffer_ = drive_position_buffer;
    drive_displacement_buffer_ = drive_displacement_buffer;
    top_k_index_buffer_ = top_k_index_buffer;
    top_k_weight_buffer_ = top_k_weight_buffer;
    original_position_buffer_ = original_position_buffer;
    original_rotation_buffer_ = original_rotation_buffer;
    gaussian_position_buffer_ = gaussian_position_buffer;
    gaussian_rotation_buffer_ = gaussian_rotation_buffer;

    // 绑定所有缓冲区到 descriptor set
    descriptor_set_->bindBufferToDescriptorSet(
        0, // binding 0: DeformableIndices
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        deformable_index_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        1, // binding 1: DriveParticlePositions
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        drive_position_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        2, // binding 2: DriveParticleDisplacements
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        drive_displacement_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        3, // binding 3: TopKMapping
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        top_k_index_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        4, // binding 4: TopKWeights
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        top_k_weight_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        5, // binding 5: OriginalPositions
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        original_position_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        6, // binding 6: OriginalRotations
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        original_rotation_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        7, // binding 7: GaussianPositions
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        gaussian_position_buffer_
    );

    descriptor_set_->bindBufferToDescriptorSet(
        8, // binding 8: GaussianRotations
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        gaussian_rotation_buffer_
    );

    // 构建 descriptor sets
    descriptor_set_->build();

    spdlog::debug("[DisplacementMapper] Resources bound successfully");
}

void DisplacementMapper::MapDisplacements(
    VkCommandBuffer cmd,
    uint32_t num_deformable,
    uint32_t num_particles,
    float coord_scale,
    const glm::vec3& coord_shift,
    const Config& config
) {
    if (!initialized_) {
        spdlog::error("[DisplacementMapper] Cannot map: not initialized");
        return;
    }

    // Push constants（与shader布局一致：vec3在最前确保16字节对齐）
    // GLSL std430: vec3 alignment=16, float占vec3尾部4字节(offset 12)
    // C++ glm::vec3 alignment=4, 所以C++和GLSL字节偏移完全一致
    struct PushConstants {
        glm::vec3 coord_shift;      // offset 0  (GLSL: vec3 align16, C++: 3 floats)
        float coord_scale;          // offset 12 (占vec3尾部4字节)
        uint32_t num_gaussians;     // offset 16
        uint32_t k;                 // offset 20
        uint32_t num_particles;     // offset 24
        uint32_t padding1;          // offset 28
    };
    PushConstants push_constants{
        coord_shift,     // MPM坐标变换shift
        coord_scale,     // MPM坐标变换scale
        num_deformable,  // num_gaussians
        8,               // k (固定为8)
        num_particles,   // num_particles
        0                // padding1
    };

    // 绑定 pipeline 和 descriptor set
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, map_pipeline_->pipeline.get());
    map_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));

    // 设置 push constants
    vkCmdPushConstants(
        cmd,
        map_pipeline_->pipelineLayout.get(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push_constants),
        &push_constants
    );

    // Dispatch
    uint32_t workgroups = (num_deformable + 255) / 256;
    vkCmdDispatch(cmd, workgroups, 1, 1);

    spdlog::trace("[DisplacementMapper] Dispatched {} workgroups for {} gaussians, coord_scale={:.4f}",
                 workgroups, num_deformable, coord_scale);
}
