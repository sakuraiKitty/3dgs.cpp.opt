#ifndef PARTICLE_GENERATOR_H
#define PARTICLE_GENERATOR_H

#include "MPMStructs.h"
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <memory>

// 前向声明
class GSScene;

namespace MPM {

/**
 * 粒子生成器
 * 从3D高斯点云生成MPM物理粒子
 */
class ParticleGenerator {
public:
    /**
     * 生成配置
     */
    struct Config {
        float downsample_scale = 0.1f;   // 降采样比例 (10%)
        uint32_t voxel_resolution = 128;  // 用于体积计算的voxel网格分辨率
        bool fill_interior = false;       // 是否填充内部点
        float min_volume = 1e-6f;          // 最小粒子体积 [m^3]
        float base_mass = 0.1f;           // 基础粒子质量 [kg]
    };

    ParticleGenerator() = default;
    ~ParticleGenerator() = default;

    /**
     * 从GSScene生成物理粒子
     *
     * @param scene 高斯场景
     * @param sim_mask 可变形区域掩码 (true=参与物理模拟)
     * @param config 生成配置
     * @return 生成的粒子数据
     */
    std::vector<ParticleData> GenerateFromScene(
        const std::shared_ptr<GSScene>& scene,
        const std::vector<bool>& sim_mask,
        const Config& config = Config()
    );

    /**
     * 从点云位置生成粒子
     *
     * @param positions 输入点云位置
     * @param config 生成配置
     * @return 生成的粒子数据
     */
    std::vector<ParticleData> GenerateFromPositions(
        const std::vector<glm::vec3>& positions,
        const Config& config = Config()
    );

    /**
     * KMeans降采样
     *
     * @param positions 输入点云
     * @param target_count 目标粒子数
     * @return 降采样后的点云
     */
    static std::vector<glm::vec3> KMeansDownsample(
        const std::vector<glm::vec3>& positions,
        size_t target_count
    );

    /**
     * 计算粒子体积
     * 基于Voxel占用率计算每个粒子的体积
     *
     * @param positions 粒子位置
     * @param resolution Voxel网格分辨率
     * @return 每个粒子的体积
     */
    static std::vector<float> ComputeParticleVolumes(
        const std::vector<glm::vec3>& positions,
        uint32_t resolution = 128
    );

    /**
     * 创建默认粒子数据
     *
     * @param positions 粒子位置
     * @param volumes 粒子体积
     * @param config 配置
     * @return 粒子数据
     */
    static std::vector<ParticleData> CreateParticleData(
        const std::vector<glm::vec3>& positions,
        const std::vector<float>& volumes,
        const Config& config
    );

private:
    /**
     * 计算点云的边界框
     */
    static struct BoundingBox {
        glm::vec3 min;
        glm::vec3 max;

        glm::vec3 GetCenter() const { return (min + max) * 0.5f; }
        glm::vec3 GetSize() const { return max - min; }
    };

    static BoundingBox ComputeBoundingBox(const std::vector<glm::vec3>& positions);

    /**
     * 归一化位置到单位立方体
     */
    static std::vector<glm::vec3> NormalizePositions(
        const std::vector<glm::vec3>& positions,
        const BoundingBox& bbox
    );
};

} // namespace MPM

#endif // PARTICLE_GENERATOR_H
