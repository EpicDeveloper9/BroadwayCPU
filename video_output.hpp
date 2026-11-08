#pragma once

#include <windows.h>

#include <d2d1.h>
#include <memory>
#include <wrl/client.h>

#include "video_frame.hpp"

namespace Broadway {

class VideoOutputWindow {
public:
    VideoOutputWindow(HINSTANCE instance, VideoFrameMailbox& frames);
    ~VideoOutputWindow();
    VideoOutputWindow(const VideoOutputWindow&) = delete;
    VideoOutputWindow& operator=(const VideoOutputWindow&) = delete;

    void Show();
    void ToggleFullscreen();
    void SetDiagnosticMode(bool enabled);
    void ClearFrame();
    bool IsOpen() const noexcept { return window != nullptr; }
    static LRESULT CALLBACK StaticWindowProc(HWND, UINT, WPARAM, LPARAM);

private:
    struct NotifyState;
    LRESULT WindowProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateDeviceResources();
    bool UploadCurrentFrame();
    void DestroyDeviceResources();
    void AcceptLatestFrame();
    void Draw(HWND);

    HINSTANCE instance;
    VideoFrameMailbox& frames;
    HWND window = nullptr;
    bool fullscreen = false;
    LONG_PTR savedStyle = 0;
    LONG_PTR savedExStyle = 0;
    RECT savedWindowRect{};
    std::shared_ptr<NotifyState> notifyState;
    std::shared_ptr<const VideoFrame> currentFrame;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> frameBitmap;
};

}
