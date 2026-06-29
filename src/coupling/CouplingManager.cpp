#include "CouplingManager.h"
#include "../vulkan/VulkanContext.h"
#include "../vulkan/Buffer.h"
#include "../vulkan/Shader.h"
#include <spdlog/spdlog.h>
#include <algorithm>

CouplingManager::CouplingManager(std::shared_ptr<VulkanContext> context)
    : context_(context) {
    spdlog::info("[CouplingManager] CouplingManager created");

    // 创建组件
    displacement_mapper_ = std::make_unique<DisplacementMapper>(context_);
    particle_mapper_ = std::make_unique<GaussianParticleMapper>();
}

CouplingManager::~CouplingManager() {
    spdlog::info("[CouplingManager] CouplingManager destroyed");
}

void CouplingManager::Initialize(
    const std::shared_ptr<GSScene>& scene,
    const std::shared_ptr<MPM::MPMManager>& mpm_manager,
    const Config& config
) {
    spdlog::info("[CouplingManager] Initializing coupling system");

    config_ = config;
    mpm_manager_ = mpm_manager;

    // 初始化 displacement_mapper
    displacement_mapper_->Initialize();

    // 获取可变形区域
    const auto& region = mpm_manager->GetRegion();
    deformable_indices_ = region.deformable_indices;

    spdlog::info("[CouplingManager] Deformable gaussians: {}", deformable_indices_.size());
    spdlog::info("[CouplingManager] Drive particles: {}", mpm_manager->GetParticleCount());

    // 安全检查：如果没有可变形高斯，跳过缓冲区创建
    if (deformable_indices_.empty()) {
        spdlog::warn("[CouplingManager] No deformable gaussians, skipping buffer creation");
        initialized_ = false;
        return;
    }

    // 1. 创建原始数据缓冲区（高斯原始位置和旋转）
    CreateOriginalDataBuffers(scene, region.GenerateSimMask(scene->getNumVertices()));

    // 2. 创建输出缓冲区（高斯新位置和旋转）
    CreateOutputBuffers(scene);

    // 3. 预计算 Top-K 映射
    PrecomputeTopKMapping(scene, mpm_manager);

    // 4. 创建 Top-K 权重缓冲区
    CreateTopKWeightBuffer();

    // 5. 创建驱动粒子位移缓冲区（vec4格式，stride=16匹配std430规则）
    uint32_t num_particles = mpm_manager->GetParticleCount();
    vk::BufferUsageFlags usage_flags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc |
        vk::BufferUsageFlagBits::eTransferDst;

    drive_displacement_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(num_particles * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 6. 设置 displacement_mapper 的资源
    displacement_mapper_->SetResources(
        deformable_index_buffer_,
        drive_position_buffer_,
        drive_displacement_buffer_,
        top_k_index_buffer_,
        top_k_weight_buffer_,
        original_position_buffer_,
        original_rotation_buffer_,
        gaussian_position_buffer_,
        gaussian_rotation_buffer_
    );

    // 7. 初始化粒子位移数组
    particle_displacements_.resize(num_particles, glm::vec3(0.0f));

    stats_.num_deformable_gaussians = static_cast<uint32_t>(deformable_indices_.size());
    stats_.num_drive_particles = num_particles;

    initialized_ = true;

    spdlog::info("[CouplingManager] Initialization complete");
}

void CouplingManager::CreateOriginalDataBuffers(
    const std::shared_ptr<GSScene>& scene,
    const std::vector<bool>& sim_mask
) {
    spdlog::info("[CouplingManager] Creating original data buffers");

    uint64_t num_gaussians = scene->getNumVertices();

    // 准备原始位置和旋转数据（vec4格式，stride=16匹配std430规则）
    std::vector<glm::vec4> original_positions(num_gaussians);
    std::vector<glm::vec4> original_rotations(num_gaussians);

    // 从场景CPU数据复制位置和旋转
    for (size_t i = 0; i < num_gaussians && i < scene->cpuPositions.size(); i++) {
        original_positions[i] = glm::vec4(scene->cpuPositions[i], 1.0f);  // vec4 for std430 alignment
        original_rotations[i] = (i < scene->cpuRotations.size())
            ? scene->cpuRotations[i]              // PLY真实旋转 (w,x,y,z)
            : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // fallback: 单位四元数
    }

    // 创建缓冲区（vec4格式，stride=16匹配std430规则）
    vk::BufferUsageFlags usage_flags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;

    original_position_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    original_rotation_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 上传数据
    original_position_buffer_->upload(
        reinterpret_cast<const char*>(original_positions.data()),
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        0
    );

    original_rotation_buffer_->upload(
        reinterpret_cast<const char*>(original_rotations.data()),
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        0
    );

    spdlog::debug("[CouplingManager] Original data buffers created: {} gaussians (vec4 format)", num_gaussians);
}

void CouplingManager::CreateOutputBuffers(const std::shared_ptr<GSScene>& scene) {
    spdlog::info("[CouplingManager] Creating output buffers");

    uint64_t num_gaussians = scene->getNumVertices();

    // 创建位置和旋转输出缓冲区
    vk::BufferUsageFlags usage_flags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc |
        vk::BufferUsageFlagBits::eTransferDst;

    gaussian_position_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 初始化位置缓冲区为原始位置（vec4 w=1.0），确保背景高斯也有有效数据
    // 这样shader只写入前景位置时，背景位置不会被garbage覆盖
    std::vector<glm::vec4> default_positions(num_gaussians);
    for (size_t i = 0; i < num_gaussians && i < scene->cpuPositions.size(); i++) {
        default_positions[i] = glm::vec4(scene->cpuPositions[i], 1.0f);
    }
    gaussian_position_buffer_->upload(
        reinterpret_cast<const char*>(default_positions.data()),
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        0
    );

    gaussian_rotation_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 初始化旋转缓冲区为PLY真实旋转，确保背景高斯旋转也有有效数据
    std::vector<glm::vec4> default_rotations(num_gaussians);
    for (size_t i = 0; i < num_gaussians && i < scene->cpuRotations.size(); i++) {
        default_rotations[i] = scene->cpuRotations[i];
    }
    // Fallback for any missing data
    for (size_t i = scene->cpuRotations.size(); i < num_gaussians; i++) {
        default_rotations[i] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    gaussian_rotation_buffer_->upload(
        reinterpret_cast<const char*>(default_rotations.data()),
        static_cast<uint32_t>(num_gaussians * sizeof(glm::vec4)),
        0
    );

    spdlog::debug("[CouplingManager] Output buffers created and initialized with {} default positions/rotations",
                  num_gaussians);
}

void CouplingManager::PrecomputeTopKMapping(
    const std::shared_ptr<GSScene>& scene,
    const std::shared_ptr<MPM::MPMManager>& mpm_manager
) {
    spdlog::info("[CouplingManager] Precomputing Top-K mapping");

    // 获取驱动粒子位置
    std::vector<glm::vec3> drive_positions = mpm_manager->GetParticlePositions();

    // CRITICAL FIX: Convert gaussian positions to normalized space before KNN
    // (matching PhysDreamer Python: sim_gaussian_pos = (xyz + shift) / scale)
    // Previously, deformable_positions were in WORLD SPACE while drive_positions
    // were in NORMALIZED SPACE → completely wrong KNN neighbors → garbage rigid
    // transform → "giant black ellipsoid" bug
    std::vector<glm::vec3> deformable_positions_norm;
    for (uint32_t idx : deformable_indices_) {
        if (idx < scene->cpuPositions.size()) {
            glm::vec3 world_pos = scene->cpuPositions[idx];
            glm::vec3 norm_pos = (world_pos + coord_shift_) / coord_scale_;
            deformable_positions_norm.push_back(norm_pos);
        }
    }

    // 预计算映射（both sides now in normalized space）
    particle_mapper_->SetDeformableIndices(deformable_indices_);
    particle_mapper_->PrecomputeMapping(deformable_positions_norm, drive_positions, 8);

    // 上传到GPU（需要VkCommandBuffer，这里先创建缓冲区）
    // 实际上传将在 Initialize 中通过 descriptor set 绑定完成
    particle_mapper_->UploadMappingToGPU(context_, VK_NULL_HANDLE);

    // 获取KNN缓冲区
    top_k_index_buffer_ = particle_mapper_->GetKNNGPUBuffer();
    deformable_index_buffer_ = particle_mapper_->GetDeformableIndexBuffer();

    // 创建驱动粒子位置缓冲区（vec4格式，stride=16匹配std430规则）
    vk::BufferUsageFlags usage_flags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;

    drive_position_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(drive_positions.size() * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 上传驱动粒子位置（vec4格式）
    std::vector<glm::vec4> drive_pos_v4(drive_positions.size());
    for (size_t i = 0; i < drive_positions.size(); i++) {
        drive_pos_v4[i] = glm::vec4(drive_positions[i], 1.0f);
    }
    drive_position_buffer_->upload(
        reinterpret_cast<const char*>(drive_pos_v4.data()),
        static_cast<uint32_t>(drive_positions.size() * sizeof(glm::vec4)),
        0
    );

    spdlog::info("[CouplingManager] Top-K mapping precomputed");
}

void CouplingManager::CreateTopKWeightBuffer() {
    spdlog::info("[CouplingManager] Creating Top-K weight buffer (inverse-distance)");

    uint32_t num_deformable = static_cast<uint32_t>(deformable_indices_.size());

    // 从 GaussianParticleMapper 获取预计算的 inverse-distance 权重
    // 之前使用全1.0均匀权重，导致位移被1/K稀释（见根因分析）
    // 现在使用 inverse-distance-squared 权重，近处粒子贡献更大
    const auto& knn_mappings = particle_mapper_->GetKNNMappings();

    // 创建权重数据（每个高斯2个vec4存储8个权重）
    std::vector<glm::vec4> weight_data(num_deformable * 2);
    for (uint32_t i = 0; i < num_deformable; i++) {
        if (i < knn_mappings.size()) {
            // 使用预计算的 inverse-distance 权重
            weight_data[i * 2 + 0] = glm::vec4(knn_mappings[i].weights[0],
                                                knn_mappings[i].weights[1],
                                                knn_mappings[i].weights[2],
                                                knn_mappings[i].weights[3]);
            weight_data[i * 2 + 1] = glm::vec4(knn_mappings[i].weights[4],
                                                knn_mappings[i].weights[5],
                                                knn_mappings[i].weights[6],
                                                knn_mappings[i].weights[7]);
        } else {
            // fallback: 均匀权重
            weight_data[i * 2 + 0] = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            weight_data[i * 2 + 1] = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    // 创建缓冲区
    vk::BufferUsageFlags usage_flags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;

    top_k_weight_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(weight_data.size() * sizeof(glm::vec4)),
        usage_flags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    // 上传权重
    top_k_weight_buffer_->upload(
        reinterpret_cast<const char*>(weight_data.data()),
        static_cast<uint32_t>(weight_data.size() * sizeof(glm::vec4)),
        0
    );

    spdlog::debug("[CouplingManager] Top-K weight buffer created");
}

void CouplingManager::DownloadParticleDisplacements() {
    // 从MPM GPU缓冲区下载粒子位移
    // 注意：这需要在GPU-CPU同步后进行
    particle_displacements_ = mpm_manager_->GetParticleDisplacements();

    // 计算平均位移统计
    if (!particle_displacements_.empty()) {
        float total_disp = 0.0f;
        for (const auto& disp : particle_displacements_) {
            total_disp += glm::length(disp);
        }
        stats_.avg_displacement = total_disp / particle_displacements_.size();
    }
}

void CouplingManager::UploadParticleDisplacements() {
    if (particle_displacements_.empty()) {
        return;
    }

    // 转换vec3位移到vec4格式（stride=16匹配GPU缓冲区和std430规则）
    std::vector<glm::vec4> disp_v4(particle_displacements_.size());
    for (size_t i = 0; i < particle_displacements_.size(); i++) {
        disp_v4[i] = glm::vec4(particle_displacements_[i], 0.0f);
    }

    // 上传粒子位移到GPU
    drive_displacement_buffer_->upload(
        reinterpret_cast<const char*>(disp_v4.data()),
        static_cast<uint32_t>(disp_v4.size() * sizeof(glm::vec4)),
        0
    );
}

void CouplingManager::ComputeParticleDisplacementsGPU(VkCommandBuffer cmd) {
    if (!mpm_manager_) {
        spdlog::error("[CouplingManager] Cannot compute displacements: no MPM manager");
        return;
    }

    // 第一次调用时创建pipeline和descriptor set
    if (!displacement_pipeline_created_) {
        spdlog::info("[CouplingManager] Creating displacement computation pipeline");

        auto shader = std::make_shared<Shader>(context_, "compute_particle_displacements");
        displacement_compute_pipeline_ = std::make_shared<ComputePipeline>(context_, shader);

        // Descriptor set bindings
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: CurrentParticles (readonly)
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: InitialPositions (readonly)
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 2: Displacements (write)
            vk::DescriptorSetLayoutBinding()
                .setBinding(2)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        for (const auto& binding : bindings) {
            displacement_compute_pipeline_->addDescriptorSetLayoutBinding(binding);
        }

        // Push constant: num_particles (uint32_t)
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));
        displacement_compute_pipeline_->addPushConstant(
            pushConstantRange.stageFlags, pushConstantRange.offset, pushConstantRange.size);

        displacement_compute_pipeline_->build();

        // Create descriptor set and bind buffers
        displacement_compute_descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);

        displacement_compute_descriptor_set_->bindBufferToDescriptorSet(
            0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
            mpm_manager_->GetParticleBuffer());

        displacement_compute_descriptor_set_->bindBufferToDescriptorSet(
            1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
            mpm_manager_->GetInitialPosBuffer());

        displacement_compute_descriptor_set_->bindBufferToDescriptorSet(
            2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
            drive_displacement_buffer_);

        displacement_compute_descriptor_set_->build();

        displacement_compute_pipeline_->addDescriptorSet(0, displacement_compute_descriptor_set_);

        displacement_pipeline_created_ = true;
        spdlog::info("[CouplingManager] Displacement computation pipeline created");
    }

    // CRITICAL FIX: Bind pipeline and descriptor set before push constants and dispatch.
    // Without this, the GPU uses whatever pipeline was last bound (e.g. G2P from MPM substep)
    // with wrong descriptor bindings → displacement shader never actually runs →
    // drive_displacement_buffer_ stays zero → no visual change on physics interaction.
    displacement_compute_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));

    // Push constants
    uint32_t num_particles = mpm_manager_->GetParticleCount();
    vkCmdPushConstants(cmd, displacement_compute_pipeline_->pipelineLayout.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(num_particles), &num_particles);

    // Dispatch
    uint32_t workgroups = (num_particles + 255) / 256;
    vkCmdDispatch(cmd, workgroups, 1, 1);

    static bool logged_first_dispatch = false;
    if (!logged_first_dispatch) {
        spdlog::info("[CouplingManager] ✓ First displacement compute dispatched: {} workgroups for {} particles",
                     workgroups, num_particles);
        logged_first_dispatch = true;
    } else {
        spdlog::trace("[CouplingManager] Displacement compute dispatched: {} workgroups for {} particles",
                      workgroups, num_particles);
    }
}

void CouplingManager::ApplyDirectDisplacement(
    VkCommandBuffer cmd,
    uint32_t target_particle,
    const glm::vec3& displacement,
    float influence_radius,
    uint32_t num_particles,
    bool zero_all
) {
    if (!initialized_) {
        spdlog::error("[CouplingManager] Cannot apply displacement: not initialized");
        return;
    }

    // 第一次调用时创建pipeline和descriptor set
    if (!displacement_application_pipeline_created_) {
        spdlog::info("[CouplingManager] Creating direct displacement application pipeline");

        auto shader = std::make_shared<Shader>(context_, "apply_mouse_displacement");
        displacement_application_pipeline_ = std::make_shared<ComputePipeline>(context_, shader);

        // Descriptor set bindings
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: DrivePositions (readonly - for distance computation)
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: DriveDisplacements (write)
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        for (const auto& binding : bindings) {
            displacement_application_pipeline_->addDescriptorSetLayoutBinding(binding);
        }

        // Push constants: target_idx(4) + displacement(12) + radius(4) + num_particles(4) + mode(4) = 28 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute, 0, 28);
        displacement_application_pipeline_->addPushConstant(
            pushConstantRange.stageFlags, pushConstantRange.offset, pushConstantRange.size);

        displacement_application_pipeline_->build();

        // Create descriptor set and bind buffers
        displacement_application_descriptor_set_ = std::make_shared<DescriptorSet>(context_, 1);

        displacement_application_descriptor_set_->bindBufferToDescriptorSet(
            0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
            drive_position_buffer_);

        displacement_application_descriptor_set_->bindBufferToDescriptorSet(
            1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute,
            drive_displacement_buffer_);

        displacement_application_descriptor_set_->build();

        displacement_application_pipeline_->addDescriptorSet(0, displacement_application_descriptor_set_);

        displacement_application_pipeline_created_ = true;
        spdlog::info("[CouplingManager] Direct displacement application pipeline created");
    }

    // Push constants（vec3必须在开头，确保std430 16字节对齐）
    struct DisplacementParams {
        glm::vec3 target_displacement;  // offset 0 (vec3 alignment=16 in std430)
        float influence_radius;         // offset 12
        uint32_t target_particle_idx;   // offset 16
        uint32_t num_particles;         // offset 20
        uint32_t mode;                  // offset 24
    };

    DisplacementParams params;
    params.target_displacement = displacement;
    params.influence_radius = influence_radius;
    params.target_particle_idx = target_particle;
    params.num_particles = num_particles;
    params.mode = zero_all ? 1u : 0u;

    // 绑定pipeline和descriptor set
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      displacement_application_pipeline_->pipeline.get());
    displacement_application_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));

    vkCmdPushConstants(cmd, displacement_application_pipeline_->pipelineLayout.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

    // Dispatch
    uint32_t workgroups = (num_particles + 255) / 256;
    vkCmdDispatch(cmd, workgroups, 1, 1);

    spdlog::trace("[CouplingManager] Direct displacement applied: particle={}, disp=({:.4f},{:.4f},{:.4f}), radius={:.4f}, mode={}",
                  target_particle, displacement.x, displacement.y, displacement.z,
                  influence_radius, params.mode);
}

void CouplingManager::Couple(VkCommandBuffer cmd, uint32_t frame_index, bool compute_displacements_from_mpm) {
    if (!initialized_ || !enabled_) {
        return;
    }

    spdlog::trace("[CouplingManager] Executing coupling for frame {} (mpm_compute={})",
                  frame_index, compute_displacements_from_mpm);

    // 1. 如果需要，从MPM粒子计算位移
    if (compute_displacements_from_mpm) {
        ComputeParticleDisplacementsGPU(cmd);

        // Memory barrier: 确保位移数据写入完成
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    // 如果 compute_displacements_from_mpm=false，
    // drive_displacement_buffer_ 已由 ApplyDirectDisplacement 设置，
    // 调用方应确保已添加了适当的 memory barrier

    // 3. 调用位移映射shader（传入坐标变换参数）
    displacement_mapper_->MapDisplacements(
        cmd,
        static_cast<uint32_t>(deformable_indices_.size()),
        static_cast<uint32_t>(mpm_manager_->GetParticleCount()),
        coord_scale_,
        coord_shift_,
        DisplacementMapper::Config{config_.displacement_scale, config_.use_rigid_transform}
    );

    // 4. Memory barrier: 确保高斯数据写入完成
    VkMemoryBarrier gauss_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    gauss_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    gauss_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &gauss_barrier, 0, nullptr, 0, nullptr);

    spdlog::trace("[CouplingManager] Coupling complete for frame {}", frame_index);
}
