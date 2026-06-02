#include "GaussianModel.h"
#include "vulkan/VulkanContext.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <limits>

bool GaussianModel::LoadPLY(const std::string& ply_path) {
    spdlog::info("[GaussianModel] Loading PLY: {}", ply_path);

    std::ifstream file(ply_path, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("[GaussianModel] Failed to open: {}", ply_path);
        return false;
    }

    PLYHeader header;
    if (!ParsePLYHeader(file, header)) {
        spdlog::error("[GaussianModel] Failed to parse PLY header");
        return false;
    }

    spdlog::info("[GaussianModel] Vertices: {}, format: {}",
                 header.num_vertices, header.is_binary ? "binary" : "ascii");

    xyz_.reserve(header.num_vertices);
    features_dc_.reserve(header.num_vertices);
    scaling_.reserve(header.num_vertices);
    rotation_.reserve(header.num_vertices);
    opacity_.reserve(header.num_vertices);
    features_rest_.reserve(header.num_vertices * 45);

    for (int i = 0; i < header.num_vertices; ++i) {
        glm::vec3 xyz;
        glm::vec3 features_dc;
        std::vector<float> features_rest(45);
        glm::vec3 scaling;
        glm::vec4 rotation;
        float opacity;

        bool success = header.is_binary
            ? ReadVertexBinary(file, header, xyz, features_dc, features_rest,
                               scaling, rotation, opacity)
            : ReadVertexASCII(file, header, xyz, features_dc, features_rest,
                              scaling, rotation, opacity);

        if (!success) {
            spdlog::error("[GaussianModel] Failed to read vertex {}", i);
            return false;
        }

        xyz_.push_back(xyz);
        features_dc_.push_back(features_dc);
        scaling_.push_back(scaling);
        rotation_.push_back(rotation);
        opacity_.push_back(opacity);
        features_rest_.insert(features_rest_.end(), features_rest.begin(), features_rest.end());
    }

    file.close();
    spdlog::info("[GaussianModel] Loaded {} gaussians", xyz_.size());
    return true;
}

bool GaussianModel::ParsePLYHeader(std::ifstream& file, PLYHeader& header) {
    std::string line;
    bool header_end = false;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string token;
        iss >> token;

        if (token == "ply") {
            continue;
        } else if (token == "format") {
            std::string format;
            iss >> format;
            header.is_binary = (format.find("binary") != std::string::npos);
        } else if (token == "element") {
            std::string element_type;
            iss >> element_type;
            if (element_type == "vertex") {
                iss >> header.num_vertices;
            }
        } else if (token == "property") {
            std::string type, name;
            iss >> type >> name;
            header.properties.push_back({type, name});
        } else if (token == "end_header") {
            header_end = true;
            break;
        }
    }

    if (!header_end) {
        spdlog::error("[GaussianModel] No end_header found");
        return false;
    }

    if (header.num_vertices <= 0) {
        spdlog::error("[GaussianModel] Invalid vertex count: {}", header.num_vertices);
        return false;
    }

    return true;
}

int GaussianModel::FindPropertyIndex(const PLYHeader& header, const std::string& name) const {
    for (size_t i = 0; i < header.properties.size(); ++i) {
        if (header.properties[i].second == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool GaussianModel::ReadVertexBinary(
    std::ifstream& file,
    const PLYHeader& header,
    glm::vec3& xyz,
    glm::vec3& features_dc,
    std::vector<float>& features_rest,
    glm::vec3& scaling,
    glm::vec4& rotation,
    float& opacity
) {
    xyz = glm::vec3(0.0f);
    features_dc = glm::vec3(0.0f);
    std::fill(features_rest.begin(), features_rest.end(), 0.0f);
    scaling = glm::vec3(0.0f);
    rotation = glm::vec4(0.0f);
    opacity = 0.0f;

    float temp;
    for (const auto& prop : header.properties) {
        const std::string& name = prop.second;
        if (name == "x") {
            file.read(reinterpret_cast<char*>(&xyz.x), sizeof(float));
        } else if (name == "y") {
            file.read(reinterpret_cast<char*>(&xyz.y), sizeof(float));
        } else if (name == "z") {
            file.read(reinterpret_cast<char*>(&xyz.z), sizeof(float));
        } else if (name == "nx" || name == "ny" || name == "nz") {
            file.read(reinterpret_cast<char*>(&temp), sizeof(float));
        } else if (name == "f_dc_0") {
            file.read(reinterpret_cast<char*>(&features_dc.x), sizeof(float));
        } else if (name == "f_dc_1") {
            file.read(reinterpret_cast<char*>(&features_dc.y), sizeof(float));
        } else if (name == "f_dc_2") {
            file.read(reinterpret_cast<char*>(&features_dc.z), sizeof(float));
        } else if (name.find("f_rest_") == 0) {
            // "f_rest_" has 7 characters, so the index starts at position 7
            if (name.length() > 7) {
                std::string idx_str = name.substr(7);
                try {
                    int idx = std::stoi(idx_str);
                    if (idx >= 0 && idx < 45) {
                        file.read(reinterpret_cast<char*>(&features_rest[idx]), sizeof(float));
                    } else {
                        file.read(reinterpret_cast<char*>(&temp), sizeof(float));
                    }
                } catch (const std::exception& e) {
                    spdlog::error("[GaussianModel] Failed to parse f_rest index: '{}' (idx_str: '{}')", name, idx_str);
                    file.read(reinterpret_cast<char*>(&temp), sizeof(float));
                }
            } else {
                spdlog::warn("[GaussianModel] Invalid f_rest property name: '{}'", name);
                file.read(reinterpret_cast<char*>(&temp), sizeof(float));
            }
        } else if (name == "opacity") {
            file.read(reinterpret_cast<char*>(&opacity), sizeof(float));
        } else if (name == "scale_0") {
            file.read(reinterpret_cast<char*>(&scaling.x), sizeof(float));
        } else if (name == "scale_1") {
            file.read(reinterpret_cast<char*>(&scaling.y), sizeof(float));
        } else if (name == "scale_2") {
            file.read(reinterpret_cast<char*>(&scaling.z), sizeof(float));
        } else if (name == "rot_0") {
            file.read(reinterpret_cast<char*>(&rotation.x), sizeof(float));
        } else if (name == "rot_1") {
            file.read(reinterpret_cast<char*>(&rotation.y), sizeof(float));
        } else if (name == "rot_2") {
            file.read(reinterpret_cast<char*>(&rotation.z), sizeof(float));
        } else if (name == "rot_3") {
            file.read(reinterpret_cast<char*>(&rotation.w), sizeof(float));
        } else {
            file.read(reinterpret_cast<char*>(&temp), sizeof(float));
        }
    }

    return !file.fail();
}

bool GaussianModel::ReadVertexASCII(
    std::ifstream& file,
    const PLYHeader& header,
    glm::vec3& xyz,
    glm::vec3& features_dc,
    std::vector<float>& features_rest,
    glm::vec3& scaling,
    glm::vec4& rotation,
    float& opacity
) {
    xyz = glm::vec3(0.0f);
    features_dc = glm::vec3(0.0f);
    std::fill(features_rest.begin(), features_rest.end(), 0.0f);
    scaling = glm::vec3(0.0f);
    rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    opacity = 0.0f;

    float temp;
    for (const auto& prop : header.properties) {
        const std::string& name = prop.second;
        if (name == "x") {
            file >> xyz.x;
        } else if (name == "y") {
            file >> xyz.y;
        } else if (name == "z") {
            file >> xyz.z;
        } else if (name == "nx" || name == "ny" || name == "nz") {
            file >> temp;
        } else if (name == "f_dc_0") {
            file >> features_dc.x;
        } else if (name == "f_dc_1") {
            file >> features_dc.y;
        } else if (name == "f_dc_2") {
            file >> features_dc.z;
        } else if (name.find("f_rest_") == 0) {
            // "f_rest_" has 7 characters, so the index starts at position 7
            if (name.length() > 7) {
                std::string idx_str = name.substr(7);
                try {
                    int idx = std::stoi(idx_str);
                    if (idx >= 0 && idx < 45) {
                        file >> features_rest[idx];
                    } else {
                        file >> temp;
                    }
                } catch (const std::exception& e) {
                    spdlog::error("[GaussianModel] Failed to parse f_rest index: '{}' (idx_str: '{}')", name, idx_str);
                    file >> temp;
                }
            } else {
                spdlog::warn("[GaussianModel] Invalid f_rest property name: '{}'", name);
                file >> temp;
            }
        } else if (name == "opacity") {
            file >> opacity;
        } else if (name == "scale_0") {
            file >> scaling.x;
        } else if (name == "scale_1") {
            file >> scaling.y;
        } else if (name == "scale_2") {
            file >> scaling.z;
        } else if (name == "rot_0") {
            file >> rotation.x;
        } else if (name == "rot_1") {
            file >> rotation.y;
        } else if (name == "rot_2") {
            file >> rotation.z;
        } else if (name == "rot_3") {
            file >> rotation.w;
        } else {
            file >> temp;
        }
    }

    return !file.fail();
}

std::vector<uint32_t> GaussianModel::GetForegroundIndices() const {
    std::vector<uint32_t> indices;
    if (sim_mask_.empty()) {
        spdlog::warn("[GaussianModel] sim_mask_ is empty, returning all indices");
        indices.resize(xyz_.size());
        std::iota(indices.begin(), indices.end(), 0);
        return indices;
    }

    indices.reserve(xyz_.size());
    for (size_t i = 0; i < sim_mask_.size(); ++i) {
        if (sim_mask_[i]) {
            indices.push_back(static_cast<uint32_t>(i));
        }
    }
    return indices;
}

std::vector<uint32_t> GaussianModel::GetBackgroundIndices() const {
    std::vector<uint32_t> indices;
    if (sim_mask_.empty()) {
        spdlog::warn("[GaussianModel] sim_mask_ is empty, returning empty background indices");
        return indices;
    }

    indices.reserve(xyz_.size());
    for (size_t i = 0; i < sim_mask_.size(); ++i) {
        if (!sim_mask_[i]) {
            indices.push_back(static_cast<uint32_t>(i));
        }
    }
    return indices;
}

bool GaussianModel::IsValid() const {
    if (xyz_.empty()) {
        spdlog::error("[GaussianModel] Invalid: xyz_ is empty");
        return false;
    }

    if (xyz_.size() != features_dc_.size() ||
        xyz_.size() != scaling_.size() ||
        xyz_.size() != rotation_.size() ||
        xyz_.size() != opacity_.size()) {
        spdlog::error("[GaussianModel] Invalid: attribute size mismatch");
        spdlog::error("[GaussianModel] xyz={}, dc={}, scaling={}, rotation={}, opacity={}",
                     xyz_.size(), features_dc_.size(), scaling_.size(),
                     rotation_.size(), opacity_.size());
        return false;
    }

    if (features_rest_.size() != xyz_.size() * 45) {
        spdlog::error("[GaussianModel] Invalid: features_rest_ size mismatch");
        spdlog::error("[GaussianModel] expected={}, actual={}",
                     xyz_.size() * 45, features_rest_.size());
        return false;
    }

    return true;
}

void GaussianModel::UploadToVulkan(const std::shared_ptr<VulkanContext>& context) {
    spdlog::info("[GaussianModel] UploadToVulkan: interface reserved for future use");
}
