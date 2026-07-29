// A plain Win32 top-level window to host a plugin's own editor.
//
// acomposter draws its interface with Direct3D and has no child-HWND concept, so
// plugin editors get their own floating windows rather than being embedded. That
// is also what makes the bridged case work without cross-process reparenting:
// the helper process owns its editor window outright, and Windows handles the
// z-order and focus like it would for any other application.
#pragma once

#include <functional>
#include <string>

namespace acm::vst2 {

class PluginEditorWindow {
public:
    PluginEditorWindow() = default;
    ~PluginEditorWindow();

    PluginEditorWindow(const PluginEditorWindow&) = delete;
    PluginEditorWindow& operator=(const PluginEditorWindow&) = delete;

    // `ownerHandle` is an optional HWND to sit above; pass null for none.
    bool create(const std::string& title, int width, int height, void* ownerHandle = nullptr);
    void destroy();

    bool open() const noexcept { return handle_ != nullptr; }
    // The HWND to hand to effEditOpen.
    void* nativeHandle() const noexcept { return handle_; }

    // Resizes the client area, honouring the plugin's requested dimensions.
    void resizeClient(int width, int height);
    void setTitle(const std::string& title);
    void bringToFront();

    // Invoked when the user closes the window; the owner should then call
    // effEditClose and tear this down. Teardown order matters, which is why the
    // window procedure hands the decision back rather than acting on WM_CLOSE.
    void setOnClose(std::function<void()> callback) { onClose_ = std::move(callback); }
    const std::function<void()>& closeCallback() const noexcept { return onClose_; }

    // Only needed in the bridge process, which has no other message loop.
    // Returns false when a WM_QUIT was seen.
    static bool pumpMessages();

private:
    void* handle_ = nullptr;
    std::function<void()> onClose_;
};

} // namespace acm::vst2
