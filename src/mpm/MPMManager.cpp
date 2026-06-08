#include "MPMManager.h"
#include "../GSScene.h"
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

namespace MPM {

MPMManager::MPMManager(std::shared_ptr<VulkanContext> context)
    : context_(context) {
    spdlog::info("[MPMManager] MPMManager created");
}

MPMManager::~MPMManager() {
    spdlog::info("[MPMManager] MPMManager destroyed");
}

void MPMManager::Initialize(const Config& config) {
    spdlog::info("[MPMManager] Initializing MPM with grid size={}, substeps={}",
                config.grid_size, config.substeps);

    config_ = config;

    // 计算网格参数
    config_.grid_spacing = config_.grid_lim / static_cast<float>(config_.grid_size);
    config_.inv_dx = 1.0f / config_.grid_spacing;

    // 计算网格节点总数
    grid_total_nodes_ = config_.grid_size * config_.grid_size * config_.grid_size;

    spdlog::info("[MPMManager] Grid spacing: {:.4f}, Total nodes: {}",
                config_.grid_spacing, grid_total_nodes_);

    // 创建 descriptor sets 和 pipelines
    CreateDescriptorSets();
    CreatePipelines();

    initialized_ = true;
}

void MPMManager::LoadParticlesFromScene(
    const std::shared_ptr<GSScene>& scene,
    const std::vector<bool>& sim_mask
) {
    if (!initialized_) {
        spdlog::error("[MPMManager] Cannot load particles: MPM not initialized");
        return;
    }

    spdlog::info("[MPMManager] Loading particles from scene...");

    // 1. 使用粒子生成器生成粒子
    cpu_particles_ = generator_.GenerateFromScene(scene, sim_mask, config_.generator_config);

    num_particles_ = static_cast<uint32_t>(cpu_particles_.size());

    // 2. 保存初始位置（用于计算位移）
    cpu_particle_initial_pos_.reserve(num_particles_);
    for (const auto& p : cpu_particles_) {
        cpu_particle_initial_pos_.push_back(p.position);
    }

    // 3. 创建GPU缓冲区
    CreateParticleBuffer();
    CreateGridBuffer();
    CreateDisplacementBuffer();

    // 4. 上传粒子数据到GPU
    particle_buffer_->upload(cpu_particles_.data(), sizeof(ParticleData) * cpu_particles_.size(), 0);

    spdlog::info("[MPMManager] Loaded {} particles successfully", num_particles_);
}

void MPMManager::LoadParticles(const std::vector<ParticleData>& particles) {
    if (!initialized_) {
        spdlog::error("[MPMManager] Cannot load particles: MPM not initialized");
        return;
    }

    spdlog::info("[MPMManager] Loading {} pre-generated particles", particles.size());

    cpu_particles_ = particles;
    num_particles_ = static_cast<uint32_t>(particles.size());

    // 保存初始位置
    cpu_particle_initial_pos_.reserve(num_particles_);
    for (const auto& p : cpu_particles_) {
        cpu_particle_initial_pos_.push_back(p.position);
    }

    // 创建GPU缓冲区
    CreateParticleBuffer();
    CreateGridBuffer();
    CreateDisplacementBuffer();

    // 上传数据
    particle_buffer_->upload(cpu_particles_.data(), sizeof(ParticleData) * cpu_particles_.size(), 0);

    spdlog::info("[MPMManager] Particles loaded successfully");
}

void MPMManager::Step(VkCommandBuffer cmd, float dt) {
    if (!enabled_ || !initialized_) {
        return;
    }

    // 执行完整的物理步进（包含多个子步）
    float sub_dt = dt / static_cast<float>(config_.substeps);

    for (uint32_t s = 0; s < config_.substeps; s++) {
        Substep(cmd, sub_dt);
    }

    // 记录性能统计（可选）
    static uint64_t frame_count = 0;
    frame_count++;
}

void MPMManager::Reset() {
    spdlog::info("[MPMManager] Resetting simulation...");

    if (!cpu_particle_initial_pos_.empty()) {
        // 重置粒子位置到初始状态
        for (size_t i = 0; i < cpu_particles_.size(); i++) {
            cpu_particles_[i].position = cpu_particle_initial_pos_[i];
            cpu_particles_[i].velocity = glm::vec3(0.0f);
            cpu_particles_[i].deformation_gradient = glm::mat3(1.0f);
        }

        // 上传到GPU
        particle_buffer_->upload(cpu_particles_.data(), sizeof(ParticleData) * cpu_particles_.size(), 0);

        spdlog::info("[MPMManager] Reset complete");
    }
}

std::vector<glm::vec3> MPMManager::GetParticleDisplacements() const {
    std::vector<glm::vec3> displacements(num_particles_);

    for (size_t i = 0; i < num_particles_; i++) {
        displacements[i] = cpu_particles_[i].position - cpu_particle_initial_pos_[i];
    }

    return displacements;
}

void MPMManager::SetRegion(const DeformableRegion& region) {
    region_ = region;
    spdlog::info("[MPMManager] Set deformable region: {} deformable, {} static",
                region.GetDeformableCount(), region.GetStaticCount());
}

void MPMManager::ApplyExternalForces(const std::vector<uint32_t>& particle_indices, const std::vector<glm::vec3>& forces) {
    if (particle_indices.size() != forces.size()) {
        spdlog::error("[MPMManager] ApplyExternalForces: particle_indices size {} != forces size {}",
                     particle_indices.size(), forces.size());
        return;
    }

    // 应用外力到粒子的速度（简化实现）
    for (size_t i = 0; i < particle_indices.size(); i++) {
        uint32_t p_id = particle_indices[i];
        if (p_id < num_particles_) {
            // F = ma => a = F/m => dv = a*dt = (F/m)*dt
            // 这里直接修改速度：v += F/m * dt
            float dt = config_.dt;
            glm::vec3 acceleration = forces[i] / cpu_particles_[p_id].mass;
            cpu_particles_[p_id].velocity += acceleration * dt;
        }
    }

    // 上传修改后的粒子数据到GPU
    particle_buffer_->upload(cpu_particles_.data(), sizeof(ParticleData) * cpu_particles_.size(), 0);

    spdlog::trace("[MPMManager] Applied external forces to {} particles", particle_indices.size());
}

std::vector<glm::vec3> MPMManager::GetParticlePositions() const {
    std::vector<glm::vec3> positions(num_particles_);

    for (size_t i = 0; i < num_particles_; i++) {
        positions[i] = cpu_particles_[i].position;
    }

    return positions;
}

void MPMManager::AutoSegmentRegion(const std::vector<glm::vec3>& all_positions) {
    spdlog::info("[MPMManager] Auto-segmenting region from {} gaussians",
                all_positions.size());

    // 简单的基于中心距离的分割
    if (all_positions.empty()) {
        spdlog::warn("[MPMManager] No positions for segmentation");
        return;
    }

    // 计算质心
    glm::vec3 center(0.0f);
    for (const auto& pos : all_positions) {
        center += pos;
    }
    center /= static_cast<float>(all_positions.size());

    // 计算到质心的距离
    std::vector<float> distances;
    distances.reserve(all_positions.size());
    for (const auto& pos : all_positions) {
        distances.push_back(glm::length(pos - center));
    }

    // 使用中位数作为阈值
    std::vector<float> sorted_distances = distances;
    std::sort(sorted_distances.begin(), sorted_distances.end());
    float threshold = sorted_distances[sorted_distances.size() / 2];

    // 分割
    DeformableRegion region;
    for (size_t i = 0; i < all_positions.size(); i++) {
        if (distances[i] < threshold) {
            // 靠近中心 -> 可变形
            region.deformable_indices.push_back(static_cast<uint32_t>(i));
            region.deformable_original_pos.push_back(all_positions[i]);
        } else {
            // 远离中心 -> 静态
            region.static_indices.push_back(static_cast<uint32_t>(i));
            region.static_original_pos.push_back(all_positions[i]);
        }
    }

    SetRegion(region);
    spdlog::info("[MPMManager] Auto-segment complete: {} deformable, {} static",
                region.GetDeformableCount(), region.GetStaticCount());
}

void MPMManager::CreateParticleBuffer() {
    spdlog::debug("[MPMManager] Creating particle buffer for {} particles", num_particles_);

    size_t buffer_size = num_particles_ * sizeof(ParticleData);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eTransferSrc;

    particle_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    spdlog::debug("[MPMManager] Particle buffer created: {} MB", buffer_size / 1024 / 1024);
}

void MPMManager::CreateGridBuffer() {
    spdlog::debug("[MPMManager] Creating grid buffer for {} nodes", grid_total_nodes_);

    size_t buffer_size = grid_total_nodes_ * sizeof(GridNode);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;

    grid_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    spdlog::debug("[MPMManager] Grid buffer created: {} MB", buffer_size / 1024 / 1024);
}

void MPMManager::CreateDisplacementBuffer() {
    spdlog::debug("[MPMManager] Creating displacement buffer for {} particles", num_particles_);

    size_t buffer_size = num_particles_ * sizeof(glm::vec3);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc |
        vk::BufferUsageFlagBits::eTransferDst;

    particle_displacement_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    spdlog::debug("[MPMManager] Displacement buffer created: {} KB", buffer_size / 1024);
}

void MPMManager::CreateDescriptorSets() {
    spdlog::info("[MPMManager] Creating descriptor sets...");

    // Descriptor set 0: 粒子 + 网格绑定（用于 P2G, G2P, Compute Stress）
    particle_grid_descriptor_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

    particle_grid_descriptor_->bindBufferToDescriptorSet(
        0, // binding 0: ParticleBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        particle_buffer_
    );

    particle_grid_descriptor_->bindBufferToDescriptorSet(
        1, // binding 1: GridBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        grid_buffer_
    );

    particle_grid_descriptor_->build();

    // Descriptor set 1: 仅网格绑定（用于 Zero Grid, Grid Update）
    grid_only_descriptor_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

    grid_only_descriptor_->bindBufferToDescriptorSet(
        0, // binding 0: GridBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        grid_buffer_
    );

    grid_only_descriptor_->build();

    spdlog::info("[MPMManager] Descriptor sets created successfully");
}

std::shared_ptr<ComputePipeline> MPMManager::CreateMPMPipeline(
    const std::string& shaderName,
    const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
    vk::PushConstantRange pushConstantRange
) {
    // 创建 shader
    auto shader = std::make_shared<Shader>(context_, shaderName);

    // 创建 pipeline
    auto pipeline = std::make_shared<ComputePipeline>(context_, shader);

    // 添加 descriptor set layouts
    for (const auto& binding : bindings) {
        pipeline->addDescriptorSetLayoutBinding(binding);
    }

    // 添加 push constant range
    pipeline->addPushConstant(
        pushConstantRange.stageFlags,
        pushConstantRange.offset,
        pushConstantRange.size
    );

    // 构建 pipeline
    pipeline->build();

    return pipeline;
}

void MPMManager::CreatePipelines() {
    spdlog::info("[MPMManager] Creating MPM compute pipelines...");

    // 1. Zero Grid Pipeline
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: GridBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            sizeof(uint32_t) * 2  // grid_size + padding
        );

        zero_grid_pipeline_ = CreateMPMPipeline(
            "src/shaders/mpm/zero_grid.comp",
            bindings,
            pushConstantRange
        );

        // 绑定 descriptor set
        zero_grid_pipeline_->addDescriptorSet(0, grid_only_descriptor_);
    }

    // 2. Compute Stress Pipeline
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: ParticleBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            sizeof(uint32_t) * 4  // num_particles + padding
        );

        compute_stress_pipeline_ = CreateMPMPipeline(
            "src/shaders/mpm/compute_stress.comp",
            bindings,
            pushConstantRange
        );

        compute_stress_pipeline_->addDescriptorSet(0, particle_grid_descriptor_);
    }

    // 3. P2G Pipeline
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: ParticleBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: GridBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            sizeof(float) + sizeof(uint32_t) * 3  // dt + grid_size + num_particles + padding
        );

        p2g_pipeline_ = CreateMPMPipeline(
            "src/shaders/mpm/p2g.comp",
            bindings,
            pushConstantRange
        );

        p2g_pipeline_->addDescriptorSet(0, particle_grid_descriptor_);
    }

    // 4. Grid Update Pipeline
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: GridBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            sizeof(uint32_t) + sizeof(glm::vec3) + sizeof(float) * 3  // grid_size + gravity + dt + damping + padding
        );

        grid_update_pipeline_ = CreateMPMPipeline(
            "src/shaders/mpm/grid_update.comp",
            bindings,
            pushConstantRange
        );

        grid_update_pipeline_->addDescriptorSet(0, grid_only_descriptor_);
    }

    // 5. G2P Pipeline
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: GridBuffer (readonly)
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: ParticleBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            sizeof(float) + sizeof(uint32_t) * 3  // dt + grid_size + num_particles + padding
        );

        g2p_pipeline_ = CreateMPMPipeline(
            "src/shaders/mpm/g2p.comp",
            bindings,
            pushConstantRange
        );

        g2p_pipeline_->addDescriptorSet(0, particle_grid_descriptor_);
    }

    spdlog::info("[MPMManager] All MPM pipelines created successfully");
}

void MPMManager::RecordPhysicsCommandBuffer(VkCommandBuffer cmd, float dt) {
    // 实现：记录完整的物理模拟命令缓冲区
    // 这将在 Step() 中调用
}

void MPMManager::Substep(VkCommandBuffer cmd, float dt) {
    // 单个 MPM 子步：Zero Grid -> Compute Stress -> P2G -> Grid Update -> G2P

    // 1. Zero Grid
    {
        struct ZeroGridParams {
            uint32_t grid_size;
            uint32_t padding[3];
        };
        ZeroGridParams params{config_.grid_size};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, zero_grid_pipeline_->pipeline.get());
        zero_grid_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, zero_grid_pipeline_->pipelineLayout.get(),
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

        uint32_t groups = (config_.grid_size + 7) / 8;
        vkCmdDispatch(cmd, groups, groups, groups);
    }

    // Memory barrier: Grid write -> Read
    {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // 2. Compute Stress (Phase 1.2: FCR材料模型)
    {
        struct StressParams {
            uint32_t num_particles;
            uint32_t padding[3];
        };
        StressParams params{num_particles_};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_stress_pipeline_->pipeline.get());
        compute_stress_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, compute_stress_pipeline_->pipelineLayout.get(),
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

        uint32_t groups = (num_particles_ + 255) / 256;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Memory barrier: Particle write (stress) -> Read
    {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // 3. P2G
    {
        struct P2GParams {
            float dt;
        uint32_t grid_size;
            uint32_t num_particles;
            float inv_dx;
            uint32_t padding[2];
        };
        P2GParams params{dt, config_.grid_size, num_particles_, config_.inv_dx};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p2g_pipeline_->pipeline.get());
        p2g_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, p2g_pipeline_->pipelineLayout.get(),
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

        uint32_t groups = (num_particles_ + 255) / 256;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Memory barrier: Grid write -> Read
    {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // 4. Grid Update
    {
        struct GridUpdateParams {
            uint32_t grid_size;
            glm::vec3 gravity;
            float dt;
            float damping;
            uint32_t padding[2];
        };
        GridUpdateParams params{config_.grid_size, config_.gravity, dt, config_.damping};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, grid_update_pipeline_->pipeline.get());
        grid_update_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, grid_update_pipeline_->pipelineLayout.get(),
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

        uint32_t groups = (config_.grid_size + 7) / 8;
        vkCmdDispatch(cmd, groups, groups, groups);
    }

    // Memory barrier: Grid write -> Read, Particle write -> Read
    {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // 5. G2P
    {
        struct G2PParams {
            float dt;
            uint32_t grid_size;
            uint32_t num_particles;
            float inv_dx;
            uint32_t padding[2];
        };
        G2PParams params{dt, config_.grid_size, num_particles_, config_.inv_dx};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g2p_pipeline_->pipeline.get());
        g2p_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, g2p_pipeline_->pipelineLayout.get(),
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

        uint32_t groups = (num_particles_ + 255) / 256;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Memory barrier: Particle write -> Read (next substep)
    {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
}

} // namespace MPM
