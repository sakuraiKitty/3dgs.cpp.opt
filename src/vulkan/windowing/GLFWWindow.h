#ifndef GLFWWINDOW_H
#define GLFWWINDOW_H

#include "../Window.h"
#include <array>

// 前向声明 GLFW 类型（避免在头文件中 include GLFW，防止 GLFW_INCLUDE_VULKAN 冲突）
struct GLFWwindow;
typedef void (*GLFWcursorposfun)(GLFWwindow*, double, double);

class GLFWWindow final : public Window {
public:
    GLFWWindow(std::string name, int width, int height);

    VkSurfaceKHR createSurface(std::shared_ptr<VulkanContext> context) override;

    std::array<bool, 3> getMouseButton() override;

    std::vector<std::string> getRequiredInstanceExtensions() override;

    [[nodiscard]] std::pair<uint32_t, uint32_t> getFramebufferSize() const override;

    std::array<double, 2> getCursorTranslation() override;

    std::array<double, 2> getCursorPosition() override;

    std::array<bool, 9> getKeys() override;

    void mouseCapture(bool capture) override;

    bool tick() override;

    /**
     * 设置光标样式
     * @param cursorType 0=默认, 1=绿色(可变形区域), 2=红色(非可变形区域)
     */
    void setCursor(int cursorType);

    /**
     * 安装光标位置回调（必须在 ImGui init 之后调用）
     * 我们的回调覆盖 ImGui 的回调，并链式转发给 ImGui
     * 确保光标位置始终通过事件驱动更新，而非轮询
     */
    void installCursorCallback();

    void* window;

private:
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    double lastX = 0.0;
    double lastY = 0.0;

    // 自定义光标
    void* defaultCursor = nullptr;
    void* greenCursor = nullptr;
    void* redCursor = nullptr;

    // ── 光标位置回调（替代 glfwGetCursorPos 轮询）──
    // ImGui 回调拦截导致 glfwGetCursorPos 返回冻结坐标
    // 解决方案：安装我们的回调在 ImGui 之上，确保光标位置始终更新
    GLFWcursorposfun prevCursorPosCallback_ = nullptr;  // ImGui 的回调（链式转发）
    double callbackCursorX_ = 0.0;  // 回调存储的光标 X
    double callbackCursorY_ = 0.0;  // 回调存储的光标 Y
    static void cursorPositionCallback(GLFWwindow* window, double x, double y);

    /**
     * 创建彩色光标
     * @param r 红色分量 (0-255)
     * @param g 绿色分量 (0-255)
     * @param b 蓝色分量 (0-255)
     */
    void* createColorCursor(int r, int g, int b);
};


#endif //GLFWWINDOW_H
