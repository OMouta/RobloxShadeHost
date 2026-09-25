#include "capture.h"
#include "depth/depth.h"
#include "log.h"
#include "overlay.h"
#include "state.h"

#include <winrt/Windows.Foundation.Metadata.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <chrono>

using winrt::Windows::Foundation::Metadata::ApiInformation;

void CreateDevice()
{
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, g.device.put(), nullptr, g.context.put()));

    // The capture pool uses the device from its own worker threads.
    g.device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);

    auto dxgiDevice = g.device.as<IDXGIDevice1>();
    dxgiDevice->SetMaximumFrameLatency(1);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    g.captureDevice = inspectable.as<IDirect3DDevice>();
}

void StartCapture(HWND target)
{
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(target, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));

    g.poolSize = item.Size();
    g.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g.captureDevice, kPixelFormat, 2, g.poolSize);
    g.frameArrived = g.pool.FrameArrived(winrt::auto_revoke, [](auto&&, auto&&) { SetEvent(g.frameEvent); });
    g.session = g.pool.CreateCaptureSession(item);

    // The real cursor is already drawn on top of the overlay.
    g.session.IsCursorCaptureEnabled(false);
    if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
    {
        GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
        g.session.IsBorderRequired(false);
    }
    // Without this, capture can be capped at 60 FPS.
    if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval"))
        g.session.MinUpdateInterval(std::chrono::milliseconds(1));

    g.session.StartCapture();
    g.target = target;
    Log(LogLevel::Info, L"Capturing Roblox (%dx%d)", g.poolSize.Width, g.poolSize.Height);
}

void StopCapture()
{
    SetEditMode(false);
    g.frameArrived.revoke();
    g.latestFrame = nullptr;
    g.session.Close();
    g.session = nullptr;
    g.pool.Close();
    g.pool = nullptr;
    g.target = nullptr;
}

void PresentLatestFrame()
{
    winrt::com_ptr<ID3D11Texture2D> surface;
    auto access = g.latestFrame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), surface.put_void()));
    D3D11_TEXTURE2D_DESC size{};
    surface->GetDesc(&size);

    if (!g.swapchain)
    {
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(g.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = size.Width;
        desc.Height = size.Height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        winrt::check_hresult(factory->CreateSwapChainForHwnd(g.device.get(), g.overlay, &desc, nullptr, nullptr, g.swapchain.put()));
        factory->MakeWindowAssociation(g.overlay, DXGI_MWA_NO_ALT_ENTER);
    }
    else
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        g.swapchain->GetDesc1(&desc);
        if (desc.Width != size.Width || desc.Height != size.Height)
            winrt::check_hresult(g.swapchain->ResizeBuffers(0, size.Width, size.Height, DXGI_FORMAT_UNKNOWN, 0));
    }

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    winrt::check_hresult(g.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
    g.context->CopyResource(backBuffer.get(), surface.get());
    UpdateDepth(surface.get());
    winrt::check_hresult(g.swapchain->Present(0, 0));
}
