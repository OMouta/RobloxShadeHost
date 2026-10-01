#include "capture.h"
#include "depth/depth.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "state.h"

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <utility>

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;

namespace
{
constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";
// Set when Windows does not allow capture without its border, so capture keeps it instead of failing to start.
std::atomic<bool> borderRequired = false;
} // namespace

void CreateDevice()
{
    // Made in locals, so a failure leaves no half-made device behind.
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, device.put(), nullptr, context.put()));

    // The capture pool uses the device from its own worker threads.
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);

    auto dxgiDevice = device.as<IDXGIDevice1>();
    dxgiDevice->SetMaximumFrameLatency(1);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    g.captureDevice = inspectable.as<IDirect3DDevice>();
    g.device = std::move(device);
    g.context = std::move(context);
}

bool DeviceLost(HRESULT error)
{
    return error == DXGI_ERROR_DEVICE_REMOVED || error == DXGI_ERROR_DEVICE_RESET || error == DXGI_ERROR_DEVICE_HUNG ||
           (g.device && FAILED(g.device->GetDeviceRemovedReason()));
}

void ReleaseDevice()
{
    StopCapture();
    g.swapchain = nullptr;
    ReleaseDepthDevice();
    g.captureDevice = nullptr;
    if (g.context)
    {
        g.context->ClearState();
        g.context->Flush();
    }
    g.context = nullptr;
    g.device = nullptr;
}

void RequestBorderlessCapture()
{
    if (!ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired"))
        return;
    try
    {
        // Windows answers in the background. Programs that are not packaged are allowed without a prompt.
        GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless)
            .Completed([](const auto& request, AsyncStatus status) {
                if (status == AsyncStatus::Completed && request.GetResults() == AppCapabilityAccessStatus::Allowed)
                    return;
                borderRequired = true;
                Log(LogLevel::Info, L"Windows did not allow capture without a border, so it may draw one around the game.");
            });
    }
    catch (const winrt::hresult_error& e)
    {
        borderRequired = true;
        Log(LogLevel::Info, L"Could not ask to capture without a border: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
    }
}

void StartCapture(HWND target)
{
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(target, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));

    g.poolSize = item.Size();
    g.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g.captureDevice, kPixelFormat, 2, g.poolSize);
    g.capturedFrames.store(0, std::memory_order_relaxed);
    g.frameStatistics.Reset(FrameStatistics::Clock::now(), 0);
    g.frameArrived = g.pool.FrameArrived(winrt::auto_revoke, [](auto&&, auto&&) {
        g.capturedFrames.fetch_add(1, std::memory_order_relaxed);
        SetEvent(g.frameEvent);
    });
    g.session = g.pool.CreateCaptureSession(item);

    // The real cursor is already drawn on top of the overlay.
    g.session.IsCursorCaptureEnabled(false);
    // RequestBorderlessCapture asked for this at startup.
    if (!borderRequired && ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired"))
        g.session.IsBorderRequired(false);
    // Without this, capture can be capped at 60 FPS.
    if (ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval"))
        g.session.MinUpdateInterval(std::chrono::milliseconds(1));

    g.session.StartCapture();
    g.target = target;
    Log(LogLevel::Info, L"Capturing %ls (%dx%d)", g.activeGame->name.c_str(), g.poolSize.Width, g.poolSize.Height);
    ShowStartHint();
}

void StopCapture()
{
    SetEditMode(false);
    g.frameArrived.revoke();
    g.latestFrame = nullptr;
    if (g.session)
        g.session.Close();
    g.session = nullptr;
    if (g.pool)
        g.pool.Close();
    g.pool = nullptr;
    g.target = nullptr;
    g.activeGame.reset();
    g.frameStatistics.Reset(FrameStatistics::Clock::now(), g.capturedFrames.load(std::memory_order_relaxed));
}

void PresentLatestFrame()
{
    const auto started = FrameStatistics::Clock::now();
    const int64_t frameTimestamp = g.latestFrame.SystemRelativeTime().count();
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
    const auto finished = FrameStatistics::Clock::now();
    g.frameStatistics.RecordPresent(frameTimestamp, std::chrono::duration<double, std::milli>(finished - started).count());
    g.frameStatistics.Update(finished, g.capturedFrames.load(std::memory_order_relaxed));
}
