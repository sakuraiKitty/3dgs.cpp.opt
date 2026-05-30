#include "MPMManager.h"
#include "GSScene.h"
#include "vulkan/CommandPool.h"
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
    particle_buffer_->uploadData(cpu_particles_);

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
    particle_buffer_->uploadData(cpu_particles_);

    spdlog::info("[MPMManager] Particles loaded successfully");
}

void MPMManager::Step(VkCommandBuffer cmd, float dt) {
    if (!enabled_ || !initialized_) {
        return;
    }

    // TODO: 下一阶段实现compute shaders后，这里会：
    // 1. 绑定pipeline和descriptor sets
    // 2. 执行子步进循环
    // 3. 同步和验证结果

    // 临时：暂时只做计数
    static uint64_t step_count = 0;
    step_count++;
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
        particle_buffer_->uploadData(cpu_particles_);

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

    region_ = region;

    spdlog::info("[MPMManager] Auto-segmentation complete: {} deformable, {} static",
                region.GetDeformableCount(), region.GetStaticCount());
}

void MPMManager::CreateParticleBuffer() {
    spdlog::debug("[MPMManager] Creating particle buffer for {} particles", num_particles_);

    size_t buffer_size = num_particles_ * sizeof(ParticleData);

    particle_buffer_ = std::make_shared<Buffer>(
        context_,
        buffer_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );

    spdlog::debug("[MPMManager] Particle buffer created: {} MB", buffer_size / 1024 / 1024);
}

void MPMManager::CreateGridBuffer() {
    spdlog::debug("[MPMManager] Creating grid buffer for {} nodes", grid_total_nodes_);

    size_t buffer_size = grid_total_nodes_ * sizeof(GridNode);

    grid_buffer_ = std::make_shared<Buffer>(
        context_,
        buffer_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );

    spdlog::debug("[MPMManager] Grid buffer created: {} MB", buffer_size / 1024 / 1024);
}

void MPMManager::CreateDisplacementBuffer() {
    spdlog::debug("[MPMManager] Creating displacement buffer for {} particles", num_particles_);

    size_t buffer_size = num_particles_ * sizeof(glm::vec3);

    particle_displacement_buffer_ = std::make_shared<Buffer>(
        context_,
        buffer_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );

    spdlog::debug("[MPMManager] Displacement buffer created: {} KB", buffer_size / 1024);
}

void MPMManager::CreatePipelines() {
    // 下一阶段实现：
    // 创建zero_grid, p2g, grid_update, g2p的compute pipelines
    spdlog::info("[MPMManager] Pipeline creation will be implemented in next phase");
}

void MPMManager::RecordPhysicsCommandBuffer(VkCommandBuffer cmd, float dt) {
    // 下一阶段实现：
    // 记录完整的物理模拟命令缓冲区
}

void MPMManager::Substep(VkCommandBuffer cmd, float dt) {
    // 下一阶段实现：
    // 执行单个MPM子步（zero_grid -> p2g -> grid_update -> g2p）
}

} // namespace MPM
