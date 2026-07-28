#include "PluginEditorWindow.h"

#include "../core/Utf.h"

#include <windows.h>

namespace acm::vst2 {
namespace {

constexpr wchar_t kWindowClassName[] = L"acomposterPluginEditor";

// The window keeps a pointer to its owning PluginEditorWindow in GWLP_USERDATA.
// Closing is routed back to the owner rather than handled here: the plugin's
// editor has to be shut down through effEditClose before the HWND goes away, and
// only the owner knows how to do that.
LRESULT CALLBACK editorWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* owner = reinterpret_cast<PluginEditorWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CLOSE:
            if (owner && owner->closeCallback()) {
                owner->closeCallback()();
            } else {
                ::ShowWindow(hwnd, SW_HIDE);
            }
            return 0;

        case WM_ERASEBKGND:
            // The plugin paints its whole client area; erasing first only makes
            // a white flash on resize.
            return 1;

        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ensureWindowClass() {
    static const bool registered = [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        windowClass.lpfnWndProc = editorWindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = ::CreateSolidBrush(RGB(14, 15, 18));
        windowClass.lpszClassName = kWindowClassName;

        return ::RegisterClassExW(&windowClass) != 0
            || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();

    return registered;
}

} // namespace

PluginEditorWindow::~PluginEditorWindow() {
    destroy();
}

bool PluginEditorWindow::create(const std::string& title, int width, int height, void* ownerHandle) {
    if (handle_ != nullptr) return true;
    if (!ensureWindowClass()) return false;

    if (width < 64) width = 64;
    if (height < 64) height = 64;

    // A plugin reports the size it wants for its client area, so grow the outer
    // window to fit the border and caption around it.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{ 0, 0, width, height };
    ::AdjustWindowRectEx(&rect, style, FALSE, 0);

    const std::wstring wideTitle = utf8ToWide(title);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClassName,
        wideTitle.c_str(),
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        static_cast<HWND>(ownerHandle),
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);

    if (!hwnd) return false;

    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    handle_ = hwnd;
    return true;
}

void PluginEditorWindow::destroy() {
    if (!handle_) return;
    HWND hwnd = static_cast<HWND>(handle_);
    handle_ = nullptr;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd);
}

void PluginEditorWindow::resizeClient(int width, int height) {
    if (!handle_ || width <= 0 || height <= 0) return;

    HWND hwnd = static_cast<HWND>(handle_);
    const auto style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

    RECT rect{ 0, 0, width, height };
    ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    ::SetWindowPos(hwnd, nullptr, 0, 0,
                   rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void PluginEditorWindow::setTitle(const std::string& title) {
    if (!handle_) return;
    ::SetWindowTextW(static_cast<HWND>(handle_), utf8ToWide(title).c_str());
}

void PluginEditorWindow::bringToFront() {
    if (!handle_) return;
    HWND hwnd = static_cast<HWND>(handle_);
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    ::SetForegroundWindow(hwnd);
}

bool PluginEditorWindow::pumpMessages() {
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return true;
}

} // namespace acm::vst2
