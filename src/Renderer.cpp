#include "Renderer.h"
#include "imgui.h"

#include <fstream>

#include "vulkan/Swapchain.h"

#include <memory>
#include "shaders.h"
#include <utility>
#include <cmath>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "vulkan/Utils.h"
#include "vulkan/windowing/GLFWWindow.h"
#include "GaussianModel.h"

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

void Renderer::initialize() {
    initializeVulkan();
    createGui();

    loadSceneToGPU();
    createPreprocessPipeline();
    createPrefixSumPipeline();
    createRadixSortPipeline();
    createPreprocessSortPipeline();
    createTileBoundaryPipeline();
    createRenderPipeline();
    createCommandPool();
    recordPreprocessCommandBuffer();

    // Build spatial hash grid for fast hover detection
    buildHoverGrid();

    // 初始化交互系统（如果MPM已初始化）
    if (mpm_initialized_) {
        initializeInteractionSystem();
    }
}

void Renderer::handleInput() {
    auto translation = window->getCursorTranslation();
    auto keys = window->getKeys(); // W, A, S, D
    auto mouse_buttons = window->getMouseButton(); // [left, middle, right]

    // 右键拖拽 → 旋转镜头（FPS 风格 look）
    // 仅在 GUI 未占用鼠标、且未处于物理交互(P+左键)时生效，避免冲突
    bool gui_wants_mouse = configuration.enableGui && guiManager.wantCaptureMouse();
    if (mouse_buttons[2] && !gui_wants_mouse && !physics_interaction_mode_) {
        const float sensitivity = 0.003f; // rad/px
        float yaw = -static_cast<float>(translation[0]) * sensitivity;
        float pitch = -static_cast<float>(translation[1]) * sensitivity;

        // yaw 绕世界 Y 轴：保持竖直向上，不引入 roll
        camera.rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * camera.rotation;

        // pitch 绕相机本地 X 轴（右向量），钳制俯仰避免在 ±90° 翻转
        glm::vec3 right = glm::normalize(camera.rotation * glm::vec3(1.0f, 0.0f, 0.0f));
        glm::quat candidate = glm::angleAxis(pitch, right) * camera.rotation;
        glm::vec3 new_forward = candidate * glm::vec3(0.0f, 0.0f, -1.0f);
        if (std::abs(glm::dot(new_forward, glm::vec3(0.0f, 1.0f, 0.0f))) < 0.99f) {
            camera.rotation = candidate;
        }
    }

    // 滚轮 → 沿相机前向 dolly（缩放/推拉），GUI 占用鼠标时让出给 ImGui
    auto scroll = window->getScrollOffset();
    if (!gui_wants_mouse && scroll[1] != 0.0) {
        const float zoomSpeed = 0.5f; // 每个滚轮刻度前进的单位数
        glm::vec3 forward = camera.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        camera.position += forward * (static_cast<float>(scroll[1]) * zoomSpeed);
    }

    // move camera (键盘控制：W/S 上下, A/D 左右, SPACE/SHIFT 上下, Q/E 旋转)
    if (!configuration.enableGui || !guiManager.wantCaptureKeyboard()) {
        const float moveSpeed = 0.1f;        // 移动速度（世界单位/帧），原 0.3 → 0.1 降灵敏度
        const float rollSpeed = 0.02f;       // roll 速度（rad/帧）
        glm::vec3 direction = glm::vec3(0.0f, 0.0f, 0.0f);
        if (keys[0]) {                       // W → 上
            direction += glm::vec3(0.0f, 1.0f, 0.0f);
        }
        if (keys[1]) {                       // A → 左
            direction += glm::vec3(-1.0f, 0.0f, 0.0f);
        }
        if (keys[2]) {                       // S → 下
            direction += glm::vec3(0.0f, -1.0f, 0.0f);
        }
        if (keys[3]) {                       // D → 右
            direction += glm::vec3(1.0f, 0.0f, 0.0f);
        }
        if (keys[4]) {                       // SPACE → 上（保留）
            direction += glm::vec3(0.0f, 1.0f, 0.0f);
        }
        if (keys[5]) {                       // LEFT_SHIFT → 下（保留）
            direction += glm::vec3(0.0f, -1.0f, 0.0f);
        }
        if (keys[6]) {
            window->mouseCapture(false);
            guiManager.mouseCapture = false;
        }
        if (keys[7]) { // F12 key - screenshot
            // 只有在没有正在保存的截图时才允许新的截图请求
            if (!screenshotSaving) {
                screenshotRequested = true;
            }
        }
        if (direction != glm::vec3(0.0f, 0.0f, 0.0f)) {
            direction = glm::normalize(direction);
            camera.position += (glm::mat4_cast(camera.rotation) * glm::vec4(direction, 1.0f)).xyz() * moveSpeed;
        }

        // Q/E → 绕相机前向轴 roll（Q 逆时针, E 顺时针）
        if (keys[9]) {                       // Q
            camera.rotation = camera.rotation * glm::angleAxis(rollSpeed, glm::vec3(0.0f, 0.0f, 1.0f));
        }
        if (keys[10]) {                      // E
            camera.rotation = camera.rotation * glm::angleAxis(-rollSpeed, glm::vec3(0.0f, 0.0f, 1.0f));
        }
    }
}

void Renderer::retrieveTimestamps() {
    std::vector<uint64_t> timestamps(queryManager->nextId);
    auto res = context->device->getQueryPoolResults(context->queryPool.get(), 0, queryManager->nextId,
                                                    timestamps.size() * sizeof(uint64_t),
                                                    timestamps.data(), sizeof(uint64_t),
                                                    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    if (res != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to retrieve timestamps");
    }

    auto metrics = queryManager->parseResults(timestamps);
    for (auto& metric: metrics) {
        if (configuration.enableGui)
            guiManager.pushMetric(metric.first, metric.second / 1000000.0);
    }
}

void Renderer::retrievePhysicsTimestamps() {
    // 仅在当帧 physics cmd 实际提交时读取（避免读到不可用 query）。
    // 用非阻塞轮询（无 eWait）：physics cmd 无 fence 提交，eWait 会触发驱动重量级 device 同步
    // → 每帧一次掉 ~2 FPS。改为 e64 only，拿不到(eNotReady)就跳过本帧，指标滞后一帧可接受。
    if (!physicsSubmittedThisFrame_) {
        return;
    }
    std::vector<uint64_t> timestamps(4);
    auto res = context->device->getQueryPoolResults(context->physicsQueryPool.get(), 0, 4,
                                                    timestamps.size() * sizeof(uint64_t),
                                                    timestamps.data(), sizeof(uint64_t),
                                                    vk::QueryResultFlagBits::e64);
    if (res != vk::Result::eSuccess) {
        // eNotReady 或其他：physics cmd 尚未完成写入，跳过本帧，不阻塞
        return;
    }
    // mpm_start=0, mpm_end=1, coupling_start=2, coupling_end=3
    // ticks → ms（假设 timestampPeriod=1ns，与 render 指标同约定；NVIDIA 驱动典型值）
    const float mpm_ms = static_cast<float>(timestamps[1] - timestamps[0]) / 1000000.0f;
    const float coupling_ms = static_cast<float>(timestamps[3] - timestamps[2]) / 1000000.0f;
    if (configuration.enableGui) {
        guiManager.pushMetric("mpm", mpm_ms);
        guiManager.pushMetric("coupling", coupling_ms);
    }
}

void Renderer::recreateSwapchain() {
    auto oldExtent = swapchain->swapchainExtent;
    spdlog::debug("Recreating swapchain");
    swapchain->recreate();
    // 重置图像索引，防止使用无效的旧索引
    currentImageIndex = UINT32_MAX;
    if (swapchain->swapchainExtent == oldExtent) {
        return;
    }

    auto [width, height] = swapchain->swapchainExtent;
    auto tileX = (width + 16 - 1) / 16;
    auto tileY = (height + 16 - 1) / 16;
    tileBoundaryBuffer->realloc(tileX * tileY * sizeof(uint32_t) * 2);

    recordPreprocessCommandBuffer();
    createRenderPipeline();
}

void Renderer::initializeVulkan() {
    spdlog::debug("Initializing Vulkan");
    window = configuration.window;
    context = std::make_shared<VulkanContext>(window->getRequiredInstanceExtensions(), std::vector<std::string>{},
                                              configuration.enableVulkanValidationLayers);

    context->createInstance();
    auto surface = static_cast<vk::SurfaceKHR>(window->createSurface(context));
    context->selectPhysicalDevice(configuration.physicalDeviceId, surface);

    vk::PhysicalDeviceFeatures pdf{};
    vk::PhysicalDeviceVulkan11Features pdf11{};
    vk::PhysicalDeviceVulkan12Features pdf12{};
    pdf.shaderStorageImageWriteWithoutFormat = true;
    pdf.shaderInt64 = true;
    // pdf.robustBufferAccess = true;
    // pdf12.shaderFloat16 = true;]
#ifndef __APPLE__
    pdf12.shaderBufferInt64Atomics = true;
    pdf12.shaderSharedInt64Atomics = true;
#endif
    // 启用时间线信号量支持
    pdf12.timelineSemaphore = true;

    // 注意: VK_EXT_shader_atomic_float 扩展和 shaderBufferFloat32AtomicAdd 特性
    // 在 VulkanContext::createLogicalDevice 中启用（pNext 链末尾）
    // ROOT CAUSE: MPM P2G 的 atomicAdd(float) 需要 shaderBufferFloat32AtomicAdd,
    //             未启用时 atomicAdd 静默失败 → grid mass=0 → 仿真冻结!

    context->createLogicalDevice(pdf, pdf11, pdf12);
    context->createDescriptorPool(FRAMES_IN_FLIGHT);  // 更新为3帧

    swapchain = std::make_shared<Swapchain>(context, window, configuration.immediateSwapchain);

    // 创建帧inflight fences (for cross-frame render sync)
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        inflightFences.emplace_back(
            context->device->createFenceUnique(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)));
    }

    // 创建专用preprocess fence (for within-frame preprocess sync)
    // 必须初始为 UNSIGNaled，因为draw()总是在等待fence之前提交preprocess命令
    // 如果初始signaled，waitForFences会在GPU实际完成前就返回 → 读到垃圾数据 → 崩溃
    preprocessFence = context->device->createFenceUnique(vk::FenceCreateInfo());

    // 创建专用physics fence (for within-frame physics GPU sync)
    // 同样初始unsignaled - physics命令用null fence提交，同一队列保序
    physicsFence = context->device->createFenceUnique(vk::FenceCreateInfo());

    // 创建 per-frame binary 信号量（acquire / render-complete）
    acquireSemaphores.reserve(FRAMES_IN_FLIGHT);
    renderSemaphores.reserve(FRAMES_IN_FLIGHT);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        acquireSemaphores.emplace_back(context->device->createSemaphoreUnique(vk::SemaphoreCreateInfo{}));
        renderSemaphores.emplace_back(context->device->createSemaphoreUnique(vk::SemaphoreCreateInfo{}));
    }
}

void Renderer::loadSceneToGPU() {
    spdlog::info("[Renderer] ===== Gaussian Model Initialization =====");

    // Step 1: Create SceneDescriptor and validate all required files
    auto descriptor = sceneLoader_.CreateDescriptor(configuration.scene);
    if (!sceneLoader_.ValidateRequiredFiles(descriptor)) {
        spdlog::critical("[Renderer] Required PLY files missing. Cannot initialize physics simulation.");
        spdlog::critical("[Renderer] Please ensure all three PLY files exist in the scene directory.");
        // Continue with basic rendering (will render all gaussians)
        spdlog::warn("[Renderer] Falling back to basic rendering (all gaussians visible)");
    } else {
        spdlog::info("[Renderer] ✓ All required PLY files validated");

        // Step 2: Load complete GaussianModel with all 3DGS attributes
        GaussianModel gaussianModel;
        if (!gaussianModel.LoadPLY(descriptor.point_cloud_ply)) {
            spdlog::error("[Renderer] Failed to load Gaussian model from: {}", descriptor.point_cloud_ply);
        } else {
            spdlog::info("[Renderer] ✓ Gaussian model loaded: {} gaussians", gaussianModel.GetCount());

            // Step 3: Load reference point clouds
            if (!sceneLoader_.LoadScene(descriptor)) {
                spdlog::error("[Renderer] Failed to load reference point clouds");
            } else {
                spdlog::info("[Renderer] ✓ Reference point clouds loaded");

                // Step 4: Compute foreground simulation mask
                std::vector<bool> sim_mask = SceneLoader::ComputeSimMask(
                    gaussianModel.xyz_,
                    sceneLoader_.GetCleanObjectPoints().positions,
                    0.01f // Threshold consistent with Python
                );
                gaussianModel.sim_mask_ = sim_mask;
                sim_mask_ = sim_mask;  // 保存用于鼠标悬停检测

                size_t foreground_count = std::count(sim_mask.begin(), sim_mask.end(), true);
                spdlog::info("[Renderer] ✓ Simulation mask computed: {} foreground / {} total",
                             foreground_count, sim_mask.size());

                // Step 5: Store deformable indices for rendering
                pendingDeformableIndices_ = gaussianModel.GetForegroundIndices();
                spdlog::info("[Renderer] ✓ Deformable indices stored: {} indices",
                             pendingDeformableIndices_.size());

                // Step 5.5: Initialize MPM Physics Simulation
                spdlog::info("[Renderer] ===== Initializing MPM Physics Simulation =====");
                if (sceneLoader_.GetMovingPartPoints().IsValid()) {
                    // 按场景名加载物理参数 preset（carnation/hat/alocasia/telephone）
                    // 取自 PhysDreamer configs/<scene>.py 的 simulate_cfg，避免硬编码 carnation 值套到 hat 上
                    const auto profile = MPM::GetScenePhysicsProfile(descriptor.scene_path);
                    spdlog::info("[Renderer] Scene physics profile: '{}' (E={}, nu={}, downsample={}, substeps={})",
                                 profile.scene_name, profile.E, profile.nu, profile.downsample_scale, profile.substeps);

                    MPM::MPMInitializer::Config mpm_config;
                    mpm_config.grid_size = profile.grid_size;
                    mpm_config.downsample_scale = profile.downsample_scale;
                    mpm_config.use_internal_fill = true;
                    mpm_config.material.E = profile.E;
                    mpm_config.material.nu = profile.nu;
                    mpm_config.material.density = profile.density;

                    auto mpm_result = MPM::MPMInitializer::Initialize(
                        descriptor,
                        mpm_config,
                        sim_mask
                    );

                    // 保存MPM初始化结果
                    mpm_particles_ = std::move(mpm_result.particles);
                    mpm_coord_transform_ = mpm_result.coord_transform;
                    mpm_top_k_mappings_ = std::move(mpm_result.top_k_mappings);
                    mpm_freeze_mask_ = std::move(mpm_result.freeze_mask);
                    mpm_simulation_aabb_ = mpm_result.simulation_aabb;
                    mpm_num_drive_particles_ = mpm_result.num_drive_particles;
                    mpm_num_render_particles_ = mpm_result.num_render_particles;
                    mpm_initialized_ = !mpm_particles_.empty();

                    if (mpm_initialized_) {
                        spdlog::info("[Renderer] ✓ MPM initialized successfully");
                        spdlog::info("[Renderer]   - Drive particles: {}", mpm_num_drive_particles_);
                        spdlog::info("[Renderer]   - Render particles: {}", mpm_num_render_particles_);
                        spdlog::info("[Renderer]   - Active particles: {}", mpm_result.stats.active_count);
                        spdlog::info("[Renderer]   - Frozen particles: {}", mpm_result.stats.frozen_count);

                        // 创建并初始化 MPMManager
                        mpm_manager_ = std::make_shared<MPM::MPMManager>(context);

                        // 配置MPM参数（沿用上方按场景名加载的 profile）
                        MPM::MPMManager::Config mpm_config;
                        mpm_config.grid_size = profile.grid_size;
                        mpm_config.grid_spacing = 1.0f / static_cast<float>(profile.grid_size);
                        mpm_config.inv_dx = static_cast<float>(profile.grid_size);
                        mpm_config.dt = 1.0f / 30.0f;
                        mpm_config.substeps = configuration.substeps;   // 命令行 --substeps 覆盖（默认 256 = PhysDreamer 最小稳定值）
                                                                          // 原按场景 profile.substeps (carnation128/hat64/alocasia128/telephone64)
                                                                          // 实时折中=离线/6；CFL: sub_dt=(1/30)/substeps < dx/c_p
                        mpm_config.damping = 1.0f;      // 初始值；运行时由 P1 释放阻尼覆盖（见 handlePhysicsInteraction Step 前）
                                                      // 拖拽中=1.0(无阻尼纯跟随)，非拖拽=0.95^(1/substeps)/子步(衰减振荡)。
                                                      // 对标 PhysDreamer gui_demo.py:156,288 release_damping=0.95/帧。
                                                      // 历史根因：damping 是【每子步】乘一次，0.9999^128=0.681/s 过阻尼→爬行无振荡；
                                                      // 现配合 P0 小半径局部变形(真实弹性恢复力)后，释放阻尼让振荡衰减归位。
                        // PhysDreamer 四场景 simulate_cfg 均无 gravity 字段——花/帽/电话由冻结边界支撑处于静止平衡，
                        // 变形只来自交互力。之前-2是调试值，驱动冻结边界应力反馈爆炸→粒子甩飞→散点
                        mpm_config.gravity = profile.gravity;

                        mpm_manager_->Initialize(mpm_config);
                        mpm_manager_->LoadParticles(mpm_particles_);
                        mpm_manager_->Enable();  // 启用物理仿真

                        // 设置可变形区域（将前景高斯索引传入MPMManager）
                        MPM::DeformableRegion region;
                        region.deformable_indices = pendingDeformableIndices_;
                        for (uint32_t idx : pendingDeformableIndices_) {
                            if (idx < gaussianModel.xyz_.size()) {
                                region.deformable_original_pos.push_back(gaussianModel.xyz_[idx]);
                            }
                        }
                        // 设置静态区域
                        for (size_t i = 0; i < sim_mask.size(); i++) {
                            if (!sim_mask[i]) {
                                region.static_indices.push_back(static_cast<uint32_t>(i));
                                if (i < gaussianModel.xyz_.size()) {
                                    region.static_original_pos.push_back(gaussianModel.xyz_[i]);
                                }
                            }
                        }
                        mpm_manager_->SetRegion(region);
                        spdlog::info("[Renderer] ✓ Deformable region set: {} deformable, {} static",
                                     region.GetDeformableCount(), region.GetStaticCount());

                        spdlog::info("[Renderer] MPMManager created, initialized and enabled");
                    } else {
                        spdlog::warn("[Renderer] MPM initialization failed, physics disabled");
                    }
                } else {
                    spdlog::warn("[Renderer] No moving_part_points.ply, MPM disabled");
                    mpm_initialized_ = false;
                }
                spdlog::info("[Renderer] ===== MPM Initialization Complete =====");
            }
        }
    }

    // Step 6: Continue with existing GSScene loading flow (compatibility)
    spdlog::info("[Renderer] Loading scene to GPU for rendering");
    scene = std::make_shared<GSScene>(configuration.scene);
    scene->load(context);

    // Step 6.5: Initialize CouplingManager (after scene and MPM are ready)
    if (mpm_initialized_ && mpm_manager_ && scene) {
        spdlog::info("[Renderer] ===== Initializing CouplingManager =====");
        coupling_manager_ = std::make_shared<CouplingManager>(context);

        CouplingManager::Config coupling_config;
        coupling_config.enable_coupling = true;
        coupling_config.displacement_scale = 1.0f;
        coupling_config.use_rigid_transform = true;

        // CRITICAL: SetCoordTransform BEFORE Initialize, so PrecomputeTopKMapping
        // uses correct coordinate transformation (not default scale=1, shift=0)
        coupling_manager_->SetCoordTransform(mpm_coord_transform_.scale, mpm_coord_transform_.shift);
        coupling_manager_->Initialize(scene, mpm_manager_, coupling_config);
        coupling_initialized_ = true;
        spdlog::info("[Renderer] ✓ CouplingManager initialized");
    }

    // Step 7: Create visibility mask buffer
    // This is done in createPreprocessPipeline(), but we log here for clarity
    spdlog::info("[Renderer] ===== Gaussian Model Initialization Complete =====");

    // NOTE: 不能在这里 resetDescriptorPool —— MPM(Step 5.5) 和 CouplingManager(Step 6.5)
    // 的 persistent descriptor set 都从 context->descriptorPool 分配，reset 会把它们全废掉，
    // 导致 vkCmdBindDescriptorSets 报 "Couldn't find VkDescriptorSet"、compute dispatch 写不到
    // grid_buffer → MPM 冻结。pool 用 eFreeDescriptorSet + UniqueDescriptorSet 已能自动回收。
}

void Renderer::createPreprocessPipeline() {
    spdlog::debug("Creating preprocess pipeline");
    uniformBuffer = Buffer::uniform(context, sizeof(UniformBuffer));
    vertexAttributeBuffer = Buffer::storage(context, scene->getNumVertices() * sizeof(VertexAttributeBuffer), false);
    tileOverlapBuffer = Buffer::storage(context, scene->getNumVertices() * sizeof(uint32_t), false);

    // Visibility mask: 1 uint per Gaussian, 1=deformable, 0=background
    visibilityMaskBuffer_ = Buffer::storage(context, scene->getNumVertices() * sizeof(uint32_t), false);

    preprocessPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "preprocess", SPV_PREPROCESS, SPV_PREPROCESS_len));
    inputSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    inputSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        scene->vertexBuffer);
    inputSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        scene->cov3DBuffer);
    // Override positions buffer (binding 2): initially filled with original positions,
    // updated by coupling system when physics is active
    overridePositionBuffer_ = Buffer::storage(context, scene->getNumVertices() * sizeof(glm::vec4), false);
    // Fill with original positions as default (vec4 with w=1.0)
    {
        std::vector<glm::vec4> default_positions(scene->getNumVertices());
        for (size_t i = 0; i < scene->getNumVertices() && i < scene->cpuPositions.size(); i++) {
            default_positions[i] = glm::vec4(scene->cpuPositions[i], 1.0f);
        }
        overridePositionBuffer_->upload(reinterpret_cast<const char*>(default_positions.data()),
                                         static_cast<uint32_t>(default_positions.size() * sizeof(glm::vec4)), 0);
    }
    inputSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        overridePositionBuffer_);
    // Override rotations buffer (binding 3): initially filled with PLY original rotations,
    // updated by coupling system when physics is active
    overrideRotationBuffer_ = Buffer::storage(context, scene->getNumVertices() * sizeof(glm::vec4), false);
    // Fill with PLY original rotations (w,x,y,z quaternion format)
    {
        std::vector<glm::vec4> default_rotations(scene->getNumVertices());
        for (size_t i = 0; i < scene->getNumVertices() && i < scene->cpuRotations.size(); i++) {
            default_rotations[i] = scene->cpuRotations[i];
        }
        for (size_t i = scene->cpuRotations.size(); i < scene->getNumVertices(); i++) {
            default_rotations[i] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);  // fallback: identity quaternion
        }
        overrideRotationBuffer_->upload(reinterpret_cast<const char*>(default_rotations.data()),
                                         static_cast<uint32_t>(default_rotations.size() * sizeof(glm::vec4)), 0);
    }
    inputSet->bindBufferToDescriptorSet(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        overrideRotationBuffer_);
    inputSet->build();
    preprocessPipeline->addDescriptorSet(0, inputSet);

    // Add push constant for physics override flag
    vk::PushConstantRange preprocessPushConstant(
        vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));
    preprocessPipeline->addPushConstant(
        preprocessPushConstant.stageFlags, preprocessPushConstant.offset, preprocessPushConstant.size);

    auto uniformOutputSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    uniformOutputSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eUniformBuffer,
                                                vk::ShaderStageFlagBits::eCompute,
                                                uniformBuffer);
    uniformOutputSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer,
                                                vk::ShaderStageFlagBits::eCompute,
                                                vertexAttributeBuffer);
    uniformOutputSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer,
                                                vk::ShaderStageFlagBits::eCompute,
                                                tileOverlapBuffer);
    // Visibility mask at binding 3
    uniformOutputSet->bindBufferToDescriptorSet(3, vk::DescriptorType::eStorageBuffer,
                                                vk::ShaderStageFlagBits::eCompute,
                                                visibilityMaskBuffer_);
    // Push constant for foreground_only flag at binding 4 (uniform)
    // We use a small uniform buffer for the flag
    uniformOutputSet->build();

    preprocessPipeline->addDescriptorSet(1, uniformOutputSet);
    preprocessPipeline->build();

    // Upload pending visibility mask now that buffer is created
    uploadVisibilityMask();
}

Renderer::Renderer(VulkanSplatting::RendererConfiguration configuration) : configuration(std::move(configuration)) {
}

void Renderer::createGui() {
    if (!configuration.enableGui) {
        return;
    }

    spdlog::debug("Creating GUI");

    imguiManager = std::make_shared<ImguiManager>(context, swapchain, window);
    imguiManager->init();
    guiManager.init();

    // 在 ImGui 初始化之后安装光标位置回调
    // ImGui_ImplGlfw_InitForVulkan(install_callbacks=true) 会替换 GLFW 回调
    // 我们的回调覆盖在 ImGui 之上，链式转发给 ImGui，确保光标位置始终更新
    auto glfwWindow = std::reinterpret_pointer_cast<GLFWWindow>(window);
    glfwWindow->installCursorCallback();
}

void Renderer::createPrefixSumPipeline() {
    spdlog::debug("Creating prefix sum pipeline");
    prefixSumPingBuffer = Buffer::storage(context, scene->getNumVertices() * sizeof(uint32_t), false);
    prefixSumPongBuffer = Buffer::storage(context, scene->getNumVertices() * sizeof(uint32_t), false);
    totalSumBufferHost = Buffer::staging(context, sizeof(uint32_t));

    prefixSumPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "prefix_sum", SPV_PREFIX_SUM, SPV_PREFIX_SUM_len));
    auto descriptorSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             prefixSumPingBuffer);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             prefixSumPongBuffer);
    descriptorSet->build();

    prefixSumPipeline->addDescriptorSet(0, descriptorSet);
    prefixSumPipeline->addPushConstant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));
    prefixSumPipeline->build();
}

void Renderer::createRadixSortPipeline() {
    spdlog::debug("Creating radix sort pipeline");
    sortKBufferEven = Buffer::storage(context, scene->getNumVertices() * sizeof(uint64_t) * sortBufferSizeMultiplier,
                                      false, 0, "sortKBufferEven");
    sortKBufferOdd = Buffer::storage(context, scene->getNumVertices() * sizeof(uint64_t) * sortBufferSizeMultiplier,
                                     false, 0, "sortKBufferOdd");
    sortVBufferEven = Buffer::storage(context, scene->getNumVertices() * sizeof(uint32_t) * sortBufferSizeMultiplier,
                                      false, 0, "sortVBufferEven");
    sortVBufferOdd = Buffer::storage(context, scene->getNumVertices() * sizeof(uint32_t) * sortBufferSizeMultiplier,
                                     false, 0, "sortVBufferOdd");

    uint32_t globalInvocationSize = scene->getNumVertices() * sortBufferSizeMultiplier / numRadixSortBlocksPerWorkgroup;
    uint32_t remainder = scene->getNumVertices() * sortBufferSizeMultiplier % numRadixSortBlocksPerWorkgroup;
    globalInvocationSize += remainder > 0 ? 1 : 0;

    auto numWorkgroups = (globalInvocationSize + 256 - 1) / 256;

    sortHistBuffer = Buffer::storage(context, numWorkgroups * 256 * sizeof(uint32_t), false);

    sortHistPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "hist", SPV_HIST, SPV_HIST_len));
    sortPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "sort", SPV_SORT, SPV_SORT_len));

    auto descriptorSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferEven);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferOdd);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortHistBuffer);
    descriptorSet->build();
    sortHistPipeline->addDescriptorSet(0, descriptorSet);
    sortHistPipeline->addPushConstant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(RadixSortPushConstants));
    sortHistPipeline->build();

    descriptorSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferEven);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferOdd);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferOdd);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferEven);
    descriptorSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortVBufferEven);
    descriptorSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortVBufferOdd);
    descriptorSet->bindBufferToDescriptorSet(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortVBufferOdd);
    descriptorSet->bindBufferToDescriptorSet(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortVBufferEven);
    descriptorSet->bindBufferToDescriptorSet(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortHistBuffer);
    descriptorSet->build();
    sortPipeline->addDescriptorSet(0, descriptorSet);
    sortPipeline->addPushConstant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(RadixSortPushConstants));
    sortPipeline->build();
}

void Renderer::createPreprocessSortPipeline() {
    spdlog::debug("Creating preprocess sort pipeline");
    preprocessSortPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "preprocess_sort", SPV_PREPROCESS_SORT, SPV_PREPROCESS_SORT_len));
    auto descriptorSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             vertexAttributeBuffer);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             prefixSumPingBuffer);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             prefixSumPongBuffer);
    descriptorSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferEven);
    descriptorSet->bindBufferToDescriptorSet(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortVBufferEven);
    descriptorSet->build();

    preprocessSortPipeline->addDescriptorSet(0, descriptorSet);
    preprocessSortPipeline->addPushConstant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));
    preprocessSortPipeline->build();
}

void Renderer::createTileBoundaryPipeline() {
    spdlog::debug("Creating tile boundary pipeline");
    auto [width, height] = swapchain->swapchainExtent;
    auto tileX = (width + 16 - 1) / 16;
    auto tileY = (height + 16 - 1) / 16;
    tileBoundaryBuffer = Buffer::storage(context, tileX * tileY * sizeof(uint32_t) * 2, false);

    tileBoundaryPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "tile_boundary", SPV_TILE_BOUNDARY, SPV_TILE_BOUNDARY_len));
    auto descriptorSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             sortKBufferEven);
    // descriptorSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
    //                                          sortKBufferOdd);
    descriptorSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                             tileBoundaryBuffer);
    descriptorSet->build();

    tileBoundaryPipeline->addDescriptorSet(0, descriptorSet);
    tileBoundaryPipeline->addPushConstant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));
    tileBoundaryPipeline->build();
}

void Renderer::createRenderPipeline() {
    spdlog::debug("Creating render pipeline");
    renderPipeline = std::make_shared<ComputePipeline>(
        context, std::make_shared<Shader>(context, "render", SPV_RENDER, SPV_RENDER_len));
    auto inputSet = std::make_shared<DescriptorSet>(context, FRAMES_IN_FLIGHT);
    inputSet->bindBufferToDescriptorSet(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        vertexAttributeBuffer);
    inputSet->bindBufferToDescriptorSet(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        tileBoundaryBuffer);
    inputSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                        sortVBufferEven);
    // Deformable index buffer for filtering (optional, can be empty for rendering all)
    if (deformableIndexBuffer_) {
        inputSet->bindBufferToDescriptorSet(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
                                            deformableIndexBuffer_);
    }
    // inputSet->bindBufferToDescriptorSet(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
    //                                     sortKBufferOdd);
    inputSet->build();

    auto outputSet = std::make_shared<DescriptorSet>(context, 1);
    for (auto& image: swapchain->swapchainImages) {
        outputSet->bindImageToDescriptorSet(0, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute,
                                            image);
    }
    outputSet->build();
    renderPipeline->addDescriptorSet(0, inputSet);
    renderPipeline->addDescriptorSet(1, outputSet);
    renderPipeline->addPushConstant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t) * 2);
    renderPipeline->build();
}

void Renderer::draw() {
    const uint32_t frameIdx = currentFrameIndex;

    // 1. 等待该索引处的上一帧完成（CPU-GPU跨帧同步）
    // inflightFences[frameIdx] 仅用于渲染提交的跨帧同步
    auto ret = context->device->waitForFences(inflightFences[frameIdx].get(), VK_TRUE, UINT64_MAX);
    if (ret != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for fence");
    }
    context->device->resetFences(inflightFences[frameIdx].get());

    // 2. 获取下一个交换链图像
    // 关键修复：用 binary acquireSemaphore 而非 null，acquire 非阻塞（CPU 不再同步等 image）。
    // 原来的 null semaphore + null fence（VUID-01780）迫使驱动同步阻塞 CPU 直到 image 可用
    // → 三缓冲流水线退化成 1 帧 → FPS 远低于 GPU 时间戳反推值。
    auto res = context->device->acquireNextImageKHR(swapchain->swapchain.get(), UINT64_MAX,
                                                    acquireSemaphores[frameIdx].get(), vk::Fence(),
                                                    &currentImageIndex);
    if (res == vk::Result::eErrorOutOfDateKHR) {
        recreateSwapchain();
        return;
    } else if (res != vk::Result::eSuccess && res != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    handleInput();
    updateHoverDetection();
    handlePhysicsInteraction();
    updateUniforms();

    // Sync render mode from GUI
    renderForegroundOnly_ = guiManager.renderBackgroundOnly;

    // 3a. Execute physics GPU commands BEFORE preprocess (if MPM simulation is active)
    // Physics updates: SetDragVelocityBC(if dragging) → MPM Step(含 home-spring) → displacement → coupling → override buffers
    // MPM持续运行（拖拽时 + 非拖拽时都运行，自然回弹靠弹性力+阻尼+home-spring）
    if (mpm_initialized_ && mpm_manager_ && mpm_manager_->IsEnabled() &&
        coupling_initialized_ && coupling_manager_) {
        auto& physicsCmd = physicsCommandBuffers[frameIdx];
        physicsCmd->reset({});
        physicsCmd->begin(vk::CommandBufferBeginInfo{});

        updatePhysicsSimulation(physicsCmd.get());

        physicsCmd->end();

        // Submit physics to same queue (no fence needed - same-queue ordering guarantees
        // physics completes before subsequent preprocess submission)
        vk::SubmitInfo physicsSubmit{};
        physicsSubmit.commandBufferCount = 1;
        physicsSubmit.pCommandBuffers = &physicsCmd.get();
        context->queues[VulkanContext::Queue::COMPUTE].queue.submit(physicsSubmit, vk::Fence());
        physicsSubmittedThisFrame_ = true;
    } else {
        physicsSubmittedThisFrame_ = false;
    }

    // 3b. 提交预处理工作（使用专用preprocessFence，而非inflightFences）
    auto preprocessCmd = preprocessCommandBuffers[0].get();
    auto preprocessSubmit = vk::SubmitInfo{}.setCommandBuffers(preprocessCmd);
    context->queues[VulkanContext::Queue::COMPUTE].queue.submit(preprocessSubmit, preprocessFence.get());

    // 等待预处理完成（使用专用fence，确保totalSumBufferHost数据有效）
    ret = context->device->waitForFences(preprocessFence.get(), VK_TRUE, UINT64_MAX);
    if (ret != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for preprocess fence");
    }
    context->device->resetFences(preprocessFence.get());

    // 4. 记录并提交渲染命令（可能需要sort buffer reallocation）
    if (!recordRenderCommandBuffer(frameIdx)) {
        // Sort buffer reallocation occurred:
        // recordPreprocessCommandBuffer() 已在 recordRenderCommandBuffer 内被调用,
        // preprocessFence 已被正确等待并reset, 所以重置命令缓冲区是安全的
        // 重新提交预处理并等待
        context->queues[VulkanContext::Queue::COMPUTE].queue.submit(preprocessSubmit, preprocessFence.get());
        ret = context->device->waitForFences(preprocessFence.get(), VK_TRUE, UINT64_MAX);
        if (ret != vk::Result::eSuccess) {
            throw std::runtime_error("Failed to wait for preprocess fence (retry)");
        }
        context->device->resetFences(preprocessFence.get());

        // 重试渲染命令缓冲区
        if (!recordRenderCommandBuffer(frameIdx)) {
            // 不应发生，安全返回等待下一帧
            spdlog::warn("Sort buffer reallocation failed twice, skipping frame");
            advanceFrame();
            return;
        }
    }

    auto renderCmd = renderCommandBuffers[frameIdx].get();

    // 6. 提交渲染命令
    // wait: acquireSemaphores[frameIdx]（image 可用）at ComputeShader stage（render compute 写 storage image）
    // signal: renderSemaphores[frameIdx]（render 完成 → present 等）
    // 修复：原用 timeline semaphore 当 signal/wait，但 present 要求 binary（VUID-03267），
    // 且 timeline 未附 VkTimelineSemaphoreSubmitInfo（VUID-03239）+ signal 值从不递增 → 驱动注入全停。
    vk::Semaphore acquireSem = acquireSemaphores[frameIdx].get();
    vk::Semaphore renderSem  = renderSemaphores[frameIdx].get();
    vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eComputeShader;

    vk::SubmitInfo renderSubmit{};
    renderSubmit.commandBufferCount = 1;
    renderSubmit.pCommandBuffers = &renderCmd;
    renderSubmit.waitSemaphoreCount = 1;
    renderSubmit.pWaitSemaphores = &acquireSem;
    renderSubmit.pWaitDstStageMask = &waitStage;
    renderSubmit.signalSemaphoreCount = 1;
    renderSubmit.pSignalSemaphores = &renderSem;

    context->queues[VulkanContext::Queue::COMPUTE].queue.submit(renderSubmit, inflightFences[frameIdx].get());

    // 处理截图请求
    if (screenshotRequested && !screenshotSaving) {
        screenshotRequested = false;
        screenshotSaving = true;
        (void)context->device->waitForFences(inflightFences[frameIdx].get(), VK_TRUE, UINT64_MAX);
        screenshotCounter++;
        std::string screenshotPath = "screenshot_" + std::to_string(screenshotCounter) + ".png";
        saveScreenshot(screenshotPath);
    }

    // 7. 呈现（等待 render 完成 binary semaphore）
    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderSem;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain->swapchain.get();
    presentInfo.pImageIndices = &currentImageIndex;

    try {
        ret = context->queues[VulkanContext::Queue::PRESENT].queue.presentKHR(presentInfo);
    } catch (vk::OutOfDateKHRError& e) {
        recreateSwapchain();
        return;
    }

    if (ret == vk::Result::eErrorOutOfDateKHR || ret == vk::Result::eSuboptimalKHR) {
        recreateSwapchain();
    } else if (ret != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    // 8. 推进帧索引
    advanceFrame();
}

void Renderer::run() {
    while (running) {
        if (!window->tick()) {
            break;
        }

        draw();

        auto now = std::chrono::high_resolution_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsTime).count();
        if (diff > 1000) {
            spdlog::debug("FPS: {}", fpsCounter);
            GUIManager::pushTextMetric("FPS", static_cast<float>(fpsCounter));
            fpsCounter = 0;
            lastFpsTime = now;
        } else {
            fpsCounter++;
        }

        retrieveTimestamps();
        retrievePhysicsTimestamps();
    }

    context->device->waitIdle();
}

void Renderer::stop() {
    // wait till device is idle
    running = false;

    context->device->waitIdle();
}

void Renderer::createCommandPool() {
    spdlog::debug("Creating command pool");
    vk::CommandPoolCreateInfo poolInfo = {};
    poolInfo.queueFamilyIndex = context->queues[VulkanContext::Queue::COMPUTE].queueFamily;
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;

    commandPool = context->device->createCommandPoolUnique(poolInfo, nullptr);

    // 分配三缓冲命令缓冲区
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = commandPool.get();
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = FRAMES_IN_FLIGHT;

    auto preprocessBuffers = context->device->allocateCommandBuffersUnique(allocInfo);
    preprocessCommandBuffers = std::move(preprocessBuffers);

    auto renderBuffers = context->device->allocateCommandBuffersUnique(allocInfo);
    renderCommandBuffers = std::move(renderBuffers);

    // 分配三缓冲物理命令缓冲区
    auto physicsBuffers = context->device->allocateCommandBuffersUnique(allocInfo);
    physicsCommandBuffers = std::move(physicsBuffers);
}

void Renderer::advanceFrame() {
    currentFrameIndex = (currentFrameIndex + 1) % FRAMES_IN_FLIGHT;
    frameCounter++;
}

uint64_t Renderer::getExpectedFrameValue() const {
    return frameCounter;
}

void Renderer::recordPreprocessCommandBuffer() {
    spdlog::debug("Recording preprocess command buffer");
    // 使用第一帧的命令缓冲区（所有帧共用相同的preprocess命令）
    auto& cmdBuffer = preprocessCommandBuffers[0];
    cmdBuffer->reset();

    auto numGroups = (scene->getNumVertices() + 255) / 256;

    cmdBuffer->begin(vk::CommandBufferBeginInfo{});

    cmdBuffer->resetQueryPool(context->queryPool.get(), 0, 12);

    preprocessPipeline->bind(cmdBuffer, 0, 0);

    // Push constant: use physics override positions (override buffer always has valid data,
    // defaulting to original positions when physics is not active)
    uint32_t use_physics_override = 1;
    cmdBuffer->pushConstants(preprocessPipeline->pipelineLayout.get(),
                             vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(uint32_t), &use_physics_override);

    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                queryManager->registerQuery("preprocess_start"));
    cmdBuffer->dispatch(numGroups, 1, 1);
    tileOverlapBuffer->computeWriteReadBarrier(cmdBuffer.get());

    vk::BufferCopy copyRegion = {0, 0, tileOverlapBuffer->size};
    cmdBuffer->copyBuffer(tileOverlapBuffer->buffer, prefixSumPingBuffer->buffer, 1, &copyRegion);

    prefixSumPingBuffer->computeWriteReadBarrier(cmdBuffer.get());

    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                queryManager->registerQuery("preprocess_end"));

    prefixSumPipeline->bind(cmdBuffer, 0, 0);
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                queryManager->registerQuery("prefix_sum_start"));
    const auto iters = static_cast<uint32_t>(std::ceil(std::log2(static_cast<float>(scene->getNumVertices()))));
    for (uint32_t timestep = 0; timestep <= iters; timestep++) {
        cmdBuffer->pushConstants(prefixSumPipeline->pipelineLayout.get(),
                                   vk::ShaderStageFlagBits::eCompute, 0,
                                   sizeof(uint32_t), &timestep);
        cmdBuffer->dispatch(numGroups, 1, 1);

        if (timestep % 2 == 0) {
            prefixSumPongBuffer->computeWriteReadBarrier(cmdBuffer.get());
            prefixSumPingBuffer->computeReadWriteBarrier(cmdBuffer.get());
        } else {
            prefixSumPingBuffer->computeWriteReadBarrier(cmdBuffer.get());
            prefixSumPongBuffer->computeReadWriteBarrier(cmdBuffer.get());
        }
    }

    auto totalSumRegion = vk::BufferCopy{(scene->getNumVertices() - 1) * sizeof(uint32_t), 0, sizeof(uint32_t)};
    if (iters % 2 == 0) {
        cmdBuffer->copyBuffer(prefixSumPingBuffer->buffer, totalSumBufferHost->buffer, 1,
                                    &totalSumRegion);
    } else {
        cmdBuffer->copyBuffer(prefixSumPongBuffer->buffer, totalSumBufferHost->buffer, 1,
                                    &totalSumRegion);
    }

    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                queryManager->registerQuery("prefix_sum_end"));

    cmdBuffer->end();
}


bool Renderer::recordRenderCommandBuffer(uint32_t currentFrame) {
    auto& cmdBuffer = renderCommandBuffers[currentFrame];
    cmdBuffer->reset();

    uint32_t numInstances = totalSumBufferHost->readOne<uint32_t>();
    // spdlog::debug("Num instances: {}", numInstances);
    guiManager.pushTextMetric("instances", numInstances);

    // Update camera info for GUI
    guiManager.cameraInfo.position = camera.position;
    guiManager.cameraInfo.rotation = camera.rotation;
    guiManager.cameraInfo.fov = camera.fov;
    guiManager.cameraInfo.nearPlane = camera.nearPlane;
    guiManager.cameraInfo.farPlane = camera.farPlane;

    // Handle save camera request
    if (guiManager.saveCameraRequested) {
        guiManager.saveCameraRequested = false;
        std::string cameraPath = "camera.txt";
        saveCamera(cameraPath);
    }

    if (numInstances > scene->getNumVertices() * sortBufferSizeMultiplier) {
        auto old = sortBufferSizeMultiplier;
        while (numInstances > scene->getNumVertices() * sortBufferSizeMultiplier) {
            sortBufferSizeMultiplier++;
        }
        spdlog::info("Reallocating sort buffers. {} -> {}", old, sortBufferSizeMultiplier);
        sortKBufferEven->realloc(scene->getNumVertices() * sizeof(uint64_t) * sortBufferSizeMultiplier);
        sortKBufferOdd->realloc(scene->getNumVertices() * sizeof(uint64_t) * sortBufferSizeMultiplier);
        sortVBufferEven->realloc(scene->getNumVertices() * sizeof(uint32_t) * sortBufferSizeMultiplier);
        sortVBufferOdd->realloc(scene->getNumVertices() * sizeof(uint32_t) * sortBufferSizeMultiplier);

        uint32_t globalInvocationSize = scene->getNumVertices() * sortBufferSizeMultiplier /
                                        numRadixSortBlocksPerWorkgroup;
        uint32_t remainder = scene->getNumVertices() * sortBufferSizeMultiplier % numRadixSortBlocksPerWorkgroup;
        globalInvocationSize += remainder > 0 ? 1 : 0;

        auto numWorkgroups = (globalInvocationSize + 256 - 1) / 256;

        sortHistBuffer->realloc(numWorkgroups * 256 * sizeof(uint32_t));

        recordPreprocessCommandBuffer();
        return false;
    }

    cmdBuffer->reset({});
    cmdBuffer->begin(vk::CommandBufferBeginInfo{});

#ifdef VKGS_ENABLE_METAL
    if (numInstances == 0 && __APPLE__) {
        cmdBuffer->end();
        return true;
    }
#endif

    vertexAttributeBuffer->computeWriteReadBarrier(cmdBuffer.get());

    const auto iters = static_cast<uint32_t>(std::ceil(std::log2(static_cast<float>(scene->getNumVertices()))));
    auto numGroups = (scene->getNumVertices() + 255) / 256;
    preprocessSortPipeline->bind(cmdBuffer, 0, iters % 2 == 0 ? 0 : 1);
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                            queryManager->registerQuery("preprocess_sort_start"));
    uint32_t tileX = (swapchain->swapchainExtent.width + 16 - 1) / 16;
    // assert(tileX == 50);
    cmdBuffer->pushConstants(preprocessSortPipeline->pipelineLayout.get(),
                                           vk::ShaderStageFlagBits::eCompute, 0,
                                           sizeof(uint32_t), &tileX);
    cmdBuffer->dispatch(numGroups, 1, 1);

    sortKBufferEven->computeWriteReadBarrier(cmdBuffer.get());
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                            queryManager->registerQuery("preprocess_sort_end"));

    // std::cout << "Num instances: " << numInstances << std::endl;

    assert(numInstances <= scene->getNumVertices() * sortBufferSizeMultiplier);
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                                queryManager->registerQuery("sort_start"));
    for (auto i = 0; i < 8; i++) {
        sortHistPipeline->bind(cmdBuffer, 0, i % 2 == 0 ? 0 : 1);
        auto invocationSize = (numInstances + numRadixSortBlocksPerWorkgroup - 1) / numRadixSortBlocksPerWorkgroup;
        invocationSize = (invocationSize + 255) / 256;

        RadixSortPushConstants pushConstants{};
        pushConstants.g_num_elements = numInstances;
        pushConstants.g_num_blocks_per_workgroup = numRadixSortBlocksPerWorkgroup;
        pushConstants.g_shift = i * 8;
        pushConstants.g_num_workgroups = invocationSize;
        cmdBuffer->pushConstants(sortHistPipeline->pipelineLayout.get(),
                                           vk::ShaderStageFlagBits::eCompute, 0,
                                           sizeof(RadixSortPushConstants), &pushConstants);

        cmdBuffer->dispatch(invocationSize, 1, 1);

        sortHistBuffer->computeWriteReadBarrier(cmdBuffer.get());

        sortPipeline->bind(cmdBuffer, 0, i % 2 == 0 ? 0 : 1);
        cmdBuffer->pushConstants(sortPipeline->pipelineLayout.get(),
                                           vk::ShaderStageFlagBits::eCompute, 0,
                                           sizeof(RadixSortPushConstants), &pushConstants);
        cmdBuffer->dispatch(invocationSize, 1, 1);

        if (i % 2 == 0) {
            sortKBufferOdd->computeWriteReadBarrier(cmdBuffer.get());
            sortVBufferOdd->computeWriteReadBarrier(cmdBuffer.get());
        } else {
            sortKBufferEven->computeWriteReadBarrier(cmdBuffer.get());
            sortVBufferEven->computeWriteReadBarrier(cmdBuffer.get());
        }
    }
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                                queryManager->registerQuery("sort_end"));

    cmdBuffer->fillBuffer(tileBoundaryBuffer->buffer, 0, VK_WHOLE_SIZE, 0);

    Utils::BarrierBuilder().queueFamilyIndex(context->queues[VulkanContext::Queue::COMPUTE].queueFamily)
            .addBufferBarrier(tileBoundaryBuffer, vk::AccessFlagBits::eTransferWrite,
                              vk::AccessFlagBits::eShaderWrite)
            .build(cmdBuffer.get(), vk::PipelineStageFlagBits::eTransfer,
                   vk::PipelineStageFlagBits::eComputeShader);

    // Since we have 64 bit keys, the sort result is always in the even buffer
    tileBoundaryPipeline->bind(cmdBuffer, 0, 0);
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                        queryManager->registerQuery("tile_boundary_start"));
    cmdBuffer->pushConstants(tileBoundaryPipeline->pipelineLayout.get(),
                                       vk::ShaderStageFlagBits::eCompute, 0,
                                       sizeof(uint32_t), &numInstances);
    cmdBuffer->dispatch((numInstances + 255) / 256, 1, 1);

    tileBoundaryBuffer->computeWriteReadBarrier(cmdBuffer.get());
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                        queryManager->registerQuery("tile_boundary_end"));

    renderPipeline->bind(cmdBuffer, 0, std::vector<uint32_t>{0, currentImageIndex});
    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                        queryManager->registerQuery("render_start"));
    auto [width, height] = swapchain->swapchainExtent;
    uint32_t constants[2] = {width, height};
    cmdBuffer->pushConstants(renderPipeline->pipelineLayout.get(),
                                       vk::ShaderStageFlagBits::eCompute, 0,
                                       sizeof(uint32_t) * 2, constants);

    // image layout transition: undefined -> general
    vk::ImageMemoryBarrier imageMemoryBarrier{};
    imageMemoryBarrier.oldLayout = vk::ImageLayout::eUndefined;
    imageMemoryBarrier.newLayout = vk::ImageLayout::eGeneral;
    imageMemoryBarrier.image = swapchain->swapchainImages[currentImageIndex]->image;
    imageMemoryBarrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eNoneKHR;
    imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
    imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cmdBuffer->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                         vk::PipelineStageFlagBits::eComputeShader,
                                         vk::DependencyFlagBits::eByRegion, nullptr, nullptr, imageMemoryBarrier);

    cmdBuffer->dispatch((width + 15) / 16, (height + 15) / 16, 1);

    // image layout transition: general -> present
    imageMemoryBarrier.oldLayout = vk::ImageLayout::eGeneral;
    imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    if (configuration.enableGui) {
        imageMemoryBarrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
        imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        cmdBuffer->pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                             vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                             vk::DependencyFlagBits::eByRegion, nullptr, nullptr, imageMemoryBarrier);

        // Take screenshot BEFORE GUI rendering (pure 3D render)
        if (screenshotRequested) {
            auto [width, height] = swapchain->swapchainExtent;

            // Create staging buffer for screenshot
            vk::DeviceSize imageSize = width * height * 4;
            vk::BufferCreateInfo bufferInfo{};
            bufferInfo.size = imageSize;
            bufferInfo.usage = vk::BufferUsageFlagBits::eTransferDst;
            bufferInfo.sharingMode = vk::SharingMode::eExclusive;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VkBuffer stagingBuffer;
            VmaAllocation stagingAllocation;
            VmaAllocationInfo stagingAllocInfo;

            VkBufferCreateInfo bufferCreateInfo = static_cast<VkBufferCreateInfo>(bufferInfo);
            vmaCreateBuffer(context->allocator, &bufferCreateInfo, &allocInfo,
                           &stagingBuffer, &stagingAllocation, &stagingAllocInfo);

            screenshotStagingBuffer = stagingBuffer;
            screenshotStagingAllocation = stagingAllocation;
            screenshotStagingAllocInfo = stagingAllocInfo;

            // Transition image to transfer source
            vk::ImageMemoryBarrier copyBarrier{};
            copyBarrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
            copyBarrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            copyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            copyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            copyBarrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
            copyBarrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            copyBarrier.image = swapchain->swapchainImages[currentImageIndex]->image;
            copyBarrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            copyBarrier.subresourceRange.baseMipLevel = 0;
            copyBarrier.subresourceRange.levelCount = 1;
            copyBarrier.subresourceRange.baseArrayLayer = 0;
            copyBarrier.subresourceRange.layerCount = 1;

            cmdBuffer->pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                                 vk::PipelineStageFlagBits::eTransfer,
                                                 vk::DependencyFlagBits{}, nullptr, nullptr, copyBarrier);

            // Copy image to buffer
            vk::BufferImageCopy copyRegion{};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;
            copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
            copyRegion.imageExtent = vk::Extent3D{width, height, 1};

            cmdBuffer->copyImageToBuffer(swapchain->swapchainImages[currentImageIndex]->image,
                                                   vk::ImageLayout::eTransferSrcOptimal,
                                                   screenshotStagingBuffer, copyRegion);

            // Transition image back to color attachment
            copyBarrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
            copyBarrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
            copyBarrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
            copyBarrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;

            cmdBuffer->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                                 vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                                 vk::DependencyFlagBits{}, nullptr, nullptr, copyBarrier);
        }

        imguiManager->draw(cmdBuffer.get(), currentImageIndex, [this]() {
            guiManager.buildGui();
            drawPhysicsOverlay();
        });

        imageMemoryBarrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
        imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;

        imageMemoryBarrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
        imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;

        cmdBuffer->pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                             vk::PipelineStageFlagBits::eComputeShader,
                                             vk::DependencyFlagBits::eByRegion, nullptr, nullptr, imageMemoryBarrier);
    } else {
        imageMemoryBarrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
        imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
        cmdBuffer->pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                             vk::PipelineStageFlagBits::eBottomOfPipe,
                                             vk::DependencyFlagBits::eByRegion, nullptr, nullptr, imageMemoryBarrier);
    }

    cmdBuffer->writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, context->queryPool.get(),
                                        queryManager->registerQuery("render_end"));

    cmdBuffer->end();

    return true;
}

void Renderer::updateUniforms() {
    UniformBuffer data{};
    auto [width, height] = swapchain->swapchainExtent;
    data.width = width;
    data.height = height;
    data.camera_position = glm::vec4(camera.position, 1.0f);

    auto rotation = glm::mat4_cast(camera.rotation);
    auto translation = glm::translate(glm::mat4(1.0f), camera.position);
    auto view = glm::inverse(translation * rotation);

    float tan_fovx = std::tan(glm::radians(camera.fov) / 2.0);
    float tan_fovy = tan_fovx * static_cast<float>(height) / static_cast<float>(width);
    data.view_mat = view;
    data.proj_mat = glm::perspective(std::atan(tan_fovy) * 2.0f,
                                     static_cast<float>(width) / static_cast<float>(height),
                                     camera.nearPlane,
                                     camera.farPlane) * view;

    data.view_mat[0][1] *= -1.0f;
    data.view_mat[1][1] *= -1.0f;
    data.view_mat[2][1] *= -1.0f;
    data.view_mat[3][1] *= -1.0f;
    data.view_mat[0][2] *= -1.0f;
    data.view_mat[1][2] *= -1.0f;
    data.view_mat[2][2] *= -1.0f;
    data.view_mat[3][2] *= -1.0f;

    data.proj_mat[0][1] *= -1.0f;
    data.proj_mat[1][1] *= -1.0f;
    data.proj_mat[2][1] *= -1.0f;
    data.proj_mat[3][1] *= -1.0f;
    data.tan_fovx = tan_fovx;
    data.tan_fovy = tan_fovy;
    data.foreground_only = renderForegroundOnly_ ? 1u : 0u;
    uniformBuffer->upload(&data, sizeof(UniformBuffer), 0);
}

void Renderer::loadCamera(const std::string& cameraPath) {
    std::ifstream file(cameraPath);
    if (!file.is_open()) {
        spdlog::warn("Failed to open camera file: {}", cameraPath);
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("position") == 0) {
            sscanf(line.c_str(), "position: %f %f %f",
                   &camera.position.x, &camera.position.y, &camera.position.z);
        } else if (line.find("rotation") == 0) {
            sscanf(line.c_str(), "rotation: %f %f %f %f",
                   &camera.rotation.w, &camera.rotation.x,
                   &camera.rotation.y, &camera.rotation.z);
        } else if (line.find("fov") == 0) {
            sscanf(line.c_str(), "fov: %f", &camera.fov);
        } else if (line.find("nearPlane") == 0) {
            sscanf(line.c_str(), "nearPlane: %f", &camera.nearPlane);
        } else if (line.find("farPlane") == 0) {
            sscanf(line.c_str(), "farPlane: %f", &camera.farPlane);
        }
    }

    spdlog::info("Loaded camera - position: ({}, {}, {})", camera.position.x, camera.position.y, camera.position.z);
    spdlog::info("Loaded camera - rotation: ({}, {}, {}, {})", camera.rotation.w, camera.rotation.x, camera.rotation.y, camera.rotation.z);
    spdlog::info("Loaded camera - fov: {}", camera.fov);
    spdlog::info("Loaded camera from: {}", cameraPath);
}

void Renderer::saveCamera(const std::string& cameraPath) {
    std::ofstream file(cameraPath);
    if (!file.is_open()) {
        spdlog::warn("Failed to create camera file: {}", cameraPath);
        return;
    }

    file << "position: " << camera.position.x << " " << camera.position.y << " " << camera.position.z << "\n";
    file << "rotation: " << camera.rotation.w << " " << camera.rotation.x << " " << camera.rotation.y << " " << camera.rotation.z << "\n";
    file << "fov: " << camera.fov << "\n";
    file << "nearPlane: " << camera.nearPlane << "\n";
    file << "farPlane: " << camera.farPlane << "\n";

    spdlog::info("Saved camera to: {}", cameraPath);
}

void Renderer::setDeformableIndices(const std::vector<uint32_t>& indices) {
    if (indices.empty()) {
        spdlog::warn("[Renderer] No deformable indices provided");
        return;
    }
    // Store indices - will be uploaded to GPU after pipeline creation
    pendingDeformableIndices_ = indices;
    spdlog::info("[Renderer] Stored {} deformable indices for later upload", indices.size());

    // If pipeline already created, upload immediately
    if (visibilityMaskBuffer_) {
        uploadVisibilityMask();
    }
}

void Renderer::uploadVisibilityMask() {
    if (!visibilityMaskBuffer_ || pendingDeformableIndices_.empty()) return;

    uint64_t num_vertices = scene->getNumVertices();
    std::vector<uint32_t> mask(num_vertices, 0);
    for (uint32_t idx : pendingDeformableIndices_) {
        if (idx < num_vertices) {
            mask[idx] = 1;
        }
    }

    spdlog::info("[Renderer] Uploading visibility mask: {} foreground / {} total",
                pendingDeformableIndices_.size(), num_vertices);

    visibilityMaskBuffer_->upload(mask.data(), num_vertices * sizeof(uint32_t));
}

void Renderer::saveScreenshot(const std::string& filePath) {
    auto [width, height] = swapchain->swapchainExtent;

    if (screenshotStagingBuffer == VK_NULL_HANDLE) {
        spdlog::error("Screenshot staging buffer is null!");
        return;
    }

    // Get image data from staging buffer (already copied by GPU)
    auto* data = static_cast<unsigned char*>(screenshotStagingAllocInfo.pMappedData);

    // Convert BGRA to RGB and flip vertically to match screen coordinates
    std::vector<unsigned char> rgbData(width * height * 3);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            // Vulkan images are typically stored bottom-to-top, so flip vertically
            uint32_t srcY = (height - 1 - y);
            uint32_t srcIdx = (srcY * width + x) * 4;
            uint32_t dstIdx = (y * width + x) * 3;

            // Swap BGR to RGB (common Vulkan format is BGRA)
            rgbData[dstIdx + 0] = data[srcIdx + 2]; // R
            rgbData[dstIdx + 1] = data[srcIdx + 1]; // G
            rgbData[dstIdx + 2] = data[srcIdx + 0]; // B
        }
    }

    // Write to PNG
    stbi_write_png(filePath.c_str(), width, height, 3, rgbData.data(), width * 3);

    // Cleanup staging buffer
    vmaDestroyBuffer(context->allocator, screenshotStagingBuffer, screenshotStagingAllocation);
    screenshotStagingBuffer = VK_NULL_HANDLE;
    screenshotStagingAllocation = VK_NULL_HANDLE;

    // 清除保存标志，允许新的截图请求
    screenshotSaving = false;

    spdlog::info("Screenshot saved to: {}", filePath);
}

// === 物理交互系统实现 ===

void Renderer::initializeInteractionSystem() {
    if (!mpm_initialized_) {
        spdlog::warn("[Renderer] MPM not initialized, skipping interaction system initialization");
        return;
    }

    spdlog::info("[Renderer] Initializing interaction system...");

    // 创建射线拾取器
    Interaction::RayCaster::Config ray_caster_config;
    ray_caster_config.max_distance = 0.5f;
    ray_caster_ = std::make_shared<Interaction::RayCaster>(context, ray_caster_config);
    ray_caster_->Initialize();

    // 创建拖拽处理器（速度插值模式）
    Interaction::DragHandler::Config drag_config;
    // dragRadius=0.2 为初始默认值；运行时由 P0 在 CFL 注入块用 AABB对角线*2% 覆盖
    // （对标 PhysDreamer gui_demo.py:186，局部抓取避免刚体旋转无回弹）
    drag_config.dragRadius = 0.2f;   // 拖拽作用半径（世界空间单位，将被 P0 覆盖）
    // alpha=0.2: 速度跟随系数，粒子只跟随 20% 鼠标速度，弹性应力有空间拉回（防飞出网格）
    drag_config.alpha = 0.2f;         // 速度跟随系数
    drag_handler_ = std::make_shared<Interaction::DragHandler>(context, drag_config);
    drag_handler_->Initialize();

    spdlog::info("[Renderer] Interaction system initialized successfully");
}

void Renderer::handlePhysicsInteraction() {
    static bool logged_init = false;
    if (!logged_init) {
        spdlog::warn("[Physics] STATE CHECK: mpm_initialized={}, ray_caster={}, drag_handler={}",
                     mpm_initialized_,
                     ray_caster_ != nullptr ? "OK" : "NULL",
                     drag_handler_ != nullptr ? "OK" : "NULL");
        logged_init = true;
    }

    if (!mpm_initialized_ || !ray_caster_ || !drag_handler_) {
        return;  // 早期返回，不处理物理交互
    }

    auto keys = window->getKeys();
    auto mouse_buttons = window->getMouseButton();
    auto cursor_pos = window->getCursorPosition();

    // 处理物理交互触发条件：P键 + 左键
    bool p_key_held = keys[8];  // P键索引

    bool left_mouse_down = mouse_buttons[0];  // 左键

    // 检测鼠标按下/释放事件（边沿检测）
    mouse_pressed_this_frame_ = left_mouse_down && !prev_left_mouse_down_;
    mouse_released_this_frame_ = !left_mouse_down && prev_left_mouse_down_;

    // 保存当前鼠标位置
    last_mouse_position_ = glm::ivec2(static_cast<int>(cursor_pos[0]), static_cast<int>(cursor_pos[1]));
    prev_left_mouse_down_ = left_mouse_down;

    // 处理鼠标按下事件（射线拾取需要命令缓冲区，延迟到 updatePhysicsSimulation）
    if (p_key_held && mouse_pressed_this_frame_) {
        spdlog::info("[Physics] P+Left Click detected at ({}, {})", last_mouse_position_.x, last_mouse_position_.y);
        mouse_pos_on_press_ = last_mouse_position_;
        physics_interaction_mode_ = true;
    }

    // 处理鼠标释放事件 — 保留动量，靠MPM阻尼自然衰减
    if (mouse_released_this_frame_ && physics_interaction_mode_) {
        spdlog::info("[Physics] Mouse released — momentum preserved, MPM damping will decay naturally");
        drag_handler_->OnMouseUp();
        physics_interaction_mode_ = false;
    }

    // 处理鼠标移动事件（在拖拽中）
    // 只在拖拽状态下输出日志，避免泛滥
    if (physics_interaction_mode_ && left_mouse_down) {
        spdlog::debug("[Physics] OnMouseMove: cursor=({},{}), isDragging={}",
                     last_mouse_position_.x, last_mouse_position_.y,
                     drag_handler_->IsDragging());
        drag_handler_->OnMouseMove(last_mouse_position_.x, last_mouse_position_.y);
    }
}

void Renderer::updatePhysicsSimulation(VkCommandBuffer cmd) {
    static bool logged_once = false;
    static bool logged_pipeline_state = false;
    if (!logged_once) {
        spdlog::info("[PhysicsSim] STATE: mpm_initialized={}, mpm_manager={}, enabled={}",
                     mpm_initialized_,
                     mpm_manager_ != nullptr ? "OK" : "NULL",
                     mpm_manager_ ? (mpm_manager_->IsEnabled() ? "YES" : "NO") : "N/A");
        logged_once = true;
    }

    // ── First-step pipeline diagnostic: verify pipelines and buffers are valid ──
    if (!logged_pipeline_state && mpm_manager_) {
        auto& cfg = mpm_manager_->GetConfig();
        auto grid_buf = mpm_manager_->GetGridBuffer();
        auto particle_buf = mpm_manager_->GetParticleBuffer();
        spdlog::info("[PipelineDiag] First physics step — verifying pipeline/descriptor state:");
        spdlog::info("[PipelineDiag]   num_particles={}, grid_size={}, grid_buffer={}, particle_buffer={}",
                     mpm_manager_->GetParticleCount(), cfg.grid_size,
                     grid_buf ? "OK" : "NULL", particle_buf ? "OK" : "NULL");
        spdlog::info("[PipelineDiag]   grid_buffer handle={}, size={}",
                     grid_buf ? (void*)grid_buf->buffer : nullptr,
                     grid_buf ? grid_buf->size : 0);
        spdlog::info("[PipelineDiag]   particle_buffer handle={}, size={}",
                     particle_buf ? (void*)particle_buf->buffer : nullptr,
                     particle_buf ? particle_buf->size : 0);
        logged_pipeline_state = true;
    }

    if (!mpm_initialized_ || !mpm_manager_) {
        return;
    }

    // Reset physics 专用 query pool（spec: 写入前必须 reset）。
    // mpm_start=0, mpm_end=1, coupling_start=2, coupling_end=3。
    // 用独立 pool，不与 render queryPool 混用，避免跨队列/跨 cmd 池导致 retrieveTimestamps 死等。
    vkCmdResetQueryPool(cmd, static_cast<VkQueryPool>(context->physicsQueryPool.get()), 0, 4);

    // ── 注入 CFL 限幅参数到 DragHandler（一次性）──
    // cfl=0.02: drag 注入速度限幅 max_vel = 0.02·dx/sub_dt ≈ 2.4 norm/s（兜底）
    //   原实测 cfl=0.05 → max_vel=6，dragVel=1.42 不触发 CFL，但 1.42 持续 256 子步
    //   → 累积应变 3.0 → 拉断飞出。CFL 只防单子步射出，累积应变改由 apply_drag_velocity_bc
    //   的应变门控（按 F 列范数/det 衰减）负责。CFL 收到 0.02 作极端甩鼠标的兜底。
    //   cfl=0.02 → max_vel=2.4：每帧 0.08 norm=5dx，单子步 J 增长 e^0.013≈1.01 安全。
    static bool cfl_injected = false;
    if (!cfl_injected && drag_handler_) {
        auto& mpm_cfg = mpm_manager_->GetConfig();
        const float frame_dt_cfl = 1.0f / 30.0f;
        const float sub_dt = frame_dt_cfl / static_cast<float>(mpm_cfg.substeps);
        constexpr float kDragCfl = 0.02f;
        drag_handler_->SetCFLParams(mpm_manager_->GetInvDx(), sub_dt, kDragCfl);
        spdlog::info("[PhysicsSim] CFL injected: inv_dx={:.6f}, sub_dt={:.6f}, substeps={}, "
                     "max_velocity={:.4f} (normalized/s)",
                     mpm_cfg.inv_dx, sub_dt, mpm_cfg.substeps,
                     kDragCfl * (1.0f / mpm_cfg.inv_dx) / sub_dt);

        // ── P0: 抓取半径 = AABB对角线占比（对标 PhysDreamer gui_demo.py:186 原则）──
        // 决定"局部变形 vs 刚体运动"的是**空间半径**（相对物体尺寸），不是粒子计数。
        // FCR 应力 τ=2μ(F−R)Fᵀ 对纯刚性运动 F≈R 有 τ≈0 → 无恢复力 → 拖拽后不回弹。
        // PD 用 2% 半径：空间局部 → 花头内部 F≠R → FCR 有恢复力 → 可回弹。
        //   (PD 2% 抓 ~109 粒子是因为其云密；我们稀疏云 2% 仅 ~3 粒子)
        //
        // 旧值 7.5%：虽只抓 ~140 粒子(占云 1%)，但**空间半径是 PD 的 3.75×**，
        // 抓取球跨越花头-茎连接区 → 整块花头作刚性转动/平移 → F≈R → FCR 零恢复
        // → 花头不归位 + 茎被迫长期弯折桥接 → 茎部 F 累积扭曲塌陷。这正是 bug 现象。
        //
        // 降到 2%（=PD 原值）：空间半径=PD 局部尺度，避免刚性模态。稀疏云 2% 仅 ~3 粒子，
        // 配合下方 P0-2(移除 F 软界 clamp) + P1(R 极分解迭代提升)，局部小变形可被
        // FCR 正确恢复，不再全场扩散（旧"3 粒子→F 炸到 1.67 全场累积"是 clamp/R
        // 非保守能量注入所致，移除后局部应变可控）。
        // 调参指引：若边界 ∇v 爆炸 → 升至 0.03；若仍刚性不回弹 → 检查 substeps≥256。
        if (mpm_manager_->HasAABB()) {
            const float diag_world = mpm_manager_->GetInitialAABBDiag() * mpm_coord_transform_.scale;
            constexpr float kGrabPortion = 0.02f;  // 空间局部（对标 PD 2%）；稀疏云 ~3 粒子
            const float grab_radius_world = diag_world * kGrabPortion;
            drag_handler_->SetDragRadius(grab_radius_world);
            // 射线命中阈值 = grab_radius（对标 PD gui_demo.py:447 grab_hit_thres=aabb*0.02）
            // 旧 max_distance=0.5 norm 太松（sim 区 ~0.56 跨度，点背景也命中最近粒子→区域外能拖）
            ray_caster_->SetMaxDistance(grab_radius_world / mpm_coord_transform_.scale);
            spdlog::info("[PhysicsSim] Grab radius = AABB_diag({:.4f}world) * {:.2f} = {:.4f}world "
                         "({:.4f}norm) — 局部抓取对标PhysDreamer",
                         diag_world, kGrabPortion, grab_radius_world,
                         grab_radius_world / mpm_coord_transform_.scale);
        }

        // ── 恢复机制：纯 FCR 弹性（对标 PhysDreamer gui_demo.py）──
        // PhysDreamer 不用 home-spring、不用 F 松弛，靠 FCR 弹性应力（F≠I→τ≠0）自然恢复。
        // 之前的 home-spring + F 松弛是治"刚体旋转锁死"症状的 workaround，但互相拆台：
        //   - F 松弛驱 F→I → τ=2μ(F−R)Fᵀ→0 → 杀死 FCR 弹性耦合 → 无恢复力
        //   - home-spring 速度冲量在 P2G→G2P 回路丢失（实测 v 比理论小 360×）→ 失效
        // 真根因是 substeps=128(<PD 最小 256)+拖拽成刚体模态；提到 256+局部 2% 抓取后
        // 拖拽产生局部变形(F≠R)，FCR 即可恢复（PD gui_demo.py:184-186 作者自述）。
        mpm_manager_->SetHomeSpring(0.0f, /*enable=*/false);  // 关闭：PD 不用位置弹簧
        mpm_manager_->SetFRelaxAlpha(0.0f);                    // 关闭：保留 FCR 弹性恢复力
        spdlog::info("[PhysicsSim] Recovery = pure FCR (PhysDreamer-aligned): home-spring OFF, F-relax OFF; "
                     "substeps={} (PD min 256). FCR τ=2μ(F−R)Fᵀ provides elastic restore.",
                     mpm_cfg.substeps);
        cfl_injected = true;
    }

    auto [fb_width, fb_height] = window->getFramebufferSize();

    // 计算视图投影矩阵（所有模式共用）
    auto rotation = glm::mat4_cast(camera.rotation);
    auto translation = glm::translate(glm::mat4(1.0f), camera.position);
    auto view = glm::inverse(translation * rotation);
    float tan_fovx = std::tan(glm::radians(camera.fov) / 2.0);
    float tan_fovy = tan_fovx * static_cast<float>(fb_height) / static_cast<float>(fb_width);
    auto proj = glm::perspective(std::atan(tan_fovy) * 2.0f,
                                 static_cast<float>(fb_width) / static_cast<float>(fb_height),
                                 camera.nearPlane, camera.farPlane);
    glm::mat4 view_proj = proj * view;
    glm::mat4 inverse_view_proj = glm::inverse(view_proj);

    // ===================================================================
    // 射线拾取（鼠标按下时）
    // ===================================================================
    if (mouse_pressed_this_frame_ && physics_interaction_mode_ && ray_caster_ && drag_handler_) {
        glm::vec2 ndc = ray_caster_->ScreenToNDC(
            mouse_pos_on_press_.x, mouse_pos_on_press_.y, fb_width, fb_height);
        glm::vec3 ray_origin, ray_direction;
        ray_caster_->NDCToWorldRay(ndc, inverse_view_proj, ray_origin, ray_direction);

        // MPM粒子在归一化坐标空间，将射线转换到归一化空间
        auto& ct = mpm_coord_transform_;
        glm::vec3 ray_origin_norm = ct.ToNormalized(ray_origin);
        glm::vec3 ray_end_norm = ct.ToNormalized(ray_origin + ray_direction);
        glm::vec3 ray_direction_norm = glm::normalize(ray_end_norm - ray_origin_norm);

        // CPU射线检测
        std::vector<glm::vec3> particle_positions = mpm_manager_->GetParticlePositions();
        auto result = ray_caster_->CastFromRayCPU(ray_origin_norm, ray_direction_norm, particle_positions);

        spdlog::info("[Physics] CPU ray cast: success={}, particle={}, distance_sq={:.6f}",
                     result.success, result.particle_index, result.distance_sq);

        // 传入相机参数，用于深度锁定拖拽平面
        glm::vec3 cam_forward = camera.rotation * glm::vec3(0, 0, -1);
        drag_handler_->OnMouseDownFromResult(
            result,
            mouse_pos_on_press_.x,
            mouse_pos_on_press_.y,
            camera.position,
            cam_forward,
            mpm_coord_transform_
        );
    }

    // ===================================================================
    // 速度插值模式 + MPM 物理模拟
    // ===================================================================
    // 新流程: Drag(速度插值) → MPM → Coupling → 渲染override
    // 拖拽时: ApplyDrag → MPM → 耦合
    // 非拖拽时: MPM继续运行（自然回弹靠弹性力+阻尼）
    // 松开鼠标时: 不清零速度，保留动量靠MPM阻尼衰减

    float frame_dt = 1.0f / 30.0f;  // MPM帧时间步长

    // ── 1. ApplyDrag（仅拖拽时：速度插值3-pass GPU流程）──
    // 非拖拽时跳过整个 drag pass，避免 extract/writeback 无读写循环引入噪声
    // Extract → Drag → Writeback → memory barrier → MPM
    auto drag_t0 = std::chrono::high_resolution_clock::now();
    if (drag_handler_ && drag_handler_->IsDragging()) {
        spdlog::debug("[PhysicsSim] IsDragging=true → computing drag params");

        // 位置反馈：回读拾取粒子当前 GPU 位置（对标 PhysDreamer gui_demo.py:335 cur_pick = particle_x[grab_idx]）
        // C3 修复：旧路径 GetParticlePositions() 返回 cpu_particles_（仅 Load/Reset 赋值，Step 后永不更新）
        //         → cur_pick 恒为初始位置 → P 控制器 dragVel=(target-init)/dt 饱和在 CFL，batch 过冲不归位。
        //         改为 GetParticlePositionGPU：单粒子 staging 回读，queue.waitIdle 保证读到上一帧已提交状态（一帧滞后）。
        const uint32_t picked = drag_handler_->GetDraggedParticle();
        if (picked != UINT32_MAX) {
            auto cur_pos = mpm_manager_->GetParticlePositionGPU(picked);  // 归一化空间
            if (cur_pos) {
                drag_handler_->SetCurrentPickWorld(
                    mpm_coord_transform_.ToOriginal(*cur_pos));
            }
        }

        auto pushConstants = drag_handler_->ComputeDragPushConstants(
            frame_dt,
            camera.position,
            camera.rotation,
            camera.fov,
            fb_height,
            mpm_coord_transform_
        );

        // 只在速度非零时打印 PushConstants，减少日志泛滥
        static int drag_log_counter = 0;
        if (drag_log_counter++ % 5 == 0 ||
            (pushConstants.dragVelocity.x != 0.0f || pushConstants.dragVelocity.y != 0.0f || pushConstants.dragVelocity.z != 0.0f)) {
            spdlog::debug("[PhysicsSim] PushConstants: isDragging={}, "
                         "center=({:.4f},{:.4f},{:.4f}), radius={:.4f}, "
                         "vel=({:.4f},{:.4f},{:.4f}), alpha={:.2f}",
                         pushConstants.isDragging,
                         pushConstants.dragCenter.x, pushConstants.dragCenter.y, pushConstants.dragCenter.z,
                         pushConstants.dragRadius,
                         pushConstants.dragVelocity.x, pushConstants.dragVelocity.y, pushConstants.dragVelocity.z,
                         pushConstants.alpha);
        }

        // P2: 拖拽速度 Dirichlet BC 交由 MPMManager 每子步派发（对标 PhysDreamer enforce_particle_velocity_by_mask）
        // 旧 ApplyDrag 3-pass 每帧一次+alpha blend 太弱（小半径下 max_disp 仅 0.0006），
        // 改为每子步 SET 半径内粒子速度=dragVelocity，持续驱动 batch 跟随鼠标 → 局部变形。
        mpm_manager_->SetDragVelocityBC(
            pushConstants.dragCenter,
            pushConstants.dragRadius,
            pushConstants.dragVelocity,
            pushConstants.maxVelocity
        );
    } else {
        // 释放：停止速度 BC 驱动，花头自由震荡（靠 P1 释放阻尼衰减归位）
        mpm_manager_->ClearDragVelocityBC();
    }
    {
        // drag 时延（CPU 侧）：含 GetParticlePositionGPU 同步回读 stall + push constants + BC 参数设置
        auto drag_t1 = std::chrono::high_resolution_clock::now();
        float drag_ms = std::chrono::duration<float, std::milli>(drag_t1 - drag_t0).count();
        if (configuration.enableGui) guiManager.pushMetric("drag", drag_ms);
    }

    // ── 2. 执行 MPM 物理模拟 ──
    // MPM持续运行（无pin，自然动力学处理回弹）
    // ── P1: 释放阻尼（对标 PhysDreamer gui_demo.py:288 release_damping=0.95/帧）──
    // 拖拽中=1.0(无阻尼纯跟随)，非拖拽=0.95^(1/substeps)/子步(衰减振荡归位)。
    // 阻尼是每子步乘一次(grid_update)，故按子步折算避免过阻尼。
    // idle(静止)时 v=0，阻尼无效，故非拖拽阶段统一用释放阻尼是安全的。
    {
        const bool dragging = drag_handler_ && drag_handler_->IsDragging();
        const uint32_t subs = mpm_manager_->GetConfig().substeps;
        const float damping = dragging
            ? 1.0f
            : std::pow(0.95f, 1.0f / static_cast<float>(subs));  // 0.95/帧 → 每子步
        mpm_manager_->SetDamping(damping);

        // home-spring 已在 init 关闭（纯 FCR 恢复，对标 PhysDreamer）。此处保持关闭，
        // 不再每帧按 !dragging 切换 —— PD 无位置弹簧，FCR τ 提供全部恢复力。
        mpm_manager_->SetHomeSpring(0.0f, /*enable=*/false);
    }
    // MPM GPU 总时延（多 pass 多 substep）：timestamp 包住整个 Step
    // （含 home-spring/drag-BC/P2G/grid_update/g2p 全部子步的 GPU 执行时间之和）
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        static_cast<VkQueryPool>(context->physicsQueryPool.get()), 0);  // mpm_start
    mpm_manager_->Step(cmd, frame_dt);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        static_cast<VkQueryPool>(context->physicsQueryPool.get()), 1);  // mpm_end

    // 诊断回读：仅在 --verbose 时执行（每 60 帧下载 2.3MB + 计算 max_tau/ratio/stretch/TEAR-WATCH）。
    // 非 verbose 完全跳过：无 GPU 回读、无指标计算、无日志。
    if (configuration.verbose) {
        // 读的是上一帧已提交的状态（当前cmd尚未提交），一帧滞后可接受
        mpm_manager_->Diagnose();
    }

    // ── 3. GPU计算粒子位移 + 耦合映射 ──
    if (coupling_initialized_ && coupling_manager_) {
        // ComputeParticleDisplacementsGPU: displacement = current_pos - initial_pos
        // Couple: MapDisplacements → 高斯位置/旋转更新
        // coupling GPU 时延：包住 Couple + buffer copies
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            static_cast<VkQueryPool>(context->physicsQueryPool.get()), 2);  // coupling_start
        coupling_manager_->Couple(cmd, currentFrameIndex, true);

        // ── 4. 复制耦合输出到 override buffers ──
        VkMemoryBarrier couple_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        couple_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        couple_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 1, &couple_barrier, 0, nullptr, 0, nullptr);

        VkBufferCopy copy_region = {0, 0,
            static_cast<VkDeviceSize>(coupling_manager_->GetGaussianPositionBuffer()->size)};
        vkCmdCopyBuffer(cmd,
                       coupling_manager_->GetGaussianPositionBuffer()->buffer,
                       overridePositionBuffer_->buffer,
                       1, &copy_region);

        VkBufferCopy rot_copy_region = {0, 0,
            static_cast<VkDeviceSize>(coupling_manager_->GetGaussianRotationBuffer()->size)};
        vkCmdCopyBuffer(cmd,
                       coupling_manager_->GetGaussianRotationBuffer()->buffer,
                       overrideRotationBuffer_->buffer,
                       1, &rot_copy_region);

        VkMemoryBarrier copy_write_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        copy_write_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_write_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &copy_write_barrier, 0, nullptr, 0, nullptr);

        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            static_cast<VkQueryPool>(context->physicsQueryPool.get()), 3);  // coupling_end

        physics_override_active_ = true;
    }
}

void Renderer::drawPhysicsOverlay() {
    // 拖拽时显示绿色箭头
    if (drag_handler_ && drag_handler_->IsDragging()) {
        auto drag_start = drag_handler_->GetDragStartScreen();
        auto current_pos = drag_handler_->GetCurrentScreen();

        ImDrawList* draw_list = ImGui::GetForegroundDrawList();

        // 绿色线段：从拖拽起点到当前位置
        draw_list->AddLine(
            ImVec2(static_cast<float>(drag_start.x), static_cast<float>(drag_start.y)),
            ImVec2(static_cast<float>(current_pos.x), static_cast<float>(current_pos.y)),
            IM_COL32(0, 255, 0, 200),
            2.0f
        );

        // 绿色圆点：拾取位置
        draw_list->AddCircleFilled(
            ImVec2(static_cast<float>(drag_start.x), static_cast<float>(drag_start.y)),
            5.0f,
            IM_COL32(0, 255, 0, 255)
        );

        // 小圆点：当前鼠标位置
        draw_list->AddCircleFilled(
            ImVec2(static_cast<float>(current_pos.x), static_cast<float>(current_pos.y)),
            3.0f,
            IM_COL32(0, 255, 100, 255)
        );

        // 位移信息文本（速度插值模式：显示拖拽参数）
        char disp_text[64];
        snprintf(disp_text, sizeof(disp_text), "alpha=%.2f radius=%.3f [particle %u]",
                 drag_handler_->GetConfig().alpha,
                 drag_handler_->GetConfig().dragRadius,
                 drag_handler_->GetDraggedParticle());
        draw_list->AddText(
            ImVec2(static_cast<float>(current_pos.x + 10), static_cast<float>(current_pos.y - 20)),
            IM_COL32(0, 255, 0, 255),
            disp_text
        );
    }

    }

void Renderer::buildHoverGrid() {
    if (!scene || scene->cpuPositions.empty()) {
        spdlog::warn("[Renderer] Cannot build hover grid: no scene positions");
        return;
    }

    hover_grid_.grid_min = scene->cpuPositions[0];
    hover_grid_.grid_max = scene->cpuPositions[0];

    // Compute scene AABB
    for (const auto& pos : scene->cpuPositions) {
        hover_grid_.grid_min = glm::min(hover_grid_.grid_min, pos);
        hover_grid_.grid_max = glm::max(hover_grid_.grid_max, pos);
    }

    // Add margin to avoid edge cases
    glm::vec3 margin(hover_grid_.cell_size);
    hover_grid_.grid_min -= margin;
    hover_grid_.grid_max += margin;

    // Insert all gaussians into spatial hash cells
    hover_grid_.cells.clear();
    for (uint32_t i = 0; i < static_cast<uint32_t>(scene->cpuPositions.size()); i++) {
        int32_t cx, cy, cz;
        hover_grid_.getCellCoords(scene->cpuPositions[i], cx, cy, cz);
        auto key = hover_grid_.cellKey(cx, cy, cz);
        hover_grid_.cells[key].push_back(i);
    }

    spdlog::info("[Renderer] Hover spatial grid built: {} cells, {} gaussians, "
                 "AABB min=({:.3f},{:.3f},{:.3f}) max=({:.3f},{:.3f},{:.3f})",
                 hover_grid_.cells.size(), scene->cpuPositions.size(),
                 hover_grid_.grid_min.x, hover_grid_.grid_min.y, hover_grid_.grid_min.z,
                 hover_grid_.grid_max.x, hover_grid_.grid_max.y, hover_grid_.grid_max.z);
}

void Renderer::updateHoverDetection() {
    // 如果 GUI 捕获了鼠标，不进行检测
    bool gui_wants_mouse = configuration.enableGui && guiManager.wantCaptureMouse();
    bool mouse_captured = guiManager.mouseCapture;

    if (gui_wants_mouse || mouse_captured) {
        window->setCursor(0);
        return;
    }

    // 如果没有 sim_mask_ 或场景未加载，不进行检测
    if (sim_mask_.empty() || !scene || hover_grid_.cells.empty()) {
        window->setCursor(0);
        return;
    }

    // 获取鼠标位置
    auto cursorPos = window->getCursorPosition();
    int mouse_x = static_cast<int>(cursorPos[0]);
    int mouse_y = static_cast<int>(cursorPos[1]);

    // 获取窗口大小
    auto [fb_width, fb_height] = window->getFramebufferSize();

    // 检查鼠标是否在窗口内
    if (mouse_x < 0 || mouse_x >= static_cast<int>(fb_width) ||
        mouse_y < 0 || mouse_y >= static_cast<int>(fb_height)) {
        window->setCursor(0);
        current_cursor_type_ = 0;
        return;
    }

    // 计算视图投影矩阵
    auto rotation = glm::mat4_cast(camera.rotation);
    auto translation = glm::translate(glm::mat4(1.0f), camera.position);
    auto view = glm::inverse(translation * rotation);

    float tan_fovx = std::tan(glm::radians(camera.fov) / 2.0);
    float tan_fovy = tan_fovx * static_cast<float>(fb_height) / static_cast<float>(fb_width);
    auto proj = glm::perspective(std::atan(tan_fovy) * 2.0f,
                                 static_cast<float>(fb_width) / static_cast<float>(fb_height),
                                 camera.nearPlane, camera.farPlane);
    glm::mat4 view_proj = proj * view;
    glm::mat4 inverse_view_proj = glm::inverse(view_proj);

    // 屏幕坐标转NDC
    float ndc_x = (2.0f * mouse_x) / fb_width - 1.0f;
    float ndc_y = 1.0f - (2.0f * mouse_y) / fb_height;

    // NDC转世界射线
    glm::vec4 near_point_ndc(ndc_x, ndc_y, 0.0f, 1.0f);
    glm::vec4 near_point_world = inverse_view_proj * near_point_ndc;

    glm::vec4 far_point_ndc(ndc_x, ndc_y, 1.0f, 1.0f);
    glm::vec4 far_point_world = inverse_view_proj * far_point_ndc;

    if (near_point_world.w != 0.0f) near_point_world /= near_point_world.w;
    if (far_point_world.w != 0.0f) far_point_world /= far_point_world.w;

    glm::vec3 ray_origin = glm::vec3(near_point_world);
    glm::vec3 ray_direction = glm::normalize(glm::vec3(far_point_world) - ray_origin);

    // 使用空间哈希网格进行射线-AABB相交检测 + 光线行进
    // O(K) 复杂度，K 为光线穿过的单元格内高斯数量（通常 << N）
    float t_min, t_max;
    if (!hover_grid_.rayAABB(ray_origin, ray_direction, t_min, t_max)) {
        // 射线不穿过场景包围盒 → 默认光标
        if (current_cursor_type_ != 0) {
            window->setCursor(0);
            current_cursor_type_ = 0;
        }
        return;
    }

    // 在场景包围盒内沿光线行进，搜索最近的高斯
    float hover_threshold_sq = 0.05f * 0.05f;  // 5cm 阈值
    float min_distance_sq = hover_threshold_sq;
    uint32_t closest_gaussian = UINT32_MAX;

    // 光线行进步长 = cell_size (避免跳过单元格)
    float t_step = hover_grid_.cell_size;
    float t_limit = std::min(t_max, t_min + 5.0f);  // 最多行进5米

    for (float t = t_min; t < t_limit; t += t_step) {
        glm::vec3 point_on_ray = ray_origin + ray_direction * t;
        int32_t cx, cy, cz;
        hover_grid_.getCellCoords(point_on_ray, cx, cy, cz);

        // 检查当前单元格及相邻单元格（3x3x3邻域）
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    auto key = hover_grid_.cellKey(cx + dx, cy + dy, cz + dz);
                    auto it = hover_grid_.cells.find(key);
                    if (it == hover_grid_.cells.end()) continue;

                    for (uint32_t idx : it->second) {
                        const glm::vec3& pos = scene->cpuPositions[idx];

                        // 计算点到射线的距离
                        glm::vec3 v = pos - ray_origin;
                        float projection = glm::dot(v, ray_direction);
                        if (projection < 0) continue;

                        glm::vec3 closest_point = ray_origin + ray_direction * projection;
                        glm::vec3 diff = pos - closest_point;
                        float distance_sq = glm::dot(diff, diff);

                        if (distance_sq < min_distance_sq) {
                            min_distance_sq = distance_sq;
                            closest_gaussian = idx;
                        }
                    }
                }
            }
        }

        // 如果找到了非常近的高斯，提前退出
        if (closest_gaussian != UINT32_MAX && min_distance_sq < 0.001f) {
            break;
        }
    }

    // 根据找到的高斯是否在可变形区域来设置光标
    int cursor_type = 0;  // 默认光标
    if (closest_gaussian != UINT32_MAX && closest_gaussian < sim_mask_.size()) {
        if (sim_mask_[closest_gaussian]) {
            cursor_type = 1;  // 手形 - 可变形区域
        } else {
            cursor_type = 2;  // 十字 - 非可变形区域
        }
    }

    if (cursor_type != current_cursor_type_) {
        spdlog::debug("[Renderer] Cursor CHANGE: {} -> type {} (gaussian={}, sim_mask={})",
                     current_cursor_type_, cursor_type,
                     closest_gaussian != UINT32_MAX ? std::to_string(closest_gaussian) : "NONE",
                     closest_gaussian != UINT32_MAX && closest_gaussian < sim_mask_.size()
                        ? (sim_mask_[closest_gaussian] ? "DEF" : "STATIC") : "N/A");
        current_cursor_type_ = cursor_type;
        window->setCursor(cursor_type);
    }
}

Renderer::~Renderer() {
}
