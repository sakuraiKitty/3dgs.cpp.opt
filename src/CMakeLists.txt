cmake_minimum_required(VERSION 3.26)
project(3dgs_cpp)

add_subdirectory(shaders)

file(GLOB EXTERNAL_SOURCE
        ${imgui_SOURCE_DIR}/*.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
        third_party/implot/implot.cpp
        third_party/implot/implot_demo.cpp
        third_party/implot/implot_items.cpp
)

file(GLOB SOURCE
        *.cpp
        vulkan/*.cpp
        vulkan/pipelines/*.cpp
        vulkan/windowing/GLFWWindow.cpp
        mpm/*.cpp
        interaction/*.cpp
        coupling/*.cpp)

# Remove DummyGUIManager.cpp from source list
list(REMOVE_ITEM SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/DummyGUIManager.cpp)

# MPM physics simulation - Phase 1核心组件已启用
# 以下组件保持启用：
# - MPMManager.cpp (MPM核心仿真)
# - ParticleGenerator.cpp (粒子生成)
# - coupling/*.cpp (高斯-粒子耦合)
# - interaction/*.cpp (用户交互系统)

# Note: MPMInitializer.cpp is automatically included by GLOB and will be compiled

add_library(3dgs_cpp STATIC
        ${SOURCE}
        ${EXTERNAL_SOURCE}
)

target_include_directories(3dgs_cpp
        PUBLIC
        ${CMAKE_SOURCE_DIR}/include/3dgs
        PRIVATE
        third_party
        ${Vulkan_INCLUDE_DIRS}
        ${GLM_INCLUDE_DIRS}
        ${glm_SOURCE_DIR}
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${spdlog_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/shaders
)

add_dependencies(3dgs_cpp shaders)

target_link_libraries(3dgs_cpp PUBLIC Vulkan::Vulkan glfw spdlog::spdlog)
if (MSVC)
    # Use UTF-8 source file encoding
    target_compile_options(3dgs_cpp PUBLIC /utf-8)
endif ()
if (UNIX)
    target_link_libraries(3dgs_cpp PUBLIC ${CMAKE_DL_LIBS})
endif ()