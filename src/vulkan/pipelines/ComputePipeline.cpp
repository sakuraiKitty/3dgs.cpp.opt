
#include "ComputePipeline.h"
#include <spdlog/spdlog.h>

ComputePipeline::ComputePipeline(const std::shared_ptr<VulkanContext>& context, std::shared_ptr<Shader> shader): Pipeline(context), shader(std::move(shader)) {
    spdlog::debug("[ComputePipeline] Constructor: loading shader");
    this->shader->load();
    spdlog::debug("[ComputePipeline] Shader loaded");
}

void ComputePipeline::build() {
    spdlog::debug("[ComputePipeline] Building pipeline layout");
    buildPipelineLayout();
    spdlog::debug("[ComputePipeline] Pipeline layout built");

    spdlog::debug("[ComputePipeline] Creating compute pipeline");
    vk::PipelineShaderStageCreateInfo pipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, shader->shader.get(), "main");
    vk::ComputePipelineCreateInfo computePipelineCreateInfo({}, pipelineShaderStageCreateInfo, pipelineLayout.get());

    spdlog::debug("[ComputePipeline] Calling createComputePipelineUnique");
    pipeline = context->device->createComputePipelineUnique(nullptr, computePipelineCreateInfo).value;
    spdlog::info("[ComputePipeline] Compute pipeline created successfully");
}

void ComputePipeline::rebuild() {
    spdlog::info("[ComputePipeline] Rebuilding compute pipeline with actual descriptor set layouts");

    // Step 1: Rebuild pipeline layout (replaces TEMP layout with actual descriptor set layout)
    rebuildPipelineLayout();

    // Step 2: Destroy old pipeline and recreate with new pipeline layout
    pipeline.reset();

    vk::PipelineShaderStageCreateInfo pipelineShaderStageCreateInfo(
        {}, vk::ShaderStageFlagBits::eCompute, shader->shader.get(), "main");
    vk::ComputePipelineCreateInfo computePipelineCreateInfo(
        {}, pipelineShaderStageCreateInfo, pipelineLayout.get());

    pipeline = context->device->createComputePipelineUnique(nullptr, computePipelineCreateInfo).value;
    spdlog::info("[ComputePipeline] Compute pipeline rebuilt successfully");
}
