#include "RayCaster.h"
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>

namespace Interaction {

RayCaster::RayCaster(std::shared_ptr<VulkanContext> context, const Config& config)
    : context_(context), config_(config) {
    spdlog::info("[RayCaster] RayCaster created");
}

RayCaster::~RayCaster() {
    spdlog::info("[RayCaster] RayCaster destroyed");
}

void RayCaster::Initialize() {
    spdlog::info("[RayCaster] Initializing GPU resources...");

    // 创建距离缓冲区（每个粒子一个float）
    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eTransferSrc;

    distance_buffer_ = std::make_shared<Buffer>(
        context_,
        sizeof(float) * 1000000,  // 初始大小1M粒子
        usageFlags,
        VMA_MEMORY_USAGE_GPU_TO_CPU,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 创建pipeline
    CreatePipeline();

    initialized_ = true;
    spdlog::info("[RayCaster] Initialization complete");
}

RayCastResult RayCaster::CastFromScreen(
    VkCommandBuffer cmd,
    int screen_x,
    int screen_y,
    uint32_t window_width,
    uint32_t window_height,
    const glm::mat4& view_proj_matrix,
    const std::shared_ptr<Buffer>& particle_buffer,
    uint32_t num_particles
) {
    if (!initialized_) {
        spdlog::error("[RayCaster] Not initialized");
        return {};
    }

    // 1. 屏幕坐标转NDC
    glm::vec2 ndc = ScreenToNDC(screen_x, screen_y, window_width, window_height);

    // 2. NDC转世界射线
    glm::mat4 inverse_view_proj = glm::inverse(view_proj_matrix);
    glm::vec3 ray_origin, ray_direction;
    NDCToWorldRay(ndc, inverse_view_proj, ray_origin, ray_direction);

    // 3. GPU射线拾取
    return CastFromRayGPU(cmd, ray_origin, ray_direction, particle_buffer, num_particles);
}

RayCastResult RayCaster::CastFromRayGPU(
    VkCommandBuffer cmd,
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const std::shared_ptr<Buffer>& particle_buffer,
    uint32_t num_particles
) {
    // 绑定资源
    descriptor_set_->bindBufferToDescriptorSet(
        0, // binding 0: ParticleBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        particle_buffer
    );

    descriptor_set_->bindBufferToDescriptorSet(
        1, // binding 1: DistanceBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        distance_buffer_
    );

    descriptor_set_->build();

    // Push constants
    struct RayCastParams {
        glm::vec3 ray_origin;
        float max_distance_sq;
        glm::vec3 ray_direction;
        uint32_t num_particles;
    };
    RayCastParams params{
        ray_origin,
        config_.max_distance * config_.max_distance,
        ray_direction,
        num_particles
    };

    // 绑定pipeline和descriptor set
    ray_cast_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
    vkCmdPushConstants(cmd, ray_cast_pipeline_->pipelineLayout.get(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

    // Dispatch（256个线程一组）
    uint32_t groups = (num_particles + 255) / 256;
    vkCmdDispatch(cmd, groups, 1, 1);

    // Memory barrier
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);

    // 读取所有距离（CPU端找到最小值）
    std::vector<char> distance_data = distance_buffer_->download();

    RayCastResult result;
    result.particle_index = UINT32_MAX;
    result.distance_sq = config_.max_distance * config_.max_distance;
    result.success = false;

    // 在CPU端找到最小距离
    float* distances = reinterpret_cast<float*>(distance_data.data());
    for (uint32_t i = 0; i < num_particles; i++) {
        if (distances[i] < result.distance_sq) {
            result.distance_sq = distances[i];
            result.particle_index = i;
            result.success = true;
        }
    }

    if (result.success) {
        result.hit_point = ray_origin + ray_direction * sqrt(result.distance_sq);
    }

    return result;
}

RayCastResult RayCaster::CastFromRayCPU(
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const std::vector<glm::vec3>& particle_positions
) {
    RayCastResult result;
    result.particle_index = UINT32_MAX;
    result.distance_sq = config_.max_distance * config_.max_distance;
    result.success = false;

    for (size_t i = 0; i < particle_positions.size(); i++) {
        const glm::vec3& pos = particle_positions[i];

        // 计算点到射线的距离
        glm::vec3 v = pos - ray_origin;
        float projection = glm::dot(v, ray_direction);
        glm::vec3 closest_point = ray_origin + ray_direction * projection;
        glm::vec3 diff = pos - closest_point;
        float distance_sq = glm::dot(diff, diff);

        if (distance_sq < result.distance_sq) {
            result.distance_sq = distance_sq;
            result.particle_index = static_cast<uint32_t>(i);
            result.hit_point = closest_point;
            result.success = true;
        }
    }

    return result;
}

void RayCaster::CreatePipeline() {
    spdlog::info("[RayCaster] Creating ray cast compute pipeline...");

    // 创建shader
    auto shader = std::make_shared<Shader>(context_, "ray_cast_particles");

    // 创建pipeline
    ray_cast_pipeline_ = std::make_shared<ComputePipeline>(context_, shader);

    // Descriptor set bindings
    std::vector<vk::DescriptorSetLayoutBinding> bindings = {
        // binding 0: ParticleBuffer
        vk::DescriptorSetLayoutBinding()
            .setBinding(0)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        // binding 1: ResultBuffer
        vk::DescriptorSetLayoutBinding()
            .setBinding(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setDescriptorCount(1)
            .setStageFlags(vk::ShaderStageFlagBits::eCompute)
    };

    for (const auto& binding : bindings) {
        ray_cast_pipeline_->addDescriptorSetLayoutBinding(binding);
    }

    // Push constant range
    vk::PushConstantRange pushConstantRange(
        vk::ShaderStageFlagBits::eCompute,
        0,
        sizeof(glm::vec3) + sizeof(float) + sizeof(glm::vec3) + sizeof(uint32_t)  // ray_origin + max_distance_sq + ray_direction + num_particles = 32 bytes
    );

    ray_cast_pipeline_->addPushConstant(
        pushConstantRange.stageFlags,
        pushConstantRange.offset,
        pushConstantRange.size
    );

    // 创建descriptor set
    descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);

    // 构建 pipeline
    ray_cast_pipeline_->build();

    spdlog::info("[RayCaster] Pipeline created successfully");
}

glm::vec2 RayCaster::ScreenToNDC(int screen_x, int screen_y, uint32_t width, uint32_t height) const {
    // 屏幕坐标转NDC: [-1, 1]
    float ndc_x = (2.0f * screen_x) / width - 1.0f;
    float ndc_y = 1.0f - (2.0f * screen_y) / height;  // Y轴翻转

    return glm::vec2(ndc_x, ndc_y);
}

void RayCaster::NDCToWorldRay(
    const glm::vec2& ndc,
    const glm::mat4& inverse_view_proj,
    glm::vec3& out_origin,
    glm::vec3& out_direction
) const {
    // Near plane point (NDC z = -1 in OpenGL, but we'll use z = 0 for camera space)
    glm::vec4 near_point_ndc(ndc.x, ndc.y, 0.0f, 1.0f);
    glm::vec4 near_point_world = inverse_view_proj * near_point_ndc;

    // Far plane point
    glm::vec4 far_point_ndc(ndc.x, ndc.y, 1.0f, 1.0f);
    glm::vec4 far_point_world = inverse_view_proj * far_point_ndc;

    // Perspective divide
    if (near_point_world.w != 0.0f) {
        near_point_world /= near_point_world.w;
    }
    if (far_point_world.w != 0.0f) {
        far_point_world /= far_point_world.w;
    }

    // 射线起点和方向
    out_origin = glm::vec3(near_point_world);
    out_direction = glm::normalize(glm::vec3(far_point_world) - out_origin);
}

} // namespace Interaction
