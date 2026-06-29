# 3dgs.cpp.opt

**Vulkan 3D Gaussian Splatting (3DGS) renderer + real-time MPM physics interaction**

`3dgs.cpp.opt` is a derivative project built on top of [3DGS.cpp](https://github.com/shg8/3DGS.cpp), incorporating the Material Point Method (MPM) physics simulation ideas from [PhysDreamer](https://github.com/snap-research/PhysDreamer). It extends the originally static Vulkan Gaussian Splatting renderer into a **real-time, draggable, deformable** physics interaction demo.


---

## 📖 Project Positioning

### Relationship with 3DGS.cpp

[3DGS.cpp](https://github.com/shg8/3DGS.cpp) (original author shg8) is a cross-platform 3D Gaussian Splatting renderer built on Vulkan compute pipelines, targeting GPUs beyond CUDA (Vulkan is more universal than CUDA and more modern than the deprecated OpenGL). This project forks from it, **preserving its entire rendering core** (gaussian preprocessing, sorting, splat rasterization, triple-buffered pipeline) and adds a physics simulation and interaction layer on top.

- **Inherited from 3DGS.cpp**: Vulkan context wrappers, buffer/swapchain management, Compute Shader render pipelines, ImGui GUI, GLFW window, camera system.
- **Added by this project**: MPM physics module, gaussian-particle coupling module, mouse drag interaction module, PhysDreamer scene loader.

### Relationship with PhysDreamer

[PhysDreamer](https://github.com/snap-research/PhysDreamer) (Snap Research) proposes using MPM to give 3DGS scenes physical interactivity, but its official implementation is based on PyTorch + CUDA — offline simulation with no real-time interaction. The goal of this project is to **reproduce and real-time-ify PhysDreamer's core physics pipeline purely on the Vulkan/C++ side**:

- **Data format compatible**: directly reads PhysDreamer's scene PLY files (`point_cloud.ply` / `moving_part_points.ply` / `clean_object_points.ply`).
- **Algorithm reproduction**: the MPM P2G (particle-to-grid) → Grid Update → G2P (grid-to-particle) pipeline, APIC affine momentum, FCR (Fixed Corotated) elastic stress, and grid-level freeze mechanism are all re-implemented in GLSL Compute Shaders, referencing PhysDreamer's CUDA kernels.
- **Real-time-ification**: turns offline simulation into per-frame GPU stepping, and maps particle displacements back to gaussians via KNN to drive rendering, achieving interactive real-time deformation.

> ⚠️ Reference note: PhysDreamer's `gui_demo.py` / `gui_utils.py` / `run_carnation.py` are not fully verified in the official repo. This project's implementation follows PhysDreamer's core physics kernels and the `carnation.py` scene configuration.

### Location within the larger workspace

This repo usually lives as a subdirectory of a larger workspace, alongside:

```
physDreamerVulkanDemo/
├── 3dgs.cpp.opt/        ← this project (Vulkan rendering + real-time MPM interaction)
├── 3DGS.cpp/            ← original 3DGS.cpp (reference)
├── PhysDreamer/         ← PhysDreamer reference implementation (scene data source)
├── physics_dreamer/     ← PhysDreamer scene data
└── ...
```

---

## ✨ Features

### Rendering (inherited from 3DGS.cpp)
- 3D Gaussian Splatting rasterization via Vulkan Compute Shaders
- Triple-buffered render pipeline (`FRAMES_IN_FLIGHT = 3`)
- Foreground/background render toggle ("Foreground Only" mode, controlling the deformable region via a visibility mask)
- Real-time ImGui GUI, FPS display, camera save/load
- `F12` pure-render screenshot (no GUI)

### Physics simulation (added by this project)
- **MPM (Material Point Method)**: 64³ background grid, full P2G → Grid Update → G2P pipeline
- **APIC (Affine Particle In Cell)**: stores affine momentum matrix to reduce numerical dissipation
- **FCR elastic constitutive model**: Fixed Corotated stress + polar decomposition R for large deformations
- **Grid freeze mechanism**: grid nodes far from `moving_part` have their velocity zeroed (reproduces PhysDreamer's boundary constraint)
- **Gaussian-particle coupling**: KNN (K=8) + inverse-distance weights interpolate particle displacements onto foreground gaussians, with dynamic rotation covariance (cov3D) updates
- **Mouse drag interaction**: ray picking + batched influence-force injection, CFL clamping to prevent explosions

---

## 🛠️ Building

### Requirements

| Item | Requirement |
|------|-------------|
| OS | Windows 11 (also Linux / macOS compatible; physics interaction is primarily verified on Windows) |
| Compiler | MSVC v143 (Visual Studio 2022) |
| CMake | 3.26+ |
| C++ standard | C++20 |
| Vulkan SDK | 1.4.350.0 (path `C:/VulkanSDK/1.4.350.0`) |
| GPU | Any Vulkan-capable GPU (RTX 40-series recommended; must support `VK_EXT_shader_atomic_float` for MPM `atomicAdd(float)`) |

### CMake configure + build (Windows / MSVC)

```bash
# Configure (specify VS2022 + v143 toolset + Release)
cmake -G "Visual Studio 17 2022" -A x64 -T v143 ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH="C:/VulkanSDK/1.4.350.0" ^
      -DVulkan_INCLUDE_DIR="C:/VulkanSDK/1.4.350.0/Include" ^
      -DVulkan_LIBRARY="C:/VulkanSDK/1.4.350.0/Lib/vulkan-1.lib" ^
      -S ./ -B ./build

# Build
cmake --build ./build --config Release -j4
```

Executable output path:

```
build/apps/viewer/Release/3dgs_viewer.exe
```

> 💡 Dependencies (glfw, glm, imgui, spdlog, libenvpp, args.hxx, Vulkan Memory Allocator, VkRadixSort, implot) are all fetched automatically via CMake `FetchContent` — no manual install needed. Only the Vulkan SDK must be installed beforehand.
>
> 💡 Shader compilation: `shaders/*.spv` are compiled by `glslangValidator` and embedded into source via the CMake `copy_spv_to_source` target as the header `shaders.h`. After editing a `.comp`, re-run this target, otherwise stale SPVs will be loaded and the MPM will freeze.

### Linux / macOS

Dependencies: `Vulkan headers, Vulkan validation layers, glslangValidator, glfw, glm` (best installed via the [LunarG Vulkan SDK](https://www.lunarg.com/vulkan-sdk/)).

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S ./ -B ./build
cmake --build ./build -j4
```

---

## 🚀 Running

### Basic usage

```bash
# Load a scene
./build/apps/viewer/Release/3dgs_viewer.exe <scene.ply>

# Load a camera config (initial view)
./build/apps/viewer/Release/3dgs_viewer.exe --camera camera.txt <scene.ply>
```

### Command-line options

```
  ./3dgs_viewer [OPTIONS] [scene]

  OPTIONS:
      -h, --help                        Display help
      --validation                      Enable Vulkan validation layers
      -v, --verbose                     Enable verbose logging
      -d[N], --device=[N]               Select physical GPU by index
      -i, --immediate-swapchain         Immediate swapchain present mode (disable vsync)
      -w[N], --width=[N]                Window width (default 1280)
      -h[N], --height=[N]               Window height (default 720)
      --no-gui                          Disable GUI
      --camera=[path]                   Load camera file
      scene                             Scene PLY file path
```

Environment variables can also be used with the `VKGS_` prefix (e.g. `VKGS_VALIDATION_LAYERS=1`, `VKGS_PHYSICAL_DEVICE=0`).

### Controls

| Action | Function |
|--------|----------|
| `W` `A` `S` `D` | Camera movement |
| Right-click drag | Rotate view (camera control) |
| Scroll | Zoom |
| **`P` + left-click drag** | **Physics interaction**: ray-pick the foreground object and apply a drag force; MPM drives real-time deformation |
| Release left button | Momentum preserved, decays naturally via MPM damping |
| GUI "Foreground Only" button | Toggle foreground/background render mode |
| `F12` | Screenshot (pure 3D render, no GUI) |

### Recommended launch (carnations scene)

```bash
./build/apps/viewer/Release/3dgs_viewer.exe \
    --camera camera.txt \
    "<yourpath>/carnations/point_cloud.ply"
```

---

## 📂 Data Format

### Scene PLY (main render input)

Standard 3DGS PLY format: each vertex is a gaussian, carrying position (`x, y, z`), normals (`nx, ny, nz`), spherical-harmonics coefficients (`f_dc_0..2` and optional higher-order `f_rest_0..`), scales (`scale_0..2`), rotation quaternion (`rot_0..3`), and opacity (`opacity`). This is the file passed directly to the viewer.

### Physics PLY files (three required)

Physics simulation **must** additionally load the following three PLYs (SceneLoader auto-infers the other two from the directory of `point_cloud.ply`):

| File | Purpose |
|------|---------|
| `point_cloud.ply` | Full point cloud (all gaussians; main render input) |
| `moving_part_points.ply` | Movable-part point cloud (MPM particle source; defines the deformable region extent) |
| `clean_object_points.ply` | Clean-object point cloud (foreground reference; used for foreground/background segmentation) |

Path inference rule:

```
Input:  D:\...\carnations\point_cloud.ply
Auto-inferred:
  - moving_part_points.ply -> D:\...\carnations\moving_part_points.ply
  - clean_object_points.ply -> D:\...\carnations\clean_object_points.ply
```

### Foreground segmentation

`SceneLoader::ComputeSimMask` corresponds to PhysDreamer's `find_far_points()`: it matches `clean_object_points` against the full point cloud using a spatial distance threshold (default 0.01) to produce `sim_mask` (true = foreground/deformable, false = background/static). Verified on the carnations scene: 32,703 foreground gaussians / 1,037,279 total gaussians.

### Camera file (camera.txt)

A text format storing the initial camera position, rotation quaternion, and FOV. Can be saved via the GUI and loaded with `--camera`.

---

## 🏗️ Architecture

```
src/
├── 3dgs.cpp                # Rendering core aggregate
├── Renderer.{h,cpp}        # Main renderer (physics-step scheduling, drag-interaction entry)
├── GSScene.{h,cpp}         # Gaussian scene management
├── GaussianModel.{h,cpp}   # Gaussian model
├── SceneLoader.{h,cpp}     # Scene loader (PLY parsing + PhysDreamer scene inference)
├── GUIManager.{h,cpp}      # ImGui GUI
├── vulkan/                 # Vulkan wrappers
│   ├── VulkanContext.{h,cpp}
│   ├── Buffer.{h,cpp}
│   ├── Swapchain.{h,cpp}
│   └── pipelines/          # Compute pipelines
├── mpm/                    # MPM physics simulation
│   ├── MPMManager.{h,cpp}  # MPM manager (pipeline scheduling)
│   ├── MPMStructs.h        # Data structures (ParticleData/GridData, etc.)
│   └── ParticleGenerator.{h,cpp}
├── coupling/               # Gaussian-physics coupling
│   ├── GaussianParticleMapper.{h,cpp}  # KNN mapping
│   └── DisplacementMapper.{h,cpp}      # Displacement/rotation transfer
└── interaction/            # Mouse interaction
    ├── DragHandler.{h,cpp} # Drag-force injection + CFL clamping
    └── RayCaster.{h,cpp}   # Ray picking

shaders/                    # GLSL Compute Shaders (.comp -> .spv)
├── preprocess.spv, render.spv, sort.spv ...          # Rendering
├── zero_grid.spv, p2g.spv, grid_update.spv, g2p.spv  # MPM pipeline
├── grid_freeze.spv                                    # Grid freeze
├── apply_mouse_force.spv, drag_particle.spv ...      # Interaction
└── map_displacement_with_rotation.spv                # Coupling

apps/viewer/main.cpp        # Entry point (CLI parsing + renderer launch)
```

### Render pipeline
1. **Preprocess**: gaussian preprocessing, dynamic cov3D computation, sorting
2. **Render**: gaussian splat rasterization (Compute Shader)
3. **GUI**: ImGui interface

### MPM physics pipeline (per frame)
1. **ZeroGrid**: zero out grid momentum
2. **P2G**: particle-to-grid (with APIC C matrix and inline FCR stress)
3. **GridFreeze**: zero velocity of frozen-region grid nodes
4. **GridUpdate**: grid momentum update + external forces + damping
5. **G2P**: grid-to-particle (update position/velocity/deformation gradient F)
6. **Coupling**: KNN maps particle displacements back to gaussians + cov3D recompute

---

## ⚙️ Key Configuration

```cpp
// Rendering
#define FRAMES_IN_FLIGHT 3   // triple buffering

// MPM default config (MPMManager::Config)
grid_size = 64;              // 64x64x64 grid
dt = 1.0f / 30.0f;           // 30 FPS
substeps = 128;              // physics substeps
gravity = {0, -9.8, 0};      // gravity (set to 0 for the carnations scene to match PhysDreamer)
```

---

## 📊 Benchmark

| Scene | Gaussians | FPS (RTX 4090) |
|-------|-----------|----------------|
| carnations | 1,037,279 | ~105 |

---

## 📝 Related Docs

- [CHANGELOG.md](CHANGELOG.md) — detailed changelog
- [PHYSICS_INTERACTION_DETAILED_DESIGN.md](PHYSICS_INTERACTION_DETAILED_DESIGN.md) — physics interaction detailed design
- [docs/](docs/) — loading guide, quick start, render initialization plans

---

## 🔑 Key Notes

1. **Physics simulation requires all three PLY files**: `point_cloud.ply`, `moving_part_points.ply`, `clean_object_points.ply`. If any is missing, the MPM will not initialize.
2. **GPU must support `VK_EXT_shader_atomic_float`**: MPM's `atomicAdd(float)` depends on this extension. Without it, grid mass stays 0 and the simulation silently freezes.
3. **After editing a `.comp` shader, rebuild and trigger `copy_spv_to_source`**: the runtime uses the embedded `shaders.h`; stale SPVs cause MPM anomalies.
4. **carnations scene parameters**: aligned with PhysDreamer's `carnation.py` (`gravity=0`, `substeps=128`). Deviating causes flower-head scatter/explosion.

---

## License

The main project inherits from 3DGS.cpp and is licensed under LGPL. Third-party library licenses: GLM(MIT), args.hxx(MIT), spdlog(MIT), ImGui(MIT), Vulkan Memory Allocator(MIT), VkRadixSort(MIT), implot(MIT), glfw(zlib), libenvpp(Apache-2.0).
