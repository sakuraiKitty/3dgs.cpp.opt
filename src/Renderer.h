#ifndef RENDERER_H
#define RENDERER_H

#define GLM_SWIZZLE

#include <atomic>
#include <unordered_map>
#include <cmath>
#include "3dgs.h"

#include "vulkan/Window.h"
#include "GSScene.h"
#include "vulkan/pipelines/ComputePipeline.h"
#include "vulkan/Swapchain.h"
#include <glm/gtc/quaternion.hpp>

#include "GUIManager.h"
#include "vulkan/ImguiManager.h"
#include "vulkan/QueryManager.h"
#include "SceneLoader.h"
#include "mpm/MPMInitializer.h"
#include "mpm/MPMStructs.h"
#include "mpm/MPMManager.h"
#include "mpm/ScenePhysicsProfile.h"
#include "interaction/RayCaster.h"
#include "interaction/DragHandler.h"
#include "coupling/CouplingManager.h"

class Renderer {
public:
    struct alignas(16) UniformBuffer {
        glm::vec4 camera_position;
        glm::mat4 proj_mat;
        glm::mat4 view_mat;
        uint32_t width;
        uint32_t height;
        float tan_fovx;
        float tan_fovy;
        uint32_t foreground_only;  // 0 = render all, 1 = foreground only
        uint32_t _pad[3];          // pad to 16-byte alignment
    };

    struct VertexAttributeBuffer {
        glm::vec4 conic_opacity;
        glm::vec4 color_radii;
        glm::uvec4 aabb;
        glm::vec2 uv;
        float depth;
        uint32_t __padding[1];
    };

    struct Camera {
        glm::vec3 position;
        glm::quat rotation;
        float fov;
        float nearPlane;
        float farPlane;

        void translate(glm::vec3 translation) {
            position += rotation * translation;
        }
    };

    struct RadixSortPushConstants {
        uint32_t g_num_elements; // == NUM_ELEMENTS
        uint32_t g_shift; // (*)
        uint32_t g_num_workgroups; // == NUMBER_OF_WORKGROUPS as defined in the section above
        uint32_t g_num_blocks_per_workgroup; // == NUM_BLOCKS_PER_WORKGROUP
    };

    explicit Renderer(VulkanSplatting::RendererConfiguration configuration);

    void createGui();

    void initialize();

    void handleInput();

    void retrieveTimestamps();
    // 读取 physics 专用 query pool（mpm/coupling GPU 时延），与 render pool 隔离。
    // 仅在当帧 physics cmd 实际提交时调用；physics 在 preprocess fence 前完成，eWait 立即返回。
    void retrievePhysicsTimestamps();

    void recreateSwapchain();

    void draw();

    void run();

    void stop();

    void loadCamera(const std::string& cameraPath);

    void saveCamera(const std::string& cameraPath);

    void saveScreenshot(const std::string& filePath);

    /**
     * 设置可变形区域索引
     * 用于在"仅渲染前景"模式下过滤高斯
     */
    void setDeformableIndices(const std::vector<uint32_t>& indices);

    /**
     * 更新鼠标悬停检测
     * 根据鼠标位置是否在可变形区域来切换光标颜色
     */
    void updateHoverDetection();

    /**
     * 绘制物理交互可视化（绿色射线、力的方向）
     */
    void drawPhysicsOverlay();

    ~Renderer();

    Camera camera {
        .position = glm::vec3(0.0f, 0.0f, 0.0f),
        .rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        .fov = 45.0f,
        .nearPlane = 0.1f,
        .farPlane = 1000.0f
    };

private:
    VulkanSplatting::RendererConfiguration configuration;
    std::shared_ptr<Window> window;
    std::shared_ptr<VulkanContext> context;
    std::shared_ptr<ImguiManager> imguiManager;
    std::shared_ptr<GSScene> scene;
    std::shared_ptr<QueryManager> queryManager = std::make_shared<QueryManager>();
    GUIManager guiManager {};

    std::shared_ptr<ComputePipeline> preprocessPipeline;
    std::shared_ptr<ComputePipeline> renderPipeline;
    std::shared_ptr<ComputePipeline> prefixSumPipeline;
    std::shared_ptr<ComputePipeline> preprocessSortPipeline;
    std::shared_ptr<ComputePipeline> sortHistPipeline;
    std::shared_ptr<ComputePipeline> sortPipeline;
    std::shared_ptr<ComputePipeline> tileBoundaryPipeline;

    std::shared_ptr<Buffer> uniformBuffer;
    std::shared_ptr<Buffer> vertexAttributeBuffer;
    std::shared_ptr<Buffer> tileOverlapBuffer;
    std::shared_ptr<Buffer> prefixSumPingBuffer;
    std::shared_ptr<Buffer> prefixSumPongBuffer;
    std::shared_ptr<Buffer> sortKBufferEven;
    std::shared_ptr<Buffer> sortKBufferOdd;
    std::shared_ptr<Buffer> sortHistBuffer;
    std::shared_ptr<Buffer> totalSumBufferHost;
    std::shared_ptr<Buffer> tileBoundaryBuffer;
    std::shared_ptr<Buffer> sortVBufferEven;
    std::shared_ptr<Buffer> sortVBufferOdd;

    // Physics rendering filter
    std::shared_ptr<Buffer> deformableIndexBuffer_;  // Buffer containing indices of deformable Gaussians
    std::shared_ptr<Buffer> visibilityMaskBuffer_;   // Dense mask: 1 uint per Gaussian (1=deformable, 0=background)
    std::shared_ptr<Buffer> overridePositionBuffer_; // Physics position override (vec4 per Gaussian)
    std::shared_ptr<Buffer> overrideRotationBuffer_; // Physics rotation override (vec4 quaternion per Gaussian)
    bool renderForegroundOnly_ = false;
    bool physics_override_active_ = false;  // true when coupling output should override positions
    SceneLoader sceneLoader_;
    std::vector<uint32_t> pendingDeformableIndices_;  // Stored indices, uploaded after pipeline creation
    std::vector<bool> sim_mask_;                      // 前景掩码（可变形区域）true=可变形, false=背景

    // MPM Physics Simulation
    std::shared_ptr<MPM::MPMManager> mpm_manager_;                     // MPM管理器
    std::shared_ptr<CouplingManager> coupling_manager_;                // 耦合管理器
    std::vector<MPM::ParticleData> mpm_particles_;                    // MPM粒子数据
    MPM::CoordinateTransform mpm_coord_transform_;                    // 坐标变换
    std::vector<MPM::TopKMapping> mpm_top_k_mappings_;                 // Top-K映射
    std::vector<bool> mpm_freeze_mask_;                               // 粒子冻结掩码
    MPM::AABB mpm_simulation_aabb_;                                   // 仿真区域包围盒
    size_t mpm_num_drive_particles_ = 0;                              // 驱动粒子数
    size_t mpm_num_render_particles_ = 0;                             // 渲染粒子数
    bool mpm_initialized_ = false;                                    // MPM是否已初始化
    bool coupling_initialized_ = false;                               // 耦合是否已初始化

    // Mouse interaction for physics (速度插值模式)
    bool physics_interaction_mode_ = false;                           // P+click交互模式
    std::vector<uint32_t> selected_particles_;                        // 选中的粒子索引
    glm::ivec2 mouse_pos_on_press_;                                   // 鼠标按下位置
    bool mouse_pressed_this_frame_ = false;                          // 本帧鼠标按下
    bool mouse_released_this_frame_ = false;                          // 本帧鼠标释放

    // Mouse hover cursor state
    int current_cursor_type_ = 0;                                    // 当前光标类型

    // Spatial hash grid for fast hover detection (replaces O(N) loop)
    struct HoverSpatialHash {
        float cell_size = 0.05f;  // Cell size in world space (5cm)
        glm::vec3 grid_min = glm::vec3(0.0f);
        glm::vec3 grid_max = glm::vec3(0.0f);  // Scene AABB
        std::unordered_map<int64_t, std::vector<uint32_t>> cells;

        int64_t cellKey(int32_t cx, int32_t cy, int32_t cz) const {
            // Spatial hash using large primes for good distribution
            return ((int64_t)cx * 73856093) ^ ((int64_t)cy * 19349669) ^ ((int64_t)cz * 83492791);
        }

        void getCellCoords(const glm::vec3& pos, int32_t& cx, int32_t& cy, int32_t& cz) const {
            cx = static_cast<int32_t>(std::floor((pos.x - grid_min.x) / cell_size));
            cy = static_cast<int32_t>(std::floor((pos.y - grid_min.y) / cell_size));
            cz = static_cast<int32_t>(std::floor((pos.z - grid_min.z) / cell_size));
        }

        // Ray-AABB intersection test (returns t_min, t_max; -1 if no hit)
        bool rayAABB(const glm::vec3& ray_origin, const glm::vec3& ray_dir,
                     float& t_min, float& t_max) const {
            t_min = -1e30f; t_max = 1e30f;
            for (int i = 0; i < 3; i++) {
                if (std::abs(ray_dir[i]) < 1e-8f) {
                    if (ray_origin[i] < grid_min[i] || ray_origin[i] > grid_max[i])
                        return false;
                } else {
                    float t1 = (grid_min[i] - ray_origin[i]) / ray_dir[i];
                    float t2 = (grid_max[i] - ray_origin[i]) / ray_dir[i];
                    if (t1 > t2) std::swap(t1, t2);
                    t_min = std::max(t_min, t1);
                    t_max = std::min(t_max, t2);
                    if (t_min > t_max) return false;
                }
            }
            t_min = std::max(t_min, 0.0f);
            return t_min <= t_max;
        }
    };
    HoverSpatialHash hover_grid_;

    // Interaction System
    std::shared_ptr<Interaction::RayCaster> ray_caster_;              // 射线拾取器
    std::shared_ptr<Interaction::DragHandler> drag_handler_;          // 拖拽处理器
    glm::ivec2 last_mouse_position_;                                  // 上一帧鼠标位置
    bool prev_left_mouse_down_ = false;                               // 上一帧左键状态

    
    std::shared_ptr<DescriptorSet> inputSet;

    std::atomic<bool> running = true;

    std::vector<vk::UniqueFence> inflightFences;
    vk::UniqueFence preprocessFence;  // Dedicated fence for preprocess completion (within-frame sync)
    vk::UniqueFence physicsFence;     // Dedicated fence for physics GPU commands

    // 帧同步：per-frame binary 信号量
    //   acquireSemaphores: acquireNextImageKHR signal → render submit wait（image 可用）
    //   renderSemaphores:  render submit signal → presentKHR wait（render 完成）
    // 修复：原用 timeline semaphore 当 present 等待信号量非法（VUID-03267），
    // 且 TimelineSemaphore::signal() 从不调用 vkSignalSemaphoreKHR → 值不递增 → present 退化忙等/全停。
    // binary 信号量是 WSI present 的标准同步原语。
    std::vector<vk::UniqueSemaphore> acquireSemaphores;
    std::vector<vk::UniqueSemaphore> renderSemaphores;

    // 帧管理：三缓冲环形缓冲区
    uint32_t currentFrameIndex = 0;
    uint64_t frameCounter = 0;

    std::shared_ptr<Swapchain> swapchain;

    vk::UniqueCommandPool commandPool;

    // 三缓冲：每帧一个命令缓冲区
    std::vector<vk::UniqueCommandBuffer> preprocessCommandBuffers;
    std::vector<vk::UniqueCommandBuffer> renderCommandBuffers;
    std::vector<vk::UniqueCommandBuffer> physicsCommandBuffers;  // 物理GPU命令（耦合、位移映射）

    // 当帧 physics cmd 是否已提交（draw 守卫通过时置 true）。
    // retrievePhysicsTimestamps 据此跳过未提交帧，避免读到永远不可用的 query → eWait 死等。
    bool physicsSubmittedThisFrame_ = false;

    uint32_t currentImageIndex;

#ifdef __APPLE__
    uint32_t numRadixSortBlocksPerWorkgroup = 256;
#else
    uint32_t numRadixSortBlocksPerWorkgroup = 32;
#endif

    int fpsCounter = 0;
    std::chrono::high_resolution_clock::time_point lastFpsTime = std::chrono::high_resolution_clock::now();

    unsigned int sortBufferSizeMultiplier = 1;

    bool screenshotRequested = false;
    bool screenshotSaving = false;  // 正在保存截图，阻塞新的请求
    int screenshotCounter = 0;

    VkBuffer screenshotStagingBuffer = VK_NULL_HANDLE;
    VmaAllocation screenshotStagingAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo screenshotStagingAllocInfo{};

    void initializeVulkan();

    void loadSceneToGPU();

    void createPreprocessPipeline();

    void createPrefixSumPipeline();

    void createRadixSortPipeline();

    void createPreprocessSortPipeline();

    void createTileBoundaryPipeline();

    void createRenderPipeline();

    void recordPreprocessCommandBuffer();

    bool recordRenderCommandBuffer(uint32_t currentFrame);

    void createCommandPool();

    // 帧管理方法
    void advanceFrame();
    uint64_t getExpectedFrameValue() const;

    void updateUniforms();

    void uploadVisibilityMask();
    void buildHoverGrid();

    // 物理交互处理
    void initializeInteractionSystem();
    void handlePhysicsInteraction();
    void updatePhysicsSimulation(VkCommandBuffer cmd);
};


#endif //RENDERER_H
