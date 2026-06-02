#ifndef MPM_STRUCTS_H
#define MPM_STRUCTS_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <string>
#include <random>

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

    // 新增字段（用于高级物理特性）
    glm::mat3 apic_matrix;           // 100-148 - APIC动量矩阵C
    uint32_t is_filled_point;        // 148-152 - 是否为内部填充点 (0=原始高斯, 1=填充点)
    float padding[1];                // 152-156 - 对齐填充

    // 总计160字节 (16字节对齐)
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
    float E;              // 杨氏模量 [Pa] - 默认1 MPa (jelly)
    float nu;            // 泊松比 - 典型值0.3
    float density;    // 密度 [kg/m^3] - 水的密度
    float yield_stress;   // 屈服应力 [Pa] - 用于塑性材料
    uint32_t material_type;   // 材料类型枚举

    MaterialConfig() {
        E = 1e6f;
        nu = 0.3f;
        density = 2000.0f;
        yield_stress = 0.0f;
        material_type = 0;
    }
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

/**
 * 坐标变换结构
 * 对应 physDreamer: demo.py setup_simulation 中的 scale/shift 变换
 *
 * 将原始世界坐标归一化到MPM网格空间 [0, 1]
 * 公式: normalized = (original + shift) / scale
 * 逆变换: original = normalized * scale - shift
 *
 * 参数计算（与Python一致）:
 *   scale = (pos_max - pos_min) * 1.8
 *   shift = -pos_min + (pos_max - pos_min) * 0.25
 *
 * 结果: 物体被映射到约 [0.139, 0.694] 范围
 */
struct CoordinateTransform {
    glm::vec3 pos_min;           // 原始点云最小值
    glm::vec3 pos_max;           // 原始点云最大值
    glm::vec3 range;             // pos_max - pos_min
    float scale;                 // 总体缩放因子
    glm::vec3 shift;             // 平移向量

    CoordinateTransform() : scale(1.0f), shift(0.0f) {}

    /**
     * 应用变换：将原始坐标转换为归一化坐标 [0, 1]
     */
    glm::vec3 ToNormalized(const glm::vec3& original) const {
        return (original + shift) / scale;
    }

    /**
     * 逆变换：将归一化坐标转换回原始坐标
     */
    glm::vec3 ToOriginal(const glm::vec3& normalized) const {
        return normalized * scale - shift;
    }

    /**
     * 从点云计算坐标变换参数
     * 对应 physDreamer: demo.py 第216-220行
     */
    static CoordinateTransform ComputeFromPoints(
        const std::vector<glm::vec3>& points
    ) {
        if (points.empty()) {
            return CoordinateTransform();
        }

        CoordinateTransform transform;

        // 计算 min/max
        transform.pos_min = glm::vec3(std::numeric_limits<float>::max());
        transform.pos_max = glm::vec3(std::numeric_limits<float>::lowest());

        for (const auto& pt : points) {
            transform.pos_min = glm::min(transform.pos_min, pt);
            transform.pos_max = glm::max(transform.pos_max, pt);
        }

        // 计算 range, scale, shift（与Python完全一致）
        transform.range = transform.pos_max - transform.pos_min;
        transform.scale = glm::length(transform.range) * 1.8f;
        transform.shift = -transform.pos_min + transform.range * 0.25f;

        return transform;
    }
};

/**
 * Top-K映射结构
 * 对应 physDreamer: local_utils.py interpolate_points_w_R()
 *
 * 用于从驱动粒子（降采样后的MPM粒子）到渲染粒子（原始高斯）的位移插值
 *
 * 每个渲染粒子找到最近的K个驱动粒子，通过加权平均获取位移
 */
struct TopKMapping {
    static constexpr int K = 8;

    std::array<uint32_t, K> particle_indices;  // K近邻驱动粒子索引
    std::array<float, K> weights;              // 插值权重（距离平方反比）
    float weight_sum;                          // 归一化因子

    TopKMapping() : weight_sum(0.0f) {
        particle_indices.fill(UINT32_MAX);
        weights.fill(0.0f);
    }

    /**
     * 从驱动粒子位移插值渲染粒子位移
     * 对应 physDreamer: interpolate_points_w_R() 第1074-1097行
     *
     * @param drive_displacements 所有驱动粒子的位移向量
     * @return 插值后的渲染粒子位移
     */
    glm::vec3 InterpolateDisplacement(
        const std::vector<glm::vec3>& drive_displacements
    ) const {
        glm::vec3 result(0.0f);

        for (int i = 0; i < K; ++i) {
            if (particle_indices[i] != UINT32_MAX &&
                particle_indices[i] < drive_displacements.size()) {
                float w = weights[i] / weight_sum;
                result += w * drive_displacements[particle_indices[i]];
            }
        }

        return result;
    }

    /**
     * 从驱动粒子位置插值渲染粒子位置（包含原始位移）
     */
    glm::vec3 InterpolatePosition(
        const std::vector<glm::vec3>& drive_original_positions,
        const std::vector<glm::vec3>& drive_displacements
    ) const {
        glm::vec3 result(0.0f);

        for (int i = 0; i < K; ++i) {
            if (particle_indices[i] != UINT32_MAX &&
                particle_indices[i] < drive_original_positions.size()) {
                float w = weights[i] / weight_sum;
                glm::vec3 new_pos = drive_original_positions[particle_indices[i]] +
                                    drive_displacements[particle_indices[i]];
                result += w * new_pos;
            }
        }

        return result;
    }
};

/**
 * AABB（轴对齐包围盒）结构
 * 用于定义MPM仿真空间范围
 */
struct AABB {
    glm::vec3 min;
    glm::vec3 max;

    AABB() : min(0.0f), max(1.0f) {}
    AABB(const glm::vec3& min_, const glm::vec3& max_) : min(min_), max(max_) {}

    glm::vec3 Center() const { return (min + max) * 0.5f; }
    glm::vec3 Size() const { return max - min; }

    /**
     * 从点云计算AABB
     * 对应 physDreamer: demo.py 第253-313行
     *
     * @param points 归一化后的点云坐标
     * @param expansion_scale 扩展系数（默认1.2倍）
     */
    static AABB ComputeFromPoints(
        const std::vector<glm::vec3>& points,
        float expansion_scale = 1.2f
    ) {
        if (points.empty()) {
            return AABB();
        }

        AABB aabb;
        aabb.min = glm::vec3(std::numeric_limits<float>::max());
        aabb.max = glm::vec3(std::numeric_limits<float>::lowest());

        for (const auto& pt : points) {
            aabb.min = glm::min(aabb.min, pt);
            aabb.max = glm::max(aabb.max, pt);
        }

        // 扩展包围盒（与Python一致）
        glm::vec3 center = aabb.Center();
        glm::vec3 size = aabb.Size();

        aabb.min = center + (aabb.min - center) * expansion_scale;
        aabb.max = center + (aabb.max - center) * expansion_scale;

        return aabb;
    }

    /**
     * 将世界坐标归一化到 [0, 1]
     */
    glm::vec3 Normalize(const glm::vec3& world_pos) const {
        glm::vec3 size = max - min;
        return (world_pos - min) / size;
    }
};

} // namespace MPM

#endif // MPM_STRUCTS_H
