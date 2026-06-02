#ifndef GAUSSIAN_MODEL_H
#define GAUSSIAN_MODEL_H

#include <vector>
#include <glm/glm.hpp>
#include <string>
#include <memory>
#include <cstdint>
#include <fstream>

// Forward declarations
class VulkanContext;

class GaussianModel {
public:
    // Core attributes
    std::vector<glm::vec3> xyz_;
    std::vector<glm::vec3> features_dc_;
    std::vector<float> features_rest_;
    std::vector<glm::vec3> scaling_;
    std::vector<glm::vec4> rotation_;
    std::vector<float> opacity_;

    // Simulation attributes
    std::vector<bool> sim_mask_;

    GaussianModel() = default;
    ~GaussianModel() = default;

    bool LoadPLY(const std::string& ply_path);
    std::vector<uint32_t> GetForegroundIndices() const;
    std::vector<uint32_t> GetBackgroundIndices() const;
    size_t GetCount() const { return xyz_.size(); }
    bool IsValid() const;
    void UploadToVulkan(const std::shared_ptr<VulkanContext>& context);

private:
    struct PLYHeader {
        int num_vertices = 0;
        std::vector<std::pair<std::string, std::string>> properties;
        bool is_binary = false;
    };

    bool ParsePLYHeader(std::ifstream& file, PLYHeader& header);
    bool ReadVertexBinary(std::ifstream& file, const PLYHeader& header,
                          glm::vec3& xyz, glm::vec3& features_dc,
                          std::vector<float>& features_rest, glm::vec3& scaling,
                          glm::vec4& rotation, float& opacity);
    bool ReadVertexASCII(std::ifstream& file, const PLYHeader& header,
                         glm::vec3& xyz, glm::vec3& features_dc,
                         std::vector<float>& features_rest, glm::vec3& scaling,
                         glm::vec4& rotation, float& opacity);
    int FindPropertyIndex(const PLYHeader& header, const std::string& name) const;
};

#endif // GAUSSIAN_MODEL_H
