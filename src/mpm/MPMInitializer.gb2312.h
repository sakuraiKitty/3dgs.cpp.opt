#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#ifndef MPM_INITIALIZER_H
#define MPM_INITIALIZER_H

#include "MPMStructs.h"
#include "../SceneLoader.h"
#include <vector>
#include <memory>
#include <random>

namespace MPM {

/**
 * MPM初始化器
 *
 * 对应 physDreamer: demo.py setup_simulation() 函数（第203-419行）
 *
 * 完整的MPM物理模拟初始化流程：
 * 1. 提取仿真点云（从sim_mask过滤前景高斯）
 * 2. 坐标变换（scale/shift）
 * 3. 加载内部填充点（optional）
 * 4. KMeans降采样
 * 5. 计算粒子体积
 * 6. 计算边界条件冻结掩码
 * 7. 建立Top-K映射
 * 8. 组装ParticleData数组
 */
class MPMInitializer {
public:
    /**
     * 初始化配置
     */
    struct Config {
        int grid_size;                    // MPM网格分辨率（默认64，对应carnation）
        int volume_resolution;           // 体积计算体素分辨率
        float downsample_scale;         // 降采样比例（10%，对应carnation）
        bool use_internal_fill;        // 是否使用内部填充点
        float boundary_threshold_scale; // 边界条件阈值系数（0.5/grid_size）
        MaterialConfig material;               // 均匀材料参数

        // carnation场景默认材料参数（从configs/carnation.py）
        Config() {
            grid_size = 64;
            volume_resolution = 128;
            downsample_scale = 0.1f;
            use_internal_fill = true;
            boundary_threshold_scale = 0.5f;
            material.E = 2140628.25f;      // Pa (Young's modulus)
            material.nu = 0.3f;            // Poisson's ratio
            material.density = 2000.0f;    // kg/m
            material.material_type = JELLY; // 果冻材料
        }
    };

    /**
     * 初始化结果
     */
    struct InitializationResult {
        std::vector<ParticleData> particles;              // 所有MPM粒子（驱动粒子）
        std::vector<uint32_t> render_particle_indices;    // 渲染粒子在原始高斯中的索引
        CoordinateTransform coord_transform;             // 坐标变换
        std::vector<TopKMapping> top_k_mappings;          // Top-K映射（render -> drive）
        std::vector<bool> freeze_mask;                   // 粒子冻结掩码（true=冻结）
        AABB simulation_aabb;                             // 仿真区域包围盒
        size_t num_drive_particles;                       // 驱动粒子数
        size_t num_render_particles;                      // 渲染粒子数

        // 统计信息
        struct Statistics {
            size_t original_point_count;    // 原始高斯总数
            size_t sim_point_count;        // 仿真点数（前景）
            size_t filled_point_count;    // 内部填充点数
            size_t downsampled_count;      // 降采样后粒子数
            size_t frozen_count;            // 冻结粒子数
            size_t active_count;           // 活跃粒子数

            Statistics() {
                original_point_count = 0;
                sim_point_count = 0;
                filled_point_count = 0;
                downsampled_count = 0;
                frozen_count = 0;
                active_count = 0;
            }
        } stats;

        InitializationResult() {
            num_drive_particles = 0;
            num_render_particles = 0;
        }
    };

    /**
     * 主初始化函数
     *
     * @param scene_desc 场景描述符（包含所有PLY路径）
     * @param config 初始化配置
     * @param sim_mask 仿真掩码（true=前景/可变形，false=背景/静态）
     * @return 初始化结果
     */
    static InitializationResult Initialize(
        const SceneLoader::SceneDescriptor& scene_desc,
        const Config& config,
        const std::vector<bool>& sim_mask
    );

private:
    // ========== 子步骤1：提取仿真点云 ==========
    /**
     * 从完整高斯点云中提取前景仿真点
     * 对应 physDreamer: demo.py 第213行
     *
     * @param all_gaussians 所有高斯位置
     * @param sim_mask 仿真掩码
     * @return 前景仿真点云
     */
    static std::vector<glm::vec3> ExtractSimulationPoints(
        const std::vector<glm::vec3>& all_gaussians,
        const std::vector<bool>& sim_mask
    );

    // ========== 子步骤2：计算坐标变换 ==========
    /**
     * 计算坐标变换参数
     * 对应 physDreamer: demo.py 第216-220行
     *
     * @param sim_points 仿真点云
     * @return 坐标变换
     */
    static CoordinateTransform ComputeCoordinateTransform(
        const std::vector<glm::vec3>& sim_points
    );

    // ========== 子步骤3：加载内部填充点 ==========
    /**
     * 加载内部填充点（可选）
     * 对应 physDreamer: demo.py 第226-251行
     *
     * @param internal_filled_ply 内部填充点PLY路径
     * @param transform 坐标变换
     * @return 内部填充点云（归一化坐标）
     */
    static std::vector<glm::vec3> LoadInternalFillPoints(
        const std::string& internal_filled_ply,
        const CoordinateTransform& transform
    );

    // ========== 子步骤4：KMeans降采样 ==========
    /**
     * KMeans降采样（CPU版本）
     * 对应 physDreamer: local_utils.py downsample_with_kmeans_gpu_with_chunk()
     *
     * 使用分块策略（chunk_size=50000）与Python一致
     *
     * @param points 输入点云
     * @param target_count 目标粒子数
     * @return 降采样后的点云（聚类中心）
     */
    static std::vector<glm::vec3> DownsampleWithKMeans(
        const std::vector<glm::vec3>& points,
        size_t target_count
    );

    /**
     * 对单个块执行KMeans聚类
     */
    static std::vector<glm::vec3> RunKMeansOnChunk(
        const std::vector<glm::vec3>& chunk,
        size_t num_clusters,
        int max_iter = 100,
        float tolerance = 1e-4f
    );

    // ========== 子步骤5：计算粒子体积 ==========
    /**
     * 计算粒子体积
     * 对应 physDreamer: gaussian_sim_utils.py get_volume()
     *
     * 使用体素化方法：在[-1,1]空间建立resolution网格，
     * 每个体素的体积由该体素内的粒子数均分
     *
     * @param points 归一化点云（应在[-1,1]范围内）
     * @param resolution 体素分辨率
     * @return 每个粒子的体积
     */
    static std::vector<float> ComputeParticleVolumes(
        const std::vector<glm::vec3>& points,
        int resolution = 128
    );

    // ========== 子步骤6：计算边界条件冻结掩码 ==========
    /**
     * 计算粒子冻结掩码
     * 对应 physDreamer: demo.py 第366行 + local_utils.py find_far_points()
     *
     * @param drive_particles 驱动粒子（归一化坐标）
     * @param moving_part_points 运动区域参考点（原始坐标）
     * @param transform 坐标变换
     * @param grid_size 网格分辨率
     * @param threshold_scale 阈值系数（默认0.5）
     * @return freeze_mask（true=冻结，false=可动）
     */
    static std::vector<bool> ComputeFreezeMask(
        const std::vector<glm::vec3>& drive_particles,
        const std::vector<glm::vec3>& moving_part_points,
        const CoordinateTransform& transform,
        int grid_size,
        float threshold_scale = 0.5f
    );

    // ========== 子步骤7：建立Top-K映射 ==========
    /**
     * 建立从渲染粒子到驱动粒子的Top-K映射
     * 对应 physDreamer: demo.py 第288-299行
     *
     * 每个渲染粒子找到最近的K=8个驱动粒子
     *
     * @param render_points 渲染粒子（原始高斯，归一化坐标）
     * @param drive_particles 驱动粒子（降采样后，归一化坐标）
     * @return Top-K映射数组（大小=render_points.size()）
     */
    static std::vector<TopKMapping> BuildTopKMapping(
        const std::vector<glm::vec3>& render_points,
        const std::vector<glm::vec3>& drive_particles
    );

    // ========== 子步骤8：组装ParticleData数组 ==========
    /**
     * 组装完整的ParticleData数组
     *
     * @param positions 粒子位置（归一化坐标）
     * @param volumes 粒子体积
     * @param freeze_mask 冻结掩码
     * @param material 材料参数
     * @return ParticleData数组
     */
    static std::vector<ParticleData> AssembleParticles(
        const std::vector<glm::vec3>& positions,
        const std::vector<float>& volumes,
        const std::vector<bool>& freeze_mask,
        const MaterialConfig& material
    );

    // ========== 辅助函数 ==========
    /**
     * 计算Lamé参数（从E和nu）
     * 对应 physDreamer: mpm_data_structure.py compute_mu_lam_from_E_nu_clean()
     */
    static void ComputeLameParameters(float E, float nu, float& mu, float& lam);

    /**
     * 将点云从[0,1]映射到[-1,1]（用于体积计算）
     */
    static std::vector<glm::vec3> MapToMinusOneToOne(
        const std::vector<glm::vec3>& points_zero_to_one
    );
};

} // namespace MPM

#endif // MPM_INITIALIZER_H
