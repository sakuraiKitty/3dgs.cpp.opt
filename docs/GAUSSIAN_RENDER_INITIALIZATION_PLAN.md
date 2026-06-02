# 高斯渲染初始化框架 - C++重构计划

> **目标**: 将physDreamer的Python高斯渲染初始化流程改造为C++版本，集成到3dgs.cpp.opt项目中
> **创建日期**: 2025-06-02
> **状态**: 设计阶段

---

## 📋 执行摘要

### 问题陈述
当前3dgs.cpp.opt项目的PLY加载机制存在以下问题：

1. **非强制加载**: clean_object_points.ply和moving_part_points.ply缺失时仅警告，不报错退出
2. **数据不完整**: SceneLoader仅加载位置数据，未加载完整的3DGS属性（球谐系数、缩放、旋转、不透明度）
3. **缺少关键掩码**: 未实现类似physDreamer的`sim_mask_in_raw_gaussian`计算
4. **架构分离**: GSScene和SceneLoader职责重叠，缺少统一的高斯模型抽象

### 解决方案概述
设计一个完整的C++高斯渲染初始化框架，包括：

1. **GaussianModel类**: 完整的3D高斯模型，包含所有属性
2. **强制文件验证**: 三个PLY文件必须存在，否则报错退出
3. **FindFarPoints函数**: C++版本的点云距离计算
4. **SimMask计算**: 计算并存储前景/背景掩码
5. **统一初始化流程**: 类似physDreamer的setup_render函数

---

## 🎯 核心需求

### 必须加载的三个PLY文件

| 文件 | 用途 | 格式 | 强制性 |
|------|------|------|--------|
| `point_cloud.ply` | 完整3D高斯点云 | Binary PLY, 包含所有3DGS属性 | **必须** |
| `clean_object_points.ply` | 前景物体参考点 | Binary PLY, 仅位置(xyz) | **必须** |
| `moving_part_points.ply` | 可移动部分参考点 | Binary PLY, 仅位置(xyz) | **必须** |

### 加载失败行为
```cpp
// 伪代码
if (!file_exists(point_cloud.ply)) {
    spdlog::critical("Missing required file: point_cloud.ply");
    spdlog::critical("Path: {}", expected_path);
    spdlog::critical("Physics simulation requires all three PLY files.");
    std::exit(EXIT_FAILURE);
}
// 同样处理其他两个文件...
```

---

## 🏗️ 架构设计

### 类关系图

```
┌─────────────────────────────────────────────────────────────┐
│                    Renderer (Main)                           │
│  - initialize()                                             │
│  - sceneLoader_: SceneLoader                               │
│  - gaussianModel_: GaussianModel                            │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              GaussianModel (New Class)                       │
│  - LoadPLY()              ← 加载完整3DGS属性                  │
│  - xyz_, features_dc_, features_rest_                       │
│  - scaling_, rotation_, opacity_                            │
│  - sim_mask_: vector<bool>                                  │
│  - GetSimMask()         ← 返回前景掩码                       │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                 SceneLoader (Enhanced)                       │
│  - LoadScene()          ← 强制验证所有文件                   │
│  - FindFarPoints()      ← 计算距离掩码                       │
│  - ComputeSimMask()     ← 计算前景掩码                       │
│  - ValidateRequiredFiles() ← 强制检查                        │
└─────────────────────────────────────────────────────────────┘
```

### 数据流

```
启动
 │
 ├─→ 1. 验证三个PLY文件存在性
 │   └─→ 失败 → 报错退出
 │
 ├─→ 2. 加载 point_cloud.ply
 │   └─→ GaussianModel::LoadPLY()
 │       └─→ 加载所有3DGS属性
 │
 ├─→ 3. 加载 clean_object_points.ply
 │   └─→ SceneLoader::LoadPLY() (仅位置)
 │
 ├─→ 4. 加载 moving_part_points.ply
 │   └─→ SceneLoader::LoadPLY() (仅位置)
 │
 ├─→ 5. 计算 sim_mask
 │   └─→ FindFarPoints(gaussian_xyz, clean_xyz)
 │       └─→ sim_mask = !far_points
 │
 └─→ 6. 初始化完成
     └─→ Renderer可以使用sim_mask进行前景/背景渲染
```

---

## 📦 数据结构设计

### GaussianModel类

```cpp
// src/GaussianModel.h (新文件)

#ifndef GAUSSIAN_MODEL_H
#define GAUSSIAN_MODEL_H

#include <vector>
#include <glm/glm.hpp>
#include <string>
#include <memory>

class VulkanContext;

/**
 * 完整的3D高斯模型，对应physDreamer的GaussianModel类
 *
 * 存储所有3DGS属性：
 * - 位置 (xyz)
 * - 球谐系数 (features_dc, features_rest)
 * - 缩放 (scaling)
 * - 旋转 (rotation)
 * - 不透明度 (opacity)
 */
class GaussianModel {
public:
    // ===== 核心属性（与physDreamer对应） =====

    // 位置 [N, 3]
    std::vector<glm::vec3> xyz_;

    // DC球谐系数 [N, 3] (基础颜色)
    std::vector<glm::vec3> features_dc_;

    // 高阶球谐系数 [N, 45] (对于sh_degree=3)
    std::vector<float> features_rest_;

    // 对数尺度 [N, 3] (实际尺度 = exp(scaling))
    std::vector<glm::vec3> scaling_;

    // 四元数旋转 [N, 4]
    std::vector<glm::vec4> rotation_;

    // Logit不透明度 [N] (实际不透明度 = sigmoid(opacity))
    std::vector<float> opacity_;

    // ===== 仿真相关属性 =====

    // 前景掩码 [N] (true = 前景/可仿真, false = 背景/静态)
    std::vector<bool> sim_mask_;

    // ===== 方法 =====

    GaussianModel() = default;
    ~GaussianModel() = default;

    /**
     * 从PLY文件加载完整的高斯模型
     * 对应 physDreamer: GaussianModel.load_ply()
     */
    bool LoadPLY(const std::string& ply_path);

    /**
     * 获取前景掩码
     * 返回属于前景（可仿真）的高斯索引
     */
    std::vector<uint32_t> GetForegroundIndices() const;

    /**
     * 获取背景掩码
     * 返回属于背景（静态）的高斯索引
     */
    std::vector<uint32_t> GetBackgroundIndices() const;

    /**
     * 获取高斯数量
     */
    size_t GetCount() const { return xyz_.size(); }

    /**
     * 验证模型是否有效
     */
    bool IsValid() const;

    /**
     * 上传到GPU缓冲区
     */
    void UploadToVulkan(const std::shared_ptr<VulkanContext>& context);

private:
    /**
     * 解析PLY头部
     */
    struct PLYHeader {
        int num_vertices = 0;
        std::vector<std::pair<std::string, std::string>> properties; // (type, name)
    };
    bool ParsePLYHeader(std::ifstream& file, PLYHeader& header);

    /**
     * 读取单个顶点的所有属性
     */
    bool ReadVertex(std::ifstream& file, const PLYHeader& header,
                    glm::vec3& xyz, glm::vec3& features_dc,
                    std::vector<float>& features_rest, glm::vec3& scaling,
                    glm::vec4& rotation, float& opacity);
};

#endif // GAUSSIAN_MODEL_H
```

### 增强的SceneLoader

```cpp
// src/SceneLoader.h (修改)

class SceneLoader {
public:
    // ... 现有代码 ...

    /**
     * 强制验证所有必需文件
     * 如果任何一个缺失，打印详细错误信息并返回false
     */
    bool ValidateRequiredFiles(const SceneDescriptor& descriptor) const;

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

    // ... 现有代码 ...
};
```

---

## 🔧 关键函数实现

### FindFarPoints实现

```cpp
// src/SceneLoader.cpp

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

    // 分块处理（与Python一致，每块10000个点）
    const size_t chunk_size = 10000;
    const size_t num_chunks = (N + chunk_size - 1) / chunk_size;

    spdlog::info("[FindFarPoints] Processing {} points against {} references ({} chunks)",
                 N, M, num_chunks);

    for (size_t chunk = 0; chunk < num_chunks; ++chunk) {
        size_t start = chunk * chunk_size;
        size_t end = std::min(start + chunk_size, N);

        // 对每个块中的点，计算到所有参考点的最小距离
        for (size_t i = start; i < end; ++i) {
            const glm::vec3& p = xyzs[i];

            // 找到最近的参考点
            float min_dist_sq = std::numeric_limits<float>::max();
            for (size_t j = 0; j < M; ++j) {
                float dist_sq = glm::dot(p - selected_points[j], p - selected_points[j]);
                if (dist_sq < min_dist_sq) {
                    min_dist_sq = dist_sq;
                }
            }

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
```

### 强制文件验证

```cpp
// src/SceneLoader.cpp

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
```

### GaussianModel::LoadPLY实现

```cpp
// src/GaussianModel.cpp

bool GaussianModel::LoadPLY(const std::string& ply_path) {
    spdlog::info("[GaussianModel] Loading PLY: {}", ply_path);

    std::ifstream file(ply_path, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("[GaussianModel] Failed to open: {}", ply_path);
        return false;
    }

    // 解析头部
    PLYHeader header;
    if (!ParsePLYHeader(file, header)) {
        spdlog::error("[GaussianModel] Failed to parse PLY header");
        return false;
    }

    spdlog::info("[GaussianModel] Vertices: {}", header.num_vertices);

    // 预分配空间
    xyz_.reserve(header.num_vertices);
    features_dc_.reserve(header.num_vertices);
    scaling_.reserve(header.num_vertices);
    rotation_.reserve(header.num_vertices);
    opacity_.reserve(header.num_vertices);

    // features_rest是扁平化的 [N, 45]
    features_rest_.reserve(header.num_vertices * 45);

    // 读取所有顶点
    for (int i = 0; i < header.num_vertices; ++i) {
        glm::vec3 xyz;
        glm::vec3 features_dc;
        std::vector<float> features_rest(45);
        glm::vec3 scaling;
        glm::vec4 rotation;
        float opacity;

        if (!ReadVertex(file, header, xyz, features_dc, features_rest,
                       scaling, rotation, opacity)) {
            spdlog::error("[GaussianModel] Failed to read vertex {}", i);
            return false;
        }

        xyz_.push_back(xyz);
        features_dc_.push_back(features_dc);
        scaling_.push_back(scaling);
        rotation_.push_back(rotation);
        opacity_.push_back(opacity);

        // 将features_rest插入到扁平化数组
        features_rest_.insert(features_rest_.end(), features_rest.begin(), features_rest.end());
    }

    file.close();

    spdlog::info("[GaussianModel] Loaded {} gaussians", xyz_.size());
    return true;
}

bool GaussianModel::ReadVertex(
    std::ifstream& file,
    const PLYHeader& header,
    glm::vec3& xyz,
    glm::vec3& features_dc,
    std::vector<float>& features_rest,
    glm::vec3& scaling,
    glm::vec4& rotation,
    float& opacity
) {
    // 期望的属性顺序（与physDreamer一致）:
    // x, y, z,
    // nx, ny, nz (法线，通常为0)
    // f_dc_0, f_dc_1, f_dc_2 (DC球谐)
    // f_rest_0 ... f_rest_45 (高阶球谐)
    // opacity
    // scale_0, scale_1, scale_2
    // rot_0, rot_1, rot_2, rot_3

    float temp;
    for (const auto& [type, name] : header.properties) {
        if (name == "x") file >> xyz.x;
        else if (name == "y") file >> xyz.y;
        else if (name == "z") file >> xyz.z;
        else if (name == "nx") file >> temp; // 忽略法线
        else if (name == "ny") file >> temp;
        else if (name == "nz") file >> temp;
        else if (name == "f_dc_0") file >> features_dc.x;
        else if (name == "f_dc_1") file >> features_dc.y;
        else if (name == "f_dc_2") file >> features_dc.z;
        else if (name.find("f_rest_") == 0) {
            // 解析 f_rest_N
            int idx = std::stoi(name.substr(8));
            if (idx < 45) file >> features_rest[idx];
            else file >> temp;
        }
        else if (name == "opacity") file >> opacity;
        else if (name == "scale_0") file >> scaling.x;
        else if (name == "scale_1") file >> scaling.y;
        else if (name == "scale_2") file >> scaling.z;
        else if (name == "rot_0") file >> rotation.x;
        else if (name == "rot_1") file >> rotation.y;
        else if (name == "rot_2") file >> rotation.z;
        else if (name == "rot_3") file >> rotation.w;
        else {
            file >> temp; // 跳过未知属性
        }
    }

    return true;
}
```

---

## 🔗 集成到Renderer

### Renderer初始化流程修改

```cpp
// src/Renderer.cpp (修改)

void Renderer::initialize() {
    // ... 现有Vulkan初始化代码 ...

    spdlog::info("[Renderer] ===== Gaussian Model Initialization =====");

    // 步骤1: 创建SceneDescriptor并验证所有文件
    auto descriptor = sceneLoader_.CreateDescriptor(configuration.scene);
    if (!sceneLoader_.ValidateRequiredFiles(descriptor)) {
        throw std::runtime_error("Required PLY files missing. Cannot initialize physics simulation.");
    }
    spdlog::info("[Renderer] ✓ All required PLY files validated");

    // 步骤2: 加载完整的高斯模型
    GaussianModel gaussianModel;
    if (!gaussianModel.LoadPLY(descriptor.point_cloud_ply)) {
        throw std::runtime_error("Failed to load Gaussian model from: " + descriptor.point_cloud_ply);
    }
    spdlog::info("[Renderer] ✓ Gaussian model loaded: {} gaussians", gaussianModel.GetCount());

    // 步骤3: 加载参考点云
    sceneLoader_.LoadScene(descriptor);
    spdlog::info("[Renderer] ✓ Reference point clouds loaded");

    // 步骤4: 计算前景仿真掩码
    std::vector<bool> sim_mask = SceneLoader::ComputeSimMask(
        gaussianModel.xyz_,
        sceneLoader_.GetCleanObjectPoints().positions,
        0.01f // 与Python一致
    );
    gaussianModel.sim_mask_ = sim_mask;

    size_t foreground_count = std::count(sim_mask.begin(), sim_mask.end(), true);
    spdlog::info("[Renderer] ✓ Simulation mask computed: {} foreground / {} total",
                 foreground_count, sim_mask.size());

    // 步骤5: 存储前景索引用于渲染
    pendingDeformableIndices_ = gaussianModel.GetForegroundIndices();
    spdlog::info("[Renderer] ✓ Deformable indices stored: {} indices",
                 pendingDeformableIndices_.size());

    // 步骤6: 上传到GPU（如果使用新的GaussianModel）
    // gaussianModel.UploadToVulkan(context);

    // 步骤7: 继续现有的GSScene加载流程（兼容性）
    auto scene = std::make_shared<GSScene>(configuration.scene);
    scene->load(context);
    this->scene = scene;

    // 步骤8: 创建前景掩码缓冲区
    createVisibilityMaskBuffer();
    setDeformableIndices(pendingDeformableIndices_);

    spdlog::info("[Renderer] ===== Gaussian Model Initialization Complete =====");

    // ... 其余初始化代码 ...
}
```

---

## 📊 验证方案

### 单元测试

```cpp
// tests/test_gaussian_model.cpp (新文件)

#include <gtest/gtest.h>
#include "GaussianModel.h"
#include "SceneLoader.h"

class GaussianModelTest : public ::testing::Test {
protected:
    std::string test_data_dir = "D:/liuyue/physDreamerVulkanDemo/PhysDreamer/data/physics_dreamer/carnations/";
};

TEST_F(GaussianModelTest, LoadPointCloud) {
    GaussianModel model;
    std::string ply_path = test_data_dir + "point_cloud.ply";

    EXPECT_TRUE(model.LoadPLY(ply_path));
    EXPECT_GT(model.GetCount(), 0);
    EXPECT_TRUE(model.IsValid());
}

TEST_F(GaussianModelTest, SimMaskComputation) {
    // 加载完整点云
    SceneLoader loader;
    auto descriptor = loader.CreateDescriptor(test_data_dir + "point_cloud.ply");
    ASSERT_TRUE(loader.ValidateRequiredFiles(descriptor));
    ASSERT_TRUE(loader.LoadScene(descriptor));

    GaussianModel model;
    ASSERT_TRUE(model.LoadPLY(descriptor.point_cloud_ply));

    // 计算sim_mask
    std::vector<bool> sim_mask = SceneLoader::ComputeSimMask(
        model.xyz_,
        loader.GetCleanObjectPoints().positions,
        0.01f
    );

    // 验证掩码
    size_t foreground_count = std::count(sim_mask.begin(), sim_mask.end(), true);
    EXPECT_GT(foreground_count, 0);
    EXPECT_LT(foreground_count, model.GetCount()); // 不是所有点都是前景

    spdlog::info("Test: {} foreground / {} total",
                 foreground_count, model.GetCount());
}

TEST_F(GaussianModelTest, FindFarPointsBasic) {
    // 简单测试
    std::vector<glm::vec3> xyzs = {
        glm::vec3(0, 0, 0),
        glm::vec3(1, 0, 0),
        glm::vec3(10, 0, 0)  // 这个点很远
    };
    std::vector<glm::vec3> refs = {
        glm::vec3(0, 0, 0),
        glm::vec3(1, 0, 0)
    };

    auto far_mask = SceneLoader::FindFarPoints(xyzs, refs, 1.0f);

    EXPECT_FALSE(far_mask[0]); // 0,0,0 是近的
    EXPECT_FALSE(far_mask[1]); // 1,0,0 是近的
    EXPECT_TRUE(far_mask[2]);  // 10,0,0 是远的
}

TEST_F(GaussianModelTest, CarnationSceneValidation) {
    // 完整的carnations场景验证
    GaussianModel model;
    std::string ply_path = test_data_dir + "point_cloud.ply";
    ASSERT_TRUE(model.LoadPLY(ply_path));

    SceneLoader loader;
    auto descriptor = loader.CreateDescriptor(ply_path);
    ASSERT_TRUE(loader.LoadScene(descriptor));

    // 验证前景高斯数量与Python一致
    std::vector<bool> sim_mask = SceneLoader::ComputeSimMask(
        model.xyz_,
        loader.GetCleanObjectPoints().positions,
        0.01f
    );

    size_t foreground_count = std::count(sim_mask.begin(), sim_mask.end(), true);

    // 期望值（从Python代码获得）
    const size_t expected_foreground = 32703;
    const size_t expected_total = 1037279;

    EXPECT_EQ(model.GetCount(), expected_total);
    EXPECT_NEAR(foreground_count, expected_foreground, expected_foreground * 0.05); // 5%容差

    spdlog::info("Carnation validation: {} foreground / {} total (expected: {} / {})",
                 foreground_count, model.GetCount(),
                 expected_foreground, expected_total);
}
```

### 运行时验证

在程序启动时添加详细日志：

```
[12:34:56] [INFO] [Renderer] ===== Gaussian Model Initialization =====
[12:34:56] [INFO] [SceneLoader] Creating descriptor for: point_cloud
[12:34:56] [INFO] [SceneLoader] Clean points: D:/.../carnations/clean_object_points.ply
[12:34:56] [INFO] [SceneLoader] Moving parts: D:/.../carnations/moving_part_points.ply
[12:34:56] [INFO] [SceneLoader] ✓ All required PLY files validated
[12:34:56] [INFO] [GaussianModel] Loading PLY: D:/.../carnations/point_cloud.ply
[12:34:56] [INFO] [GaussianModel] Vertices: 1037279
[12:34:56] [INFO] [GaussianModel] Loaded 1037279 gaussians
[12:34:56] [INFO] [Renderer] ✓ Gaussian model loaded: 1037279 gaussians
[12:34:56] [INFO] [SceneLoader] Loaded clean object points: 104311 vertices
[12:34:56] [INFO] [SceneLoader] Loaded moving part points: 97223 vertices
[12:34:56] [INFO] [Renderer] ✓ Reference point clouds loaded
[12:34:56] [INFO] [ComputeSimMask] Computing simulation mask...
[12:34:56] [INFO] [ComputeSimMask] Total gaussians: 1037279
[12:34:56] [INFO] [ComputeSimMask] Clean reference points: 104311
[12:34:56] [INFO] [FindFarPoints] Processing 1037279 points against 104311 references (104 chunks)
[12:34:56] [INFO] [FindFarPoints] Processed 10/104 chunks
[12:34:56] [INFO] [FindFarPoints] Processed 20/104 chunks
...
[12:35:02] [INFO] [FindFarPoints] Processed 104/104 chunks
[12:35:02] [INFO] [FindFarPoints] Result: 1004576 far, 32703 near (threshold=0.01)
[12:35:02] [INFO] [ComputeSimMask] Result: 32703 foreground (simulable), 1004576 background (static)
[12:35:02] [INFO] [Renderer] ✓ Simulation mask computed: 32703 foreground / 1037279 total
[12:35:02] [INFO] [Renderer] ✓ Deformable indices stored: 32703 indices
[12:35:02] [INFO] [Renderer] ===== Gaussian Model Initialization Complete =====
```

---

## 📅 实施计划

### 阶段1: 核心实现 (2-3天)

| 任务 | 文件 | 优先级 | 估计时间 |
|------|------|--------|----------|
| 实现GaussianModel类 | `src/GaussianModel.{h,cpp}` | P0 | 4h |
| 实现FindFarPoints函数 | `src/SceneLoader.cpp` | P0 | 2h |
| 实现ComputeSimMask函数 | `src/SceneLoader.cpp` | P0 | 1h |
| 增强ValidateRequiredFiles | `src/SceneLoader.cpp` | P0 | 1h |

### 阶段2: 集成 (1天)

| 任务 | 文件 | 优先级 | 估计时间 |
|------|------|--------|----------|
| 修改Renderer初始化流程 | `src/Renderer.cpp` | P0 | 3h |
| 更新SceneLoader头文件 | `src/SceneLoader.h` | P0 | 1h |
| 更新CMakeLists.txt | `CMakeLists.txt` | P1 | 0.5h |

### 阶段3: 测试验证 (1天)

| 任务 | 文件 | 优先级 | 估计时间 |
|------|------|--------|----------|
| 编写单元测试 | `tests/test_gaussian_model.cpp` | P0 | 3h |
| 运行carnations场景验证 | - | P0 | 1h |
| 性能基准测试 | - | P1 | 2h |

### 阶段4: 文档和清理 (0.5天)

| 任务 | 文件 | 优先级 | 估计时间 |
|------|------|--------|----------|
| 更新CLAUDE.md | `CLAUDE.md` | P1 | 1h |
| 更新CHANGELOG.md | `CHANGELOG.md` | P1 | 0.5h |

---

## 🎯 成功标准

### 功能要求

1. ✅ **强制文件验证**: 三个PLY文件缺失时程序报错退出
2. ✅ **完整属性加载**: GaussianModel加载所有3DGS属性
3. ✅ **正确掩码计算**: sim_mask与Python版本一致（误差<5%）
4. ✅ **渲染集成**: 前景/背景渲染正常工作

### 性能要求

1. ✅ **加载时间**: carnations场景（1M高斯）加载时间 < 5秒
2. ✅ **掩码计算时间**: FindFarPoints处理1M点对100K点 < 10秒
3. ✅ **内存开销**: GaussianModel内存占用 < 原GSScene + 20%

### 兼容性要求

1. ✅ **向后兼容**: 现有GSScene加载流程保持可用
2. ✅ **数据格式**: 支持标准的3DGS PLY格式
3. ✅ **场景一致性**: 与physDreamer Python结果一致

---

## 📚 参考资料

### physDreamer源码

| 文件 | 路径 | 关键函数 |
|------|------|----------|
| setup_render | `projects/inference/demo.py:522-586` | 初始化高斯渲染 |
| GaussianModel | `physdreamer/gaussian_3d/scene/gaussian_model.py` | 高斯模型类 |
| find_far_points | `projects/inference/local_utils.py:259-286` | 距离掩码计算 |

### 3DGS属性说明

| 属性 | Python形状 | C++类型 | 说明 |
|------|-----------|---------|------|
| xyz | [N, 3] | vector<vec3> | 位置 |
| features_dc | [N, 3] | vector<vec3> | 球谐DC分量 |
| features_rest | [N, 45] | vector<float> | 高阶球谐 |
| scaling | [N, 3] | vector<vec3> | 对数尺度 |
| rotation | [N, 4] | vector<vec4> | 四元数 |
| opacity | [N, 1] | vector<float> | Logit不透明度 |

---

## 🔄 后续扩展

### 阶段2: MPM粒子映射（待规划）

1. 使用moving_part_points.ply计算freeze_mask
2. 将前景高斯映射到MPM粒子
3. 实现高斯-粒子双向更新

### 阶段3: 物理仿真集成（待规划）

1. 集成MPM仿真循环
2. 实现位移场传播到高斯
3. 仿真结果实时渲染

---

## 📝 变更历史

| 日期 | 版本 | 变更内容 |
|------|------|----------|
| 2025-06-02 | 1.0 | 初始设计文档 |
