#include "Renderer.h"

#include <fstream>

#include "vulkan/Swapchain.h"

#include <memory>
#include "shaders.h"
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "vulkan/Utils.h"
#include "GaussianModel.h"

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

void Renderer::initialize() {
    initializeVulkan();
    createGui();

    // MPM Manager will be initialized later when VkCommandBuffer compatibility is resolved
    // mpm_manager_ = std::make_shared<MPM::MPMManager>(context);
    spdlog::info("[Renderer] MPM Manager initialization deferred (pending type compatibility fix)");

    loadSceneToGPU();
    createPreprocessPipeline();
    createPrefixSumPipeline();
    createRadixSortPipeline();
    createPreprocessSortPipeline();
    createTileBoundaryPipeline();
    createRenderPipeline();
    createCommandPool();
    recordPreprocessCommandBuffer();

    // 初始化交互系统（如果MPM已初始化）
    if (mpm_initialized_) {
        initializeInteractionSystem();
    }
}

void Renderer::handleInput() {
    auto translation = window->getCursorTranslation();
    auto keys = window->getKeys(); // W, A, S, D

    if ((!configuration.enableGui || (!guiManager.wantCaptureMouse() && !guiManager.mouseCapture)) && window->
        getMouseButton()[0]) {
        window->mouseCapture(true);
        guiManager.mouseCapture = true;
    }

    // rotate camera
    if (!configuration.enableGui || guiManager.mouseCapture) {
        if (translation[0] != 0.0 || translation[1] != 0.0) {
            camera.rotation = glm::rotate(camera.rotation, static_cast<float>(translation[0]) * 0.005f,
                                          glm::vec3(0.0f, -1.0f, 0.0f));
            camera.rotation = glm::rotate(camera.rotation, static_cast<float>(translation[1]) * 0.005f,
                                          glm::vec3(-1.0f, 0.0f, 0.0f));
        }
    }

    // move camera
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

    context->createLogicalDevice(pdf, pdf11, pdf12);
    context->createDescriptorPool(FRAMES_IN_FLIGHT);  // 更新为3帧

    swapchain = std::make_shared<Swapchain>(context, window, configuration.immediateSwapchain);

    // 创建帧inflight fences
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        inflightFences.emplace_back(
            context->device->createFenceUnique(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)));
    }

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
                    mpm_config.material.E = 2140628.25f;   // carnation默认值
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

    // Step 7: Create visibility mask buffer
    // This is done in createPreprocessPipeline(), but we log here for clarity
    spdlog::info("[Renderer] ===== Gaussian Model Initialization Complete =====");

    // reset descriptor pool
    context->device->resetDescriptorPool(context->descriptorPool.get());
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
    inputSet->build();
    preprocessPipeline->addDescriptorSet(0, inputSet);

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
    const uint64_t expectedValue = getExpectedFrameValue();

    // 1. 等待该索引处的上一帧完成（CPU-GPU同步）
    auto ret = context->device->waitForFences(inflightFences[frameIdx].get(), VK_TRUE, UINT64_MAX);
    if (ret != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for fence");
    }
    context->device->resetFences(inflightFences[frameIdx].get());

    // 2. 获取下一个交换链图像
    // 注意：使用 fence 而非信号量来同步，避免信号量索引与图像索引的对应问题
    // 当前的 fence 已经在前面等待过了，所以可以重用
    auto res = context->device->acquireNextImageKHR(swapchain->swapchain.get(), UINT64_MAX,
                                                    vk::Semaphore(), inflightFences[frameIdx].get(),
                                                    &currentImageIndex);
    if (res == vk::Result::eErrorOutOfDateKHR) {
        recreateSwapchain();
        return;
    } else if (res != vk::Result::eSuccess && res != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

startOfRenderLoop:
    handleInput();

    // 处理物理交互输入
    handlePhysicsInteraction();

    updateUniforms();

    // Sync render mode from GUI
    renderForegroundOnly_ = guiManager.renderBackgroundOnly;

    // 3. 提交预处理工作（使用第一个命令缓冲区，所有帧共用）
    auto preprocessCmd = preprocessCommandBuffers[0].get();
    auto preprocessSubmit = vk::SubmitInfo{}.setCommandBuffers(preprocessCmd);
    context->queues[VulkanContext::Queue::COMPUTE].queue.submit(preprocessSubmit, inflightFences[frameIdx].get());

    // 注意：这里仍然需要等待预处理完成，因为后续渲染需要预处理结果
    // 这是架构限制，真正的并行需要阶段2的graphics管线
    ret = context->device->waitForFences(inflightFences[frameIdx].get(), VK_TRUE, UINT64_MAX);
    if (ret != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for preprocess fence");
    }
    context->device->resetFences(inflightFences[frameIdx].get());

    // 执行物理仿真（在渲染之前）
    if (mpm_initialized_ && mpm_manager_ && mpm_manager_->IsEnabled()) {
        auto& renderCmd = renderCommandBuffers[frameIdx];
        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(renderCmd.get(), &beginInfo);
        updatePhysicsSimulation(renderCmd.get());
        vkEndCommandBuffer(renderCmd.get());
    }

    // 4. 记录并提交渲染命令
    if (!recordRenderCommandBuffer(frameIdx)) {
        goto startOfRenderLoop;
    }

    auto renderCmd = renderCommandBuffers[frameIdx].get();

    // 不再等待图像可用信号量（改用 fence 同步）
    // 只需要等待渲染完成的时间线信号量用于 present
    vk::Semaphore renderSemaphore = frameTimelineSemaphores[frameIdx]->getHandle();

    vk::SubmitInfo renderSubmit{};
    renderSubmit.waitSemaphoreCount = 0;  // 不等待信号量，fence 已经保证了同步
    renderSubmit.pWaitSemaphores = nullptr;
    renderSubmit.pWaitDstStageMask = nullptr;
    renderSubmit.commandBufferCount = 1;
    renderSubmit.pCommandBuffers = &renderCmd;
    renderSubmit.signalSemaphoreCount = 1;
    renderSubmit.pSignalSemaphores = &renderSemaphore;

    context->queues[VulkanContext::Queue::COMPUTE].queue.submit(renderSubmit, inflightFences[frameIdx].get());

    // 处理截图请求
    if (screenshotRequested && !screenshotSaving) {
        screenshotRequested = false;
        screenshotSaving = true;  // 设置保存标志，阻塞新的截图请求
        context->device->waitForFences(inflightFences[frameIdx].get(), VK_TRUE, UINT64_MAX);
        screenshotCounter++;
        std::string screenshotPath = "screenshot_" + std::to_string(screenshotCounter) + ".png";
        saveScreenshot(screenshotPath);
    }

    // 5. 呈现（等待渲染完成）
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

    // 6. 推进帧索引
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

        imguiManager->draw(cmdBuffer.get(), currentImageIndex, std::bind(&GUIManager::buildGui, &guiManager));

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

    // 创建拖拽处理器
    Interaction::DragHandler::Config drag_config;
    drag_config.stiffness = 50.0f;
    drag_config.max_force = 100.0f;
    drag_handler_ = std::make_shared<Interaction::DragHandler>(context, drag_config);
    drag_handler_->Initialize();

    spdlog::info("[Renderer] Interaction system initialized successfully");
}

void Renderer::handlePhysicsInteraction() {
    if (!mpm_initialized_ || !ray_caster_ || !drag_handler_) {
        return;
    }

    auto keys = window->getKeys();
    auto mouse_buttons = window->getMouseButton();
    auto cursor_pos = window->getCursorPosition();

    // 检测物理交互触发条件：P键 + 右键
    bool p_key_held = keys[8];  // P键索引
    bool right_mouse_down = mouse_buttons[2];

    // 检测鼠标按下/释放事件（边沿检测）
    mouse_pressed_this_frame_ = right_mouse_down && !prev_right_mouse_down_;
    mouse_released_this_frame_ = !right_mouse_down && prev_right_mouse_down_;

    // 保存当前鼠标位置
    last_mouse_position_ = glm::ivec2(static_cast<int>(cursor_pos[0]), static_cast<int>(cursor_pos[1]));
    prev_right_mouse_down_ = right_mouse_down;

    // 处理鼠标按下事件（射线拾取需要命令缓冲区，延迟到 updatePhysicsSimulation）
    if (p_key_held && mouse_pressed_this_frame_) {
        mouse_pos_on_press_ = last_mouse_position_;
        physics_interaction_mode_ = true;
    }

    // 处理鼠标释放事件
    if (mouse_released_this_frame_ && physics_interaction_mode_) {
        drag_handler_->OnMouseUp();
        physics_interaction_mode_ = false;
        is_dragging_ = false;
    }

    // 处理鼠标移动事件（在拖拽中）
    if (physics_interaction_mode_ && right_mouse_down) {
        drag_handler_->OnMouseMove(last_mouse_position_.x, last_mouse_position_.y);
        is_dragging_ = drag_handler_->IsDragging();
    }
}

void Renderer::updatePhysicsSimulation(VkCommandBuffer cmd) {
    if (!mpm_initialized_ || !mpm_manager_ || !mpm_manager_->IsEnabled()) {
        return;
    }

    // 处理鼠标按下时的射线拾取
    if (mouse_pressed_this_frame_ && physics_interaction_mode_ && ray_caster_ && drag_handler_) {
        auto [fb_width, fb_height] = window->getFramebufferSize();

        // 计算视图投影矩阵（与 updateUniforms 相同）
        auto rotation = glm::mat4_cast(camera.rotation);
        auto translation = glm::translate(glm::mat4(1.0f), camera.position);
        auto view = glm::inverse(translation * rotation);

        float tan_fovx = std::tan(glm::radians(camera.fov) / 2.0);
        float tan_fovy = tan_fovx * static_cast<float>(fb_height) / static_cast<float>(fb_width);
        auto proj = glm::perspective(std::atan(tan_fovy) * 2.0f,
                                     static_cast<float>(fb_width) / static_cast<float>(fb_height),
                                     camera.nearPlane,
                                     camera.farPlane);
        glm::mat4 view_proj = proj * view;

        drag_handler_->OnMouseDown(
            mouse_pos_on_press_.x,
            mouse_pos_on_press_.y,
            *ray_caster_,
            fb_width,
            fb_height,
            view_proj,
            mpm_manager_->GetParticleBuffer(),
            mpm_manager_->GetParticleCount(),
            cmd
        );
    }

    // 如果正在拖拽，应用拖拽力
    if (drag_handler_ && drag_handler_->IsDragging()) {
        float dt = 1.0f / 30.0f;  // 固定物理时间步
        drag_handler_->ApplyForce(cmd, mpm_manager_->GetParticleBuffer(), dt);
    }

    // 执行MPM物理步进
    mpm_manager_->Step(cmd, 1.0f / 30.0f);
}

Renderer::~Renderer() {
}
