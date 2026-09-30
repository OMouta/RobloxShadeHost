#include "capture.h"
#include "depth/depth.h"
#include "hdr.h"
#include "log.h"
#include "overlay.h"
#include "state.h"

#include <winrt/Windows.Foundation.Metadata.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3dcompiler.h>

#include <chrono>
#include <stdexcept>

using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace
{
// A full-screen triangle from SV_VertexID alone, and an sRGB encode of the tone-mapped scRGB
// value. Only compiled and used while g.hdrActive; SDR frames never touch this at all.
constexpr char kTonemapShader[] = R"(
cbuffer Params : register(b0) { float SdrWhiteNits; float3 _pad; };
Texture2D<float4> Source : register(t0);
SamplerState PointClamp : register(s0);

void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 EncodeSrgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, c <= 0.0031308);
}

float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // scRGB defines 1.0 as the nominal 80-nit SDR reference white, so this cancels out whatever
    // brighter or dimmer nit value Windows' "SDR content brightness" setting currently maps SDR
    // white to, and only then clamps and encodes.
    float3 scrgb = Source.Sample(PointClamp, uv).rgb;
    float3 sdr = saturate(scrgb * (80.0 / SdrWhiteNits));
    return float4(EncodeSrgb(sdr), 1.0);
}
)";

void CreateTonemapResources()
{
    winrt::com_ptr<ID3DBlob> vsCode, psCode, errors;
    HRESULT hr = D3DCompile(kTonemapShader, sizeof(kTonemapShader) - 1, "tonemap", nullptr, nullptr, "VS", "vs_5_0", 0, 0, vsCode.put(), errors.put());
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "failed to compile the HDR tonemap vertex shader");
    errors = nullptr;
    hr = D3DCompile(kTonemapShader, sizeof(kTonemapShader) - 1, "tonemap", nullptr, nullptr, "PS", "ps_5_0", 0, 0, psCode.put(), errors.put());
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "failed to compile the HDR tonemap pixel shader");

    winrt::check_hresult(g.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, g.tonemapVS.put()));
    winrt::check_hresult(g.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, g.tonemapPS.put()));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    winrt::check_hresult(g.device->CreateSamplerState(&sampler, g.tonemapSampler.put()));

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16; // float + float3 padding, rewritten every frame below.
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(g.device->CreateBuffer(&desc, nullptr, g.tonemapParams.put()));
}

// Windows.Graphics.Capture's own surface is not assumed to support binding as a shader resource
// directly (the depth model's own preprocessing avoids that too, see depth.cpp's frameCopy), so
// it is copied into an owned texture that is guaranteed to, recreated only when its size changes.
void UpdateTonemapSource(ID3D11Texture2D* surface, const D3D11_TEXTURE2D_DESC& size)
{
    D3D11_TEXTURE2D_DESC current{};
    if (g.tonemapSourceCopy)
        g.tonemapSourceCopy->GetDesc(&current);
    if (!g.tonemapSourceCopy || current.Width != size.Width || current.Height != size.Height || current.Format != size.Format)
    {
        D3D11_TEXTURE2D_DESC copy = size;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        copy.SampleDesc = { 1, 0 };
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = 0;
        g.tonemapSourceCopy = nullptr;
        g.tonemapSourceView = nullptr;
        winrt::check_hresult(g.device->CreateTexture2D(&copy, nullptr, g.tonemapSourceCopy.put()));
        winrt::check_hresult(g.device->CreateShaderResourceView(g.tonemapSourceCopy.get(), nullptr, g.tonemapSourceView.put()));
    }
    g.context->CopyResource(g.tonemapSourceCopy.get(), surface);
}

// Tone-maps the FP16 scRGB capture down into the BGRA8 back buffer, since only the capture step
// needs HDR precision to avoid clipping; ReShade and the depth model both expect normal sRGB.
void TonemapToBackBuffer(ID3D11Texture2D* surface, ID3D11Texture2D* backBuffer, const D3D11_TEXTURE2D_DESC& size)
{
    if (!g.tonemapVS)
        CreateTonemapResources();
    UpdateTonemapSource(surface, size);

    ID3D11RenderTargetView* targetView = nullptr;
    for (int i = 0; i < 2 && !targetView; ++i)
        if (g.tonemapTargetBuffers[i] == backBuffer)
            targetView = g.tonemapTargetViews[i].get();
    if (!targetView)
    {
        // Neither slot matched (a fresh buffer, e.g. right after a resize): evict slot 0 and shift,
        // so the two most recently seen buffers are always the ones cached.
        g.tonemapTargetBuffers[0] = g.tonemapTargetBuffers[1];
        g.tonemapTargetViews[0] = g.tonemapTargetViews[1];
        g.tonemapTargetBuffers[1] = backBuffer;
        winrt::check_hresult(g.device->CreateRenderTargetView(backBuffer, nullptr, g.tonemapTargetViews[1].put()));
        targetView = g.tonemapTargetViews[1].get();
    }

    const struct { float sdrWhiteNits; float pad[3]; } params{ g.sdrWhiteNits, {} };
    g.context->UpdateSubresource(g.tonemapParams.get(), 0, nullptr, &params, 0, 0);

    const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(size.Width), static_cast<float>(size.Height), 0, 1 };
    ID3D11ShaderResourceView* srvs[] = { g.tonemapSourceView.get() };
    ID3D11SamplerState* samplers[] = { g.tonemapSampler.get() };
    ID3D11RenderTargetView* rtvs[] = { targetView };
    ID3D11Buffer* constants[] = { g.tonemapParams.get() };

    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(g.tonemapVS.get(), nullptr, 0);
    g.context->PSSetShader(g.tonemapPS.get(), nullptr, 0);
    g.context->PSSetShaderResources(0, 1, srvs);
    g.context->PSSetSamplers(0, 1, samplers);
    g.context->PSSetConstantBuffers(0, 1, constants);
    g.context->RSSetViewports(1, &viewport);
    g.context->OMSetRenderTargets(1, rtvs, nullptr);
    g.context->Draw(3, 0);

    srvs[0] = nullptr;
    rtvs[0] = nullptr;
    g.context->PSSetShaderResources(0, 1, srvs);
    g.context->OMSetRenderTargets(1, rtvs, nullptr);
}
} // namespace

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

    // SDR stays B8G8R8A8 with a direct copy, exactly as without this check. Only HDR needs FP16 to
    // avoid clipping, and is tone-mapped back down before ReShade or the depth model see it.
    g.hdrActive = IsAdvancedColorEnabled(target);
    g.captureFormat = g.hdrActive ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized;
    g.sdrWhiteNits = g.hdrActive ? GetSdrWhiteLevelNits(target) : 80.0f;

    g.poolSize = item.Size();
    g.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g.captureDevice, g.captureFormat, 2, g.poolSize);
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
    Log(LogLevel::Info, L"Capturing Roblox (%dx%d)%ls", g.poolSize.Width, g.poolSize.Height, g.hdrActive ? L", HDR" : L"");
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
    // Both paths leave a normal sRGB B8G8R8A8 image in backBuffer, which is also what the depth
    // model reads: neither it nor ReShade's effects are written to expect linear scRGB input.
    if (g.hdrActive)
        TonemapToBackBuffer(surface.get(), backBuffer.get(), size);
    else
        g.context->CopyResource(backBuffer.get(), surface.get());
    UpdateDepth(backBuffer.get());
    winrt::check_hresult(g.swapchain->Present(0, 0));
}
