#ifndef DRAG_HANDLER_H
#define DRAG_HANDLER_H

#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include "../vulkan/pipelines/ComputePipeline.h"
#include "../vulkan/Shader.h"
#include "../vulkan/DescriptorSet.h"
#include "RayCaster.h"
#include <glm/glm.hpp>
#include <memory>

namespace Interaction {

/**
 * 拖拽状态
 */
enum class DragState {
    Idle = 0,           // 空闲
    ParticlePicked = 1, // 已拾取粒子
    Dragging = 2        // 正在拖拽
};

/**
 * 拖拽处理器
 * 管理鼠标拖拽交互，计算施加的力
 *
 * 功能：
 * - 管理拖拽状态机
 * - 计算拖拽力（基于拖拽距离）
 * - 将力应用到粒子
 */
class DragHandler {
public:
    /**
     * 配置
     */
    struct Config {
        float stiffness = 50.0f;       // 刚度系数 [N/m]
        float max_force = 100.0f;      // 最大力 [N]
        float drag_plane_depth = 2.0f; // 拖拽平面深度 [m]
    };

    explicit DragHandler(std::shared_ptr<VulkanContext> context, const Config& config = Config());

    ~DragHandler();

    /**
     * 初始化GPU资源
     */
    void Initialize();

    /**
     * 鼠标按下事件
     *
     * @param screen_x 鼠标X坐标
     * @param screen_y 鼠标Y坐标
     * @param ray_caster 射线投射器
     * @param window_width 窗口宽度
     * @param window_height 窗口高度
     * @param view_proj 视图投影矩阵
     * @param particle_buffer 粒子缓冲区
     * @param num_particles 粒子数量
     * @param cmd Vulkan命令缓冲区
     */
    void OnMouseDown(
        int screen_x,
        int screen_y,
        RayCaster& ray_caster,
        uint32_t window_width,
        uint32_t window_height,
        const glm::mat4& view_proj,
        const std::shared_ptr<Buffer>& particle_buffer,
        uint32_t num_particles,
        VkCommandBuffer cmd
    );

    /**
     * 鼠标移动事件
     *
     * @param screen_x 当前鼠标X坐标
     * @param screen_y 当前鼠标Y坐标
     */
    void OnMouseMove(int screen_x, int screen_y);

    /**
     * 鼠标释放事件
     */
    void OnMouseUp();

    /**
     * 应用拖拽力到粒子（每帧调用）
     *
     * @param cmd Vulkan命令缓冲区
     * @param particle_buffer 粒子缓冲区
     * @param dt 时间步长
     */
    void ApplyForce(
        VkCommandBuffer cmd,
        const std::shared_ptr<Buffer>& particle_buffer,
        float dt
    );

    /**
     * 状态查询
     */
    bool IsDragging() const { return state_ == DragState::Dragging; }
    bool IsIdle() const { return state_ == DragState::Idle; }
    uint32_t GetDraggedParticle() const { return dragged_particle_; }
    glm::vec3 GetCurrentForce() const { return current_force_; }
    const DragState& GetState() const { return state_; }

    /**
     * 获取拖拽信息（用于可视化）
     */
    struct DragInfo {
        glm::vec3 particle_position;  // 粒子位置
        glm::vec3 force_vector;        // 力向量
        bool active;                   // 是否激活
    };
    DragInfo GetDragInfo() const;

    /**
     * 更新配置
     */
    void SetStiffness(float stiffness) { config_.stiffness = stiffness; }
    void SetMaxForce(float max_force) { config_.max_force = max_force; }
    const Config& GetConfig() const { return config_; }

private:
    /**
     * 计算拖拽力
     */
    glm::vec3 CalculateDragForce(
        int current_screen_x,
        int current_screen_y,
        uint32_t window_width,
        uint32_t window_height,
        const glm::mat4& inverse_view_proj
    );

    /**
     * 屏幕位移转世界位移
     */
    glm::vec3 ScreenDeltaToWorldDelta(
        const glm::vec2& screen_delta,
        const glm::mat4& inverse_view_proj,
        uint32_t window_width,
        uint32_t window_height
    );

private:
    std::shared_ptr<VulkanContext> context_;
    Config config_;

    // 状态
    DragState state_ = DragState::Idle;
    uint32_t dragged_particle_ = UINT32_MAX;
    glm::vec3 drag_start_pos_;           // 拖拽起始粒子位置
    glm::ivec2 drag_start_screen_;       // 拖拽起始屏幕坐标
    glm::ivec2 current_screen_;          // 当前屏幕坐标
    glm::vec3 current_force_;            // 当前施加的力

    // Compute pipeline（应用力）
    std::shared_ptr<ComputePipeline> apply_force_pipeline_;
    std::shared_ptr<DescriptorSet> descriptor_set_;

    bool initialized_ = false;
};

} // namespace Interaction

#endif // DRAG_HANDLER_H
