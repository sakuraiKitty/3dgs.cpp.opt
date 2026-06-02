#include "MPMInitializer.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <queue>
#include <limits>
#include <random>

namespace MPM {

// ============================================================
// 主初始化函数
// ============================================================

MPMInitializer::InitializationResult MPMInitializer::Initialize(
    const SceneLoader::SceneDescriptor& scene_desc,
    const Config& config,
    const std::vector<bool>& sim_mask
) {
    spdlog::info("[MPMInitializer] ========== Starting MPM Initialization ==========");

    InitializationResult result;

    // ========== 步骤1：加载完整点云 ==========
    spdlog::info("[MPMInitializer] Step 1: Loading point clouds...");
    SceneLoader::PLYData point_cloud = SceneLoader::LoadPLY(scene_desc.point_cloud_ply);
    SceneLoader::PLYData moving_part = SceneLoader::LoadPLY(scene_desc.moving_part_points_ply);

    if (!point_cloud.IsValid()) {
        spdlog::error("[MPMInitializer] Failed to load point cloud: {}", scene_desc.point_cloud_ply);
        return result;
    }

    result.stats.original_point_count = point_cloud.positions.size();
    spdlog::info("[MPMInitializer] Loaded {} gaussians from {}", point_cloud.positions.size(), scene_desc.point_cloud_ply);

    if (moving_part.IsValid()) {
        spdlog::info("[MPMInitializer] Loaded {} moving part points from {}", moving_part.positions.size(), scene_desc.moving_part_points_ply);
    } else {
        spdlog::warn("[MPMInitializer] No moving_part_points.ply found, boundary conditions will NOT be set!");
    }

    // ========== 步骤2：提取仿真点云（前景）==========
    spdlog::info("[MPMInitializer] Step 2: Extracting simulation points (foreground)...");
    std::vector<glm::vec3> sim_points = ExtractSimulationPoints(point_cloud.positions, sim_mask);
    result.stats.sim_point_count = sim_points.size();
    spdlog::info("[MPMInitializer] Extracted {} foreground points ({}%)",
                 sim_points.size(),
                 100.0 * sim_points.size() / point_cloud.positions.size());

    if (sim_points.empty()) {
        spdlog::error("[MPMInitializer] No simulation points found!");
        return result;
    }

    // ========== 步骤3：计算坐标变换 ==========
    spdlog::info("[MPMInitializer] Step 3: Computing coordinate transform...");
    result.coord_transform = ComputeCoordinateTransform(sim_points);
    spdlog::info("[MPMInitializer] Transform: scale={:.6f}, shift=({:.6f}, {:.6f}, {:.6f})",
                 result.coord_transform.scale,
                 result.coord_transform.shift.x,
                 result.coord_transform.shift.y,
                 result.coord_transform.shift.z);

    // ========== 步骤4：变换到归一化空间 ==========
    spdlog::info("[MPMInitializer] Step 4: Transforming to normalized space [0,1]...");
    std::vector<glm::vec3> sim_points_normalized;
    sim_points_normalized.reserve(sim_points.size());
    for (const auto& pt : sim_points) {
        sim_points_normalized.push_back(result.coord_transform.ToNormalized(pt));
    }

    // ========== 步骤5：加载内部填充点（可选）==========
    std::vector<glm::vec3> drive_particles = sim_points_normalized;

    if (config.use_internal_fill && !scene_desc.internal_filled_ply.empty()) {
        spdlog::info("[MPMInitializer] Step 5: Loading internal fill points...");
        std::vector<glm::vec3> fill_points = LoadInternalFillPoints(
            scene_desc.internal_filled_ply,
            result.coord_transform
        );

        if (!fill_points.empty()) {
            size_t old_count = drive_particles.size();
            drive_particles.insert(drive_particles.end(), fill_points.begin(), fill_points.end());
            result.stats.filled_point_count = fill_points.size();
            spdlog::info("[MPMInitializer] Added {} fill points (total: {})",
                         fill_points.size(), drive_particles.size());
        } else {
            spdlog::info("[MPMInitializer] No internal fill points found, using original points only");
        }
    } else {
        spdlog::info("[MPMInitializer] Step 5: Skipping internal fill");
    }

    // ========== 步骤6：KMeans降采样 ==========
    spdlog::info("[MPMInitializer] Step 6: KMeans downsampling...");
    size_t target_count = static_cast<size_t>(drive_particles.size() * config.downsample_scale);
    spdlog::info("[MPMInitializer] Downsampling {} -> {} particles (scale={:.1f}%)",
                 drive_particles.size(), target_count, config.downsample_scale * 100.0f);

    std::vector<glm::vec3> downsampled = DownsampleWithKMeans(drive_particles, target_count);
    result.stats.downsampled_count = downsampled.size();
    spdlog::info("[MPMInitializer] Downsampled to {} particles", downsampled.size());

    // ========== 步骤7：计算粒子体积 ==========
    spdlog::info("[MPMInitializer] Step 7: Computing particle volumes...");
    // 将[0,1]映射到[-1,1]用于体积计算
    std::vector<glm::vec3> points_minus1_to_1 = MapToMinusOneToOne(downsampled);
    std::vector<float> volumes = ComputeParticleVolumes(points_minus1_to_1, config.volume_resolution);

    // 统计体积信息
    float total_volume = std::accumulate(volumes.begin(), volumes.end(), 0.0f);
    float avg_volume = total_volume / volumes.size();
    spdlog::info("[MPMInitializer] Total volume: {:.6f}, Avg per particle: {:.9f}",
                 total_volume, avg_volume);

    // ========== 步骤8：计算边界条件冻结掩码 ==========
    if (moving_part.IsValid()) {
        spdlog::info("[MPMInitializer] Step 8: Computing freeze mask (boundary conditions)...");
        result.freeze_mask = ComputeFreezeMask(
            downsampled,
            moving_part.positions,
            result.coord_transform,
            config.grid_size,
            config.boundary_threshold_scale
        );

        result.stats.frozen_count = std::count(result.freeze_mask.begin(), result.freeze_mask.end(), true);
        result.stats.active_count = downsampled.size() - result.stats.frozen_count;

        spdlog::info("[MPMInitializer] Freeze mask: {} frozen, {} active ({:.1f}%)",
                     result.stats.frozen_count,
                     result.stats.active_count,
                     100.0 * result.stats.active_count / downsampled.size());
    } else {
        spdlog::warn("[MPMInitializer] Step 8: Skipping freeze mask (no moving_part_points)");
        result.freeze_mask.assign(downsampled.size(), false); // 全部可动
        result.stats.active_count = downsampled.size();
    }

    // ========== 步骤9：计算AABB ==========
    result.simulation_aabb = AABB::ComputeFromPoints(downsampled, 1.2f);
    spdlog::info("[MPMInitializer] Simulation AABB: min=({:.3f},{:.3f},{:.3f}), max=({:.3f},{:.3f},{:.3f})",
                 result.simulation_aabb.min.x, result.simulation_aabb.min.y, result.simulation_aabb.min.z,
                 result.simulation_aabb.max.x, result.simulation_aabb.max.y, result.simulation_aabb.max.z);

    // ========== 步骤10：组装ParticleData数组 ==========
    spdlog::info("[MPMInitializer] Step 10: Assembling particle data...");
    result.particles = AssembleParticles(downsampled, volumes, result.freeze_mask, config.material);
    result.num_drive_particles = result.particles.size();

    spdlog::info("[MPMInitializer] Assembled {} particles", result.particles.size());

    // ========== 步骤11：建立Top-K映射 ==========
    spdlog::info("[MPMInitializer] Step 11: Building Top-K mappings...");
    std::vector<glm::vec3> render_points_normalized;
    std::vector<uint32_t> render_indices;

    for (size_t i = 0; i < sim_mask.size(); ++i) {
        if (sim_mask[i]) {
            render_points_normalized.push_back(result.coord_transform.ToNormalized(point_cloud.positions[i]));
            render_indices.push_back(static_cast<uint32_t>(i));
        }
    }

    result.render_particle_indices = render_indices;
    result.num_render_particles = render_points_normalized.size();

    spdlog::info("[MPMInitializer] Building Top-K mappings for {} render particles -> {} drive particles",
                 render_points_normalized.size(), downsampled.size());

    result.top_k_mappings = BuildTopKMapping(render_points_normalized, downsampled);

    // 验证Top-K映射
    bool all_valid = true;
    for (const auto& mapping : result.top_k_mappings) {
        for (int i = 0; i < TopKMapping::K; ++i) {
            if (mapping.particle_indices[i] >= downsampled.size()) {
                all_valid = false;
                break;
            }
        }
    }
    spdlog::info("[MPMInitializer] Top-K mapping validation: {}", all_valid ? "PASS" : "FAIL");

    // ========== 完成 ==========
    spdlog::info("[MPMInitializer] ========== MPM Initialization Complete ==========");
    spdlog::info("[MPMInitializer] Summary:");
    spdlog::info("[MPMInitializer]   Original gaussians: {}", result.stats.original_point_count);
    spdlog::info("[MPMInitializer]   Foreground points: {}", result.stats.sim_point_count);
    spdlog::info("[MPMInitializer]   Filled points: {}", result.stats.filled_point_count);
    spdlog::info("[MPMInitializer]   Downsampled to: {}", result.stats.downsampled_count);
    spdlog::info("[MPMInitializer]   Frozen particles: {}", result.stats.frozen_count);
    spdlog::info("[MPMInitializer]   Active particles: {}", result.stats.active_count);
    spdlog::info("[MPMInitializer]   Render particles: {}", result.num_render_particles);

    return result;
}

// ============================================================
// 子步骤1：提取仿真点云
// ============================================================

std::vector<glm::vec3> MPMInitializer::ExtractSimulationPoints(
    const std::vector<glm::vec3>& all_gaussians,
    const std::vector<bool>& sim_mask
) {
    std::vector<glm::vec3> sim_points;

    if (all_gaussians.size() != sim_mask.size()) {
        spdlog::error("[MPMInitializer] Size mismatch: {} gaussians vs {} mask entries",
                      all_gaussians.size(), sim_mask.size());
        return sim_points;
    }

    sim_points.reserve(all_gaussians.size()); // 预分配最大可能

    for (size_t i = 0; i < all_gaussians.size(); ++i) {
        if (sim_mask[i]) {
            sim_points.push_back(all_gaussians[i]);
        }
    }

    sim_points.shrink_to_fit();
    return sim_points;
}

// ============================================================
// 子步骤2：计算坐标变换
// ============================================================

CoordinateTransform MPMInitializer::ComputeCoordinateTransform(
    const std::vector<glm::vec3>& sim_points
) {
    return CoordinateTransform::ComputeFromPoints(sim_points);
}

// ============================================================
// 子步骤3：加载内部填充点
// ============================================================

std::vector<glm::vec3> MPMInitializer::LoadInternalFillPoints(
    const std::string& internal_filled_ply,
    const CoordinateTransform& transform
) {
    std::vector<glm::vec3> fill_points;

    SceneLoader::PLYData fill_data = SceneLoader::LoadPLY(internal_filled_ply);
    if (!fill_data.IsValid()) {
        spdlog::warn("[MPMInitializer] No fill points found at {}", internal_filled_ply);
        return fill_points;
    }

    // 变换到归一化空间
    fill_points.reserve(fill_data.positions.size());
    for (const auto& pt : fill_data.positions) {
        fill_points.push_back(transform.ToNormalized(pt));
    }

    return fill_points;
}

// ============================================================
// 子步骤4：KMeans降采样
// ============================================================

std::vector<glm::vec3> MPMInitializer::DownsampleWithKMeans(
    const std::vector<glm::vec3>& points,
    size_t target_count
) {
    if (points.size() <= target_count) {
        spdlog::warn("[MPMInitializer] Input size {} <= target {}, no downsampling needed",
                     points.size(), target_count);
        return points;
    }

    const size_t chunk_size = 50000;  // 与Python一致
    const size_t num_chunks = (points.size() + chunk_size - 1) / chunk_size;
    const size_t target_per_chunk = target_count / num_chunks;

    spdlog::info("[MPMInitializer] KMeans: {} chunks, target {} per chunk",
                 num_chunks, target_per_chunk);

    std::vector<glm::vec3> centroids;
    centroids.reserve(target_count);

    // 分块处理
    for (size_t i = 0; i < num_chunks; ++i) {
        size_t start = i * chunk_size;
        size_t end = std::min(start + chunk_size, points.size());

        std::vector<glm::vec3> chunk(points.begin() + start, points.begin() + end);

        spdlog::debug("[MPMInitializer] Processing chunk {}/{} ({} points)",
                      i + 1, num_chunks, chunk.size());

        auto chunk_centroids = RunKMeansOnChunk(chunk, target_per_chunk);
        centroids.insert(centroids.end(), chunk_centroids.begin(), chunk_centroids.end());
    }

    spdlog::info("[MPMInitializer] KMeans complete: {} centroids", centroids.size());
    return centroids;
}

std::vector<glm::vec3> MPMInitializer::RunKMeansOnChunk(
    const std::vector<glm::vec3>& chunk,
    size_t num_clusters,
    int max_iter,
    float tolerance
) {
    if (num_clusters == 0 || chunk.empty()) {
        return {};
    }

    if (num_clusters >= chunk.size()) {
        return chunk; // 不需要降采样
    }

    std::vector<glm::vec3> centroids(num_clusters);

    // 随机初始化聚类中心（从数据点中随机选择）
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, chunk.size() - 1);

    for (size_t i = 0; i < num_clusters; ++i) {
        centroids[i] = chunk[dist(gen)];
    }

    // Lloyd迭代
    std::vector<size_t> assignments(chunk.size());
    std::vector<size_t> cluster_sizes(num_clusters);
    std::vector<glm::vec3> new_centroids(num_clusters);
    std::vector<glm::vec3> cluster_sums(num_clusters, glm::vec3(0.0f));

    for (int iter = 0; iter < max_iter; ++iter) {
        // 重置
        std::fill(cluster_sizes.begin(), cluster_sizes.end(), 0);
        std::fill(cluster_sums.begin(), cluster_sums.end(), glm::vec3(0.0f));

        // 分配每个点到最近的聚类中心
        for (size_t i = 0; i < chunk.size(); ++i) {
            float min_dist_sq = std::numeric_limits<float>::max();
            size_t best_cluster = 0;

            for (size_t j = 0; j < num_clusters; ++j) {
                glm::vec3 diff = chunk[i] - centroids[j];
                float dist_sq = glm::dot(diff, diff);
                if (dist_sq < min_dist_sq) {
                    min_dist_sq = dist_sq;
                    best_cluster = j;
                }
            }

            assignments[i] = best_cluster;
            cluster_sums[best_cluster] += chunk[i];
            cluster_sizes[best_cluster]++;
        }

        // 更新聚类中心
        float max_shift_sq = 0.0f;
        for (size_t j = 0; j < num_clusters; ++j) {
            if (cluster_sizes[j] > 0) {
                new_centroids[j] = cluster_sums[j] / static_cast<float>(cluster_sizes[j]);
            } else {
                new_centroids[j] = centroids[j]; // 保持不变
            }

            glm::vec3 shift = new_centroids[j] - centroids[j];
            float shift_sq = glm::dot(shift, shift);
            max_shift_sq = std::max(max_shift_sq, shift_sq);
        }

        centroids = new_centroids;

        // 检查收敛
        if (max_shift_sq < tolerance * tolerance) {
            spdlog::debug("[MPMInitializer] KMeans converged after {} iterations", iter + 1);
            break;
        }
    }

    return centroids;
}

// ============================================================
// 子步骤5：计算粒子体积
// ============================================================

std::vector<float> MPMInitializer::ComputeParticleVolumes(
    const std::vector<glm::vec3>& points,
    int resolution
) {
    std::vector<float> volumes(points.size());

    // 使用体素化方法（与Python一致）
    // 在[-1, 1]空间建立 resolution^3 的体素网格
    // 每个体素的体积 = (2 / (resolution - 1))^3

    const float cell_volume = std::pow(2.0f / (resolution - 1), 3.0f);

    // 统计每个体素中的粒子数
    std::unordered_map<uint64_t, uint32_t> voxel_counts;

    for (size_t i = 0; i < points.size(); ++i) {
        // 将[-1,1]映射到[0, resolution-1]
        glm::ivec3 voxel_idx = glm::ivec3(
            (points[i] + glm::vec3(1.0f)) * glm::vec3(0.5f * float(resolution - 1))
        );

        // 确保在有效范围内
        voxel_idx = glm::clamp(voxel_idx, glm::ivec3(0), glm::ivec3(resolution - 1));

        // 编码体素索引
        uint64_t key = static_cast<uint64_t>(voxel_idx.x) |
                      (static_cast<uint64_t>(voxel_idx.y) << 16) |
                      (static_cast<uint64_t>(voxel_idx.z) << 32);

        voxel_counts[key]++;
    }

    // 统计信息
    size_t num_non_empty = voxel_counts.size();
    size_t max_count = 0;
    size_t min_count = std::numeric_limits<size_t>::max();
    for (const auto& kv : voxel_counts) {
        max_count = std::max(max_count, static_cast<size_t>(kv.second));
        min_count = std::min(min_count, static_cast<size_t>(kv.second));
    }

    spdlog::info("[MPMInitializer] Volume stats: non_empty_voxels={}, max_in_voxel={}, min_in_voxel={}",
                 num_non_empty, max_count, min_count);

    // 计算每个粒子的体积
    for (size_t i = 0; i < points.size(); ++i) {
        glm::ivec3 voxel_idx = glm::ivec3(
            (points[i] + glm::vec3(1.0f)) * glm::vec3(0.5f * float(resolution - 1))
        );
        voxel_idx = glm::clamp(voxel_idx, glm::ivec3(0), glm::ivec3(resolution - 1));

        uint64_t key = static_cast<uint64_t>(voxel_idx.x) |
                      (static_cast<uint64_t>(voxel_idx.y) << 16) |
                      (static_cast<uint64_t>(voxel_idx.z) << 32);

        auto it = voxel_counts.find(key);
        if (it != voxel_counts.end()) {
            volumes[i] = cell_volume / static_cast<float>(it->second);
        } else {
            volumes[i] = cell_volume; // 不应该发生
        }
    }

    return volumes;
}

// ============================================================
// 子步骤6：计算边界条件冻结掩码
// ============================================================

std::vector<bool> MPMInitializer::ComputeFreezeMask(
    const std::vector<glm::vec3>& drive_particles,
    const std::vector<glm::vec3>& moving_part_points,
    const CoordinateTransform& transform,
    int grid_size,
    float threshold_scale
) {
    std::vector<bool> freeze_mask(drive_particles.size(), false);

    if (moving_part_points.empty()) {
        spdlog::warn("[MPMInitializer] No moving part points, all particles will be active");
        return freeze_mask;
    }

    // 将moving_part_points变换到归一化空间
    std::vector<glm::vec3> moving_normalized;
    moving_normalized.reserve(moving_part_points.size());
    for (const auto& pt : moving_part_points) {
        moving_normalized.push_back(transform.ToNormalized(pt));
    }

    // 计算阈值：thres = threshold_scale / grid_size
    // 对应 physDreamer: demo.py 第366行
    float threshold = threshold_scale / static_cast<float>(grid_size);
    float threshold_sq = threshold * threshold;

    spdlog::info("[MPMInitializer] Freeze threshold: {:.6f} (threshold_scale={:.2f}, grid_size={})",
                 threshold, threshold_scale, grid_size);

    // 使用SceneLoader::FindFarPoints
    freeze_mask = SceneLoader::FindFarPoints(drive_particles, moving_normalized, threshold);

    return freeze_mask;
}

// ============================================================
// 子步骤7：建立Top-K映射
// ============================================================

std::vector<TopKMapping> MPMInitializer::BuildTopKMapping(
    const std::vector<glm::vec3>& render_points,
    const std::vector<glm::vec3>& drive_particles
) {
    std::vector<TopKMapping> mappings(render_points.size());

    const size_t num_render = render_points.size();
    const size_t num_drive = drive_particles.size();

    spdlog::info("[MPMInitializer] Building Top-K mapping: {} render -> {} drive", num_render, num_drive);

    // 为每个渲染粒子找到K个最近的驱动粒子
    const size_t chunk_size = 5000;

    for (size_t chunk_start = 0; chunk_start < num_render; chunk_start += chunk_size) {
        size_t chunk_end = std::min(chunk_start + chunk_size, num_render);

        spdlog::debug("[MPMInitializer] Processing render particles {}/{}",
                     chunk_start, num_render);

        for (size_t i = chunk_start; i < chunk_end; ++i) {
            // 使用优先队列找K个最近邻
            using DistIndex = std::pair<float, size_t>;
            std::priority_queue<DistIndex, std::vector<DistIndex>, std::greater<DistIndex>> pq;

            for (size_t j = 0; j < num_drive; ++j) {
                glm::vec3 diff = render_points[i] - drive_particles[j];
                float dist_sq = glm::dot(diff, diff);

                if (pq.size() < TopKMapping::K) {
                    pq.push({dist_sq, j});
                } else if (dist_sq < pq.top().first) {
                    pq.pop();
                    pq.push({dist_sq, j});
                }
            }

            // 提取结果
            int k = 0;
            float weight_sum = 0.0f;
            while (!pq.empty() && k < TopKMapping::K) {
                auto [dist_sq, idx] = pq.top();
                pq.pop();

                // 倒序存储（最近的在前）
                mappings[i].particle_indices[TopKMapping::K - 1 - k] = static_cast<uint32_t>(idx);

                // 权重 = 距离平方的倒数
                float weight = 1.0f / (dist_sq + 1e-6f);
                mappings[i].weights[TopKMapping::K - 1 - k] = weight;
                weight_sum += weight;

                k++;
            }

            // 剩余的填充无效索引
            for (int j = k; j < TopKMapping::K; ++j) {
                mappings[i].particle_indices[j] = UINT32_MAX;
                mappings[i].weights[j] = 0.0f;
            }

            mappings[i].weight_sum = weight_sum;
        }
    }

    return mappings;
}

// ============================================================
// 子步骤8：组装ParticleData数组
// ============================================================

std::vector<ParticleData> MPMInitializer::AssembleParticles(
    const std::vector<glm::vec3>& positions,
    const std::vector<float>& volumes,
    const std::vector<bool>& freeze_mask,
    const MaterialConfig& material
) {
    const size_t num_particles = positions.size();
    std::vector<ParticleData> particles(num_particles);

    // 计算Lamé参数
    float mu, lam;
    ComputeLameParameters(material.E, material.nu, mu, lam);

    for (size_t i = 0; i < num_particles; ++i) {
        ParticleData& p = particles[i];

        p.position = positions[i];
        p.velocity = glm::vec3(0.0f);
        p.freeze_flag = freeze_mask[i] ? 1u : 0u;
        p.deformation_gradient = glm::mat3(1.0f); // 单位矩阵 = 无变形
        p.volume = volumes[i];
        p.material_id = material.material_type;
        p.youngs_modulus = material.E;
        p.poisson_ratio = material.nu;
        p.density = material.density;

        // 质量密度 × 体积
        p.mass = material.density * volumes[i];

        // APIC矩阵初始化为零
        p.apic_matrix = glm::mat3(0.0f);

        // 假设这些都是原始高斯（非填充点）
        // 实际使用时可以根据需要设置
        p.is_filled_point = 0u;
    }

    return particles;
}

// ============================================================
// 辅助函数
// ============================================================

void MPMInitializer::ComputeLameParameters(float E, float nu, float& mu, float& lam) {
    // 对应 physDreamer: mpm_data_structure.py compute_mu_lam_from_E_nu_clean()
    mu = E / (2.0f * (1.0f + nu));
    lam = E * nu / ((1.0f + nu) * (1.0f - 2.0f * nu));
}

std::vector<glm::vec3> MPMInitializer::MapToMinusOneToOne(
    const std::vector<glm::vec3>& points_zero_to_one
) {
    std::vector<glm::vec3> result;
    result.reserve(points_zero_to_one.size());

    for (const auto& pt : points_zero_to_one) {
        result.push_back(pt * glm::vec3(2.0f) - glm::vec3(1.0f));
    }

    return result;
}

} // namespace MPM
