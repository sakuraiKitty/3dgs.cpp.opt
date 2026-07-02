#ifndef RAY_CASTER_H
#define RAY_CASTER_H

#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include "../vulkan/pipelines/ComputePipeline.h"
#include "../vulkan/Shader.h"
#include "../vulkan/DescriptorSet.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Interaction {

/**
 * 射线投射结果
 */
struct RayCastResult {
    uint32_t particle_index = UINT32_MAX;  // 拾取的粒子索引
    float distance_sq = FLT_MAX;            // 距离平方
    glm::vec3 hit_point;                   // 击中点位置
    bool success = false;                  // 是否成功拾取
};

/**
 * 射线投射器
 * 用于从屏幕坐标拾取3D粒子
 *
 * 功能：
 * - 将屏幕坐标转换为世界空间射线
 * - GPU并行查找最近粒子
 * - 返回拾取结果
 */
class RayCaster {
public:
    /**
     * 配置
     */
    struct Config {
        float max_distance = 0.5f;     // 最大拾取距离 [m]
        uint32_t max_particles = 100000; // 最大粒子数
    };

    explicit RayCaster(std::shared_ptr<VulkanContext> context, const Config& config = Config());

    ~RayCaster();

    /**
     * 初始化GPU资源
     */
    void Initialize();

    /**
     * 从屏幕坐标拾取粒子
     *
     * @param cmd Vulkan命令缓冲区
     * @param screen_x 屏幕X坐标 [像素]
     * @param screen_y 屏幕Y坐标 [像素]
     * @param window_width 窗口宽度
     * @param window_height 窗口高度
     * @param view_proj_matrix 视图投影矩阵
     * @param particle_buffer 粒子位置缓冲区
     * @param num_particles 粒子数量
     * @return 拾取结果
     */
    RayCastResult CastFromScreen(
        VkCommandBuffer cmd,
        int screen_x,
        int screen_y,
        uint32_t window_width,
        uint32_t window_height,
        const glm::mat4& view_proj_matrix,
        const std::shared_ptr<Buffer>& particle_buffer,
        uint32_t num_particles
    );

    /**
     * 从射线参数拾取粒子（GPU版本）
     */
    RayCastResult CastFromRayGPU(
        VkCommandBuffer cmd,
        const glm::vec3& ray_origin,
        const glm::vec3& ray_direction,
        const std::shared_ptr<Buffer>& particle_buffer,
        uint32_t num_particles
    );

    /**
     * CPU版本的射线拾取（用于测试/调试）
     */
    RayCastResult CastFromRayCPU(
        const glm::vec3& ray_origin,
        const glm::vec3& ray_direction,
        const std::vector<glm::vec3>& particle_positions
    );

    /**
     * 设置最大拾取距离（射线-粒子垂直距离阈值，超过判未命中）
     * 对标 PhysDreamer gui_demo.py:447 `if dist < grab_hit_thres` else grab miss。
     * 默认 0.5 太松（sim 区 ~0.56 跨度，点背景也命中）→ 改为 aabb*0.02（=grab_radius）。
     */
    void SetMaxDistance(float d) { config_.max_distance = d; }

/**
     * 屏幕坐标转NDC
     */
    glm::vec2 ScreenToNDC(int screen_x, int screen_y, uint32_t width, uint32_t height) const;

    /**
     * NDC转世界射线
     */
    void NDCToWorldRay(
        const glm::vec2& ndc,
        const glm::mat4& inverse_view_proj,
        glm::vec3& out_origin,
        glm::vec3& out_direction
    ) const;

private:
    /**
     * 创建Compute Pipeline
     */
    void CreatePipeline();

private:
    std::shared_ptr<VulkanContext> context_;
    Config config_;

    // Compute pipeline
    std::shared_ptr<ComputePipeline> ray_cast_pipeline_;
    std::shared_ptr<DescriptorSet> descriptor_set_;           // Layout descriptor set（未使用）
    std::shared_ptr<DescriptorSet> runtime_descriptor_set_;    // Runtime descriptor set（实际使用）

    // 距离缓冲区（GPU -> CPU）
    std::shared_ptr<Buffer> distance_buffer_;

    bool initialized_ = false;
    bool runtime_descriptor_set_created_ = false;
};

} // namespace Interaction

#endif // RAY_CASTER_H
