# 更新日志 (CHANGELOG)

## 2026年5月28日 - 截图功能与GUI优化

### ✨ 新增功能

#### F12 截图功能
- **快捷键**: 按 `F12` 键截取当前帧画面
- **文件格式**: PNG 格式
- **文件命名**: `screenshot_1.png`, `screenshot_2.png`, ... (自动递增)
- **截图内容**: 纯 3DGS 渲染结果，不包含 GUI 窗口
- **实现位置**: `src/Renderer.h`, `src/Renderer.cpp`, `src/vulkan/Window.h`, `src/vulkan/windowing/GLFWWindow.h/cpp`

### 🔧 GUI 优化

#### Performance 窗口简化
- **删除内容**: 移除 FPS 实时曲线图
- **保留内容**: Metrics 窗口中的 FPS 文本计数显示
- **优化原因**: 简化界面，FPS 数值在 Metrics 窗口显示更直观

### 🔧 技术实现

#### 截图实现细节
- **截取时机**: 在 3D 渲染完成后、GUI 渲染**之前**截取
- **颜色转换**: BGRA → RGB (修复 Vulkan 颜色格式问题)
- **图像翻转**: 垂直翻转以匹配屏幕坐标系
- **GPU 到 CPU 复制**: 使用 Vulkan staging buffer 传输图像数据

#### 渲染管线顺序
```
3D 渲染
  ↓
截取纯 3D 渲染结果 (F12 触发)
  ↓
GUI 渲染 (Performance/Metrics/Camera Info)
  ↓
Present
```

### 📝 使用方法

#### 截图
```bash
./3dgs_viewer.exe <scene.ply>
# 程序运行中按 F12 键截图
# 截图保存到: screenshot_N.png
```

### 🐛 修复问题

#### 截图功能修复
- **问题 1**: 截图包含 GUI 窗口
  - **修复**: 在 GUI 渲染之前截取图像
- **问题 2**: 颜色显示错误
  - **修复**: 正确转换 BGRA 到 RGB
- **问题 3**: 图像方向颠倒
  - **修复**: 垂直翻转图像数据

### 📁 修改文件

**新增文件**:
- `src/third_party/stb_image_write.h` - PNG 编码库

**修改文件**:
- `src/Renderer.h` - 添加截图相关成员变量和方法
- `src/Renderer.cpp` - 实现截图功能，FPS 显示优化
- `src/vulkan/Window.h` - 扩展键盘接口 (7→8 键)
- `src/vulkan/windowing/GLFWWindow.h/cpp` - 添加 F12 键支持
- `src/vulkan/windowing/MetalWindow.h/cpp` - 接口兼容性更新
- `CHANGELOG.md` - 更新日志

### ✅ 测试验证
- ✅ F12 截图功能正常
- ✅ 截图不包含 GUI 窗口
- ✅ 颜色显示正确
- ✅ 图像方向正确
- ✅ Performance 窗口简洁，FPS 仅在 Metrics 显示

---

## 2026年5月28日 - 相机信息管理与保存功能

### ✨ 新增功能

#### Camera Info 窗口
- **功能**: 在GUI界面中添加实时相机信息显示窗口
- **实现位置**: `src/GUIManager.h`, `src/GUIManager.cpp`, `src/Renderer.cpp`
- **显示内容**:
  - **位置坐标**: X, Y, Z 精确到小数点后3位
  - **旋转信息**:
    - 欧拉角形式 (Pitch, Yaw, Roll)
    - 四元数形式 (W, X, Y, Z)
  - **视野参数**:
    - FOV (角度)
    - 近裁剪面距离
    - 远裁剪面距离
  - **保存按钮**: 一键保存当前相机配置

#### 相机保存与加载
- **保存功能**: 点击 "Save Camera" 按钮保存当前视角到 `camera.txt`
- **加载功能**: 使用 `--camera <文件路径>` 命令行参数加载保存的相机配置
- **文件格式**: 简单文本格式，便于编辑和分享

#### 命令行参数
```bash
# 加载保存的相机配置
./3dgs_viewer.exe --camera camera.txt <场景文件.ply>
```

### 🔧 技术实现

#### 数据结构
```cpp
struct CameraInfo {
    glm::vec3 position;      // 相机位置
    glm::quat rotation;      // 相机旋转 (四元数)
    float fov;               // 视野角度 (度)
    float nearPlane;         // 近裁剪面距离
    float farPlane;          // 远裁剪面距离
};
```

#### 相机文件格式
```
position: <x> <y> <z>
rotation: <w> <x> <y> <z>
fov: <角度>
nearPlane: <距离>
farPlane: <距离>
```

#### 核心修改
- **src/GUIManager.h**: 添加 CameraInfo 结构体和 saveCameraRequested 标志
- **src/GUIManager.cpp**: 实现 Camera Info 窗口显示和保存按钮
- **src/Renderer.h**: 添加 loadCamera() 和 saveCamera() 方法
- **src/Renderer.cpp**: 实现相机文件读写逻辑
- **include/3dgs/3dgs.h**: 添加 loadCamera() 接口
- **src/3dgs.cpp**: 修复 start() 方法避免重复创建 renderer
- **apps/viewer/main.cpp**: 添加 --camera 命令行参数支持

### 🐛 重要修复

#### Renderer 对象重复创建问题
- **问题**: `VulkanSplatting::start()` 每次都创建新的 renderer 对象
- **影响**: 导致 `initialize()` 后加载的相机信息在 `start()` 时丢失
- **修复**: 修改 `start()` 方法检查 renderer 是否已存在，避免重复创建
- **位置**: `src/3dgs.cpp`

### ✅ 测试验证
- ✅ 相机信息实时更新正确
- ✅ 欧拉角和四元数转换准确
- ✅ 保存功能正常工作
- ✅ 加载功能正确恢复视角
- ✅ 命令行参数解析正确
- ✅ 相机文件格式读写一致

### 📝 使用示例

#### 保存相机配置
1. 启动程序并调整到想要的视角
2. 在 "Camera Info" 窗口中点击 "Save Camera" 按钮
3. 相机信息保存到当前目录的 `camera.txt` 文件

#### 加载相机配置
```bash
# 使用绝对路径
./3dgs_viewer.exe --camera d:/path/to/camera.txt scene.ply

# 使用相对路径 (camera.txt 在当前目录)
./3dgs_viewer.exe --camera camera.txt scene.ply
```

### 🎯 应用场景
- 快速恢复常用观察角度
- 分享相机配置给其他用户
- 批量渲染时保持一致视角
- 调试和测试特定视角

---

## 2026年5月28日 - 新增FPS显示功能

### ✨ 新增功能

#### 实时FPS监控显示
- **功能**: 在GUI界面中添加实时FPS显示和性能监控
- **实现位置**: `src/Renderer.cpp`
- **显示内容**:
  - **Performance窗口**: 实时FPS曲线图，显示最近1-30秒的FPS变化趋势
  - **Metrics窗口**: 当前FPS数值显示，精确到小数点后2位
  - **可调节历史**: 支持滑动调节显示历史时间范围（1-30秒）
  - **自动缩放**: Y轴自动适应FPS数值范围

#### 技术实现
- 利用现有的FPS计数器，每秒更新一次FPS数据
- 通过`GUIManager::pushMetric()`推送FPS数据到GUI
- 在Performance窗口显示实时曲线图
- 在Metrics窗口显示当前FPS数值
- 性能影响几乎为零，仅增加简单计数器

### 🎯 使用效果

**Performance窗口** - 实时FPS曲线图
```
┌─────────────────────────────┐
│     Performance             │
│  ┌───────────────────────┐  │
│  │   FPS实时曲线图       │  │
│  │   (可调历史范围)      │  │
│  └───────────────────────┘  │
│  History: [====|====|====]  │
└─────────────────────────────┘
```

**Metrics窗口** - 当前FPS数值
```
FPS: 60.00
```

### ✅ 测试验证
- ✅ FPS数据正确计算和显示
- ✅ 图表实时更新流畅
- ✅ 数值显示准确（精确到0.01 FPS）
- ✅ 历史数据滚动正常
- ✅ 界面简洁，无重复显示

### 📊 性能监控优势
- 实时了解渲染性能
- 分析FPS变化趋势
- 评估不同模型的渲染效率
- 辅助性能优化决策

---

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
