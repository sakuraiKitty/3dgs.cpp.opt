#ifndef MPM_STRUCTS_H
#define MPM_STRUCTS_H

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace MPM {

/**
 * MPM粒子数据结构
 * 对齐到16字节以优化GPU内存访问
 *
 * 物理含义：
 * - position: 粒子在3D空间中的位置
 * - velocity: 粒子速度
 * - mass: 粒子质量 (density * volume)
 * - volume: 粒子体积
 * - freeze_flag: 边界条件标志 (0=可动, 1=冻结)
 * - deformation_gradient: 弹性变形梯度 F
 * - stress: 应力张量
 * - material参数: 杨氏模量、泊松比等
 */
struct alignas(16) ParticleData {
    glm::vec3 position;              // 0-12   - 位置 [m]
    float mass;                      // 12-16  - 质量 [kg]
    glm::vec3 velocity;               // 16-28  - 速度 [m/s]
    uint32_t freeze_flag;            // 28-32  - 冻结标志 (0=可动, 1=冻结)
    glm::mat3 deformation_gradient;  // 32-80  - 变形梯度 F (3x3矩阵)
    float volume;                    // 80-84  - 体积 [m^3]
    uint32_t material_id;            // 84-88  - 材料ID
    float youngs_modulus;            // 88-92  - 杨氏模量 E [Pa]
    float poisson_ratio;             // 92-96  - 泊松比 nu
    float density;                   // 96-100 - 密度 [kg/m^3]
    float padding[11];               // 100-148- 对齐到16字节边界

    // 总计128字节 (16字节对齐)
};

/**
 * MPM网格节点数据结构
 * 用于粒子到网格(P2G)和网格到粒子(G2P)的传递
 *
 * 物理含义：
 * - velocity: 网格节点速度
 * - mass: 累积到节点的质量
 * - force: 节点力（可选，用于外力）
 * - active_count: 影响该节点的粒子数
 */
struct alignas(16) GridNode {
    glm::vec3 velocity;      // 0-12   - 速度 [m/s]
    float mass;              // 12-16  - 累积质量 [kg]
    glm::vec3 force;         // 16-28  - 累积力 [N]
    uint32_t active_count;   // 28-32  - 活跃粒子计数

    // 总计32字节
};

/**
 * 材料配置
 * 定义材料的物理属性
 */
struct MaterialConfig {
    float E = 1e6f;              // 杨氏模量 [Pa] - 默认1 MPa (jelly)
    float nu = 0.3f;            // 泊松比 - 典型值0.3
    float density = 2000.0f;    // 密度 [kg/m^3] - 水的密度
    float yield_stress = 0.0f;   // 屈服应力 [Pa] - 用于塑性材料
    uint32_t material_type = 0;   // 材料类型枚举
};

/**
 * 材料类型枚举
 */
enum MaterialType : uint32_t {
    JELLY = 0,        // 果冻 - 弹性材料 (FCR模型)
    METAL = 1,        // 金属 - 塑性材料 (von Mises)
    SAND = 2,         // 沙子 - 颗粒材料 (Drucker-Prager)
    FOAM = 3,         // 泡沫 - 粘弹塑性
    SNOW = 4,         // 雪 - 粘塑性
    PLASTICINE = 5,   // 橡皮泥 - 塑性带损伤
    NEO_HOOKEAN = 6   // 新胡克材料 - 超弹性
};

/**
 * 可变形区域定义
 * 区分可变形区域(前景)和静态区域(背景)
 */
struct DeformableRegion {
    // 可变形高斯的索引和位置
    std::vector<uint32_t> deformable_indices;        // 可变形高斯的全局索引
    std::vector<glm::vec3> deformable_original_pos; // 原始位置(备份)

    // 静态高斯的索引和位置
    std::vector<uint32_t> static_indices;            // 静态高斯的全局索引
    std::vector<glm::vec3> static_original_pos;       // 原始位置(备份)

    // 从PLY文件加载区域定义
    bool LoadFromPLY(
        const std::string& clean_object_points_ply,
        const std::string& moving_part_points_ply,
        const std::vector<glm::vec3>& all_gaussians
    );

    // 自动分割(基于距离阈值)
    void AutoSegmentFromCenter(
        const std::vector<glm::vec3>& all_gaussians,
        float threshold = 0.1f
    );

    // 生成sim_mask向量(true=可变形)
    std::vector<bool> GenerateSimMask(size_t total_gaussians) const;

    // 统计
    size_t GetDeformableCount() const { return deformable_indices.size(); }
    size_t GetStaticCount() const { return static_indices.size(); }
    size_t GetTotalCount() const { return deformable_indices.size() + static_indices.size(); }

    // 验证
    bool IsValid() const {
        return !deformable_indices.empty() || !static_indices.empty();
    }
};

/**
 * K近邻映射结果
 * 用于高斯-物理耦合
 */
struct KNNMapping {
    std::array<uint32_t, 8> particle_indices; // K近邻粒子索引 (K=8)
    std::array<float, 8> weights;             // 插值权重(可选)
};

} // namespace MPM

#endif // MPM_STRUCTS_H
