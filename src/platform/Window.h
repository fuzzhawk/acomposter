// The application window.
//
// A thin Win32 wrapper whose only job is to own an HWND, translate messages
// into the UI's InputState, and tell the application when to resize. It knows
// nothing about drawing: the renderer takes the HWND and works independently.
#pragma once

#include "../ui/Ui.h"

#include <functional>
#include <string>

namespace acm::platform {

class Window {
public:
    Window();
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const std::string& title, int width, int height);
    void destroy();

    void* handle() const noexcept { return handle_; }
    bool open() const noexcept { return handle_ != nullptr && !shouldClose_; }
    void requestClose() noexcept { shouldClose_ = true; }

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    // Scale factor from the window's DPI; 1.0 at 96 dpi.
    float dpiScale() const noexcept { return dpiScale_; }

    void setTitle(const std::string& title);

    // Drains the message queue and folds everything that happened into `input`.
    // Returns false when the window has been asked to close.
    bool pumpEvents(ui::InputState& input);

    void applyCursor(ui::Cursor cursor);

    // Called when the client area changes size, before the next frame.
    std::function<void(int width, int height)> onResize;
    // Asked before closing. Return false to veto (an unsaved patch).
    std::function<bool()> onCloseRequested;

    // Message-pump scratch. Public only because the window procedure is a free
    // function - Win32 cannot call a non-static member - and it needs somewhere
    // to accumulate events between frames.
    struct Impl;
    Impl* state() const noexcept { return impl_; }

private:
    Impl* impl_ = nullptr;

    void* handle_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float dpiScale_ = 1.0f;
    bool shouldClose_ = false;
};

} // namespace acm::platform
