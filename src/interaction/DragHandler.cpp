#include "DragHandler.h"
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>

namespace Interaction {

DragHandler::DragHandler(std::shared_ptr<VulkanContext> context, const Config& config)
    : context_(context), config_(config) {
    spdlog::info("[DragHandler] DragHandler created with stiffness={}", config_.stiffness);
}

DragHandler::~DragHandler() {
    spdlog::info("[DragHandler] DragHandler destroyed");
}

void DragHandler::Initialize() {
    spdlog::info("[DragHandler] Initializing GPU resources...");

    try {
        // 创建应用力的compute pipeline
        spdlog::info("[DragHandler] Creating shader: apply_mouse_force");
        auto shader = std::make_shared<Shader>(context_, "apply_mouse_force");
        spdlog::info("[DragHandler] Loading shader...");
        shader->load();
        spdlog::info("[DragHandler] Shader loaded successfully");

        spdlog::info("[DragHandler] Creating compute pipeline...");
        apply_force_pipeline_ = std::make_shared<ComputePipeline>(context_, shader);

        // Descriptor set bindings
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: ParticleBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        for (const auto& binding : bindings) {
            apply_force_pipeline_->addDescriptorSetLayoutBinding(binding);
        }

        // Push constant range
        // shader需要44字节（根据validation错误消息）
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            44  // shader要求的44字节
        );

        apply_force_pipeline_->addPushConstant(
            pushConstantRange.stageFlags,
            pushConstantRange.offset,
            pushConstantRange.size
        );

        spdlog::info("[DragHandler] Building pipeline...");
        apply_force_pipeline_->build();
        spdlog::info("[DragHandler] Pipeline built successfully");

        spdlog::info("[DragHandler] Creating descriptor set...");
        descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);
        spdlog::info("[DragHandler] Descriptor set created");

        initialized_ = true;
        spdlog::info("[DragHandler] Initialization complete");
    } catch (const std::exception& e) {
        spdlog::error("[DragHandler] Initialization failed: {}", e.what());
        throw;
    }
}

void DragHandler::OnMouseDown(
    int screen_x,
    int screen_y,
    RayCaster& ray_caster,
    uint32_t window_width,
    uint32_t window_height,
    const glm::mat4& view_proj,
    const std::shared_ptr<Buffer>& particle_buffer,
    uint32_t num_particles,
    VkCommandBuffer cmd
) {
    if (!initialized_) {
        spdlog::error("[DragHandler] Not initialized");
        return;
    }

    spdlog::debug("[DragHandler] Mouse down at ({}, {})", screen_x, screen_y);

    // 射线拾取
    auto result = ray_caster.CastFromScreen(
        cmd,
        screen_x,
        screen_y,
        window_width,
        window_height,
        view_proj,
        particle_buffer,
        num_particles
    );

    if (result.success) {
        state_ = DragState::ParticlePicked;
        dragged_particle_ = result.particle_index;
        drag_start_pos_ = result.hit_point;
        drag_start_screen_ = glm::ivec2(screen_x, screen_y);
        current_screen_ = drag_start_screen_;
        current_force_ = glm::vec3(0.0f);

        spdlog::info("[DragHandler] Picked particle {} at ({}, {})",
                     dragged_particle_, screen_x, screen_y);
    } else {
        spdlog::debug("[DragHandler] No particle picked");
    }
}

void DragHandler::OnMouseMove(int screen_x, int screen_y) {
    if (state_ == DragState::Idle) {
        return;
    }

    current_screen_ = glm::ivec2(screen_x, screen_y);

    // 如果已经拾取粒子，进入拖拽状态
    if (state_ == DragState::ParticlePicked) {
        // 判断是否有足够的移动距离（避免误触）
        glm::vec2 delta = glm::vec2(current_screen_ - drag_start_screen_);
        if (glm::length(delta) > 5.0f) {  // 5像素阈值
            state_ = DragState::Dragging;
            spdlog::info("[DragHandler] Started dragging particle {}", dragged_particle_);
        }
    }

    // 计算当前拖拽力（将在ApplyForce时使用）
    if (state_ == DragState::Dragging) {
        // 力将在ApplyForce中计算
    }
}

void DragHandler::OnMouseUp() {
    if (state_ != DragState::Idle) {
        spdlog::info("[DragHandler] Mouse up - releasing particle {}", dragged_particle_);
        state_ = DragState::Idle;
        dragged_particle_ = UINT32_MAX;
        current_force_ = glm::vec3(0.0f);
    }
}

void DragHandler::ApplyForce(
    VkCommandBuffer cmd,
    const std::shared_ptr<Buffer>& particle_buffer,
    float dt
) {
    if (state_ != DragState::Dragging || dragged_particle_ == UINT32_MAX) {
        return;
    }

    // 计算拖拽力（简化版本：基于屏幕位移）
    glm::vec2 screen_delta = glm::vec2(current_screen_ - drag_start_screen_);
    float screen_distance = glm::length(screen_delta);

    if (screen_distance < 1.0f) {
        return;  // 位移太小，不施加力
    }

    // 力的大小与拖拽距离成正比
    float force_magnitude = config_.stiffness * screen_distance * 0.01f;  // 缩放因子
    force_magnitude = glm::min(force_magnitude, config_.max_force);

    // 力的方向：从起点指向当前鼠标位置（投影到世界空间）
    // 简化：假设在相机平面上拖拽
    glm::vec3 force_direction = glm::vec3(screen_delta.x, -screen_delta.y, 0.0f);
    if (glm::length(force_direction) > 0.0f) {
        force_direction = glm::normalize(force_direction);
    }

    current_force_ = force_direction * force_magnitude;

    // 只在第一次时绑定particle buffer并build descriptor set
    if (!descriptor_set_built_) {
        spdlog::debug("[DragHandler] Building descriptor set for apply force");
        descriptor_set_->bindBufferToDescriptorSet(
            0,
            vk::DescriptorType::eStorageBuffer,
            vk::ShaderStageFlagBits::eCompute,
            particle_buffer
        );
        descriptor_set_->build();
        descriptor_set_built_ = true;
    }

    // Push constants
    struct ApplyForceParams {
        uint32_t particle_index;
        glm::vec3 force;
        float max_velocity;
        float dt;
        uint32_t padding[2];
    };
    ApplyForceParams params{
        dragged_particle_,
        current_force_,
        10.0f,  // 最大速度限制 [m/s]
        dt
    };

    // 绑定pipeline和dispatch
    apply_force_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
    vkCmdPushConstants(cmd, apply_force_pipeline_->pipelineLayout.get(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, 1, 1, 1);

    spdlog::trace("[DragHandler] Applied force {} to particle {}",
                  glm::length(current_force_), dragged_particle_);
}

DragHandler::DragInfo DragHandler::GetDragInfo() const {
    DragInfo info;
    info.active = (state_ == DragState::Dragging);
    info.particle_position = drag_start_pos_;  // 简化：使用起始位置
    info.force_vector = current_force_;
    return info;
}

glm::vec3 DragHandler::CalculateDragForce(
    int current_screen_x,
    int current_screen_y,
    uint32_t window_width,
    uint32_t window_height,
    const glm::mat4& inverse_view_proj
) {
    // 计算屏幕位移
    glm::vec2 screen_delta = glm::vec2(
        current_screen_x - drag_start_screen_.x,
        current_screen_y - drag_start_screen_.y
    );

    // 转换为世界位移
    glm::vec3 world_delta = ScreenDeltaToWorldDelta(
        screen_delta,
        inverse_view_proj,
        window_width,
        window_height
    );

    // 力 = 刚度 * 位移
    glm::vec3 force = world_delta * config_.stiffness;

    // 限制最大力
    float force_magnitude = glm::length(force);
    if (force_magnitude > config_.max_force) {
        force = glm::normalize(force) * config_.max_force;
    }

    return force;
}

glm::vec3 DragHandler::ScreenDeltaToWorldDelta(
    const glm::vec2& screen_delta,
    const glm::mat4& inverse_view_proj,
    uint32_t window_width,
    uint32_t window_height
) {
    // 简化实现：将屏幕位移映射到世界空间
    // 假设物体在drag_plane_depth深度

    float ndc_scale_x = 2.0f / window_width;
    float ndc_scale_y = 2.0f / window_height;

    glm::vec2 ndc_delta = screen_delta * glm::vec2(ndc_scale_x, -ndc_scale_y);

    // 在drag_plane_depth深度处的世界位移
    // 这是一个近似，假设投影中心在相机前方
    glm::vec3 world_delta = glm::vec3(ndc_delta.x * config_.drag_plane_depth,
                                      ndc_delta.y * config_.drag_plane_depth,
                                      0.0f);

    return world_delta;
}

} // namespace Interaction
