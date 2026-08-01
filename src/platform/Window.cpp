#include "Window.h"

#include "../core/Utf.h"

#include <algorithm>
#include <vector>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

namespace acm::platform {
namespace {

constexpr wchar_t kWindowClassName[] = L"acomposterMainWindow";

} // namespace

// The message handler accumulates into this between pumpEvents calls; the
// window procedure is a static callback and cannot capture.
struct Window::Impl {
    ui::InputState pending;
    bool resized = false;
    int newWidth = 0;
    int newHeight = 0;
    bool closeRequested = false;
    ui::Cursor cursor = ui::Cursor::Arrow;

    HCURSOR cursorArrow = nullptr;
    HCURSOR cursorHand = nullptr;
    HCURSOR cursorHorizontal = nullptr;
    HCURSOR cursorVertical = nullptr;
    HCURSOR cursorDiagonal = nullptr;
    HCURSOR cursorText = nullptr;
    HCURSOR cursorCross = nullptr;
    HCURSOR cursorNo = nullptr;

    // Tracked so the UI gets a delta even when the pointer is captured and the
    // absolute position stops changing at the screen edge.
    ui::Vec2 lastMousePosition{ 0.0f, 0.0f };
    bool haveLastMouse = false;
};

namespace {

Window* windowFromHandle(HWND hwnd) {
    return reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

} // namespace

// ---------------------------------------------------------------------------

Window::Window() : impl_(new Impl()) {
    impl_->cursorArrow = ::LoadCursorW(nullptr, IDC_ARROW);
    impl_->cursorHand = ::LoadCursorW(nullptr, IDC_HAND);
    impl_->cursorHorizontal = ::LoadCursorW(nullptr, IDC_SIZEWE);
    impl_->cursorVertical = ::LoadCursorW(nullptr, IDC_SIZENS);
    impl_->cursorDiagonal = ::LoadCursorW(nullptr, IDC_SIZENWSE);
    impl_->cursorText = ::LoadCursorW(nullptr, IDC_IBEAM);
    impl_->cursorCross = ::LoadCursorW(nullptr, IDC_CROSS);
    impl_->cursorNo = ::LoadCursorW(nullptr, IDC_NO);
}

Window::~Window() {
    destroy();
    delete impl_;
}

bool Window::create(const std::string& title, int width, int height) {
    // Per-monitor DPI awareness so the interface is sharp on a laptop panel and
    // on a projector at the same time. Falls back quietly on older builds.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = nullptr;   // set per frame from the UI
    windowClass.hbrBackground = ::CreateSolidBrush(RGB(8, 9, 11));
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);

    if (!::RegisterClassExW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect{ 0, 0, width, height };
    ::AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_ACCEPTFILES,   // dropping a sample on the canvas is a core gesture
        kWindowClassName,
        utf8ToWide(title).c_str(),
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) return false;

    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    handle_ = hwnd;

    const UINT dpi = ::GetDpiForWindow(hwnd);
    dpiScale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;

    RECT client{};
    ::GetClientRect(hwnd, &client);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    ::DragAcceptFiles(hwnd, TRUE);

    return true;
}

void Window::destroy() {
    if (!handle_) return;
    ::SetWindowLongPtrW(static_cast<HWND>(handle_), GWLP_USERDATA, 0);
    ::DestroyWindow(static_cast<HWND>(handle_));
    handle_ = nullptr;
}

void Window::setTitle(const std::string& title) {
    if (handle_) ::SetWindowTextW(static_cast<HWND>(handle_), utf8ToWide(title).c_str());
}

void Window::applyCursor(ui::Cursor cursor) {
    impl_->cursor = cursor;

    HCURSOR handle = impl_->cursorArrow;
    switch (cursor) {
        case ui::Cursor::Hand:             handle = impl_->cursorHand; break;
        case ui::Cursor::ResizeHorizontal: handle = impl_->cursorHorizontal; break;
        case ui::Cursor::ResizeVertical:   handle = impl_->cursorVertical; break;
        case ui::Cursor::ResizeDiagonal:   handle = impl_->cursorDiagonal; break;
        case ui::Cursor::Text:             handle = impl_->cursorText; break;
        case ui::Cursor::Crosshair:        handle = impl_->cursorCross; break;
        case ui::Cursor::NotAllowed:       handle = impl_->cursorNo; break;
        case ui::Cursor::Arrow:            break;
    }
    ::SetCursor(handle);
}

bool Window::pumpEvents(ui::InputState& input) {
    // Carry the held state forward; only the edges are per-frame.
    input.clearPerFrame();

    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            shouldClose_ = true;
            break;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // Fold everything the window procedure collected into the caller's state.
    ui::InputState& pending = impl_->pending;

    // A modal loop belonging to somebody else - a drag out to another
    // application, a plugin's own editor, a shell dialog - swallows the
    // button-up that would have cleared a held button, and the state would then
    // stay down until the next click, with the UI convinced something was being
    // dragged the whole time. The physical state can only clear a button here,
    // never set one, so a press still has to arrive as a real message to count.
    {
        static constexpr int kMouseKeys[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
        for (int i = 0; i < 3; ++i) {
            if (!pending.mouseDown[i]) continue;
            if ((::GetAsyncKeyState(kMouseKeys[i]) & 0x8000) != 0) continue;
            pending.mouseDown[i] = false;
            pending.mouseReleased[i] = true;
        }
    }

    input.mousePosition = pending.mousePosition;
    input.wheel = pending.wheel;
    input.wheelHorizontal = pending.wheelHorizontal;
    input.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    input.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    input.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    input.windowFocused = (::GetForegroundWindow() == static_cast<HWND>(handle_));

    for (int i = 0; i < 3; ++i) {
        input.mouseDown[i] = pending.mouseDown[i];
        input.mousePressed[i] = pending.mousePressed[i];
        input.mouseReleased[i] = pending.mouseReleased[i];
        input.mouseDoubleClicked[i] = pending.mouseDoubleClicked[i];
    }

    input.mouseDelta = impl_->haveLastMouse
                           ? input.mousePosition - impl_->lastMousePosition
                           : ui::Vec2{ 0.0f, 0.0f };
    impl_->lastMousePosition = input.mousePosition;
    impl_->haveLastMouse = true;

    input.textInput = pending.textInput;
    input.keyEvents = pending.keyEvents;
    std::copy(std::begin(pending.keyHeld), std::end(pending.keyHeld), std::begin(input.keyHeld));
    input.droppedFiles = pending.droppedFiles;
    input.dropPosition = pending.dropPosition;

    pending.clearPerFrame();

    if (impl_->resized) {
        impl_->resized = false;
        width_ = impl_->newWidth;
        height_ = impl_->newHeight;
        if (onResize) onResize(width_, height_);
    }

    if (impl_->closeRequested) {
        impl_->closeRequested = false;
        // The application gets the final say: an unsaved patch can veto.
        if (!onCloseRequested || onCloseRequested()) shouldClose_ = true;
    }

    return !shouldClose_;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

namespace {

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Window* window = windowFromHandle(hwnd);
    if (!window) return ::DefWindowProcW(hwnd, message, wParam, lParam);

    Window::Impl& state = *window->state();

    const auto setButton = [&](int index, bool down, bool doubleClick) {
        if (down) {
            state.pending.mouseDown[index] = true;
            state.pending.mousePressed[index] = true;
            if (doubleClick) state.pending.mouseDoubleClicked[index] = true;
            ::SetCapture(hwnd);
        } else {
            state.pending.mouseDown[index] = false;
            state.pending.mouseReleased[index] = true;
            if (!state.pending.mouseDown[0] && !state.pending.mouseDown[1]
                && !state.pending.mouseDown[2])
                ::ReleaseCapture();
        }
    };

    switch (message) {
        case WM_CLOSE:
            state.closeRequested = true;
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                state.resized = true;
                state.newWidth = LOWORD(lParam);
                state.newHeight = HIWORD(lParam);
            }
            return 0;

        case WM_DPICHANGED: {
            // Windows supplies the rectangle the window should move to; honour
            // it or the window ends up the wrong size on the new monitor.
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_MOUSEMOVE:
            state.pending.mousePosition = { static_cast<float>(GET_X_LPARAM(lParam)),
                                            static_cast<float>(GET_Y_LPARAM(lParam)) };
            return 0;

        case WM_LBUTTONDOWN:   setButton(0, true, false); return 0;
        case WM_LBUTTONDBLCLK: setButton(0, true, true); return 0;
        case WM_LBUTTONUP:     setButton(0, false, false); return 0;
        case WM_RBUTTONDOWN:   setButton(1, true, false); return 0;
        case WM_RBUTTONDBLCLK: setButton(1, true, true); return 0;
        case WM_RBUTTONUP:     setButton(1, false, false); return 0;
        case WM_MBUTTONDOWN:   setButton(2, true, false); return 0;
        case WM_MBUTTONUP:     setButton(2, false, false); return 0;

        case WM_MOUSEWHEEL:
            state.pending.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            return 0;

        case WM_MOUSEHWHEEL:
            state.pending.wheelHorizontal +=
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int code = static_cast<int>(wParam);
            if (code >= 0 && code < 256) state.pending.keyHeld[code] = true;
            state.pending.keyEvents.push_back(
                ui::InputState::KeyEvent{ code, true, (lParam & (1 << 30)) != 0 });
            // Alt on its own would otherwise open the system menu and swallow
            // the next keystroke.
            if (message == WM_SYSKEYDOWN && wParam != VK_F4) return 0;
            break;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int code = static_cast<int>(wParam);
            if (code >= 0 && code < 256) state.pending.keyHeld[code] = false;
            state.pending.keyEvents.push_back(ui::InputState::KeyEvent{ code, false, false });
            break;
        }

        case WM_CHAR: {
            const auto codepoint = static_cast<std::uint32_t>(wParam);
            // Surrogates arrive as two messages; the text field only deals in
            // whole code points, so recombine here.
            static std::uint32_t highSurrogate = 0;
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                highSurrogate = codepoint;
            } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF && highSurrogate != 0) {
                state.pending.textInput.push_back(
                    0x10000 + ((highSurrogate - 0xD800) << 10) + (codepoint - 0xDC00));
                highSurrogate = 0;
            } else {
                highSurrogate = 0;
                if (codepoint >= 0x20 && codepoint != 0x7F)
                    state.pending.textInput.push_back(codepoint);
            }
            return 0;
        }

        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

            POINT point{};
            ::DragQueryPoint(drop, &point);
            state.pending.dropPosition = { static_cast<float>(point.x), static_cast<float>(point.y) };

            for (UINT i = 0; i < count; ++i) {
                const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
                std::vector<wchar_t> buffer(length + 1);
                ::DragQueryFileW(drop, i, buffer.data(), length + 1);
                state.pending.droppedFiles.push_back(wideToUtf8(std::wstring_view(buffer.data(), length)));
            }

            ::DragFinish(drop);
            return 0;
        }

        case WM_SETCURSOR:
            // Taking over the cursor entirely; the UI decides what it should be.
            if (LOWORD(lParam) == HTCLIENT) {
                window->applyCursor(state.cursor);
                return TRUE;
            }
            break;

        case WM_ERASEBKGND:
            return 1;   // the renderer clears every frame

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            // Below this the layout stops making sense.
            info->ptMinTrackSize.x = 900;
            info->ptMinTrackSize.y = 600;
            return 0;
        }

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace
} // namespace acm::platform
