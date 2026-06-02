# 高斯渲染初始化 - 快速实施指南

> **目标**: 将physDreamer的Python高斯渲染初始化改造成C++版本
> **详细计划**: 见 [GAUSSIAN_RENDER_INITIALIZATION_PLAN.md](GAUSSIAN_RENDER_INITIALIZATION_PLAN.md)

---

## 🎯 核心目标

**问题**: 当前SceneLoader加载PLY文件是可选的，缺失时仅警告不退出。physDreamer需要**三个强制PLY文件**进行物理仿真。

**解决方案**: 实现强制文件验证 + 完整GaussianModel + SimMask计算

---

## 📦 必需的三个PLY文件

```
carnations/
├── point_cloud.ply              # 完整3D高斯点云 (1,037,279点)
├── clean_object_points.ply      # 前景物体参考点 (104,311点)
└── moving_part_points.ply       # 可移动部分参考点 (97,223点)
```

**加载失败行为**: 打印详细错误信息 → `std::exit(EXIT_FAILURE)`

---

## 🔑 关键函数对应关系

| Python (physDreamer) | C++ (3dgs.cpp.opt) | 说明 |
|---------------------|-------------------|------|
| `setup_render()` | `Renderer::initialize()` | 高斯渲染初始化入口 |
| `GaussianModel.load_ply()` | `GaussianModel::LoadPLY()` | 加载完整3DGS属性 |
| `find_far_points()` | `SceneLoader::FindFarPoints()` | 计算距离掩码 |
| `sim_mask_in_raw_gaussian` | `GaussianModel::sim_mask_` | 前景掩码 |

---

## 📁 新增/修改文件清单

### 新增文件

```
src/
├── GaussianModel.h          # 完整的高斯模型类
├── GaussianModel.cpp        # PLY加载实现
└── tests/
    └── test_gaussian_model.cpp  # 单元测试
```

### 修改文件

```
src/
├── SceneLoader.h           # 添加 FindFarPoints, ComputeSimMask
├── SceneLoader.cpp         # 实现新函数 + 强制验证
└── Renderer.cpp            # 集成新初始化流程
```

---

## 🚀 快速开始

### 1. 实现GaussianModel类 (4h)

```bash
# 创建新文件
touch src/GaussianModel.h
touch src/GaussianModel.cpp
```

**核心代码框架**:
```cpp
class GaussianModel {
    std::vector<glm::vec3> xyz_;           // 位置
    std::vector<glm::vec3> features_dc_;   // 球谐DC
    std::vector<float> features_rest_;     // 球谐高阶
    std::vector<glm::vec3> scaling_;       // 缩放
    std::vector<glm::vec4> rotation_;      // 旋转
    std::vector<float> opacity_;           // 不透明度
    std::vector<bool> sim_mask_;           // 前景掩码

    bool LoadPLY(const std::string& ply_path);  // 加载完整PLY
    std::vector<uint32_t> GetForegroundIndices() const;  // 获取前景索引
};
```

### 2. 实现FindFarPoints (2h)

```cpp
// src/SceneLoader.cpp
std::vector<bool> SceneLoader::FindFarPoints(
    const std::vector<glm::vec3>& xyzs,
    const std::vector<glm::vec3>& selected_points,
    float threshold
) {
    // 分块处理，每块10000个点
    // 计算每个点到所有参考点的最小距离
    // 返回 far_mask (true = 远, false = 近)
}
```

### 3. 强制文件验证 (1h)

```cpp
// src/SceneLoader.cpp
bool SceneLoader::ValidateRequiredFiles(const SceneDescriptor& descriptor) const {
    // 检查三个文件是否都存在
    // 缺失时打印详细错误信息:
    //   - 缺失文件名
    //   - 期望路径
    //   - 所需文件列表
    //   - 示例目录结构
    // 返回 false（调用方应退出）
}
```

### 4. 集成到Renderer (3h)

```cpp
// src/Renderer.cpp - Renderer::initialize()
void Renderer::initialize() {
    // ... Vulkan初始化 ...

    // 1. 验证文件
    auto descriptor = sceneLoader_.CreateDescriptor(configuration.scene);
    if (!sceneLoader_.ValidateRequiredFiles(descriptor)) {
        throw std::runtime_error("Required PLY files missing");
    }

    // 2. 加载完整高斯模型
    GaussianModel gaussianModel;
    gaussianModel.LoadPLY(descriptor.point_cloud_ply);

    // 3. 加载参考点云
    sceneLoader_.LoadScene(descriptor);

    // 4. 计算前景掩码
    gaussianModel.sim_mask_ = SceneLoader::ComputeSimMask(
        gaussianModel.xyz_,
        sceneLoader_.GetCleanObjectPoints().positions,
        0.01f
    );

    // 5. 设置渲染
    pendingDeformableIndices_ = gaussianModel.GetForegroundIndices();
    setDeformableIndices(pendingDeformableIndices_);

    // ... 继续现有流程 ...
}
```

### 5. 单元测试 (3h)

```bash
# 创建测试文件
touch tests/test_gaussian_model.cpp
```

**核心测试用例**:
- `TEST_F(GaussianModelTest, LoadPointCloud)` - 加载point_cloud.ply
- `TEST_F(GaussianModelTest, SimMaskComputation)` - 计算前景掩码
- `TEST_F(GaussianModelTest, CarnationSceneValidation)` - 与Python结果对比

**期望验证结果** (carnations场景):
```
Total gaussians: 1,037,279
Foreground (simulable): 32,703 (3.15%)
Background (static): 1,004,576 (96.85%)
```

---

## ✅ 验证清单

### 功能验证

- [ ] 缺失PLY文件时程序退出并打印详细错误信息
- [ ] GaussianModel成功加载point_cloud.ply的所有属性
- [ ] FindFarPoints正确计算距离掩码
- [ ] SimMask与Python版本误差 < 5%
- [ ] 前景/背景渲染正常工作

### 性能验证

- [ ] carnations场景加载时间 < 5秒
- [ ] FindFarPoints (1M对100K) 处理时间 < 10秒
- [ ] 内存开销 < 原GSScene + 20%

### 场景验证

- [ ] carnations场景: 32,703 前景高斯
- [ ] 前景渲染模式仅显示可变形区域
- [ ] 与physDreamer Python结果一致

---

## 📚 代码片段库

### 强制文件验证错误信息模板

```cpp
spdlog::critical("==============================================");
spdlog::critical("SCENE LOADING ERROR - MISSING REQUIRED FILES");
spdlog::critical("==============================================");
spdlog::critical("Missing: {}", filename);
spdlog::critical("Expected path: {}", full_path);
spdlog::critical("");
spdlog::critical("Physics simulation requires ALL three PLY files:");
spdlog::critical("  1. point_cloud.ply - Complete Gaussian point cloud");
spdlog::critical("  2. clean_object_points.ply - Foreground object reference");
spdlog::critical("  3. moving_part_points.ply - Movable part reference");
spdlog::critical("");
spdlog::critical("Example directory structure:");
spdlog::critical("  carnations/");
spdlog::critical("    ├── point_cloud.ply");
spdlog::critical("    ├── clean_object_points.ply");
spdlog::critical("    └── moving_part_points.ply");
spdlog::critical("==============================================");
```

### ComputeSimMask实现模板

```cpp
std::vector<bool> SceneLoader::ComputeSimMask(
    const std::vector<glm::vec3>& all_positions,
    const std::vector<glm::vec3>& clean_positions,
    float threshold
) {
    spdlog::info("[ComputeSimMask] Total gaussians: {}", all_positions.size());
    spdlog::info("[ComputeSimMask] Clean reference points: {}", clean_positions.size());

    // 1. 找到远离clean点的背景点
    std::vector<bool> not_sim_mask = FindFarPoints(all_positions, clean_positions, threshold);

    // 2. 反转得到前景掩码
    std::vector<bool> sim_mask(all_positions.size());
    for (size_t i = 0; i < all_positions.size(); ++i) {
        sim_mask[i] = !not_sim_mask[i];
    }

    // 3. 统计并日志
    size_t foreground_count = std::count(sim_mask.begin(), sim_mask.end(), true);
    size_t background_count = all_positions.size() - foreground_count;
    spdlog::info("[ComputeSimMask] Result: {} foreground, {} background",
                 foreground_count, background_count);

    return sim_mask;
}
```

---

## 🔗 相关文档

- [详细计划](GAUSSIAN_RENDER_INITIALIZATION_PLAN.md) - 完整设计文档
- [CLAUDE.md](../CLAUDE.md) - 项目配置
- [CHANGELOG.md](../CHANGELOG.md) - 更新日志

---

## 📊 预期结果

### 运行日志输出

```
[12:34:56] [INFO] [Renderer] ===== Gaussian Model Initialization =====
[12:34:56] [INFO] [SceneLoader] ✓ All required PLY files validated
[12:34:56] [INFO] [GaussianModel] Loaded 1037279 gaussians
[12:34:56] [INFO] [SceneLoader] Loaded clean object points: 104311 vertices
[12:34:56] [INFO] [SceneLoader] Loaded moving part points: 97223 vertices
[12:34:56] [INFO] [ComputeSimMask] Result: 32703 foreground, 1004576 background
[12:34:56] [INFO] [Renderer] ✓ Deformable indices stored: 32703 indices
[12:34:56] [INFO] [Renderer] ===== Gaussian Model Initialization Complete =====
```

### 缺失文件错误输出

```
[12:34:56] [CRITICAL] ==============================================
[12:34:56] [CRITICAL] SCENE LOADING ERROR - MISSING REQUIRED FILES
[12:34:56] [CRITICAL] ==============================================
[12:34:56] [CRITICAL] Missing: clean_object_points.ply
[12:34:56] [CRITICAL] Expected path: D:/.../carnations/clean_object_points.ply
[12:34:56] [CRITICAL]
[12:34:56] [CRITICAL] Physics simulation requires ALL three PLY files:
[12:34:56] [CRITICAL]   1. point_cloud.ply - Complete Gaussian point cloud
[12:34:56] [CRITICAL]   2. clean_object_points.ply - Foreground object reference
[12:34:56] [CRITICAL]   3. moving_part_points.ply - Movable part reference
[12:34:56] [CRITICAL] ==============================================
```

---

**创建日期**: 2025-06-02
**状态**: 准备实施
