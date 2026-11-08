#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "video_output.hpp"

#include <d2d1helper.h>
#include <dxgiformat.h>
#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>

namespace Broadway {
namespace {
constexpr wchar_t kVideoClass[] = L"BroadwayCPU.VideoOutputWindow";
constexpr wchar_t kVideoTitle[] = L"BroadwayCPU — Wii Video Output";
constexpr UINT kNewFrameMessage = WM_APP + 41;

bool RegisterVideoClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = VideoOutputWindow::StaticWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kVideoClass;
    if (RegisterClassExW(&wc) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
}

struct VideoOutputWindow::NotifyState {
    std::atomic<HWND> target{nullptr};
};

VideoOutputWindow::VideoOutputWindow(HINSTANCE hInstance, VideoFrameMailbox& mailbox)
    : instance(hInstance), frames(mailbox), notifyState(std::make_shared<NotifyState>()) {}

VideoOutputWindow::~VideoOutputWindow() {
    notifyState->target.store(nullptr, std::memory_order_release);
    frames.SetNotify({});
    if (window) DestroyWindow(window);
    DestroyDeviceResources();
}

void VideoOutputWindow::Show() {
    if (window && IsWindow(window)) {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        BringWindowToTop(window);
        return;
    }
    if (!RegisterVideoClass(instance))
        throw std::runtime_error("Could not register the Wii video output window class.");

    HWND created = CreateWindowExW(WS_EX_APPWINDOW, kVideoClass, kVideoTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 720,
        nullptr, nullptr, instance, this);
    if (!created) throw std::runtime_error("Could not create the Wii video output window.");
    window = created;
    notifyState->target.store(created, std::memory_order_release);
    std::weak_ptr<NotifyState> weakState = notifyState;
    frames.SetNotify([weakState] {
        if (const auto state = weakState.lock()) {
            if (const HWND target = state->target.load(std::memory_order_acquire))
                PostMessageW(target, kNewFrameMessage, 0, 0);
        }
    });
    AcceptLatestFrame();
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
}

void VideoOutputWindow::ToggleFullscreen() {
    if (!window) return;
    if (!fullscreen) {
        savedStyle = GetWindowLongPtrW(window, GWL_STYLE);
        savedExStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (!GetWindowRect(window, &savedWindowRect)) return;
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
        SetWindowLongPtrW(window, GWL_STYLE, (savedStyle & ~LONG_PTR(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, savedExStyle & ~LONG_PTR(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(window, HWND_TOP, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen = true;
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, savedStyle);
        SetWindowLongPtrW(window, GWL_EXSTYLE, savedExStyle);
        SetWindowPos(window, HWND_NOTOPMOST, savedWindowRect.left, savedWindowRect.top,
            savedWindowRect.right - savedWindowRect.left,
            savedWindowRect.bottom - savedWindowRect.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen = false;
    }
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK VideoOutputWindow::StaticWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    VideoOutputWindow* self = reinterpret_cast<VideoOutputWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = static_cast<VideoOutputWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window = hwnd;
    }
    return self ? self->WindowProc(hwnd, message, wp, lp) : DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT VideoOutputWindow::WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        CreateDeviceResources();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Draw(hwnd);
        return 0;
    case WM_SIZE:
        if (renderTarget) {
            const UINT width = LOWORD(lp), height = HIWORD(lp);
            if (width && height) renderTarget->Resize(D2D1::SizeU(width, height));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        RECT minimum{0, 0, 480, 360};
        const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        AdjustWindowRectEx(&minimum, style, FALSE, exStyle);
        info->ptMinTrackSize.x = minimum.right - minimum.left;
        info->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case kNewFrameMessage:
        AcceptLatestFrame();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) { ToggleFullscreen(); return 0; }
        if (wp == VK_ESCAPE && fullscreen) { ToggleFullscreen(); return 0; }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1ll << 29))) { ToggleFullscreen(); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        notifyState->target.store(nullptr, std::memory_order_release);
        frames.SetNotify({});
        currentFrame.reset();
        DestroyDeviceResources();
        window = nullptr;
        fullscreen = false;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (window == hwnd) window = nullptr;
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

bool VideoOutputWindow::CreateDeviceResources() {
    if (!factory) {
        const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(factory.GetAddressOf()));
        if (FAILED(hr)) return false;
    }
    if (!renderTarget && window) {
        RECT client{};
        GetClientRect(window, &client);
        const auto props = D2D1::RenderTargetProperties();
        const auto hwndProps = D2D1::HwndRenderTargetProperties(window,
            D2D1::SizeU(UINT(std::max<LONG>(1, client.right)), UINT(std::max<LONG>(1, client.bottom))),
            D2D1_PRESENT_OPTIONS_NONE);
        if (FAILED(factory->CreateHwndRenderTarget(props, hwndProps, &renderTarget))) return false;
    }
    return true;
}

void VideoOutputWindow::DestroyDeviceResources() {
    frameBitmap.Reset();
    renderTarget.Reset();
    factory.Reset();
}

void VideoOutputWindow::AcceptLatestFrame() {
    auto frame = frames.TakeLatest();
    if (!frame) return;
    if (!CreateDeviceResources()) return;
    try {
        BGRAFrame converted = ConvertToBGRA8(*frame);
        if (converted.rowPitch > UINT_MAX) return;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
        const HRESULT hr = renderTarget->CreateBitmap(D2D1::SizeU(converted.width, converted.height),
            converted.bytes.data(), UINT(converted.rowPitch), D2D1::BitmapProperties(format), &bitmap);
        if (FAILED(hr)) return;
        currentFrame = std::move(frame);
        frameBitmap = std::move(bitmap);
    } catch (...) {
        // A bad/oversized frame is dropped at the presentation boundary.
    }
}

void VideoOutputWindow::Draw(HWND hwnd) {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd, &paint);
    bool paintedWithDirect2D = false;
    if (CreateDeviceResources()) {
        renderTarget->BeginDraw();
        renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        if (frameBitmap) {
            const D2D1_SIZE_F area = renderTarget->GetSize();
            const D2D1_SIZE_F source = frameBitmap->GetSize();
            const float aspect = source.width / source.height;
            float width = area.width, height = width / aspect;
            if (height > area.height) { height = area.height; width = height * aspect; }
            const float left = (area.width - width) * 0.5f;
            const float top = (area.height - height) * 0.5f;
            const D2D1_RECT_F destination{left, top, left + width, top + height};
            renderTarget->DrawBitmap(frameBitmap.Get(), destination, 1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        const HRESULT hr = renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) DestroyDeviceResources();
        else if (SUCCEEDED(hr)) paintedWithDirect2D = true;
    }
    if (!paintedWithDirect2D) {
        HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(paint.hdc, &paint.rcPaint, black);
    }
    EndPaint(hwnd, &paint);
}

}
