#pragma once

#include <unknwn.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <string>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

constexpr int kEditModeHotkey = 1;
constexpr int kOverlayToggleHotkey = 2;
// Posted when the ReShade menu is closed with ReShade's own key.
constexpr UINT kMenuClosedMessage = WM_APP + 1;

struct State
{
    HWND overlay = nullptr;
    HWND target = nullptr;
    HWND indicator = nullptr;
    std::wstring inputHotkey;
    std::wstring overlayHotkey;
    std::wstring indicatorText;
    bool editMode = false;
    bool inputHotkeyRegistered = false;
    bool overlayVisible = false;
    bool captureEnabled = true;
    RECT overlayRect{};

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGISwapChain1> swapchain;
    IDirect3DDevice captureDevice{ nullptr };

    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
    Direct3D11CaptureFrame latestFrame{ nullptr };
    SizeInt32 poolSize{};
    HANDLE frameEvent = nullptr;

    // Decided once per StartCapture from the target's monitor. SDR capture and presentation are
    // untouched (B8G8R8A8, direct copy); HDR needs FP16 capture to avoid clipping, tone-mapped back
    // down to normal sRGB before ReShade or the depth model ever see it, since neither expects
    // linear scRGB input.
    bool hdrActive = false;
    DirectXPixelFormat captureFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
    float sdrWhiteNits = 80.0f;

    // Lazily created the first time a frame is actually presented while hdrActive, since they are
    // only ever needed in that case. tonemapParams is rewritten every frame (sdrWhiteNits can
    // change on a later StartCapture without recreating these), and tonemapSourceCopy/View and
    // tonemapTargetView are only recreated when the surface or back buffer they wrap changes.
    winrt::com_ptr<ID3D11VertexShader> tonemapVS;
    winrt::com_ptr<ID3D11PixelShader> tonemapPS;
    winrt::com_ptr<ID3D11SamplerState> tonemapSampler;
    winrt::com_ptr<ID3D11Buffer> tonemapParams;
    winrt::com_ptr<ID3D11Texture2D> tonemapSourceCopy;
    winrt::com_ptr<ID3D11ShaderResourceView> tonemapSourceView;
    // A 2-entry cache, matching the swap chain's own buffer count: a flip-model swap chain with 2
    // buffers alternates GetBuffer(0) between exactly those 2 underlying resources frame to frame,
    // so a single cached pointer would recreate the view on every single frame instead of every
    // other one.
    ID3D11Texture2D* tonemapTargetBuffers[2] = {};
    winrt::com_ptr<ID3D11RenderTargetView> tonemapTargetViews[2];
};

extern State g;
