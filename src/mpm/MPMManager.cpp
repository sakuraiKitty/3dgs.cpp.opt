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
    CreateInitialPosBuffer();

    // 上传粒子数据到GPU
    particle_buffer_->upload(cpu_particles_.data(), sizeof(ParticleData) * cpu_particles_.size(), 0);

    // 上传初始位置（vec4格式，stride=16匹配std430规则）
    std::vector<glm::vec4> initial_pos_v4(num_particles_);
    for (size_t i = 0; i < num_particles_; i++) {
        initial_pos_v4[i] = glm::vec4(cpu_particle_initial_pos_[i], 1.0f);
    }
    initial_pos_buffer_->upload(reinterpret_cast<const char*>(initial_pos_v4.data()),
                                 static_cast<uint32_t>(num_particles_ * sizeof(glm::vec4)), 0);

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
    CreateInitialPosBuffer();

    // 上传数据
    particle_buffer_->upload(cpu_particles_.data(), sizeof(ParticleData) * cpu_particles_.size(), 0);

    // 上传初始位置（vec4格式，stride=16匹配std430规则）
    std::vector<glm::vec4> initial_pos_v4(num_particles_);
    for (size_t i = 0; i < num_particles_; i++) {
        initial_pos_v4[i] = glm::vec4(cpu_particle_initial_pos_[i], 1.0f);
    }
    initial_pos_buffer_->upload(reinterpret_cast<const char*>(initial_pos_v4.data()),
                                 static_cast<uint32_t>(num_particles_ * sizeof(glm::vec4)), 0);

    spdlog::info("[MPMManager] Particles loaded successfully");
}

void MPMManager::Step(VkCommandBuffer cmd, float dt, uint32_t override_substeps) {
    if (!enabled_ || !initialized_) {
        return;
    }

    uint32_t actual_substeps = override_substeps > 0 ? override_substeps : config_.substeps;
    float sub_dt = dt / static_cast<float>(actual_substeps);

    for (uint32_t s = 0; s < actual_substeps; s++) {
        Substep(cmd, sub_dt);
    }

    // 记录性能统计（可选）
    static uint64_t frame_count = 0;
    frame_count++;
}

void MPMManager::Diagnose() {
    if (!enabled_ || !initialized_ || num_particles_ == 0 || !particle_buffer_) {
        return;
    }

    // 节流：每 diag_interval_ 帧回读一次
    if (++diag_frame_counter_ < diag_interval_) {
        return;
    }
    diag_frame_counter_ = 0;

    // 同步回读粒子缓冲（2.3MB，one-time cmd buffer 阻塞 compute queue，~1ms）
    auto raw = particle_buffer_->download();
    if (raw.size() < sizeof(ParticleData) * num_particles_) {
        spdlog::warn("[MPM-Diag] download size {} < expected {}", raw.size(),
                     sizeof(ParticleData) * num_particles_);
        return;
    }

    const ParticleData* parts = reinterpret_cast<const ParticleData*>(raw.data());

    float max_disp = 0.0f, max_vel = 0.0f, max_strain = 0.0f;
    float sum_strain = 0.0f;
    uint32_t moved = 0, frozen = 0, nan_count = 0;
    uint32_t strain_high = 0, strain_mid = 0;  // >0.1, >0.01
    const float disp_thresh = 1e-4f;

    // ── 旋转 vs 拉伸诊断 ──
    // FCR 应力 τ=2μ(F−R)Fᵀ+λJ(J−1)I：纯旋转 F≈R → τ≈0（无恢复力）。
    // 追踪 max|F−R|(拉伸) vs max|F−I|(应变)，比值≈0 → F 是旋转主导 → 应力≈0 → 花头刚体旋转无回弹。
    float max_stretch = 0.0f, max_tau = 0.0f;
    uint32_t stretch_low = 0;  // |F−R|/|F−I| < 0.1（旋转主导）的粒子数

    auto safe_norm = [](const glm::vec3& v) -> glm::vec3 {
        float l = glm::length(v);
        return l < 1e-8f ? glm::vec3(0.0f) : v / l;
    };
    auto frob = [](const glm::mat3& m) -> float {
        return std::sqrt(m[0][0]*m[0][0]+m[0][1]*m[0][1]+m[0][2]*m[0][2]+
                         m[1][0]*m[1][0]+m[1][1]*m[1][1]+m[1][2]*m[1][2]+
                         m[2][0]*m[2][0]+m[2][1]*m[2][1]+m[2][2]*m[2][2]);
    };

    for (uint32_t i = 0; i < num_particles_; i++) {
        const auto& p = parts[i];

        if (p.freeze_flag != 0u) { frozen++; continue; }

        // NaN 检测（标量检查，避免 glm::isnan 跨版本兼容问题）
        if (std::isnan(p.position.x) || std::isnan(p.position.y) || std::isnan(p.position.z) ||
            std::isnan(p.velocity.x) || std::isnan(p.velocity.y) || std::isnan(p.velocity.z)) {
            nan_count++;
            continue;
        }

        glm::vec3 disp = p.position - cpu_particle_initial_pos_[i];
        float d = glm::length(disp);
        float v = glm::length(p.velocity);

        if (d > max_disp) max_disp = d;
        if (v > max_vel) max_vel = v;
        if (d > disp_thresh) moved++;

        // 应变指标：max|F - I|（F 偏离单位阵的程度，决定应力大小）
        glm::mat3 F = GetDeformationGradient(p);
        glm::mat3 I(1.0f);
        glm::mat3 diff = F - I;
        float strain = std::sqrt(diff[0][0]*diff[0][0] + diff[0][1]*diff[0][1] + diff[0][2]*diff[0][2] +
                                 diff[1][0]*diff[1][0] + diff[1][1]*diff[1][1] + diff[1][2]*diff[1][2] +
                                 diff[2][0]*diff[2][0] + diff[2][1]*diff[2][1] + diff[2][2]*diff[2][2]);
        if (strain > max_strain) max_strain = strain;
        sum_strain += strain;
        if (strain > 0.1f) strain_high++;
        else if (strain > 0.01f) strain_mid++;

        // Gram-Schmidt 极分解 R（与 shader extract_rotation_gram_schmidt 一致）
        glm::vec3 c0 = F[0], c1 = F[1], c2 = F[2];
        glm::vec3 r0 = safe_norm(c0);
        glm::vec3 r1 = safe_norm(c1 - glm::dot(c1, r0) * r0);
        glm::vec3 r2 = safe_norm(c2 - glm::dot(c2, r0) * r0 - glm::dot(c2, r1) * r1);
        glm::mat3 R(r0, r1, r2);
        glm::mat3 FmR = F - R;
        float stretch = frob(FmR);
        if (stretch > max_stretch) max_stretch = stretch;
        if (strain > 0.01f && stretch < 0.1f * strain) stretch_low++;

        // FCR Kirchhoff 应力 τ=2μ(F−R)Fᵀ+λJ(J−1)I，对称化（与 mpm_stress.glsl 一致）
        float E = p.youngs_modulus;
        float nu = p.poisson_ratio;
        float mu = E / (2.0f * (1.0f + nu));
        float lam = E * nu / ((1.0f + nu) * (1.0f - 2.0f * nu));
        // F 的行列式 J
        float J = F[0][0]*(F[1][1]*F[2][2]-F[1][2]*F[2][1])
                - F[0][1]*(F[1][0]*F[2][2]-F[1][2]*F[2][0])
                + F[0][2]*(F[1][0]*F[2][1]-F[1][1]*F[2][0]);
        glm::mat3 Ft(F[0][0], F[1][0], F[2][0],
                     F[0][1], F[1][1], F[2][1],
                     F[0][2], F[1][2], F[2][2]);
        glm::mat3 tau = 2.0f * mu * FmR * Ft + lam * J * (J - 1.0f) * glm::mat3(1.0f);
        tau = (tau + glm::transpose(tau)) * 0.5f;
        float tau_mag = frob(tau);
        if (tau_mag > max_tau) max_tau = tau_mag;
    }

    // 采样第一个非冻结粒子的材料参数（诊断 units/scale 失配）
    float sample_E = 0, sample_vol = 0, sample_mass = 0, sample_rho = 0;
    for (uint32_t i = 0; i < num_particles_; i++) {
        if (parts[i].freeze_flag == 0u) {
            sample_E = parts[i].youngs_modulus;
            sample_vol = parts[i].volume;
            sample_mass = parts[i].mass;
            sample_rho = parts[i].density;
            break;
        }
    }
    uint32_t n_active = num_particles_ - frozen;
    float avg_strain = n_active > 0 ? sum_strain / n_active : 0.0f;
    float stretch_ratio = max_strain > 1e-6f ? max_stretch / max_strain : 0.0f;

    spdlog::info("[MPM-Diag] moved={}/{} frozen={} nan={} | max_disp={:.6f} max_vel={:.6f} "
                 "max_strain={:.4f} avg_strain={:.5f} (high>0.1:{}, mid>0.01:{})",
                 moved, num_particles_, frozen, nan_count, max_disp, max_vel,
                 max_strain, avg_strain, strain_high, strain_mid);
    spdlog::info("[MPM-Diag]   ROTvsSTRETCH: max|F-R|={:.4f} max|tau|={:.2f} | ratio={:.3f} "
                 "stretch<10%strain:{} | 若ratio≈0且tau≈0→F旋转主导→无恢复力(花头刚体旋转)",
                 max_stretch, max_tau, stretch_ratio, stretch_low);
    spdlog::info("[MPM-Diag]   sample: E={:.1f} vol={:.3e} mass={:.3e} rho={:.1f} | "
                 "expect dv/substep = tau*gradw*dt/rho ≈ {:.3f}",
                 sample_E, sample_vol, sample_mass, sample_rho,
                 (sample_E / (2.0f*(1.0f+0.3f))) * 64.0f * 0.00026f / sample_rho);
}

void MPMManager::Reset() {
    spdlog::info("[MPMManager] Resetting simulation...");

    if (!cpu_particle_initial_pos_.empty()) {
        // 重置粒子位置到初始状态
        for (size_t i = 0; i < cpu_particles_.size(); i++) {
            cpu_particles_[i].position = cpu_particle_initial_pos_[i];
            cpu_particles_[i].velocity = glm::vec3(0.0f);
            SetDeformationGradient(cpu_particles_[i], glm::mat3(1.0f));
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

    // 如果所有缓冲区都已创建，则绑定到 descriptor sets
    if (particle_buffer_ && grid_buffer_ && !descriptor_sets_built_) {
        BuildDescriptorSets();
    }
}

void MPMManager::CreateGridBuffer() {
    spdlog::debug("[MPMManager] Creating grid buffer for {} nodes", grid_total_nodes_);

    size_t buffer_size = grid_total_nodes_ * sizeof(GridNode);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eTransferSrc;  // 诊断 readback (downloadTo) 需要 TRANSFER_SRC

    grid_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    spdlog::debug("[MPMManager] Grid buffer created: {} MB", buffer_size / 1024 / 1024);

    // 如果所有缓冲区都已创建，则绑定到 descriptor sets
    if (particle_buffer_ && grid_buffer_ && !descriptor_sets_built_) {
        BuildDescriptorSets();
    }
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

void MPMManager::CreateInitialPosBuffer() {
    spdlog::debug("[MPMManager] Creating initial position buffer for {} particles", num_particles_);

    // vec4格式（stride=16匹配GLSL std430规则，避免vec3 stride=12 vs std430 stride=16错位）
    size_t buffer_size = num_particles_ * sizeof(glm::vec4);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eTransferSrc;  // 诊断 readback (downloadTo) 需要 TRANSFER_SRC

    initial_pos_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    spdlog::debug("[MPMManager] Initial position buffer created: {} KB (vec4 format)", buffer_size / 1024);
}

void MPMManager::BuildDescriptorSets() {
    spdlog::info("[MPMManager] Building descriptor sets...");

    // Bind particle and grid to particle_grid_descriptor_
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

    // Bind grid to grid_only_descriptor_
    grid_only_descriptor_->bindBufferToDescriptorSet(
        0, // binding 0: GridBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        grid_buffer_
    );

    grid_only_descriptor_->build();

    // Build G2P descriptor set: binding 0=Grid(readonly), binding 1=Particle(write)
    // G2P shader期望与P2G相反的binding顺序
    g2p_descriptor_->bindBufferToDescriptorSet(
        0, // binding 0: GridBuffer (readonly in G2P shader)
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        grid_buffer_
    );

    g2p_descriptor_->bindBufferToDescriptorSet(
        1, // binding 1: ParticleBuffer (write in G2P shader)
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        particle_buffer_
    );

    g2p_descriptor_->build();

    descriptor_sets_built_ = true;

    // ── CRITICAL FIX: Rebuild all pipelines with actual descriptor set layouts ──
    // Pipelines were originally built during CreatePipelines() with TEMP descriptor set layouts,
    // because descriptor sets hadn't been built yet at that time.
    // Now that descriptor sets ARE built with their own actual layouts, we MUST rebuild
    // the pipeline layout and pipeline to use the real layout — otherwise vkCmdBindDescriptorSets
    // uses a mismatched layout object and compute shaders silently fail to execute.
    spdlog::info("[MPMManager] Rebuilding all pipelines with actual descriptor set layouts...");
    zero_grid_pipeline_->rebuild();
    p2g_pipeline_->rebuild();
    grid_update_pipeline_->rebuild();
    grid_freeze_pipeline_->rebuild();
    g2p_pipeline_->rebuild();
    spdlog::info("[MPMManager] All pipelines rebuilt successfully");

    // ── Diagnostic: Verify pipeline state ──
    spdlog::info("[MPMManager] Pipeline state after rebuild:");
    spdlog::info("[MPMManager]   zero_grid: pipeline={}, layout={}",
                 (void*)zero_grid_pipeline_->pipeline.get(),
                 (void*)zero_grid_pipeline_->pipelineLayout.get());
    spdlog::info("[MPMManager]   p2g: pipeline={}, layout={}",
                 (void*)p2g_pipeline_->pipeline.get(),
                 (void*)p2g_pipeline_->pipelineLayout.get());
    spdlog::info("[MPMManager]   grid_update: pipeline={}, layout={}",
                 (void*)grid_update_pipeline_->pipeline.get(),
                 (void*)grid_update_pipeline_->pipelineLayout.get());
    spdlog::info("[MPMManager]   grid_freeze: pipeline={}, layout={}",
                 (void*)grid_freeze_pipeline_->pipeline.get(),
                 (void*)grid_freeze_pipeline_->pipelineLayout.get());
    spdlog::info("[MPMManager]   g2p: pipeline={}, layout={}",
                 (void*)g2p_pipeline_->pipeline.get(),
                 (void*)g2p_pipeline_->pipelineLayout.get());
    spdlog::info("[MPMManager] Descriptor sets built + pipelines rebuilt successfully (including G2P)");
}

void MPMManager::CreateDescriptorSets() {
    spdlog::info("[MPMManager] Creating descriptor sets...");

    // Descriptor set 0: 粒子 + 网格绑定（用于 P2G, Compute Stress）
    // P2G 期望: binding 0=Particle(readonly), binding 1=Grid(write)
    particle_grid_descriptor_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

    // Descriptor set 1: 仅网格绑定（用于 Zero Grid, Grid Update）
    grid_only_descriptor_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

    // Descriptor set 2: G2P 专用（binding 0=Grid readonly, binding 1=Particle write）
    // G2P shader期望的binding顺序与P2G相反，不能共用同一个descriptor set
    g2p_descriptor_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

    // 注意：缓冲区绑定将在 CreateParticleBuffer() 和 CreateGridBuffer() 中完成
    // 因为此时缓冲区还未创建

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

        // ZeroGridParams: {uint32_t grid_size; uint32_t padding[3];} = 16 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            16  // sizeof(ZeroGridParams) — must match C++ struct size
        );

        zero_grid_pipeline_ = CreateMPMPipeline(
            "zero_grid",
            bindings,
            pushConstantRange
        );

        // 绑定 descriptor set
        zero_grid_pipeline_->addDescriptorSet(0, grid_only_descriptor_);
    }

    // 2. P2G Pipeline (compute_stress 已合并到 P2G shader 中，不再有独立 pipeline)
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

        // P2GParams: {float dt; uint grid_size; uint num_particles; float inv_dx; uint padding[2];} = 24 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            32  // sizeof(P2GParams) — covers all fields including inv_dx
        );

        p2g_pipeline_ = CreateMPMPipeline(
            "p2g",
            bindings,
            pushConstantRange
        );

        p2g_pipeline_->addDescriptorSet(0, particle_grid_descriptor_);
    }

    // 3. Grid Update Pipeline
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: GridBuffer
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        // GridUpdateParams: {uint grid_size; float gravity_x/y/z; float dt; float damping; uint padding[2];} = 32 bytes
        // CRITICAL: gravity uses 3 separate floats (not vec3) to avoid std430 alignment mismatch
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            32  // sizeof(GridUpdateParams) — all scalar fields, tight packing
        );

        grid_update_pipeline_ = CreateMPMPipeline(
            "grid_update",
            bindings,
            pushConstantRange
        );

        grid_update_pipeline_->addDescriptorSet(0, grid_only_descriptor_);
    }

    // 4. Grid Freeze Pipeline（冻结区域速度归零 — Grid Update之后、G2P之前）
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            // binding 0: ParticleBuffer (readonly — 只读冻结粒子位置)
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: GridBuffer (write — 归零冻结区域速度)
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute)
        };

        // GridFreezeParams: {uint num_particles; float inv_dx; uint grid_size; uint padding;} = 16 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            16
        );

        grid_freeze_pipeline_ = CreateMPMPipeline(
            "grid_freeze",
            bindings,
            pushConstantRange
        );

        // 共用 particle_grid_descriptor_（binding 0=Particle, binding 1=Grid — 与P2G布局一致）
        grid_freeze_pipeline_->addDescriptorSet(0, particle_grid_descriptor_);
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

        // G2PParams: {float dt; uint grid_size; uint num_particles; float inv_dx; uint padding[2];} = 24 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            32  // sizeof(G2PParams) — same layout as P2GParams
        );

        g2p_pipeline_ = CreateMPMPipeline(
            "g2p",
            bindings,
            pushConstantRange
        );

        g2p_pipeline_->addDescriptorSet(0, g2p_descriptor_);
    }

    spdlog::info("[MPMManager] All MPM pipelines created successfully (including Grid Freeze)");
}

void MPMManager::RecordPhysicsCommandBuffer(VkCommandBuffer cmd, float dt) {
    // 实现：记录完整的物理模拟命令缓冲区
    // 这将在 Step() 中调用
}

void MPMManager::Substep(VkCommandBuffer cmd, float dt) {
    // 单个 MPM 子步：Zero Grid → P2G(含inline应力) → Grid Update → Grid Freeze → G2P
    // 拖拽交互由 DragHandler 的 ApplyDrag 在 Step 前单独处理（速度插值模式）

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

    // 2. P2G (应力计算已inline合并，不再有独立compute_stress dispatch)
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

    // 3. Grid Update
    {
        // CRITICAL: 使用3个独立float替代glm::vec3 — std430中vec3 alignment=16,
        // 但C++ glm::vec3 alignment=4, 导致gravity字段offset不匹配:
        // C++ offset 4 vs GLSL offset 16 → shader读到dt/damping而非gravity!
        // 改为float后alignment=4, 所有字段紧密排列, C++/GLSL布局完全一致
        struct GridUpdateParams {
            uint32_t grid_size;     // offset 0
            float gravity_x;        // offset 4
            float gravity_y;        // offset 8
            float gravity_z;        // offset 12
            float dt;               // offset 16
            float damping;          // offset 20
            float max_velocity;     // offset 24 — CFL 速度上限 (0=不限制)
            uint32_t padding;       // offset 28
            // sizeof = 32 (28 bytes + 4 padding for struct alignment)
        };
        static_assert(sizeof(GridUpdateParams) == 32,
            "GridUpdateParams must match GLSL push constant layout of 32 bytes");
        // CFL: |v|*dt <= 0.5*dx → max_v = 0.5*dx/dt = 0.5/(inv_dx*dt)
        // 截断应力爆炸产生的网格巨速，防止粒子射出网格永久冻结
        const float grid_cfl_max_velocity = 0.5f / (config_.inv_dx * dt);
        GridUpdateParams params{config_.grid_size,
                                 config_.gravity.x, config_.gravity.y, config_.gravity.z,
                                 dt, config_.damping,
                                 grid_cfl_max_velocity, 0u};

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

    // 4. Grid Freeze（冻结区域速度归零 — 对应 PhysDreamer apply_grid_bc_w_freeze_pts）
    {
        struct GridFreezeParams {
            uint32_t num_particles;    // offset 0
            float inv_dx;              // offset 4
            uint32_t grid_size;        // offset 8
            uint32_t padding;          // offset 12
        };
        static_assert(sizeof(GridFreezeParams) == 16,
            "GridFreezeParams must match GLSL push constant layout of 16 bytes");
        GridFreezeParams params{num_particles_, config_.inv_dx, config_.grid_size};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, grid_freeze_pipeline_->pipeline.get());
        grid_freeze_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, grid_freeze_pipeline_->pipelineLayout.get(),
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

        uint32_t groups = (num_particles_ + 255) / 256;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Memory barrier: Grid write (freeze) -> Read (G2P)
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

    // Memory barrier: Particle write -> Read (next substep, after G2P step 5)
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
