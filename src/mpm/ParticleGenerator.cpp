#include "ParticleGenerator.h"
#include "../GSScene.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <random>
#include <cmath>
#include <unordered_map>
#include <numeric>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace MPM {

std::vector<ParticleData> ParticleGenerator::GenerateFromScene(
    const std::shared_ptr<GSScene>& scene,
    const std::vector<bool>& sim_mask,
    const Config& config
) {
    spdlog::info("[ParticleGenerator] Generating particles from scene...");

    // 1. 获取高斯点云位置
    // 注意：需要从GSScene获取原始点云位置
    // 这里假设有一个方法可以获取，具体实现需要根据GSScene的接口调整
    std::vector<glm::vec3> all_positions; // TODO: Implement scene->GetAllPositions()
    std::vector<glm::vec3> sim_positions;

    // 2. 根据sim_mask筛选可变形区域的点
    for (size_t i = 0; i < all_positions.size(); i++) {
        if (i < sim_mask.size() && sim_mask[i]) {
            sim_positions.push_back(all_positions[i]);
        }
    }

    spdlog::info("[ParticleGenerator] Total gaussians: {}, Simulated: {}",
                all_positions.size(), sim_positions.size());

    // 3. 如果没有可变形点，使用全部点
    if (sim_positions.empty()) {
        spdlog::warn("[ParticleGenerator] No simulated particles, using all positions");
        sim_positions = all_positions;
    }

    // 4. 降采样
    size_t target_count = static_cast<size_t>(sim_positions.size() * config.downsample_scale);
    target_count = std::max(size_t(100), target_count); // 至少100个粒子

    spdlog::info("[ParticleGenerator] Downsampling from {} to {} particles",
                sim_positions.size(), target_count);

    std::vector<glm::vec3> downsampled = KMeansDownsample(sim_positions, target_count);

    // 5. 计算体积
    std::vector<float> volumes = ComputeParticleVolumes(
        downsampled,
        config.voxel_resolution
    );

    // 6. 创建粒子数据
    std::vector<ParticleData> particles = CreateParticleData(
        downsampled,
        volumes,
        config
    );

    spdlog::info("[ParticleGenerator] Generated {} particles successfully", particles.size());

    return particles;
}

std::vector<ParticleData> ParticleGenerator::GenerateFromPositions(
    const std::vector<glm::vec3>& positions,
    const Config& config
) {
    spdlog::info("[ParticleGenerator] Generating particles from {} positions",
                positions.size());

    // 降采样
    size_t target_count = std::max(size_t(100),
        static_cast<size_t>(positions.size() * config.downsample_scale));

    std::vector<glm::vec3> downsampled = KMeansDownsample(positions, target_count);

    // 计算体积
    std::vector<float> volumes = ComputeParticleVolumes(downsampled, config.voxel_resolution);

    // 创建粒子数据
    return CreateParticleData(downsampled, volumes, config);
}

std::vector<glm::vec3> ParticleGenerator::KMeansDownsample(
    const std::vector<glm::vec3>& positions,
    size_t target_count
) {
    if (positions.size() <= target_count) {
        return positions; // 不需要降采样
    }

    spdlog::debug("[ParticleGenerator] Running KMeans downsampling: {} -> {}",
                  positions.size(), target_count);

    std::vector<glm::vec3> centers(target_count);
    std::vector<uint32_t> assignments(positions.size());

    // 1. 初始化中心：随机选择target_count个点
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, positions.size() - 1);

    for (size_t i = 0; i < target_count; i++) {
        centers[i] = positions[dist(gen)];
    }

    // 2. KMeans迭代
    const int max_iterations = 20;
    const double convergence_threshold = 1e-4;

    for (int iter = 0; iter < max_iterations; iter++) {
        // 2.1 分配点到最近的中心
        bool changed = false;

        #pragma omp parallel for
        for (size_t i = 0; i < positions.size(); i++) {
            float min_dist = std::numeric_limits<float>::max();
            uint32_t best_center = 0;

            for (size_t j = 0; j < target_count; j++) {
                glm::vec3 diff = positions[i] - centers[j];
                float dist = glm::dot(diff, diff);
                if (dist < min_dist) {
                    min_dist = dist;
                    best_center = static_cast<uint32_t>(j);
                }
            }

            if (assignments[i] != best_center) {
                assignments[i] = best_center;
                changed = true;
            }
        }

        // 2.2 更新中心
        std::vector<glm::vec3> new_centers(target_count, glm::vec3(0.0f));
        std::vector<uint32_t> counts(target_count, 0);

        for (size_t i = 0; i < positions.size(); i++) {
            uint32_t center_idx = assignments[i];
            new_centers[center_idx] += positions[i];
            counts[center_idx]++;
        }

        // 归一化
        for (size_t j = 0; j < target_count; j++) {
            if (counts[j] > 0) {
                new_centers[j] /= static_cast<float>(counts[j]);
            }
        }

        // 2.3 检查收敛
        double max_shift = 0.0;
        for (size_t j = 0; j < target_count; j++) {
            double shift = glm::length(new_centers[j] - centers[j]);
            max_shift = std::max(max_shift, shift);
            centers[j] = new_centers[j];
        }

        if (max_shift < convergence_threshold) {
            spdlog::debug("[ParticleGenerator] KMeans converged at iteration {}", iter);
            break;
        }
    }

    // 3. 返回中心点作为降采样结果
    spdlog::debug("[ParticleGenerator] KMeans completed, {} centers", centers.size());
    return centers;
}

std::vector<float> ParticleGenerator::ComputeParticleVolumes(
    const std::vector<glm::vec3>& positions,
    uint32_t resolution
) {
    spdlog::debug("[ParticleGenerator] Computing particle volumes with resolution {}", resolution);

    // 1. 计算边界框并归一化
    BoundingBox bbox = ComputeBoundingBox(positions);
    std::vector<glm::vec3> normalized = NormalizePositions(positions, bbox);

    // 2. Voxel网格计数
    std::vector<std::vector<uint32_t>> voxel_contents(resolution * resolution * resolution);
    std::vector<uint32_t> voxel_particle_count(resolution * resolution * resolution, 0);

    // 3x3x3 stencil用于平滑
    const int stencil_radius = 1;
    const int stencil_size = 2 * stencil_radius + 1; // 3x3x3 = 27

    #pragma omp parallel for
    for (size_t i = 0; i < normalized.size(); i++) {
        // 映射到voxel网格
        glm::vec3 pos = normalized[i]; // [0, 1]范围
        glm::ivec3 voxel(
            static_cast<int>(pos.x * resolution),
            static_cast<int>(pos.y * resolution),
            static_cast<int>(pos.z * resolution)
        );

        // 边界检查
        voxel.x = std::clamp(voxel.x, 0, static_cast<int>(resolution) - 1);
        voxel.y = std::clamp(voxel.y, 0, static_cast<int>(resolution) - 1);
        voxel.z = std::clamp(voxel.z, 0, static_cast<int>(resolution) - 1);

        uint32_t linear_idx = voxel.x + voxel.y * resolution + voxel.z * resolution * resolution;

        #pragma omp critical
        {
            voxel_contents[linear_idx].push_back(static_cast<uint32_t>(i));
            voxel_particle_count[linear_idx]++;
        }
    }

    // 3. 计算每个粒子的体积
    std::vector<float> volumes(positions.size());
    glm::vec3 bbox_size = bbox.GetSize();
    float voxel_volume = (bbox_size.x * bbox_size.y * bbox_size.z) /
                         static_cast<float>(resolution * resolution * resolution);

    #pragma omp parallel for
    for (size_t i = 0; i < positions.size(); i++) {
        glm::vec3 pos = normalized[i];
        glm::ivec3 voxel(
            static_cast<int>(pos.x * resolution),
            static_cast<int>(pos.y * resolution),
            static_cast<int>(pos.z * resolution)
        );

        voxel.x = std::clamp(voxel.x, 0, static_cast<int>(resolution) - 1);
        voxel.y = std::clamp(voxel.y, 0, static_cast<int>(resolution) - 1);
        voxel.z = std::clamp(voxel.z, 0, static_cast<int>(resolution) - 1);

        uint32_t linear_idx = voxel.x + voxel.y * resolution + voxel.z * resolution * resolution;
        uint32_t count = voxel_particle_count[linear_idx];

        // 体积 = voxel体积 / 该voxel中的粒子数
        float volume = (count > 0) ? (voxel_volume / static_cast<float>(count)) : 1e-6f;
        volumes[i] = std::max(1e-6f, volume); // 最小体积限制
    }

    spdlog::debug("[ParticleGenerator] Volume computation complete, avg volume: {:.2e}",
                std::accumulate(volumes.begin(), volumes.end(), 0.0f) / volumes.size());

    return volumes;
}

std::vector<ParticleData> ParticleGenerator::CreateParticleData(
    const std::vector<glm::vec3>& positions,
    const std::vector<float>& volumes,
    const Config& config
) {
    std::vector<ParticleData> particles(positions.size());

    #pragma omp parallel for
    for (size_t i = 0; i < positions.size(); i++) {
        ParticleData& p = particles[i];

        // 位置和速度
        p.position = positions[i];
        p.velocity = glm::vec3(0.0f);

        // 质量和体积
        p.volume = volumes[i];
        p.mass = config.base_mass * p.volume; // m = density * volume

        // 变形梯度初始化为单位矩阵
        SetDeformationGradient(p, glm::mat3(1.0f));

        // 材料属性
        p.material_id = 0; // 默认jelly
        p.youngs_modulus = 1e6f; // 1 MPa
        p.poisson_ratio = 0.3f;
        p.density = 2000.0f; // 水的密度

        // 冻结标志 (默认可动)
        p.freeze_flag = 0;
    }

    spdlog::info("[ParticleGenerator] Created {} particles", particles.size());
    return particles;
}

ParticleGenerator::BoundingBox ParticleGenerator::ComputeBoundingBox(
    const std::vector<glm::vec3>& positions
) {
    if (positions.empty()) {
        return {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    }

    BoundingBox bbox;
    bbox.min = positions[0];
    bbox.max = positions[0];

    for (const auto& pos : positions) {
        bbox.min = glm::min(bbox.min, pos);
        bbox.max = glm::max(bbox.max, pos);
    }

    return bbox;
}

std::vector<glm::vec3> ParticleGenerator::NormalizePositions(
    const std::vector<glm::vec3>& positions,
    const BoundingBox& bbox
) {
    std::vector<glm::vec3> normalized(positions.size());

    glm::vec3 bbox_size = bbox.GetSize();
    glm::vec3 inv_size = 1.0f / bbox_size; // 避免除零

    #pragma omp parallel for
    for (size_t i = 0; i < positions.size(); i++) {
        // 归一化到[0, 1]范围
        normalized[i] = (positions[i] - bbox.min) * inv_size;
    }

    return normalized;
}

} // namespace MPM
