#ifndef RENDERER_H
#define RENDERER_H

#define GLM_SWIZZLE

#include <atomic>
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
#include "interaction/RayCaster.h"
#include "interaction/DragHandler.h"

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
    bool renderForegroundOnly_ = false;
    SceneLoader sceneLoader_;
    std::vector<uint32_t> pendingDeformableIndices_;  // Stored indices, uploaded after pipeline creation
    std::vector<bool> sim_mask_;                      // 前景掩码（可变形区域）true=可变形, false=背景

    // MPM Physics Simulation
    std::shared_ptr<MPM::MPMManager> mpm_manager_;                     // MPM管理器
    std::vector<MPM::ParticleData> mpm_particles_;                    // MPM粒子数据
    MPM::CoordinateTransform mpm_coord_transform_;                    // 坐标变换
    std::vector<MPM::TopKMapping> mpm_top_k_mappings_;                 // Top-K映射
    std::vector<bool> mpm_freeze_mask_;                               // 粒子冻结掩码
    MPM::AABB mpm_simulation_aabb_;                                   // 仿真区域包围盒
    size_t mpm_num_drive_particles_ = 0;                              // 驱动粒子数
    size_t mpm_num_render_particles_ = 0;                             // 渲染粒子数
    bool mpm_initialized_ = false;                                    // MPM是否已初始化

    // Mouse interaction for physics
    bool physics_interaction_mode_ = false;                           // 物理交互模式
    bool is_dragging_ = false;                                        // 是否正在拖拽
    std::vector<uint32_t> selected_particles_;                        // 选中的粒子索引
    glm::vec3 drag_start_pos_;                                        // 拖拽起始位置
    glm::ivec2 mouse_pos_on_press_;                                   // 鼠标按下位置
    bool mouse_pressed_this_frame_ = false;                          // 本帧鼠标按下
    bool mouse_released_this_frame_ = false;                          // 本帧鼠标释放

    // Mouse hover cursor state
    int current_cursor_type_ = 0;                                    // 当前光标类型

    // Interaction System
    std::shared_ptr<Interaction::RayCaster> ray_caster_;              // 射线拾取器
    std::shared_ptr<Interaction::DragHandler> drag_handler_;          // 拖拽处理器
    glm::ivec2 last_mouse_position_;                                  // 上一帧鼠标位置
    bool prev_left_mouse_down_ = false;                               // 上一帧左键状态

    std::shared_ptr<DescriptorSet> inputSet;

    std::atomic<bool> running = true;

    std::vector<vk::UniqueFence> inflightFences;

    // 时间线信号量用于帧同步
    std::vector<std::unique_ptr<TimelineSemaphore>> frameTimelineSemaphores;

    // 帧管理：三缓冲环形缓冲区
    uint32_t currentFrameIndex = 0;
    uint64_t frameCounter = 0;

    std::shared_ptr<Swapchain> swapchain;

    vk::UniqueCommandPool commandPool;

    // 三缓冲：每帧一个命令缓冲区
    std::vector<vk::UniqueCommandBuffer> preprocessCommandBuffers;
    std::vector<vk::UniqueCommandBuffer> renderCommandBuffers;

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

    // 物理交互处理
    void initializeInteractionSystem();
    void handlePhysicsInteraction();
    void updatePhysicsSimulation(VkCommandBuffer cmd);
};


#endif //RENDERER_H
