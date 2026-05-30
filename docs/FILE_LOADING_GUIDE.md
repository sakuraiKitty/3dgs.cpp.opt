# 物理仿真文件加载指南

## 概述

本指南说明如何在3DGS.cpp应用中加载物理仿真所需的文件，包括：
- **PLY文件**：点云数据（高斯点云、可变形区域定义）
- **PT文件**：预训练模型（材料场、速度场）- 可选，第一版使用固定参数

## 文件结构

### 完整场景目录结构

```
your_scene/
├── point_cloud.ply                    # 必需 - 完整3D高斯点云
├── your_scene_clean_object_points.ply   # 可选 - 前景物体点云（可变形区域）
├── your_scene_moving_part_points.ply   # 可选 - 可变形区域点云（边界条件）
├── your_scene_internal_filled_points.ply # 可选 - 内部填充点
└── models/
    └── physdreamer/
        └── your_scene/
            └── model/
                ├── sim_fields.pt        # 可选 - 材料场网络
                └── velo_fields.pt       # 可选 - 速度场网络
```

### 文件格式说明

#### 1. point_cloud.ply（必需）
标准的PLY格式，包含3D高斯的所有数据：
```ply
format ascii 1.0
element vertex
property float x
property float y
property float z
property float nx
property float ny
property float nz
property float f_dc
property f_dc
...
property float opacity
property float scale_x
property float scale_y
property scale_z
property float rot_x
property float rot_y
property float rot_z
end_header
element vertex 12345
0.1 0.2 0.3 0.0 0.0 0.0 ... (12345行数据)
```

#### 2. clean_object_points.ply（可选）
简化的PLY格式，只包含位置：
```ply
format ascii 1.0
element vertex
property float x
property float y
property float z
end_header
element vertex 1000
0.1 0.2 0.3
0.4 0.5 0.6
...
```

**用途**：定义需要物理仿真的前景物体区域

#### 3. moving_part_points.ply（可选）
格式同clean_object_points.ply

**用途**：定义区域内可以自由移动的部分（边界条件）

#### 4. sim_fields.pt 和 velo_fields.pt（可选）
PyTorch保存的模型文件：
```python
# 模型结构（PhysDreamer训练结果）
TriplaneFields:
  - aabb: [2, 3] 轴对齐包围盒
  - resolutions: [3] 每个平面的分辨率
  - feat_dim: 特征维度
  - decoder: MLP解码器
  - output_dim: 输出维度

# 用途
sim_fields.pt  → 空间变化的杨氏模量偏移
velo_fields.pt → 空间变化的初始速度
```

## 使用方法

### 方法1：自动推断路径（推荐）

```cpp
// Renderer::initialize() 中

#include "SceneLoader.h"

void Renderer::initialize() {
    // ... 现有初始化代码 ...

    // 1. 创建场景加载器
    SceneLoader loader;

    // 2. 从point_cloud.ply自动推断其他文件路径
    auto descriptor = loader.CreateDescriptor(configuration.scene);

    // 3. 加载场景文件
    if (!loader.LoadScene(descriptor)) {
        spdlog::error("Failed to load scene files");
        return;
    }

    // 4. 获取可变形区域
    MPM::DeformableRegion region = loader.AutoSegmentRegion();

    // 5. 初始化MPMManager
    mpm_manager_ = loader.InitializeMPMManager(context, region);

    // 6. 加载物理粒子
    mpm_manager_->LoadParticlesFromScene(scene, region.GenerateSimMask(scene->GetNumVertices()));

    // ... 继续渲染初始化 ...
}
```

### 方法2：手动指定路径

```cpp
SceneLoader::SceneDescriptor descriptor;
descriptor.scene_path = "path/to/your_scene/";
descriptor.point_cloud_ply = "path/to/your_scene/point_cloud.ply";
descriptor.clean_object_points_ply = "path/to/foreground.ply";
descriptor.moving_part_points_ply = "path/to/moving_parts.ply";
descriptor.internal_filled_ply = "path/to/internal.ply"; // 可选
descriptor.sim_fields_pt = "path/to/sim_fields.pt";     // 可选
descriptor.velo_fields_pt = "path/to/velo_fields.pt";    // 可选

SceneLoader loader;
if (!loader.LoadScene(descriptor)) {
    // 处理错误
}
```

## 与现有系统集成

### 扩展GSScene支持外部数据

由于GSScene目前只支持从文件加载，我们需要扩展它：

```cpp
// 在GSScene.h中添加

class GSScene {
public:
    // 现有接口
    void load(const std::shared_ptr<VulkanContext>& context);

    // 新接口：从原始数据加载
    void loadFromRawData(
        const std::shared_ptr<VulkanContext>& context,
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<float>& shs,
        const std::vector<float>& opacities,
        const std::vector<glm::vec3>& scales,
        const std::vector<glm::vec4>& rotations
    );

    // 获取原始点云位置
    std::vector<glm::vec3> GetAllPositions() const;

    // 获取顶点数量
    uint64_t GetNumVertices() const { return header.numVertices; }
};
```

### 完整初始化示例

```cpp
void Renderer::initialize() {
    spdlog::info("=== Initializing 3DGS Physics ===");

    // 现有Vulkan初始化
    initializeVulkan();
    createGui();

    // ===== 新增：物理仿真初始化 =====

    // 1. 加载场景文件
    spdlog::info("Loading physics scene files...");
    SceneLoader loader;

    auto descriptor = loader.CreateDescriptor(configuration.scene);
    if (!loader.LoadScene(descriptor)) {
        spdlog::warn("Failed to load physics files, physics will be disabled");
        physics_available_ = false;
        goto continue_rendering; // 跳过物理初始化
    }

    physics_available_ = true;

    // 2. 准备可变形区域
    MPM::DeformableRegion region = loader.AutoSegmentRegion();

    // 3. 初始化MPMManager
    mpm_manager_ = loader.InitializeMPMManager(context, region);

    // 4. 生成物理粒子
    spdlog::info("Generating physics particles...");
    mpm_manager_->LoadParticlesFromScene(scene, region.GenerateSimMask(scene->GetNumVertices()));

    // 5. 初始化高斯-物理耦合
    gaussian_particle_mapper_ = std::make_shared<GaussianParticleMapper>();
    auto all_positions = scene->GetAllPositions(); // 需要实现此方法

    gaussian_particle_mapper_->PrecomputeMapping(
        all_positions,
        mpm_manager_->GetParticlePositions(),
        8  // K=8
    );

    // 上传KNN映射到GPU
    VkCommandBuffer cmd = context->beginOneTimeCommandBuffer();
    gaussian_particle_mapper_->UploadMappingToGPU(context, cmd);
    context->endOneTimeCommandBuffer(std::move(cmd), VulkanContext::Queue::COMPUTE);

    // 6. 初始化位移映射器
    displacement_mapper_ = std::make_shared<DisplacementMapper>(context);
    displacement_mapper_->CreatePipeline(context);

    // 7. 创建原始数据备份（用于错误回滚）
    CreateOriginalDataBackup();

    spdlog::info("Physics initialization complete");

    // ===== 继续渲染管线 =====
    continue_rendering:

continue_rendering:
    // 加载场景到GPU
    loadSceneToGPU();

    // 创建渲染管线
    createPreprocessPipeline();
    createPrefixSumPipeline();
    createRadixSortPipeline();
    createPreprocessSortPipeline();
    createTileBoundaryPipeline();
    createRenderPipeline();
    createCommandPool();
    recordPreprocessCommandBuffer();

    spdlog::info("=== Initialization Complete ===");
}
```

## 文件准备工具

### 工具1：从现有3DGS提取点云

如果你已经有了point_cloud.ply，可以提取前景区域：

```python
# tools/extract_foreground.py
import numpy as np
from plyfile import PlyData

def extract_foreground(input_ply, output_ply, threshold=0.01):
    """
    从完整点云中提取前景区域
    
    基于简单的距离阈值或用户手动分割
    """
    plydata = PlyData.read(input_ply)
    points = plydata['vertex']

    # 示例：提取中心区域
    center = np.mean(points, axis=0)
    distances = np.linalg.norm(points - center, axis=1)
    foreground = points[distances < threshold]

    # 保存前景点云
    foreground_ply = output_ply
    with open(foreground_ply, 'w') as f:
        f.write("ply\n")
        f.write("format ascii 1.0\n")
        f.write("element vertex\n")
        f.write("property float x\n")
        f.write("property float y\n")
        f.write("property float z\n")
        f.write("end_header\n")
        f.write(f"element vertex {len(foreground)}\n")
        for p in foreground:
            f.write(f"{p[0]} {p[1]} {p[2]}\n")

    print(f"Extracted {len(foreground)} points to {output_ply}")
```

### 工具2：生成测试场景

如果没有现成的场景文件，可以生成一个：

```cpp
// tools/generate_test_scene.cpp
void generate_test_scene(const std::string& output_path) {
    std::vector<glm::vec3> positions;

    // 生成一个球体
    for (int i = 0; i < 1000; i++) {
        float theta = 2.0f * M_PI * i / 1000.0f;
        float phi = M_PI * (1 + 2.0f * (i % 5)) / 5.0f;
        float r = 0.5f;

        positions.push_back(glm::vec3(
            r * sin(phi) * cos(theta),
            r * sin(phi) * sin(theta),
            r * cos(theta)
        ));
    }

    // 保存为PLY
    save_ply(output_path, positions);
}
```

## 故障排查

### 问题1：文件找不到

**症状**：
```
[SceneLoader] Required file not found: ...
```

**解决方案**：
1. 检查文件路径是否正确
2. 确保场景目录结构符合预期
3. 使用SceneLoader的自动推断功能

### 问题2：PLY格式不匹配

**症状**：
```
[SceneLoader] Invalid vertex count: ...
```

**解决方案**：
1. 检查PLY文件格式
2. 确保是ASCII格式（二进制PLY需要额外处理）
3. 验证vertex属性是否正确

### 问题3：内存不足

**症状**：
```
[SceneLoader] Too many vertices, out of memory
```

**解决方案**：
1. 增加降采样比例
2. 使用内部填充点而不是所有点
3. 考虑使用更粗的网格

### 问题4：区域分割错误

**症状**：
物理仿真影响了背景物体

**解决方案**：
1. 检查clean_object_points.ply是否只包含前景
2. 调整AutoSegmentRegion中的距离阈值
3. 手动提供精确的区域定义

## 性能优化建议

### 大场景处理策略

```
场景规模（高斯数）    粒子数    子步数    网格    预期FPS
< 50K                < 10K     128      64      45-60
50K - 100K           10K-25K  128-256   64-128   30-45
100K - 200K          25K-50K  256-512   128-128  15-30
```

### 文件加载优化

1. **异步加载**：在后台线程加载PLY文件
2. **流式处理**：逐行读取而非全部加载到内存
3. **缓存**：缓存解析后的点云数据

## 下一阶段

文件加载系统完成后，需要：

1. ✅ 扩展GSScene支持外部数据加载
2. ⏳ 集成到Renderer::initialize()
3. ⏳ 添加文件选择对话框
4. ⏳ 实现PT文件加载器（第二版本）

## 参考资料

- PhysDreamer项目：`D:\liuyue\physDreamerVulkanDemo\PhysDreamer`
- 计划文档：`C:\Users\Administrator\.claude\plans\refactored-wibbling-glacier.md`

---

**创建时间**：2026年5月30日
**版本**：1.0
