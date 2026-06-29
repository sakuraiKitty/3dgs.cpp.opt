#include "DragHandler.h"
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>

namespace Interaction {

DragHandler::DragHandler(std::shared_ptr<VulkanContext> context, const Config& config)
    : context_(context), config_(config) {
    spdlog::info("[DragHandler] Created with dragRadius={}, alpha={}",
                 config_.dragRadius, config_.alpha);
}

DragHandler::~DragHandler() {
    spdlog::info("[DragHandler] Destroyed");
}

void DragHandler::Initialize() {
    spdlog::info("[DragHandler] Initializing GPU resources...");

    try {
        // ── 1. drag_particle pipeline (核心拖拽shader) ──
        // 严格遵循设计文档：binding 0=vec3 pos[], binding 1=vec3 vel[]
        // Push constant: 48 bytes (DragPC layout)
        spdlog::info("[DragHandler] Creating drag_pipeline...");
        auto drag_shader = std::make_shared<Shader>(context_, "drag_particle");
        drag_shader->load();
        drag_pipeline_ = std::make_shared<ComputePipeline>(context_, drag_shader);

        std::vector<vk::DescriptorSetLayoutBinding> drag_bindings = {
            // binding 0: ParticlePos (vec3 pos[])
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: ParticleVel (vec3 vel[])
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        };
        for (const auto& b : drag_bindings) {
            drag_pipeline_->addDescriptorSetLayoutBinding(b);
        }
        drag_pipeline_->addPushConstant(
            vk::ShaderStageFlagBits::eCompute, 0, sizeof(DragPushConstants));
        drag_pipeline_->build();

        drag_descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);
        spdlog::info("[DragHandler] drag_pipeline created (48B push constant)");

        // ── 2. extract_particle_fields pipeline (同步: ParticleData → pos/vel) ──
        // binding 0=ParticleData, binding 1=dragPos, binding 2=dragVel
        spdlog::info("[DragHandler] Creating extract_pipeline...");
        auto extract_shader = std::make_shared<Shader>(context_, "extract_particle_fields");
        extract_shader->load();
        extract_pipeline_ = std::make_shared<ComputePipeline>(context_, extract_shader);

        std::vector<vk::DescriptorSetLayoutBinding> extract_bindings = {
            // binding 0: ParticleData particles[]
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: dragPos (vec3[])
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 2: dragVel (vec3[])
            vk::DescriptorSetLayoutBinding()
                .setBinding(2)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        };
        for (const auto& b : extract_bindings) {
            extract_pipeline_->addDescriptorSetLayoutBinding(b);
        }
        // extract shader 无 push constant
        extract_pipeline_->build();

        extract_descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);
        spdlog::info("[DragHandler] extract_pipeline created");

        // ── 3. writeback_velocity pipeline (同步: vel → ParticleData) ──
        // binding 0=dragVel, binding 1=ParticleData
        spdlog::info("[DragHandler] Creating writeback_pipeline...");
        auto writeback_shader = std::make_shared<Shader>(context_, "writeback_velocity");
        writeback_shader->load();
        writeback_pipeline_ = std::make_shared<ComputePipeline>(context_, writeback_shader);

        std::vector<vk::DescriptorSetLayoutBinding> writeback_bindings = {
            // binding 0: dragVel (vec3[])
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: ParticleData particles[]
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        };
        for (const auto& b : writeback_bindings) {
            writeback_pipeline_->addDescriptorSetLayoutBinding(b);
        }
        // writeback shader 无 push constant
        writeback_pipeline_->build();

        writeback_descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);
        spdlog::info("[DragHandler] writeback_pipeline created");

        initialized_ = true;
        spdlog::info("[DragHandler] Initialization complete (3 pipelines + 3 descriptor sets)");
    } catch (const std::exception& e) {
        spdlog::error("[DragHandler] Initialization failed: {}", e.what());
        throw;
    }
}

void DragHandler::OnMouseDownFromResult(
    const RayCastResult& result,
    int screen_x,
    int screen_y,
    const glm::vec3& cameraPosition,
    const glm::vec3& cameraForward,
    const MPM::CoordinateTransform& coordTransform
) {
    if (!initialized_) {
        spdlog::error("[DragHandler] Not initialized");
        return;
    }

    if (result.success) {
        dragState_.isDragging = true;
        dragState_.pickedParticle = result.particle_index;
        dragState_.lastMousePos = glm::dvec2(screen_x, screen_y);

        // 严格遵循设计文档 Section 4.1.1:
        // 1. RayCastResult.hit_point 是归一化空间位置
        // 2. 转换到世界空间得到 clickWorldPos
        // 3. 计算 clickDepth = dot(clickWorldPos - camera.position, camera.forward)
        dragState_.clickWorldPos = coordTransform.ToOriginal(result.hit_point);
        dragState_.clickDepth = glm::dot(
            dragState_.clickWorldPos - cameraPosition,
            cameraForward
        );

        dragStartScreen_ = glm::ivec2(screen_x, screen_y);
        currentScreen_ = dragStartScreen_;

        spdlog::info("[DragHandler] Picked particle {} at ({}, {}) "
                     "clickWorldPos=({:.4f},{:.4f},{:.4f}) clickDepth={:.4f}",
                     dragState_.pickedParticle, screen_x, screen_y,
                     dragState_.clickWorldPos.x, dragState_.clickWorldPos.y,
                     dragState_.clickWorldPos.z, dragState_.clickDepth);
    } else {
        spdlog::debug("[DragHandler] No particle picked (CPU ray cast)");
    }
}

void DragHandler::OnMouseMove(int screen_x, int screen_y) {
    if (!dragState_.isDragging) {
        return;
    }

    // 只更新 currentScreen_，不更新 lastMousePos
    // lastMousePos 由 ComputeDragPushConstants 在消耗 delta 后更新
    // 这样每帧的 delta = currentScreen_ - lastMousePos = 鼠标移动增量
    spdlog::debug("[DragHandler] OnMouseMove: screen=({},{}), currentScreen_before=({},{}), lastMousePos=({:.1f},{:.1f})",
                 screen_x, screen_y, currentScreen_.x, currentScreen_.y,
                 dragState_.lastMousePos.x, dragState_.lastMousePos.y);
    currentScreen_ = glm::ivec2(screen_x, screen_y);
}

void DragHandler::OnMouseUp() {
    if (dragState_.isDragging) {
        spdlog::info("[DragHandler] Mouse up — releasing particle {} "
                     "(保留拖拽末端动量，靠MPM阻尼自然衰减)",
                     dragState_.pickedParticle);
    }
    // 严格遵循设计文档 Section 4.1.3:
    // 禁止清零粒子速度！保留拖拽末端动量，靠MPM阻尼自然衰减
    dragState_.isDragging = false;
    dragState_.pickedParticle = UINT32_MAX;
}

DragPushConstants DragHandler::ComputeDragPushConstants(
    float deltaTime,
    const glm::vec3& cameraPosition,
    const glm::quat& cameraRotation,
    float cameraFov,
    uint32_t windowHeight,
    const MPM::CoordinateTransform& coordTransform
) {
    DragPushConstants pc{};
    pc.isDragging = 0;  // 默认：未拖拽
    pc.maxVelocity = cfl_set_ ? cfl_max_velocity_ : 0.0f;  // 0 = 着色器不限制

    if (!dragState_.isDragging) {
        return pc;
    }

    // 严格遵循设计文档 Section 4.1.2:

    // 1. 计算单帧鼠标像素增量
    // currentScreen_ 由 OnMouseMove 更新，lastMousePos 由本函数在消耗 delta 后更新
    const double dx = static_cast<double>(currentScreen_.x) - dragState_.lastMousePos.x;
    const double dy = static_cast<double>(currentScreen_.y) - dragState_.lastMousePos.y;
    spdlog::debug("[DragHandler] ComputeDrag: currentScreen=({},{}), lastMousePos=({:.1f},{:.1f}), dx={:.1f}, dy={:.1f}",
                 currentScreen_.x, currentScreen_.y,
                 dragState_.lastMousePos.x, dragState_.lastMousePos.y, dx, dy);

    // 消耗 delta：更新 lastMousePos 为当前屏幕坐标
    // 下次调用时 delta = 0（除非 OnMouseMove 又更新了 currentScreen_）
    dragState_.lastMousePos = glm::dvec2(currentScreen_.x, currentScreen_.y);

    // 2. 像素转世界空间系数：与深度、FOV、分辨率绑定
    const float pixelToWorld =
        (2.0f * dragState_.clickDepth * glm::tan(glm::radians(cameraFov) * 0.5f)) /
        static_cast<float>(windowHeight);

    // 3. 相机方向向量
    const glm::vec3 camRight   = cameraRotation * glm::vec3(1, 0, 0);
    const glm::vec3 camUp      = cameraRotation * glm::vec3(0, 1, 0);
    // 屏幕Y轴向下，与相机up方向相反

    // 4. 计算世界空间拖拽速度（除以deltaTime消除帧率依赖）
    const glm::vec3 dragVel_world = (camRight * static_cast<float>(dx)
                                    + (-camUp) * static_cast<float>(dy))
                                    * pixelToWorld / deltaTime;

    // 5. 计算当前拖拽中心（沿相机平面平移，深度不变）
    const glm::vec3 dragCenter_world = dragState_.clickWorldPos
        + camRight * static_cast<float>(dx) * pixelToWorld
        + (-camUp) * static_cast<float>(dy) * pixelToWorld;

    // 6. 转换到归一化空间
    pc.dragCenter   = coordTransform.ToNormalized(dragCenter_world);
    pc.dragRadius   = config_.dragRadius / coordTransform.scale;
    pc.dragVelocity = dragVel_world / coordTransform.scale;
    pc.alpha        = config_.alpha;
    pc.isDragging   = 1;

    // info 级打印（每 10 次拖拽一帧）——确认 CFL 限幅是否注入到 pc.maxVelocity
    // 期望：cfl_set=1, maxVel≈3.0；若 maxVel=0 → SetCFLParams 未调用；若 maxVel=30 → 旧 exe
    static int pc_log_cnt = 0;
    if (pc_log_cnt++ % 10 == 0) {
        spdlog::info("[DragHandler] PC: center=({:.4f},{:.4f},{:.4f}) radius={:.4f} "
                     "|dragVel|={:.4f} alpha={:.2f} maxVel={:.4f} cfl_set={}",
                     pc.dragCenter.x, pc.dragCenter.y, pc.dragCenter.z,
                     pc.dragRadius,
                     glm::length(pc.dragVelocity),
                     pc.alpha, pc.maxVelocity, cfl_set_);
    }

    return pc;
}

void DragHandler::CreateSyncBuffers(uint32_t num_particles) {
    // vec3[] stride=16 (GLSL std430 base alignment=16)
    // buffer 大小 = num_particles * 16 bytes
    const uint32_t buffer_size = num_particles * sizeof(glm::vec4);  // sizeof(glm::vec4) = 16

    spdlog::info("[DragHandler] Creating sync buffers: {} particles, {} bytes each",
                 num_particles, buffer_size);

    drag_pos_buffer_ = Buffer::storage(context_, buffer_size, false, 0,
                                       "Drag Pos Buffer");
    drag_vel_buffer_ = Buffer::storage(context_, buffer_size, false, 0,
                                       "Drag Vel Buffer");

    // 初始化为零（避免首次 extract 前的垃圾数据）
    // GPU-only buffer 无法直接 upload，依赖 extract shader 填充

    sync_buffers_created_ = true;
    spdlog::info("[DragHandler] Sync buffers created");
}

void DragHandler::BuildDescriptorSets(const std::shared_ptr<Buffer>& particle_buffer) {
    spdlog::info("[DragHandler] Building descriptor sets (lazy init)");

    // ── drag descriptor set: binding 0=pos, binding 1=vel ──
    drag_descriptor_set_->bindBufferToDescriptorSet(
        0, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, drag_pos_buffer_);
    drag_descriptor_set_->bindBufferToDescriptorSet(
        1, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, drag_vel_buffer_);
    drag_descriptor_set_->build();
    drag_pipeline_->addDescriptorSet(0, drag_descriptor_set_);

    // ── extract descriptor set: binding 0=ParticleData, 1=pos, 2=vel ──
    extract_descriptor_set_->bindBufferToDescriptorSet(
        0, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, particle_buffer);
    extract_descriptor_set_->bindBufferToDescriptorSet(
        1, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, drag_pos_buffer_);
    extract_descriptor_set_->bindBufferToDescriptorSet(
        2, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, drag_vel_buffer_);
    extract_descriptor_set_->build();
    extract_pipeline_->addDescriptorSet(0, extract_descriptor_set_);

    // ── writeback descriptor set: binding 0=vel, 1=ParticleData ──
    writeback_descriptor_set_->bindBufferToDescriptorSet(
        0, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, drag_vel_buffer_);
    writeback_descriptor_set_->bindBufferToDescriptorSet(
        1, vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute, particle_buffer);
    writeback_descriptor_set_->build();
    writeback_pipeline_->addDescriptorSet(0, writeback_descriptor_set_);

    descriptor_sets_built_ = true;
    spdlog::info("[DragHandler] All 3 descriptor sets built and bound to pipelines");
}

void DragHandler::RecordComputeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void DragHandler::ApplyDrag(
    VkCommandBuffer cmd,
    const std::shared_ptr<Buffer>& particle_buffer,
    uint32_t num_particles,
    const DragPushConstants& pushConstants
) {
    if (!initialized_) {
        spdlog::error("[DragHandler] Not initialized");
        return;
    }

    // ── 延迟初始化：首次调用时创建 sync buffers 和 descriptor sets ──
    if (!sync_buffers_created_) {
        CreateSyncBuffers(num_particles);
    }
    if (!descriptor_sets_built_) {
        BuildDescriptorSets(particle_buffer);
    }

    const uint32_t groups = (num_particles + 255) / 256;

    // ── Pass 1: Extract (ParticleData → drag_pos + drag_vel) ──
    spdlog::debug("[DragHandler] Pass 1: Extract (ParticleData → pos/vel)");
    extract_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
    vkCmdDispatch(cmd, groups, 1, 1);
    RecordComputeBarrier(cmd);

    // ── Pass 2: Drag (速度插值 on drag buffers) ──
    spdlog::debug("[DragHandler] Pass 2: Drag (velocity interpolation)");
    drag_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
    vkCmdPushConstants(cmd, drag_pipeline_->pipelineLayout.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(DragPushConstants), &pushConstants);
    vkCmdDispatch(cmd, groups, 1, 1);
    RecordComputeBarrier(cmd);

    // ── Pass 3: Writeback (drag_vel → ParticleData.velocity) ──
    spdlog::debug("[DragHandler] Pass 3: Writeback (vel → ParticleData)");
    writeback_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
    vkCmdDispatch(cmd, groups, 1, 1);
    RecordComputeBarrier(cmd);

    spdlog::debug("[DragHandler] ApplyDrag complete: 3 passes dispatched, "
                 "isDragging={}, dragCenter=({:.4f},{:.4f},{:.4f})",
                 pushConstants.isDragging,
                 pushConstants.dragCenter.x,
                 pushConstants.dragCenter.y,
                 pushConstants.dragCenter.z);
}

} // namespace Interaction
