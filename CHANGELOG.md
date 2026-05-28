# 更新日志 (CHANGELOG)

## 2026年5月28日 - Windows平台编译修复与项目验证

### 🔧 修复内容

#### 1. Vulkan SDK兼容性修复
- **问题**: 项目代码与Vulkan SDK 1.4.350.0版本存在兼容性问题
- **修复**: 调整了`src/vulkan/VulkanContext.h`中的宏定义顺序
  - 将`VULKAN_HPP_DISPATCH_LOADER_DYNAMIC`和`VULKAN_HPP_TYPESAFE_CONVERSION`移到vulkan.hpp包含之前
  - 为`VULKAN_HPP_TYPESAFE_CONVERSION`添加显式值`1`
- **影响**: 解决了编译时的C1017错误（无效的整数常量表达式）

#### 2. 枚举助手文件更新
- **问题**: 项目自带的`vk_enum_string_helper.h`文件过旧，缺少新版Vulkan SDK中的枚举值
- **修复**: 用Vulkan SDK 1.4.350.0中的最新版本替换了`src/third_party/vk_enum_string_helper.h`
- **影响**: 解决了以下未声明标识符错误：
  - `VK_DRIVER_ID_MESA_AGXV`
  - `VK_PIPELINE_CREATE_2_RAY_TRACING_DISPLACEMENT_MICROMAP_BIT_NV`
  - `VK_BUFFER_USAGE_2_EXECUTION_GRAPH_SCRATCH_BIT_AMDX`

### 🏗️ 编译配置

#### CMake配置参数
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="<your path>/VulkanSDK/1.4.350.0" \
      -DVulkan_INCLUDE_DIR="<your path>/VulkanSDK/1.4.350.0/Include" \
      -DVulkan_LIBRARY="<your path>/VulkanSDK/1.4.350.0/Lib/vulkan-1.lib" \
      -S ./ -B ./build
```

#### 编译环境
- **操作系统**: Windows 11 Pro (10.0.26200)
- **编译器**: MSVC 19.51.36246.0 (Visual Studio 2022)
- **Vulkan SDK**: 1.4.350.0
- **CMake**: 4.3
- **构建类型**: Release

### ✅ 验证测试

#### 模型渲染测试
- **模型文件**: `D:\liuyue\physDreamerVulkanDemo\physics_dreamer\physics_dreamer\carnations\point_cloud.ply`
- **文件大小**: 257MB
- **加载时间**: 219ms
- **GPU**: NVIDIA GeForce RTX 4090 Laptop GPU (自动选择)

#### 运行结果
- ✅ 成功加载PLY格式点云模型
- ✅ 完成3D协方差矩阵预计算
- ✅ Vulkan计算管线正常运行
- ✅ 实时高斯溅射渲染功能正常
- ✅ 应用程序正常退出（退出代码：0）

### 📁 生成的文件

```
build/
├── apps/
│   └── viewer/
│       └── Release/
│           └── 3dgs_viewer.exe    # 主程序可执行文件
├── src/
│   └── Release/
│       └── 3dgs_cpp.lib           # 核心库文件
└── shaders/                       # 编译后的着色器文件
```

### 🚀 使用方法

#### 基本用法
```bash
cd build/apps/viewer/Release
./3dgs_viewer.exe <模型文件路径.ply>
```

#### 可选参数
- `--help`: 显示帮助信息
- `--verbose`: 启用详细日志
- `--device=N`: 选择GPU设备（N为设备索引）
- `--validation`: 启用Vulkan验证层
- `--width=N`: 设置窗口宽度
- `--height=N`: 设置窗口高度
- `--no-gui`: 禁用GUI界面

### 📝 技术要点

1. **跨平台支持**: 项目使用Vulkan API，支持Windows、Linux、macOS等多个平台
2. **高性能渲染**: 利用Vulkan计算管线实现GPU加速的高斯溅射
3. **实时交互**: 支持实时相机控制和参数调整
4. **无CUDA依赖**: 纯Vulkan实现，不依赖NVIDIA CUDA，可在各种GPU上运行

### 🔍 已知问题

暂无。项目在当前环境下运行稳定。

### 📅 下一步计划

- [ ] 性能基准测试
- [ ] 支持更多模型格式
- [ ] 优化大规模点云渲染性能
- [ ] 添加更多渲染控制选项

---

## 项目信息

**项目名称**: 3DGS.cpp  
**原项目地址**: https://github.com/shg8/3DGS.cpp  
**许可证**: LGPL (主项目)，各第三方库使用各自许可证  
**技术栈**: Vulkan, C++20, CMake, GLM, GLFW, ImGui  
