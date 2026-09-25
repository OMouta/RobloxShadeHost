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

constexpr auto kPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
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
};

extern State g;
