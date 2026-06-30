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

// Initialize() 已内联到头文件：GPU 拖拽改由 MPMManager 每子步 SET 速度 BC 实现，
// DragHandler 仅负责 CPU 端拾取/位置反馈/PC 计算，无需 GPU 资源。

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
        cur_pick_set_ = false;  // 重置：首帧用鼠标速度，后续帧由 Renderer 注入 GPU 回读位置

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
    cur_pick_set_ = false;
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

    // 4. 计算拖拽目标（鼠标在抓取深度平面的世界坐标）
    //    target = 点击点 + 累积鼠标增量（对标 PhysDreamer grab_center_press_world + drag_offset_world）
    const glm::vec3 target_world = dragState_.clickWorldPos
        + camRight * static_cast<float>(dx) * pixelToWorld
        + (-camUp) * static_cast<float>(dy) * pixelToWorld;

    // 5. 位置反馈速度（P 控制器，对标 PhysDreamer gui_demo.py:340 world_v = (target - cur_pick)/frame_dt）
    //    旧实现 dragVel = 鼠标速度(delta/dt)，batch 被强制到鼠标速度，弹性抵抗时边界持续撕裂(strain 2.46)。
    //    位置反馈：dragVel = (target - 当前粒子位置)/dt，batch 被拉向鼠标目标，到位 v=0，
    //    变形被鼠标位移界住（防撕裂）。dragCenter = 当前粒子位置（球随 batch 移动，近似固定掩码）。
    glm::vec3 dragVel_world;
    glm::vec3 dragCenter_world;
    if (cur_pick_set_) {
        dragCenter_world = current_pick_world_;          // 球心=当前粒子位置（跟随 batch）
        dragVel_world = (target_world - current_pick_world_) / deltaTime;
    } else {
        // 首帧无回读：退化为鼠标速度，球心=目标
        dragCenter_world = target_world;
        dragVel_world = (camRight * static_cast<float>(dx)
                         + (-camUp) * static_cast<float>(dy)) * pixelToWorld / deltaTime;
    }

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

} // namespace Interaction

