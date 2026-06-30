#ifndef DRAG_HANDLER_H
#define DRAG_HANDLER_H

#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include "../vulkan/pipelines/ComputePipeline.h"
#include "../vulkan/Shader.h"
#include "../vulkan/DescriptorSet.h"
#include "RayCaster.h"
#include "../mpm/MPMStructs.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

namespace Interaction {

/**
 * 拖拽状态（CPU缓存，每帧更新）
 * 严格遵循设计文档 Section 3.1
 */
struct DragState {
    bool      isDragging = false;
    glm::dvec2 lastMousePos;      // 上一帧鼠标屏幕坐标（像素）
    glm::vec3 clickWorldPos;      // 首次点击的世界空间坐标
    float     clickDepth;         // 点击点的相机空间深度（锁定拖拽平面）
    uint32_t  pickedParticle = UINT32_MAX;  // 拾取的粒子索引（可视化用）
};

/**
 * Push Constant 结构体（GPU传输，严格控制在128字节内）
 * 严格遵循设计文档 Section 3.1 — 布局与 GLSL std430 完全匹配
 *
 * 布局 (48 bytes):
 *   offset 0:  vec3 dragCenter    (12 bytes) — 与 float dragRadius 组合 = 16 bytes
 *   offset 12: float dragRadius   (4 bytes)  — vec3 的第4字节槽
 *   offset 16: vec3 dragVelocity  (12 bytes) — 与 float alpha 组合 = 16 bytes
 *   offset 28: float alpha        (4 bytes)  — vec3 的第4字节槽
 *   offset 32: int32 isDragging   (4 bytes)
 *   offset 36: float maxVelocity  (4 bytes)  — CFL 速度上限 (0=不限制)
 *   offset 40: int32 _pad[2]      (8 bytes)
 *   Total: 48 bytes ✓
 */
struct DragPushConstants {
    glm::vec3 dragCenter;     // 拖拽中心（归一化空间）
    float     dragRadius;     // 拖拽半径（归一化空间）
    glm::vec3 dragVelocity;   // 拖拽速度（归一化空间/s）
    float     alpha;          // 速度跟随系数 0~1
    int32_t   isDragging;     // 0=未拖拽 1=拖拽中
    float     maxVelocity;    // CFL 速度上限: |vel|*sub_dt <= 0.5*dx (0=不限制)
    int32_t   _pad[2];        // 4字节对齐填充
};

/**
 * 拖拽处理器 — 速度插值模式
 * 管理鼠标拖拽交互，通过速度插值影响粒子
 *
 * 核心原理（设计文档 Section 2.3）：
 * 在每帧 MPM 求解前，通过计算着色器对拖拽范围内的粒子执行速度插值，
 * 替代直接强制赋值，降低数值冲击，同时保留拖拽跟手感。
 *
 * GPU 流程（3-pass）:
 *   Extract → Drag → Writeback → MPM Step
 */
class DragHandler {
public:
    /**
     * 配置 — 严格遵循设计文档 Section 5
     */
    struct Config {
        float dragRadius = 0.15f;   // 拖拽作用半径（世界空间单位）— 0.05 太小致局部应力爆炸
        float alpha = 0.4f;         // 速度跟随系数 — 0.8 过高致边界剪切 F 越界
    };

    explicit DragHandler(std::shared_ptr<VulkanContext> context, const Config& config = Config());

    ~DragHandler();

    /**
     * 初始化（保留为接口；GPU 拖拽已移至 MPMManager 每子步 SET BC，此处无 GPU 资源需创建）
     */
    void Initialize() { initialized_ = true; }

    /**
     * 鼠标按下事件 — 使用预计算的射线拾取结果
     * 严格遵循设计文档 Section 4.1.1
     *
     * @param result 射线拾取结果
     * @param screen_x 鼠标X坐标
     * @param screen_y 鼠标Y坐标
     * @param cameraPosition 相机位置（用于深度锁定）
     * @param cameraForward 相机前方向（用于计算 clickDepth）
     * @param coordTransform MPM坐标变换（归一化→世界转换）
     */
    void OnMouseDownFromResult(
        const RayCastResult& result,
        int screen_x,
        int screen_y,
        const glm::vec3& cameraPosition,
        const glm::vec3& cameraForward,
        const MPM::CoordinateTransform& coordTransform
    );

    /**
     * 鼠标移动事件 — 更新 lastMousePos
     * 不在内部计算力/速度，由 ComputeDragPushConstants 负责
     */
    void OnMouseMove(int screen_x, int screen_y);

    /**
     * 鼠标释放事件
     * 严格遵循设计文档 Section 4.1.3：
     * 禁止清零粒子速度！保留拖拽末端动量，靠MPM阻尼自然衰减
     */
    void OnMouseUp();

    /**
     * 计算拖拽 Push Constants（核心算法）
     * 严格遵循设计文档 Section 4.1.2
     *
     * @param deltaTime 帧时间步长
     * @param cameraPosition 相机位置
     * @param cameraRotation 相机旋转（quat）
     * @param cameraFov 相机FOV（度数）
     * @param windowHeight 窗口高度
     * @param coordTransform MPM坐标变换（世界→归一化转换）
     * @return DragPushConstants 结构体
     */
    DragPushConstants ComputeDragPushConstants(
        float deltaTime,
        const glm::vec3& cameraPosition,
        const glm::quat& cameraRotation,
        float cameraFov,
        uint32_t windowHeight,
        const MPM::CoordinateTransform& coordTransform
    );

    /**
     * 应用拖拽到粒子（已废弃：拖拽改由 MPMManager 每子步 SET 速度 BC 实现，见 SetDragVelocityBC）
     * 声明移除。如需恢复旧 3-pass GPU 流程，参考 git 历史 DragHandler.cpp::ApplyDrag。
     */

    /**
     * 设置当前抓取粒子的世界坐标（位置反馈用，每帧由 Renderer 从 GPU 回读后注入）
     * 对标 PhysDreamer gui_demo.py:335-340: cur_pick = 当前粒子位置，grab_v = (target - cur_pick)/frame_dt
     * 位置反馈 P 控制器：batch 被拉向鼠标目标，到位 v=0，变形被鼠标位移界住（防撕裂）。
     */
    void SetCurrentPickWorld(const glm::vec3& p) { current_pick_world_ = p; cur_pick_set_ = true; }

    /**
     * 状态查询
     */
    bool IsDragging() const { return dragState_.isDragging; }
    bool IsIdle() const { return !dragState_.isDragging; }
    uint32_t GetDraggedParticle() const { return dragState_.pickedParticle; }
    glm::ivec2 GetDragStartScreen() const { return dragStartScreen_; }
    glm::ivec2 GetCurrentScreen() const { return currentScreen_; }
    glm::vec3 GetClickWorldPos() const { return dragState_.clickWorldPos; }
    float GetClickDepth() const { return dragState_.clickDepth; }
    const Config& GetConfig() const { return config_; }

    /**
     * 更新配置
     */
    void SetDragRadius(float radius) { config_.dragRadius = radius; }
    void SetAlpha(float alpha) { config_.alpha = alpha; }

    /**
     * 设置 CFL 限幅参数（由 Renderer 在 MPM 初始化后注入）
     * maxVelocity = cfl * dx / sub_dt，使 |vel|*sub_dt <= cfl*dx
     * 防止拖拽注入速度过大导致粒子一子步射出网格 → 应力爆炸 → 永久冻结
     *
     * @param invDx 网格间距倒数 (1/dx)
     * @param subDt MPM 子步时间步长 (frame_dt / substeps)
     * @param cfl CFL 系数 (默认 0.5，单子步位移 <= 0.5 个网格单元)
     */
    void SetCFLParams(float invDx, float subDt, float cfl = 0.5f) {
        if (invDx > 0.0f && subDt > 0.0f) {
            float dx = 1.0f / invDx;
            cfl_max_velocity_ = cfl * dx / subDt;
            cfl_set_ = true;
        }
    }

private:
    std::shared_ptr<VulkanContext> context_;
    Config config_;

    // 拖拽状态
    DragState dragState_;
    glm::ivec2 dragStartScreen_;       // 拖拽起始屏幕坐标
    glm::ivec2 currentScreen_;         // 当前屏幕坐标

    // 位置反馈：当前抓取粒子世界坐标（GPU 回读，每帧更新）
    glm::vec3 current_pick_world_ = glm::vec3(0.0f);
    bool cur_pick_set_ = false;

    // ── 状态标志 ──
    bool initialized_ = false;

    // ── CFL 限幅（由 Renderer 注入 MPM 网格参数）──
    bool  cfl_set_ = false;
    float cfl_max_velocity_ = 0.0f;  // |vel| 上限，使单子步位移 <= 0.5*dx
};

} // namespace Interaction

#endif // DRAG_HANDLER_H
