#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>

// Forward declarations
class GSScene;
class VulkanContext;

namespace MPM {
    struct DeformableRegion;
}

/**
 * Scene loader for physics simulation
 * Loads point cloud and deformable region data from PhysDreamer scenes
 */
class SceneLoader {
public:
    struct SceneDescriptor {
        std::string scene_path;
        std::string point_cloud_ply;
        std::string clean_object_points_ply;
        std::string moving_part_points_ply;
        std::string internal_filled_ply;
        std::string sim_fields_pt;
        std::string velo_fields_pt;
    };

    struct PLYData {
        std::vector<glm::vec3> positions;
        size_t vertex_count = 0;
        bool IsValid() const { return !positions.empty() && vertex_count > 0; }
    };

    SceneLoader() = default;
    ~SceneLoader() = default;

    SceneDescriptor CreateDescriptor(const std::string& point_cloud_path);
    static PLYData LoadPLY(const std::string& ply_path);
    bool LoadScene(const SceneDescriptor& descriptor);
    bool ValidateRequiredFiles(const SceneDescriptor& descriptor) const;

    // Segment deformable region by matching clean_object_points against full point cloud
    // Returns indices of matching gaussians
    struct DeformableRegionResult {
        std::vector<uint32_t> deformable_indices;
        std::vector<uint32_t> static_indices;
        bool IsValid() const { return !deformable_indices.empty(); }
    };
    DeformableRegionResult MatchAgainstPointCloud(const std::vector<glm::vec3>& allPositions) const;

    const PLYData& GetPointCloud() const { return point_cloud_data_; }
    const PLYData& GetCleanObjectPoints() const { return clean_object_data_; }
    const PLYData& GetMovingPartPoints() const { return moving_part_data_; }

    /**
     * FindFarPoints函数 - C++版本
     * 对应 physDreamer: local_utils.find_far_points()
     *
     * @param xyzs 要分类的点云 [N, 3]
     * @param selected_points 参考点云 [M, 3]
     * @param threshold 距离阈值
     * @return far_mask [N] (true = 远, false = 近)
     */
    static std::vector<bool> FindFarPoints(
        const std::vector<glm::vec3>& xyzs,
        const std::vector<glm::vec3>& selected_points,
        float threshold = 0.01f
    );

    /**
     * 计算前景仿真掩码
     * 对应 physDreamer: find_far_points() + sim_mask_in_raw_gaussian
     *
     * @param all_positions 完整高斯点云位置
     * @param clean_positions 前景物体参考点
     * @param threshold 距离阈值（默认0.01，与Python一致）
     * @return sim_mask (true = 前景, false = 背景)
     */
    static std::vector<bool> ComputeSimMask(
        const std::vector<glm::vec3>& all_positions,
        const std::vector<glm::vec3>& clean_positions,
        float threshold = 0.01f
    );

private:
    PLYData point_cloud_data_;
    PLYData clean_object_data_;
    PLYData moving_part_data_;
    PLYData internal_filled_data_;
    std::string scene_directory_;

    bool FileExists(const std::string& path) const;
};

#endif // SCENE_LOADER_H
