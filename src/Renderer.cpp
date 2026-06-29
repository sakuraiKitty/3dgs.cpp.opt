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

    // 移除鼠标移动控制相机旋转的功能
    // 鼠标仅用于：光标悬停检测 + 物理交互（P + 左键）
    // 相机只能通过键盘控制

    // move camera (键盘控制)
    if (!configuration.enableGui || !guiManager.wantCaptureKeyboard()) {
        glm::vec3 direction = glm::vec3(0.0f, 0.0f, 0.0f);
        if (keys[0]) {
            direction += glm::vec3(0.0f, 0.0f, -1.0f);
        }
        if (keys[1]) {
            direction += glm::vec3(-1.0f, 0.0f, 0.0f);
        }
        if (keys[2]) {
            direction += glm::vec3(0.0f, 0.0f, 1.0f);
        }
        if (keys[3]) {
            direction += glm::vec3(1.0f, 0.0f, 0.0f);
        }
        if (keys[4]) {
            direction += glm::vec3(0.0f, 1.0f, 0.0f);
        }
        if (keys[5]) {
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
            camera.position += (glm::mat4_cast(camera.rotation) * glm::vec4(direction, 1.0f)).xyz() * 0.3f;
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

    // 创建时间线信号量（每帧一个）
    frameTimelineSemaphores.reserve(FRAMES_IN_FLIGHT);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        frameTimelineSemaphores.emplace_back(
            std::make_unique<TimelineSemaphore>(*context->device, 0));
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
                    MPM::MPMInitializer::Config mpm_config;
                    mpm_config.grid_size = 64;
                    mpm_config.downsample_scale = 0.1f;
                    mpm_config.use_internal_fill = true;
                    mpm_config.material.E = 2140628.25f;   // carnation原版值 (carnation.py init_young)
                    mpm_config.material.nu = 0.3f;
                    mpm_config.material.density = 2000.0f;

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

                        // 配置MPM参数
                        MPM::MPMManager::Config mpm_config;
                        mpm_config.grid_size = 64;
                        mpm_config.dt = 1.0f / 30.0f;
                        mpm_config.substeps = 128;     // 原版carnation.py substep=768(离线)；实时折中128
                                                      // CFL: E=2.14MPa→c_p≈38, dx=1/64, sub_dt=(1/30)/128=0.00026 < dx/c_p=0.00041 ✓
                        mpm_config.damping = 0.999f;    // 0.999^128≈0.88 阻尼合理
                        // 原版carnation.py无gravity字段——花由冻结茎支撑处于静止平衡，变形只来自交互力
                        // 之前-2是调试值，驱动冻结边界应力反馈爆炸→粒子甩飞→花头散点
                        mpm_config.gravity = {0.0f, 0.0f, 0.0f};

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
    // 关键修复：不再使用 inflightFences 作为 acquire 的信号fence
    // 原来的 bug: acquireNextImageKHR 用 inflightFences 信号化后,
    // preprocess submit 无法再次信号化同一fence → preprocess 等待无效 → 读取垃圾数据
    // 修复: 使用 null fence（UINT64_MAX timeout 已经阻塞等待直到图像可用）
    auto res = context->device->acquireNextImageKHR(swapchain->swapchain.get(), UINT64_MAX,
                                                    vk::Semaphore(), vk::Fence(),
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
    // Physics updates: ApplyDrag(if dragging) → MPM Step → displacement → coupling → override buffers
    // MPM持续运行（拖拽时 + 非拖拽时都运行，自然回弹靠弹性力+阻尼）
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

    // 6. 提交渲染命令（使用inflightFences进行跨帧同步）
    vk::Semaphore renderSemaphore = frameTimelineSemaphores[frameIdx]->getHandle();

    vk::SubmitInfo renderSubmit{};
    renderSubmit.commandBufferCount = 1;
    renderSubmit.pCommandBuffers = &renderCmd;
    renderSubmit.signalSemaphoreCount = 1;
    renderSubmit.pSignalSemaphores = &renderSemaphore;

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

    // 7. 呈现（等待渲染完成）
    vk::Semaphore timelineHandle = frameTimelineSemaphores[frameIdx]->getHandle();
    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &timelineHandle;
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

    // 创建拖拽处理器（力驱动MPM模式 — force作为加速度）
    Interaction::DragHandler::Config drag_config;
    // dragRadius=0.2: 拖拽作用域覆盖更大花头区域，降低速度梯度 ∇v=dragVel/radius → F 不再越界
    //   (0.15 + 16 norm/s → ∇v=110/s → 1帧 J×e^10 爆炸 → 拖拽区与主体断裂 → “上下分离”)
    drag_config.dragRadius = 0.2f;   // 拖拽作用半径（世界空间单位）
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

    // 添加日志来检测按键状态
    static int log_counter = 0;
    if (p_key_held && log_counter++ % 10 == 0) { // P键按下时每秒输出6次
        spdlog::debug("[Physics] P key held: {}, Left mouse: {}, Right mouse: {}",
                     p_key_held, mouse_buttons[0], mouse_buttons[2]);
    }

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

    // ── 注入 CFL 限幅参数到 DragHandler（一次性）──
    // cfl=0.05: drag 注入速度限幅 max_vel = 0.05·dx/sub_dt ≈ 3 norm/s
    //   原 cfl=0.5 → max_vel=30 norm/s，拖拽 16.58 norm/s 不受限：
    //     1帧 0.55 norm = 35dx，30帧累积 16.5 norm ≫ 网格域 1.0 → 粒子 clamp 边界 → “上下分离”
    //     ∇v=16.58/0.15=110/s → J 1帧增长 e^10.9 ≈ 54000× → F 爆炸 → 应力断裂
    //   cfl=0.05 → max_vel=3：每帧 0.1 norm=6.4dx，J 增长 e^1.5≈4.5× 安全，弹性可拉回
    static bool cfl_injected = false;
    if (!cfl_injected && drag_handler_) {
        auto& mpm_cfg = mpm_manager_->GetConfig();
        const float frame_dt_cfl = 1.0f / 30.0f;
        const float sub_dt = frame_dt_cfl / static_cast<float>(mpm_cfg.substeps);
        constexpr float kDragCfl = 0.05f;
        drag_handler_->SetCFLParams(mpm_manager_->GetInvDx(), sub_dt, kDragCfl);
        spdlog::info("[PhysicsSim] CFL injected: inv_dx={:.6f}, sub_dt={:.6f}, substeps={}, "
                     "max_velocity={:.4f} (normalized/s)",
                     mpm_cfg.inv_dx, sub_dt, mpm_cfg.substeps,
                     kDragCfl * (1.0f / mpm_cfg.inv_dx) / sub_dt);
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
    if (drag_handler_ && drag_handler_->IsDragging()) {
        spdlog::debug("[PhysicsSim] IsDragging=true → computing drag params");
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

        drag_handler_->ApplyDrag(
            cmd,
            mpm_manager_->GetParticleBuffer(),
            mpm_manager_->GetParticleCount(),
            pushConstants
        );
    } else {
        spdlog::debug("[PhysicsSim] IsDragging=false → skipping ApplyDrag");
    }

    // ── 2. 执行 MPM 物理模拟 ──
    // MPM持续运行（无pin，自然动力学处理回弹）
    mpm_manager_->Step(cmd, frame_dt);

    // ── 3. GPU计算粒子位移 + 耦合映射 ──
    if (coupling_initialized_ && coupling_manager_) {
        // ComputeParticleDisplacementsGPU: displacement = current_pos - initial_pos
        // Couple: MapDisplacements → 高斯位置/旋转更新
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
