#ifndef VULKAN_SPLATTING_COMPUTEPIPELINE_H
#define VULKAN_SPLATTING_COMPUTEPIPELINE_H


#include "Pipeline.h"
#include <memory>
#include "../Shader.h"

class ComputePipeline : public Pipeline {
public:
    explicit ComputePipeline(const std::shared_ptr<VulkanContext> &context, std::shared_ptr<Shader> shader);;

    void build() override;

    /**
     * Rebuild pipeline layout + compute pipeline using actual descriptor set layouts.
     * Called after descriptor sets are built (replaces TEMP layout from initial build).
     */
    void rebuild() override;
private:
    std::shared_ptr<Shader> shader;
};


#endif //VULKAN_SPLATTING_COMPUTEPIPELINE_H
