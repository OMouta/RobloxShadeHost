#include "depth.h"
#include "depth_model.h"
#include "../addon.h"
#include "../config.h"
#include "../log.h"
#include "../state.h"

#include <reshade.hpp>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
constexpr wchar_t kModelFile[] = L"depth-anything-v2-small.onnx";
// Longest side of the model input. Depth Anything V2 expects multiples of 14 around 518.
constexpr int kModelSize = 518;
// Default RESHADE_DEPTH_LINEARIZATION_FAR_PLANE, so no preprocessor changes are needed in ReShade.
constexpr float kFarPlane = 1000.0f;
// Blend factor for the per-frame depth range, so the whole image does not pulse when something
// close enters or leaves the view.
constexpr float kRangeSmoothing = 0.2f;

// Box-filters the frame down to the model size and writes planar, ImageNet-normalized RGB.
constexpr char kPreprocessShader[] = R"(
Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Sizes : register(b0) { uint2 SourceSize; uint2 OutputSize; };
static const float3 Mean = float3(0.485, 0.456, 0.406);
static const float3 Std = float3(0.229, 0.224, 0.225);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= OutputSize.x || id.y >= OutputSize.y)
        return;
    uint2 begin = id.xy * SourceSize / OutputSize;
    uint2 end = max((id.xy + 1) * SourceSize / OutputSize, begin + 1);
    float3 sum = 0;
    for (uint y = begin.y; y < end.y; ++y)
        for (uint x = begin.x; x < end.x; ++x)
            sum += Source.Load(int3(x, y, 0)).rgb;
    float3 color = (sum / ((end.x - begin.x) * (end.y - begin.y)) - Mean) / Std;
    uint plane = OutputSize.x * OutputSize.y;
    uint index = id.y * OutputSize.x + id.x;
    Output[index] = color.r;
    Output[index + plane] = color.g;
    Output[index + 2 * plane] = color.b;
}
)";

struct Depth
{
    bool enabled = false;
    std::wstring directory;

    int width = 0;  // model input and output size, follows the frame's aspect ratio
    int height = 0;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    winrt::com_ptr<ID3D11ComputeShader> shader;
    winrt::com_ptr<ID3D11Buffer> sizes;
    winrt::com_ptr<ID3D11Texture2D> frameCopy;
    winrt::com_ptr<ID3D11ShaderResourceView> frameView;
    winrt::com_ptr<ID3D11Buffer> preprocessed;
    winrt::com_ptr<ID3D11UnorderedAccessView> preprocessedView;
    winrt::com_ptr<ID3D11Buffer> staging;
    bool stagingPending = false;

    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    std::vector<reshade::api::effect_runtime*> runtimes;

    // ReShade wraps every D3D12 device created in this process in a proxy, and DirectML fails on the
    // proxy with DXGI_ERROR_DEVICE_REMOVED. The worker creates the device, ReShade reports the native
    // one through init_device, and the model runs on that. The proxy is kept so ReShade releases the
    // native device last.
    winrt::com_ptr<ID3D12Device> d3d12Proxy;
    winrt::com_ptr<ID3D12Device> d3d12;
    std::atomic<ID3D12Device*> nativeD3D12 = nullptr;

    // The worker owns the model. It only reads width, height and input while busy is set, and the main
    // thread only touches them while it is clear.
    std::thread worker;
    HANDLE inputReady = nullptr;
    std::atomic<bool> stopping = false;
    std::atomic<bool> busy = false;
    std::atomic<bool> resultReady = false;
    std::atomic<bool> failed = false;
    std::vector<float> input;
    std::vector<float> result;
    std::vector<float> encoded;
    bool haveRange = false;
    float rangeMin = 0;
    float rangeMax = 1;
};
Depth d;

void Bind(reshade::api::effect_runtime* runtime)
{
    const reshade::api::resource_view view{ reinterpret_cast<uint64_t>(d.view.get()) };
    runtime->update_texture_bindings("DEPTH", view, view);
    runtime->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime* runtime, reshade::api::effect_uniform_variable variable) {
        char source[32];
        if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
            runtime->set_uniform_value_bool(variable, d.view != nullptr);
    });
}

void OnInitRuntime(reshade::api::effect_runtime* runtime)
{
    d.runtimes.push_back(runtime);
    if (d.view)
        Bind(runtime);
}

void OnDestroyRuntime(reshade::api::effect_runtime* runtime)
{
    d.runtimes.erase(std::remove(d.runtimes.begin(), d.runtimes.end(), runtime), d.runtimes.end());
}

void OnReloadedEffects(reshade::api::effect_runtime* runtime)
{
    // Effect textures were recreated, and the generic depth add-on has just bound nothing to DEPTH.
    if (d.view)
        Bind(runtime);
}

void OnInitDevice(reshade::api::device* device)
{
    if (device->get_api() == reshade::api::device_api::d3d12)
        d.nativeD3D12 = reinterpret_cast<ID3D12Device*>(device->get_native());
}

// Runs on the worker thread, where init_device fires before D3D12CreateDevice returns.
void CreateD3D12Device()
{
    d.nativeD3D12 = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d.d3d12Proxy.put()))))
        throw std::runtime_error("DirectX 12 is unavailable on this GPU.");
    // Without ReShade's hooks there is no proxy and the created device is the native one.
    if (ID3D12Device* native = d.nativeD3D12)
        d.d3d12.copy_from(native);
    else
        d.d3d12 = d.d3d12Proxy;
}

void Worker()
{
    std::unique_ptr<DepthModel> model;
    int loadedWidth = 0, loadedHeight = 0;
    for (;;)
    {
        WaitForSingleObject(d.inputReady, INFINITE);
        if (d.stopping)
            return;
        try
        {
            if (d.width != loadedWidth || d.height != loadedHeight)
            {
                model.reset();
                if (!d.d3d12)
                    CreateD3D12Device();
                model = std::make_unique<DepthModel>();
                model->Load(d.directory, kModelFile, d.width, d.height, d.d3d12.get());
                loadedWidth = d.width;
                loadedHeight = d.height;
                Log(LogLevel::Ok, L"Depth model ready (%dx%d)", d.width, d.height);
            }
            model->Run(d.input, d.width, d.height, d.result);
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Error, L"Depth estimation stopped: %hs", e.what());
            d.failed = true;
        }
        d.resultReady = true;
    }
}

void CompileShader()
{
    winrt::com_ptr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kPreprocessShader, sizeof(kPreprocessShader) - 1, "depth", nullptr, nullptr, "main", "cs_5_0", 0, 0, code.put(), errors.put());
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compute shaders are unavailable on this GPU");
    winrt::check_hresult(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, d.shader.put()));

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(g.device->CreateBuffer(&desc, nullptr, d.sizes.put()));
}

// Sizes the model input to the frame's aspect ratio and recreates every size-dependent resource.
void Resize(const D3D11_TEXTURE2D_DESC& frame)
{
    const bool landscape = frame.Width >= frame.Height;
    const float ratio = landscape ? static_cast<float>(frame.Height) / frame.Width : static_cast<float>(frame.Width) / frame.Height;
    const int shortSide = std::max(14, static_cast<int>(std::lround(kModelSize * ratio / 14)) * 14);
    d.width = landscape ? kModelSize : shortSide;
    d.height = landscape ? shortSide : kModelSize;
    d.sourceWidth = frame.Width;
    d.sourceHeight = frame.Height;
    const size_t pixels = static_cast<size_t>(d.width) * d.height;
    d.input.resize(3 * pixels);
    d.encoded.assign(pixels, 1.0f);
    d.haveRange = false;
    d.stagingPending = false;

    D3D11_TEXTURE2D_DESC copy = frame;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.SampleDesc = { 1, 0 };
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    d.frameCopy = nullptr;
    d.frameView = nullptr;
    winrt::check_hresult(g.device->CreateTexture2D(&copy, nullptr, d.frameCopy.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.frameCopy.get(), nullptr, d.frameView.put()));

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = static_cast<UINT>(3 * pixels * sizeof(float));
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer.StructureByteStride = sizeof(float);
    d.preprocessed = nullptr;
    d.preprocessedView = nullptr;
    winrt::check_hresult(g.device->CreateBuffer(&buffer, nullptr, d.preprocessed.put()));
    winrt::check_hresult(g.device->CreateUnorderedAccessView(d.preprocessed.get(), nullptr, d.preprocessedView.put()));
    buffer.Usage = D3D11_USAGE_STAGING;
    buffer.BindFlags = 0;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    buffer.MiscFlags = 0;
    d.staging = nullptr;
    winrt::check_hresult(g.device->CreateBuffer(&buffer, nullptr, d.staging.put()));

    const UINT sizes[4] = { frame.Width, frame.Height, static_cast<UINT>(d.width), static_cast<UINT>(d.height) };
    g.context->UpdateSubresource(d.sizes.get(), 0, nullptr, sizes, 0, 0);

    // Starts out at the far plane until the first estimate arrives.
    D3D11_TEXTURE2D_DESC depth{};
    depth.Width = d.width;
    depth.Height = d.height;
    depth.MipLevels = 1;
    depth.ArraySize = 1;
    depth.Format = DXGI_FORMAT_R32_FLOAT;
    depth.SampleDesc = { 1, 0 };
    depth.Usage = D3D11_USAGE_DYNAMIC;
    depth.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    depth.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const D3D11_SUBRESOURCE_DATA initial{ d.encoded.data(), static_cast<UINT>(d.width * sizeof(float)), 0 };
    d.texture = nullptr;
    d.view = nullptr;
    winrt::check_hresult(g.device->CreateTexture2D(&depth, &initial, d.texture.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.texture.get(), nullptr, d.view.put()));
    for (auto* runtime : d.runtimes)
        Bind(runtime);
}

// Converts relative inverse depth to the non-linear depth ReShade linearizes by default:
// linear = z / (far - z * (far - 1)), solved for z with linear = 0 nearest and 1 at the far plane.
void PublishResult()
{
    float lo = d.result[0], hi = d.result[0];
    for (float value : d.result)
    {
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    if (!d.haveRange)
    {
        d.rangeMin = lo;
        d.rangeMax = hi;
        d.haveRange = true;
    }
    d.rangeMin += (lo - d.rangeMin) * kRangeSmoothing;
    d.rangeMax += (hi - d.rangeMax) * kRangeSmoothing;
    const float scale = 1.0f / std::max(d.rangeMax - d.rangeMin, 1e-6f);
    for (size_t i = 0; i < d.result.size(); ++i)
    {
        const float linear = 1.0f - std::clamp((d.result[i] - d.rangeMin) * scale, 0.0f, 1.0f);
        d.encoded[i] = linear * kFarPlane / (1.0f + linear * (kFarPlane - 1.0f));
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(g.context->Map(d.texture.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    for (int y = 0; y < d.height; ++y)
        memcpy(static_cast<char*>(mapped.pData) + y * mapped.RowPitch, d.encoded.data() + static_cast<size_t>(y) * d.width, d.width * sizeof(float));
    g.context->Unmap(d.texture.get(), 0);
}
} // namespace

bool InitDepth()
{
    d.directory = ExeDirectory();
    if (GetFileAttributesW((d.directory + kModelFile).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log(LogLevel::Info, L"Depth estimation is not installed. Depth-based effects will not work.");
        return false;
    }
    if (!AddonRegistered())
    {
        Log(LogLevel::Warning, L"Depth estimation is off. It needs ReShade with full add-on support, with RobloxShadeHost enabled in ReShade's Add-ons tab.");
        return false;
    }
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
    reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);

    try
    {
        CompileShader();
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Depth estimation is off: %hs", e.what());
        return false;
    }
    d.inputReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d.worker = std::thread(Worker);
    d.enabled = true;
    return true;
}

void UpdateDepth(ID3D11Texture2D* frame)
{
    if (!d.enabled)
        return;

    if (d.resultReady)
    {
        d.resultReady = false;
        d.busy = false;
        if (d.failed)
        {
            d.enabled = false;
            return;
        }
        PublishResult();
    }
    if (d.busy)
        return;

    D3D11_TEXTURE2D_DESC desc{};
    frame->GetDesc(&desc);
    if (desc.Width != d.sourceWidth || desc.Height != d.sourceHeight)
        Resize(desc);

    if (d.stagingPending)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = g.context->Map(d.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return;
        winrt::check_hresult(hr);
        memcpy(d.input.data(), mapped.pData, d.input.size() * sizeof(float));
        g.context->Unmap(d.staging.get(), 0);
        d.stagingPending = false;
        d.busy = true;
        SetEvent(d.inputReady);
        return;
    }

    g.context->CopyResource(d.frameCopy.get(), frame);
    ID3D11ShaderResourceView* views[] = { d.frameView.get() };
    ID3D11UnorderedAccessView* targets[] = { d.preprocessedView.get() };
    ID3D11Buffer* constants[] = { d.sizes.get() };
    g.context->CSSetShader(d.shader.get(), nullptr, 0);
    g.context->CSSetShaderResources(0, 1, views);
    g.context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    g.context->CSSetConstantBuffers(0, 1, constants);
    g.context->Dispatch((d.width + 15) / 16, (d.height + 15) / 16, 1);
    views[0] = nullptr;
    targets[0] = nullptr;
    g.context->CSSetShaderResources(0, 1, views);
    g.context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    g.context->CopyResource(d.staging.get(), d.preprocessed.get());
    d.stagingPending = true;
}

void ShutdownDepth()
{
    if (d.worker.joinable())
    {
        d.stopping = true;
        SetEvent(d.inputReady);
        d.worker.join();
    }
    d.d3d12 = nullptr;
    d.d3d12Proxy = nullptr;
    if (d.inputReady)
        CloseHandle(d.inputReady);
}
