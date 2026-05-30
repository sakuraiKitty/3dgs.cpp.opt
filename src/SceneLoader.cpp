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
    return FileExists(descriptor.point_cloud_ply);
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
