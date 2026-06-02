# 文档索引 (Documentation Index)

> **3dgs.cpp.opt 项目文档中心**
> 最后更新: 2025-06-02

---

## 📋 核心文档

### 项目配置

| 文档 | 路径 | 说明 |
|------|------|------|
| 项目配置 | [CLAUDE.md](../CLAUDE.md) | 编译配置、技术栈、项目架构 |
| 更新日志 | [CHANGELOG.md](../CHANGELOG.md) | 详细开发历史和变更记录 |

### 物理仿真集成

| 文档 | 路径 | 说明 |
|------|------|------|
| 详细集成计划 | [PHYSICS_INTEGRATION_DETAILED_PLAN.md](PHYSICS_INTEGRATION_DETAILED_PLAN.md) | MPM物理仿真集成完整路线图 |
| 高斯初始化计划 | [GAUSSIAN_RENDER_INITIALIZATION_PLAN.md](GAUSSIAN_RENDER_INITIALIZATION_PLAN.md) | **NEW**: physDreamer → C++ 重构设计 |
| 快速实施指南 | [QUICK_START_GUIDE.md](QUICK_START_GUIDE.md) | **NEW**: 5步实施指南 + 代码模板 |

---

## 🎯 快速导航

### 我想...

#### 了解项目如何编译运行
→ 查看 [CLAUDE.md](../CLAUDE.md) 的编译和运行条件部分

#### 了解物理仿真集成计划
→ 查看 [PHYSICS_INTEGRATION_DETAILED_PLAN.md](PHYSICS_INTEGRATION_DETAILED_PLAN.md)

#### 实施高斯渲染初始化
→ 1. 阅读 [QUICK_START_GUIDE.md](QUICK_START_GUIDE.md)
→ 2. 参考 [GAUSSIAN_RENDER_INITIALIZATION_PLAN.md](GAUSSIAN_RENDER_INITIALIZATION_PLAN.md) 了解细节

#### 查看最近的更新
→ 查看 [CHANGELOG.md](../CHANGELOG.md) 的最新条目

#### 理解三个PLY文件的作用
→ 查看 [GAUSSIAN_RENDER_INITIALIZATION_PLAN.md](GAUSSIAN_RENDER_INITIALIZATION_PLAN.md) 的"三个PLY文件的作用"章节

---

## 📊 文档关系图

```
┌─────────────────────────────────────────┐
│          CLAUDE.md (项目配置)            │
│  - 编译配置                               │
│  - 技术栈                                 │
│  - 项目架构                               │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│   PHYSICS_INTEGRATION_DETAILED_PLAN.md   │
│   (物理仿真集成总计划)                    │
│  - 5个阶段                                │
│  - MPM架构                                │
│  - 高斯-粒子耦合                          │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│ GAUSSIAN_RENDER_INITIALIZATION_PLAN.md   │
│   (高斯初始化子计划 - 阶段1详细)           │
│  - GaussianModel设计                      │
│  - FindFarPoints实现                      │
│  - 强制文件验证                            │
│  - 单元测试                               │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│        QUICK_START_GUIDE.md              │
│   (快速实施指南)                          │
│  - 5步实施                               │
│  - 代码模板                               │
│  - 验证清单                               │
└─────────────────────────────────────────┘
```

---

## 🏗️ 物理仿真集成文档结构

```
docs/
├── INDEX.md                              # 本文件
├── PHYSICS_INTEGRATION_DETAILED_PLAN.md  # 总体计划
├── GAUSSIAN_RENDER_INITIALIZATION_PLAN.md # 阶段1详细设计 (NEW)
└── QUICK_START_GUIDE.md                  # 阶段1快速指南 (NEW)
```

### 各阶段文档规划

| 阶段 | 名称 | 状态 | 文档 |
|------|------|------|------|
| 阶段1.1 | 数据结构与初始化 | ✅ 完成 | CHANGELOG (2025-05-30) |
| 阶段1.2 | GaussianModel实现 | 📋 设计 | GAUSSIAN_RENDER_INITIALIZATION_PLAN.md |
| 阶段2 | MPM Compute Shader | 📅 待规划 | - |
| 阶段3 | 高斯-粒子映射 | 📅 待规划 | - |
| 阶段4 | 仿真循环集成 | 📅 待规划 | - |

---

## 🔑 关键概念解释

### 三个PLY文件

| 文件 | 作用 | physDreamer对应 |
|------|------|----------------|
| `point_cloud.ply` | 完整3D高斯点云 | `GaussianModel.load_ply()` |
| `clean_object_points.ply` | 前景物体参考点 | `sim_mask_in_raw_gaussian` 计算 |
| `moving_part_points.ply` | 可移动部分参考点 | `freeze_mask` 计算 (阶段2) |

### 核心函数对应

| Python (physDreamer) | C++ (3dgs.cpp.opt) | 文档位置 |
|---------------------|-------------------|----------|
| `setup_render()` | `Renderer::initialize()` | QUICK_START_GUIDE.md |
| `GaussianModel.load_ply()` | `GaussianModel::LoadPLY()` | GAUSSIAN_RENDER_INITIALIZATION_PLAN.md |
| `find_far_points()` | `SceneLoader::FindFarPoints()` | GAUSSIAN_RENDER_INITIALIZATION_PLAN.md |
| `sim_mask_in_raw_gaussian` | `GaussianModel::sim_mask_` | QUICK_START_GUIDE.md |

---

## 📝 文档规范

### 新增文档时

1. **创建文档**: 在`docs/`目录下创建`.md`文件
2. **添加索引**: 在本文件中添加条目
3. **更新CHANGELOG**: 记录文档创建
4. **交叉引用**: 在相关文档中添加链接

### 文档命名约定

- 计划文档: `*_PLAN.md`
- 指南文档: `*_GUIDE.md`
- 设计文档: `*_DESIGN.md`
- 索引文档: `INDEX.md`

---

## 🔗 外部参考

### physDreamer源码

| 组件 | 路径 | 说明 |
|------|------|------|
| setup_render | `projects/inference/demo.py:522-586` | 高斯渲染初始化 |
| GaussianModel | `physdreamer/gaussian_3d/scene/gaussian_model.py` | 高斯模型类 |
| find_far_points | `projects/inference/local_utils.py:259-286` | 距离掩码计算 |

### 3DGS相关

- **3D Gaussian Splatting 论文**: [Kerbl et al., 2023](https://repo-sam.inria.fr/fungraph/3d-gaussian-splatting/)
- **原始3DGS.cpp**: [GitHub](https://github.com/graphdeco-inria/3d-gaussian-splatting)

---

## 📞 文档维护

### 责任人
- **创建**: Joe (用户)
- **维护**: Claude (AI助手)
- **审核**: Joe

### 更新频率
- **开发中**: 每个阶段完成后更新
- **稳定后**: 每周或重大变更时更新

---

**创建日期**: 2025-06-02
**文档版本**: 1.0
