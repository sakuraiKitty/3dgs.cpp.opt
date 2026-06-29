#include "GLFWWindow.h"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/ext/matrix_transform.hpp>
#include <spdlog/spdlog.h>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// 必须定义这个才能使用 glfwGetWin32Window
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

GLFWWindow::GLFWWindow(std::string name, int width, int height) {
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(width, height, name.c_str(), nullptr, nullptr);

    spdlog::info("[GLFWWindow] Window created");
}

VkSurfaceKHR GLFWWindow::createSurface(std::shared_ptr<VulkanContext> context) {
    if (glfwCreateWindowSurface(context->instance.get(), static_cast<GLFWwindow *>(window), nullptr, &surface) !=
        VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }
    return surface;
}

std::array<bool, 3> GLFWWindow::getMouseButton() {
    return {
        glfwGetMouseButton(static_cast<GLFWwindow *>(window), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS,
        glfwGetMouseButton(static_cast<GLFWwindow *>(window), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS,
        glfwGetMouseButton(static_cast<GLFWwindow *>(window), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS
    };
}

std::vector<std::string> GLFWWindow::getRequiredInstanceExtensions() {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    auto extensions = std::vector<std::string>{};
    for (uint32_t i = 0; i < glfwExtensionCount; i++) {
        extensions.emplace_back(glfwExtensions[i]);
    }
    return extensions;
}

std::pair<uint32_t, uint32_t> GLFWWindow::getFramebufferSize() const {
    int width, height;
    glfwGetFramebufferSize(static_cast<GLFWwindow *>(window), &width, &height);
    return {width, height};
}

std::array<double, 2> GLFWWindow::getCursorTranslation() {
    // 使用回调存储的位置计算增量（替代 glfwGetCursorPos 轮询）
    const auto translation = std::array<double, 2>{callbackCursorX_ - lastX, callbackCursorY_ - lastY};
    lastX = callbackCursorX_;
    lastY = callbackCursorY_;
    return translation;
}

std::array<double, 2> GLFWWindow::getCursorPosition() {
    // 使用回调存储的位置（替代 glfwGetCursorPos 轮询）
    // glfwGetCursorPos 在 ImGui 回调拦截下会返回冻结坐标
    return {callbackCursorX_, callbackCursorY_};
}

void GLFWWindow::cursorPositionCallback(GLFWwindow* window, double x, double y) {
    GLFWWindow* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->callbackCursorX_ = x;
        self->callbackCursorY_ = y;
    }
    // 链式转发给 ImGui 的回调（确保 ImGui 也能收到光标事件）
    if (self && self->prevCursorPosCallback_) {
        self->prevCursorPosCallback_(window, x, y);
    }
}

void GLFWWindow::installCursorCallback() {
    auto* glfw_win = static_cast<GLFWwindow*>(window);
    // 设置窗口用户指针，以便回调能找到 GLFWWindow 实例
    glfwSetWindowUserPointer(glfw_win, this);
    // 保存 ImGui 的回调，并在其之上安装我们的回调
    // glfwSetCursorPosCallback 返回之前安装的回调（ImGui 的）
    prevCursorPosCallback_ = glfwSetCursorPosCallback(glfw_win, cursorPositionCallback);
    // 用 glfwGetCursorPos 初始化回调位置（仅用于首次初始化）
    double init_x, init_y;
    glfwGetCursorPos(glfw_win, &init_x, &init_y);
    callbackCursorX_ = init_x;
    callbackCursorY_ = init_y;
    lastX = init_x;
    lastY = init_y;
    spdlog::info("[GLFWWindow] Cursor position callback installed "
                 "(prevCallback={}, initPos=({:.1f},{:.1f}))",
                 prevCursorPosCallback_ ? "ImGui" : "null", init_x, init_y);
}

std::array<bool, 9> GLFWWindow::getKeys() {
    return {
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_W) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_A) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_S) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_D) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_SPACE) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_ESCAPE) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_F12) == GLFW_PRESS,
        glfwGetKey(static_cast<GLFWwindow *>(window), GLFW_KEY_P) == GLFW_PRESS
    };
}

void GLFWWindow::mouseCapture(bool capture) {
    if (capture) {
        glfwSetInputMode(static_cast<GLFWwindow *>(window), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    } else {
        glfwSetInputMode(static_cast<GLFWwindow *>(window), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

bool GLFWWindow::tick() {
    glfwPollEvents();
    return !glfwWindowShouldClose(static_cast<GLFWwindow *>(window));
}

void GLFWWindow::setCursor(int cursorType) {
    static int last_cursor_type = -1;
    static bool log_once = true;

    if (log_once) {
        spdlog::debug("[GLFWWindow] setCursor method active");
        log_once = false;
    }

    if (cursorType == last_cursor_type && last_cursor_type != -1) {
        return;
    }

#ifdef _WIN32
    // 使用 Windows API 直接设置光标
    HCURSOR hCursor = NULL;

    switch (cursorType) {
        case 0: // 默认箭头
            hCursor = LoadCursor(NULL, IDC_ARROW);
            break;
        case 1: // 手形 - 可变形区域
            hCursor = LoadCursor(NULL, IDC_HAND);
            break;
        case 2: // 十字 - 非可变形区域
            hCursor = LoadCursor(NULL, IDC_CROSS);
            break;
    }

    if (hCursor) {
        // 获取 GLFW 窗口的 HWND
        HWND hwnd = glfwGetWin32Window(static_cast<GLFWwindow*>(window));
        if (hwnd) {
            // 设置窗口类的光标（更持久）
            SetClassLongPtr(hwnd, GCLP_HCURSOR, (LONG_PTR)hCursor);
            // 立即设置当前光标
            SetCursor(hCursor);
            spdlog::debug("[GLFWWindow] Set cursor type {} (HWND={})", cursorType, (void*)hwnd);
        } else {
            spdlog::error("[GLFWWindow] Failed to get HWND");
        }
    } else {
        spdlog::error("[GLFWWindow] Failed to load cursor for type {}", cursorType);
    }
#endif

    last_cursor_type = cursorType;
}

void* GLFWWindow::createColorCursor(int r, int g, int b) {
    // 创建一个 16x16 的光标图像
    const int cursorSize = 16;
    // 为每个像素分配内存
    unsigned char* pixels = new unsigned char[cursorSize * cursorSize * 4]; // RGBA

    // 绘制箭头形状
    for (int y = 0; y < cursorSize; y++) {
        for (int x = 0; x < cursorSize; x++) {
            int idx = (y * cursorSize + x) * 4;
            bool isArrow = false;

            // 简单的箭头形状
            if (y < cursorSize / 2) {
                if (x <= y) {
                    isArrow = true;
                }
            } else {
                if (x < cursorSize / 2) {
                    isArrow = true;
                }
            }

            if (isArrow) {
                pixels[idx + 0] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
                pixels[idx + 3] = 255; // Alpha
            } else {
                pixels[idx + 0] = 0;
                pixels[idx + 1] = 0;
                pixels[idx + 2] = 0;
                pixels[idx + 3] = 0; // 透明
            }
        }
    }

    GLFWimage image;
    image.width = cursorSize;
    image.height = cursorSize;
    image.pixels = pixels;

    GLFWcursor* cursor = glfwCreateCursor(&image, 0, 0);

    // glfwCreateCursor 会复制图像数据，所以现在可以删除我们的临时数组
    delete[] pixels;

    if (!cursor) {
        spdlog::error("[GLFWWindow] Failed to create cursor with color RGB({}, {}, {})", r, g, b);
    } else {
        spdlog::info("[GLFWWindow] Created cursor RGB({}, {}, {})", r, g, b);
    }
    return cursor;
}