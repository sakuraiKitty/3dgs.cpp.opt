#include "SceneLoader.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace fs = std::filesystem;

SceneLoader::SceneDescriptor SceneLoader::CreateDescriptor(
    const std::string& point_cloud_path
) {
    SceneDescriptor desc;
    desc.point_cloud_ply = point_cloud_path;

    fs::path ply_path(point_cloud_path);
    std::string scene_dir = ply_path.parent_path().string();
    desc.scene_path = scene_dir;
    std::string basename = ply_path.stem().string();

    spdlog::info("[SceneLoader] Creating descriptor for: {}", basename);

    desc.clean_object_points_ply = scene_dir + "/" + "clean_object_points.ply";
    desc.moving_part_points_ply = scene_dir + "/" + "moving_part_points.ply";
    desc.internal_filled_ply = scene_dir + "/" + "internal_filled_points.ply";

    spdlog::info("[SceneLoader] Clean points: {}", desc.clean_object_points_ply);
    spdlog::info("[SceneLoader] Moving parts: {}", desc.moving_part_points_ply);

    return desc;
}

bool SceneLoader::LoadScene(const SceneDescriptor& descriptor) {
    spdlog::info("[SceneLoader] Loading scene from: {}", descriptor.scene_path);
    scene_directory_ = descriptor.scene_path;

    if (!FileExists(descriptor.point_cloud_ply)) {
        spdlog::error("[SceneLoader] Point cloud not found: {}", descriptor.point_cloud_ply);
        return false;
    }
    point_cloud_data_ = LoadPLY(descriptor.point_cloud_ply);
    spdlog::info("[SceneLoader] Loaded point cloud: {} vertices", point_cloud_data_.vertex_count);

    if (FileExists(descriptor.clean_object_points_ply)) {
        clean_object_data_ = LoadPLY(descriptor.clean_object_points_ply);
        spdlog::info("[SceneLoader] Loaded clean object points: {} vertices",
                    clean_object_data_.vertex_count);
    } else {
        spdlog::warn("[SceneLoader] Clean object points not found");
    }

    if (FileExists(descriptor.moving_part_points_ply)) {
        moving_part_data_ = LoadPLY(descriptor.moving_part_points_ply);
        spdlog::info("[SceneLoader] Loaded moving part points: {} vertices",
                    moving_part_data_.vertex_count);
    } else {
        spdlog::warn("[SceneLoader] Moving part points not found");
    }

    return true;
}

SceneLoader::PLYData SceneLoader::LoadPLY(const std::string& ply_path) {
    PLYData data;
    std::ifstream file(ply_path, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("[SceneLoader] Failed to open PLY: {}", ply_path);
        return data;
    }

    // Parse header
    std::string line;
    int vertex_count = 0;
    bool binary = false;
    int property_count = 0;  // count vertex properties to compute stride

    while (std::getline(file, line)) {
        if (line.find("format binary") != std::string::npos) {
            binary = true;
        }
        if (line.find("element vertex") != std::string::npos) {
            size_t pos = line.rfind(' ');
            if (pos != std::string::npos) {
                vertex_count = std::stoi(line.substr(pos + 1));
            }
        }
        if (line.find("property ") == 0 && line.find("element") == std::string::npos) {
            property_count++;
        }
        if (line.find("end_header") != std::string::npos) {
            break;
        }
    }

    if (vertex_count <= 0) {
        spdlog::error("[SceneLoader] Invalid vertex count: {}", vertex_count);
        return data;
    }

    data.vertex_count = static_cast<size_t>(vertex_count);
    data.positions.reserve(vertex_count);

    if (binary) {
        // Binary PLY: each vertex is property_count floats (4 bytes each)
        // For our point cloud PLYs: x(float) y(float) z(float) = 12 bytes per vertex
        // For full Gaussian PLYs: many more properties, much larger stride
        // We only need the first 3 floats (x,y,z)
        size_t vertex_stride = property_count * sizeof(float);

        // Fallback: if property_count seems wrong, try 3 floats (12 bytes)
        // by reading the first vertex and checking if the file is large enough
        size_t file_size = fs::file_size(ply_path);
        size_t expected_size = vertex_count * vertex_stride;
        if (expected_size > file_size * 2) {
            // Stride too large, recalculate from file size
            // Skip face data at end: file_size - header_end = vertex_data + face_data
            // For point-only PLYs (0 faces): stride = remaining / vertex_count
            vertex_stride = (file_size - static_cast<size_t>(file.tellg())) / vertex_count;
        }

        std::vector<char> buffer(vertex_stride);
        for (int i = 0; i < vertex_count; i++) {
            file.read(buffer.data(), vertex_stride);
            if (file.gcount() < 12) break;
            float x, y, z;
            memcpy(&x, buffer.data(), sizeof(float));
            memcpy(&y, buffer.data() + sizeof(float), sizeof(float));
            memcpy(&z, buffer.data() + 2 * sizeof(float), sizeof(float));
            data.positions.push_back(glm::vec3(x, y, z));
        }
    } else {
        // ASCII PLY
        for (int i = 0; i < vertex_count; i++) {
            glm::vec3 pos;
            if (!(file >> pos.x >> pos.y >> pos.z)) {
                spdlog::warn("[SceneLoader] Stopped reading at vertex {}", i);
                break;
            }
            // Skip remaining properties on this line
            file.ignore(256, '\n');
            data.positions.push_back(pos);
        }
    }

    file.close();
    spdlog::debug("[SceneLoader] Loaded {} positions from PLY ({} format)",
                data.positions.size(), binary ? "binary" : "ascii");
    return data;
}

bool SceneLoader::ValidateRequiredFiles(const SceneDescriptor& descriptor) const {
    bool all_valid = true;
    std::vector<std::pair<std::string, std::string>> missing_files;

    // 检查 point_cloud.ply
    if (!FileExists(descriptor.point_cloud_ply)) {
        missing_files.push_back({"point_cloud.ply", descriptor.point_cloud_ply});
        all_valid = false;
    }

    // 检查 clean_object_points.ply
    if (!FileExists(descriptor.clean_object_points_ply)) {
        missing_files.push_back({"clean_object_points.ply", descriptor.clean_object_points_ply});
        all_valid = false;
    }

    // 检查 moving_part_points.ply
    if (!FileExists(descriptor.moving_part_points_ply)) {
        missing_files.push_back({"moving_part_points.ply", descriptor.moving_part_points_ply});
        all_valid = false;
    }

    if (!all_valid) {
        spdlog::critical("==============================================");
        spdlog::critical("SCENE LOADING ERROR - MISSING REQUIRED FILES");
        spdlog::critical("==============================================");
        spdlog::critical("");

        for (const auto& [name, path] : missing_files) {
            spdlog::critical("Missing: {}", name);
            spdlog::critical("Expected path: {}", path);
            spdlog::critical("");
        }

        spdlog::critical("Physics simulation requires ALL three PLY files:");
        spdlog::critical("  1. point_cloud.ply - Complete Gaussian point cloud");
        spdlog::critical("  2. clean_object_points.ply - Foreground object reference");
        spdlog::critical("  3. moving_part_points.ply - Movable part reference");
        spdlog::critical("");
        spdlog::critical("Please ensure all files exist in the scene directory:");
        spdlog::critical("  {}", descriptor.scene_path);
        spdlog::critical("");
        spdlog::critical("Example: D:/path/to/carnations/");
        spdlog::critical("           ├── point_cloud.ply");
        spdlog::critical("           ├── clean_object_points.ply");
        spdlog::critical("           └── moving_part_points.ply");
        spdlog::critical("==============================================");
    }

    return all_valid;
}

SceneLoader::DeformableRegionResult SceneLoader::MatchAgainstPointCloud(
    const std::vector<glm::vec3>& allPositions
) const {
    DeformableRegionResult result;

    if (!clean_object_data_.IsValid() || allPositions.empty()) {
        spdlog::warn("[SceneLoader] Cannot match: missing data");
        return result;
    }

    size_t numClean = clean_object_data_.positions.size();
    size_t numAll = allPositions.size();
    spdlog::info("[SceneLoader] Matching {} clean points against {} gaussians (spatial hash)...",
                numClean, numAll);

    // Build spatial hash grid for all gaussian positions
    // Use a grid cell size that matches the typical spacing
    const float cellSize = 0.005f;  // Grid cell size
    const float threshold = 0.001f; // Match distance threshold

    auto hashKey = [cellSize](const glm::vec3& p) -> int64_t {
        int64_t ix = static_cast<int64_t>(std::floor(p.x / cellSize));
        int64_t iy = static_cast<int64_t>(std::floor(p.y / cellSize));
        int64_t iz = static_cast<int64_t>(std::floor(p.z / cellSize));
        // Simple hash combining
        return ix * 73856093LL ^ iy * 19349663LL ^ iz * 83492791LL;
    };

    // Map from hash -> list of gaussian indices
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    grid.reserve(numAll / 4);
    for (size_t i = 0; i < numAll; i++) {
        grid[hashKey(allPositions[i])].push_back(static_cast<uint32_t>(i));
    }

    // For each clean point, find matching gaussian(s) in neighboring cells
    std::vector<bool> is_deformable(numAll, false);
    size_t matched = 0;
    float thresholdSq = threshold * threshold;

    for (size_t c = 0; c < numClean; c++) {
        const glm::vec3& cleanPos = clean_object_data_.positions[c];
        int64_t cx = static_cast<int64_t>(std::floor(cleanPos.x / cellSize));
        int64_t cy = static_cast<int64_t>(std::floor(cleanPos.y / cellSize));
        int64_t cz = static_cast<int64_t>(std::floor(cleanPos.z / cellSize));

        float bestDistSq = thresholdSq;
        uint32_t bestIdx = UINT32_MAX;

        // Check 3x3x3 neighborhood
        for (int64_t dx = -1; dx <= 1; dx++) {
            for (int64_t dy = -1; dy <= 1; dy++) {
                for (int64_t dz = -1; dz <= 1; dz++) {
                    int64_t key = (cx+dx) * 73856093LL ^ (cy+dy) * 19349663LL ^ (cz+dz) * 83492791LL;
                    auto it = grid.find(key);
                    if (it == grid.end()) continue;
                    for (uint32_t idx : it->second) {
                        float dSq = glm::dot(allPositions[idx] - cleanPos,
                                            allPositions[idx] - cleanPos);
                        if (dSq < bestDistSq) {
                            bestDistSq = dSq;
                            bestIdx = idx;
                        }
                    }
                }
            }
        }

        if (bestIdx != UINT32_MAX && !is_deformable[bestIdx]) {
            is_deformable[bestIdx] = true;
            matched++;
        }
    }

    spdlog::info("[SceneLoader] Matched {} deformable gaussians out of {} total",
                matched, numAll);

    for (size_t i = 0; i < numAll; i++) {
        if (is_deformable[i]) {
            result.deformable_indices.push_back(static_cast<uint32_t>(i));
        } else {
            result.static_indices.push_back(static_cast<uint32_t>(i));
        }
    }

    return result;
}

bool SceneLoader::FileExists(const std::string& path) const {
    return fs::exists(path);
}

std::vector<bool> SceneLoader::FindFarPoints(
    const std::vector<glm::vec3>& xyzs,
    const std::vector<glm::vec3>& selected_points,
    float threshold
) {
    const size_t N = xyzs.size();
    const size_t M = selected_points.size();

    std::vector<bool> far_mask(N, false);
    if (M == 0 || N == 0) return far_mask;

    const float threshold_sq = threshold * threshold;

    spdlog::info("[FindFarPoints] Processing {} points against {} references (spatial hash)...",
                 N, M);

    // ========== 空间哈希优化 ==========
    // 使用空间哈希将复杂度从 O(N×M) 降低到 O(N)
    // 基于 SceneLoader 中的空间哈希实现

    // 计算合适的网格单元大小（基于阈值）
    const float cellSize = threshold * 0.5f;  // 确保邻域搜索覆盖阈值范围
    const float searchRadius = threshold;     // 搜索半径

    auto hashKey = [cellSize](const glm::vec3& p) -> int64_t {
        int64_t ix = static_cast<int64_t>(std::floor(p.x / cellSize));
        int64_t iy = static_cast<int64_t>(std::floor(p.y / cellSize));
        int64_t iz = static_cast<int64_t>(std::floor(p.z / cellSize));
        return ix * 73856093LL ^ iy * 19349663LL ^ iz * 83492791LL;
    };

    // 构建参考点的空间哈希网格
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    grid.reserve(M / 4);
    for (size_t i = 0; i < M; i++) {
        grid[hashKey(selected_points[i])].push_back(static_cast<uint32_t>(i));
    }

    // 分块处理（每块10000个点）
    const size_t chunk_size = 10000;
    const size_t num_chunks = (N + chunk_size - 1) / chunk_size;

    for (size_t chunk = 0; chunk < num_chunks; ++chunk) {
        size_t start = chunk * chunk_size;
        size_t end = std::min(start + chunk_size, N);

        // 对每个块中的点，使用空间哈希加速查找
        for (size_t i = start; i < end; ++i) {
            const glm::vec3& p = xyzs[i];

            // 计算该点所在的网格单元
            int64_t cx = static_cast<int64_t>(std::floor(p.x / cellSize));
            int64_t cy = static_cast<int64_t>(std::floor(p.y / cellSize));
            int64_t cz = static_cast<int64_t>(std::floor(p.z / cellSize));

            float min_dist_sq = std::numeric_limits<float>::max();

            // 搜索邻域单元（根据阈值确定搜索范围）
            const int searchCells = static_cast<int>(std::ceil(searchRadius / cellSize));

            for (int64_t dx = -searchCells; dx <= searchCells; dx++) {
                for (int64_t dy = -searchCells; dy <= searchCells; dy++) {
                    for (int64_t dz = -searchCells; dz <= searchCells; dz++) {
                        int64_t key = (cx + dx) * 73856093LL ^ (cy + dy) * 19349663LL ^ (cz + dz) * 83492791LL;
                        auto it = grid.find(key);
                        if (it == grid.end()) continue;

                        // 检查该单元内的所有参考点
                        for (uint32_t idx : it->second) {
                            float dist_sq = glm::dot(p - selected_points[idx], p - selected_points[idx]);
                            if (dist_sq < min_dist_sq) {
                                min_dist_sq = dist_sq;
                                // 如果已经找到非常近的点，可以提前退出
                                if (min_dist_sq < 1e-6f) goto found_closest;
                            }
                        }
                    }
                }
            }
            found_closest:

            // 如果最小距离超过阈值，标记为"远"
            far_mask[i] = (min_dist_sq > threshold_sq);
        }

        // 进度日志
        if ((chunk + 1) % 10 == 0 || chunk == num_chunks - 1) {
            spdlog::debug("[FindFarPoints] Processed {}/{} chunks",
                         chunk + 1, num_chunks);
        }
    }

    // 统计
    size_t far_count = std::count(far_mask.begin(), far_mask.end(), true);
    spdlog::info("[FindFarPoints] Result: {} far, {} near (threshold={})",
                 far_count, N - far_count, threshold);

    return far_mask;
}

std::vector<bool> SceneLoader::ComputeSimMask(
    const std::vector<glm::vec3>& all_positions,
    const std::vector<glm::vec3>& clean_positions,
    float threshold
) {
    spdlog::info("[ComputeSimMask] Computing simulation mask...");
    spdlog::info("[ComputeSimMask] Total gaussians: {}", all_positions.size());
    spdlog::info("[ComputeSimMask] Clean reference points: {}", clean_positions.size());

    // 找到远离clean点的背景点
    std::vector<bool> not_sim_mask = FindFarPoints(all_positions, clean_positions, threshold);

    // 反转：前景 = 非背景
    std::vector<bool> sim_mask(all_positions.size());
    for (size_t i = 0; i < all_positions.size(); ++i) {
        sim_mask[i] = !not_sim_mask[i];
    }

    // 统计
    size_t foreground_count = std::count(sim_mask.begin(), sim_mask.end(), true);
    size_t background_count = all_positions.size() - foreground_count;

    spdlog::info("[ComputeSimMask] Result: {} foreground (simulable), {} background (static)",
                 foreground_count, background_count);

    return sim_mask;
}
