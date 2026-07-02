#include "MPMManager.h"
#include "../GSScene.h"
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

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

    // 保存初始位置 + 计算初始 AABB（归一化空间，用于交互抓取半径自适应）
    cpu_particle_initial_pos_.reserve(num_particles_);
    aabb_min_ = glm::vec3(std::numeric_limits<float>::max());
    aabb_max_ = glm::vec3(std::numeric_limits<float>::lowest());
    for (const auto& p : cpu_particles_) {
        cpu_particle_initial_pos_.push_back(p.position);
        aabb_min_ = glm::min(aabb_min_, p.position);
        aabb_max_ = glm::max(aabb_max_, p.position);
    }
    aabb_diag_ = glm::length(aabb_max_ - aabb_min_);
    aabb_computed_ = true;
    spdlog::info("[MPMManager] Initial AABB min=({:.4f},{:.4f},{:.4f}) max=({:.4f},{:.4f},{:.4f}) diag={:.4f}",
                 aabb_min_.x, aabb_min_.y, aabb_min_.z,
                 aabb_max_.x, aabb_max_.y, aabb_max_.z, aabb_diag_);

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

    // ── 拉断前兆诊断（应变门控配套）──
    // min_det: det(F) 最小值，<0.3 = G2P 已限幅介入；<0.1 = 塌缩前兆
    // max_stretch_col: F 列范数最大值 = 拉伸比，>2.0 = G2P 已限幅截断
    // gated: deform=max(列范数,1/|J|) > 1.5 或 det<0.1 的粒子数 = 被 apply_drag_velocity_bc 弹性门控衰减的粒子
    //   （弹性硬截断：超 1.5 不再跟随鼠标，非塑性；与 shader deform=max(stretch,1/|J|) smoothstep(1.2,1.5) 一致）
    float min_det = 1e9f, max_stretch_col = 0.0f;
    uint32_t gated = 0;

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

        // 极分解 R（与 shader extract_rotation_polar 一致，Newton 迭代 R=(R+R^{-T})/2）
        // 旧用 Gram-Schmidt 与 shader 不一致 → 大旋转下 GS 产生自平衡伪应力 →
        // diag 的 max|tau| 偏高误导（真着色器 τ 用极分解，可能小得多）。
        glm::mat3 R = F;
        float detF_diag = F[0][0]*(F[1][1]*F[2][2]-F[1][2]*F[2][1])
                        - F[0][1]*(F[1][0]*F[2][2]-F[1][2]*F[2][0])
                        + F[0][2]*(F[1][0]*F[2][1]-F[1][1]*F[2][0]);
        if (std::fabs(detF_diag) < 1e-6f) {
            // F 近奇异 → 回退 Gram-Schmidt（与 shader fallback 一致）
            glm::vec3 gc0 = F[0], gc1 = F[1], gc2 = F[2];
            glm::vec3 gr0 = safe_norm(gc0);
            glm::vec3 gr1 = safe_norm(gc1 - glm::dot(gc1, gr0) * gr0);
            glm::vec3 gr2 = safe_norm(gc2 - glm::dot(gc2, gr0) * gr0 - glm::dot(gc2, gr1) * gr1);
            R = glm::mat3(gr0, gr1, gr2);
        } else {
            for (int it = 0; it < 12; ++it) {
                glm::mat3 R_inv = glm::inverse(R);
                glm::mat3 R_inv_T = glm::transpose(R_inv);
                R = (R + R_inv_T) * 0.5f;
            }
        }
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

        // ── 拉断前兆统计（与 apply_drag_velocity_bc 应变门控指标一致）──
        // deform = max(列范数, 1/|J|)：兼顾拉伸与压缩塌缩（实测主导失效模式是 min_det 跌到 0.09）
        if (J < min_det) min_det = J;
        float col_stretch = std::max({glm::length(F[0]), glm::length(F[1]), glm::length(F[2])});
        if (col_stretch > max_stretch_col) max_stretch_col = col_stretch;
        float compress = 1.0f / std::max(std::fabs(J), 1e-4f);
        float deform = std::max(col_stretch, compress);
        if (deform > 1.5f || J < 0.1f) gated++;
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
    // 拉断前兆：min_det→0/负 = 内翻塌缩即将甩飞；gated>0 = 应变门控已介入衰减拖拽速度
    if (min_det > 1e8f) min_det = 0.0f;  // 全冻结时无样本
    spdlog::info("[MPM-Diag]   TEAR-WATCH: min_det={:.4f} max_stretch_col={:.4f} gated(deform>1.5orJ<0.1):{}/{} | "
                 "min_det<0.3=体积限幅介入 <0.1=塌缩前兆 stretch>2.0=拉伸限幅 gated>0=门控停跟随(非塑性)",
                 min_det, max_stretch_col, gated, n_active);
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

std::optional<glm::vec3> MPMManager::GetParticlePositionGPU(uint32_t index) {
    if (!initialized_ || num_particles_ == 0 || !particle_buffer_) {
        return std::nullopt;
    }
    if (index >= num_particles_) {
        return std::nullopt;
    }

    // 同步 staging 回读单粒子（176B）。endOneTimeCommandBuffer 内 queue.waitIdle()
    // 保证读到上一帧已提交的 Step 结果（一帧滞后），供拖拽 P 控制器 cur_pick 使用。
    // 修复 C3：旧路径 GetParticlePositions() 返回 cpu_particles_（init 后永不更新），
    //         导致 cur_pick 恒为初始位置 → dragVel=(target-init)/dt 饱和在 CFL 上限，
    //         batch 过冲不归位、home-spring 也因 disp 失真而无恢复力。
    const VkDeviceSize offset =
        static_cast<VkDeviceSize>(index) * static_cast<VkDeviceSize>(sizeof(ParticleData));
    ParticleData p = particle_buffer_->readOne<ParticleData>(offset);

    // NaN 防护（GPU 异常时 position 可能变 NaN，反馈进 P 控制器会污染整批）
    if (std::isnan(p.position.x) || std::isnan(p.position.y) || std::isnan(p.position.z)) {
        return std::nullopt;
    }
    return p.position;
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
    // 含 initial_pos_buffer_（PinFrozen 绑定需要），未全创建则等 CreateInitialPosBuffer 触发
    if (particle_buffer_ && grid_buffer_ && initial_pos_buffer_ && !descriptor_sets_built_) {
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

    // 冻结掩码缓冲区（GridFreeze 融合：P2G 标记/ZeroGrid 清/GridUpdate 读）
    CreateFreezeMaskBuffer();

    // 如果所有缓冲区都已创建，则绑定到 descriptor sets
    if (particle_buffer_ && grid_buffer_ && initial_pos_buffer_ && !descriptor_sets_built_) {
        BuildDescriptorSets();
    }
}

void MPMManager::CreateFreezeMaskBuffer() {
    // 每个网格节点 1 个 uint32（grid_size³）。P2G atomicOr 标记冻结粒子的 floor 节点，
    // GridUpdate 读后零化其速度，ZeroGrid 每子步清零。替代原独立 GridFreeze dispatch。
    size_t buffer_size = grid_total_nodes_ * sizeof(uint32_t);

    vk::BufferUsageFlags usageFlags =
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eTransferSrc;

    freeze_mask_buffer_ = std::make_shared<Buffer>(
        context_,
        static_cast<uint32_t>(buffer_size),
        usageFlags,
        VMA_MEMORY_USAGE_GPU_ONLY,
        static_cast<VmaAllocationCreateFlags>(0)
    );

    spdlog::debug("[MPMManager] Freeze mask buffer created: {} KB ({} nodes)",
                 buffer_size / 1024, grid_total_nodes_);
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

    // initial_pos_buffer_ 是最后创建的；此时 particle/grid 均已就绪，触发 BuildDescriptorSets
    if (particle_buffer_ && grid_buffer_ && initial_pos_buffer_ && !descriptor_sets_built_) {
        BuildDescriptorSets();
    }
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

    // binding 2: FreezeMask（P2G 写——冻结粒子标 floor 节点）
    particle_grid_descriptor_->bindBufferToDescriptorSet(
        2,
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        freeze_mask_buffer_
    );

    particle_grid_descriptor_->build();

    // Bind grid to grid_only_descriptor_
    grid_only_descriptor_->bindBufferToDescriptorSet(
        0, // binding 0: GridBuffer
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        grid_buffer_
    );

    // binding 1: FreezeMask（ZeroGrid 写清零，GridUpdate 读零化）
    grid_only_descriptor_->bindBufferToDescriptorSet(
        1,
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        freeze_mask_buffer_
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

    g2p_descriptor_->bindBufferToDescriptorSet(
        2, // binding 2: InitPos (readonly vec4[]) — PinFrozen 合并进 G2P 末尾
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        initial_pos_buffer_
    );

    g2p_descriptor_->build();

    // Build PinFrozen descriptor set: binding 0=Particle(write), binding 1=InitPos(readonly)
    particle_init_descriptor_->bindBufferToDescriptorSet(
        0,
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        particle_buffer_
    );
    particle_init_descriptor_->bindBufferToDescriptorSet(
        1,
        vk::DescriptorType::eStorageBuffer,
        vk::ShaderStageFlagBits::eCompute,
        initial_pos_buffer_
    );
    particle_init_descriptor_->build();

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
    drag_bc_pipeline_->rebuild();
    pin_frozen_pipeline_->rebuild();
    home_spring_pipeline_->rebuild();
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

    // Descriptor set 3: 粒子 + 初始位置（PinFrozen: binding 0=Particle write, binding 1=InitPos readonly）
    particle_init_descriptor_ = std::make_shared<DescriptorSet>(context_, FRAMES_IN_FLIGHT);

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
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: FreezeMask（清零）
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
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
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 2: FreezeMask（冻结粒子标 floor 节点）
            vk::DescriptorSetLayoutBinding()
                .setBinding(2)
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
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 1: FreezeMask（读——零化冻结节点速度，GridFreeze 融合）
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
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

    // 5. G2P Pipeline（PinFrozen 已合并进 G2P 末尾，省 1 dispatch/子步）
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
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            // binding 2: InitPos (readonly vec4[]) — PinFrozen 合并进 G2P 末尾所需
            vk::DescriptorSetLayoutBinding()
                .setBinding(2)
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

    // 6. Drag Velocity BC Pipeline（每子步速度 Dirichlet BC，对标 PhysDreamer enforce_particle_velocity_by_mask）
    // binding 0=ParticleBuffer(write velocity)，复用 particle_grid_descriptor_（与 grid_freeze 同布局）
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        };
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            48  // sizeof(DragBCParams) — center/radius/velocity/alpha/isDragging/maxVelocity/pad[2]
        );
        drag_bc_pipeline_ = CreateMPMPipeline(
            "apply_drag_velocity_bc",
            bindings,
            pushConstantRange
        );
        drag_bc_pipeline_->addDescriptorSet(0, particle_grid_descriptor_);
    }

    // 7. Pin Frozen Pipeline（每子步粒子级硬冻结，对标 PhysDreamer gui_demo.py:313）
    // binding 0=ParticleBuffer(write pos/vel), binding 1=InitPos(readonly vec4[])
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        };
        // PinParams: {uint num_particles; uint pad[3];} = 16 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            16
        );
        pin_frozen_pipeline_ = CreateMPMPipeline(
            "pin_frozen_particles",
            bindings,
            pushConstantRange
        );
        pin_frozen_pipeline_->addDescriptorSet(0, particle_init_descriptor_);
    }

    // 8. Home Spring Pipeline（每子步 ZeroGrid 后，为刚体模态提供恢复力）
    //    binding 0=ParticleBuffer(write velocity), binding 1=InitPos(readonly vec4[])
    //    复用 particle_init_descriptor_（与 PinFrozen 同布局）
    {
        std::vector<vk::DescriptorSetLayoutBinding> bindings = {
            vk::DescriptorSetLayoutBinding()
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
            vk::DescriptorSetLayoutBinding()
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eCompute),
        };
        // HomeSpringParams: {float k; float dt; uint num; uint enable; float f_relax_alpha; uint pad} = 24 bytes
        vk::PushConstantRange pushConstantRange(
            vk::ShaderStageFlagBits::eCompute,
            0,
            sizeof(HomeSpringParams)
        );
        home_spring_pipeline_ = CreateMPMPipeline(
            "apply_home_spring",
            bindings,
            pushConstantRange
        );
        home_spring_pipeline_->addDescriptorSet(0, particle_init_descriptor_);
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

    // 1a. 位置 home-spring（每子步，ZeroGrid 后、DragBC 前）
    // 为刚体模态（整体平移/旋转）提供恢复力——FCR 客观材料对纯旋转零应力，无此弹簧则
    // 花头拖拽后绕花茎刚体旋转卡死不回弹（ratio=max|F-R|/max|F-I|≈0.057 佐证）。
    // 速度冲量 v += -k*(x-x0)*dt；DragBC 的 SET 随后覆盖被抓粒子→弹簧不影响拖拽 batch。
    // 写 particle.velocity，由 DragBC 后的 barrier（SHADER_WRITE→READ）覆盖给 P2G。
    if (home_spring_.enable != 0u && home_spring_pipeline_) {
        home_spring_.dt = dt;
        home_spring_.num_particles = num_particles_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, home_spring_pipeline_->pipeline.get());
        home_spring_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, home_spring_pipeline_->pipelineLayout.get(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(home_spring_), &home_spring_);
        uint32_t pgroups = (num_particles_ + 63) / 64;  // local_size_x=64：53wg→213wg 填满 96 SM（占用率优化）
        vkCmdDispatch(cmd, pgroups, 1, 1);
    }

    // 1b. 拖拽速度 Dirichlet BC（每子步，对标 PhysDreamer pre_p2g_operations / enforce_particle_velocity_by_mask）
    // 仅 isDragging 时派发：半径内非冻结粒子 SET velocity = drag_bc_.velocity（持续驱动 batch）
    // 写 particle.velocity，由下方 barrier（SHADER_WRITE→READ，全局）覆盖，P2G 读到更新后的速度
    if (drag_bc_.isDragging != 0 && drag_bc_pipeline_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, drag_bc_pipeline_->pipeline.get());
        drag_bc_pipeline_->bind(cmd, 0, Pipeline::DescriptorOption(0));
        vkCmdPushConstants(cmd, drag_bc_pipeline_->pipelineLayout.get(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(drag_bc_), &drag_bc_);
        uint32_t pgroups = (num_particles_ + 63) / 64;  // local_size_x=64：53wg→213wg 填满 96 SM（占用率优化）
        vkCmdDispatch(cmd, pgroups, 1, 1);
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

        uint32_t groups = (num_particles_ + 63) / 64;  // local_size_x=64：53wg→213wg 填满 96 SM（占用率优化）
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

    // 4. Grid Freeze 已融合进 GridUpdate（P2G 标记 freeze_mask → GridUpdate 内零化冻结节点速度）。
    //    原独立 dispatch + barrier C(GridUpdate→GridFreeze) 省掉；数值一致（单节点 floor 语义）。
    //    下方 barrier 同步 GridUpdate 的 grid.velocity/freeze_mask 写 → G2P 读。

    // Memory barrier: Grid write (update + freeze-zero) -> Read (G2P)
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

        uint32_t groups = (num_particles_ + 63) / 64;  // local_size_x=64：53wg→213wg 填满 96 SM（占用率优化）
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // 6. PinFrozen 已合并进 G2P shader 末尾（冻结粒子硬钉 init/0/I/0）。
    //    原独立 dispatch + 跨 dispatch 写后写排序省掉；G2P 单线程内顺序覆写，数值一致。
    //    下方 barrier 仍需保留：保证本子步 G2P 写 particle 对下一子步 P2G/HomeSpring/DragBC 可见。

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
