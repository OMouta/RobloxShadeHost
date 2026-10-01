#include "depth_model.h"

#include <windows.h>
#include <DirectXPackedVector.h>
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>

#include <stdexcept>

namespace
{
struct OrtError
{
    const OrtApi* api;
    void operator()(OrtStatus* status) const
    {
        if (!status)
            return;
        std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error(message);
    }
};

ONNXTensorElementDataType ElementType(const OrtApi* api, OrtTypeInfo* info)
{
    const OrtTensorTypeAndShapeInfo* tensor = nullptr;
    OrtError{ api }(api->CastTypeInfoToTensorInfo(info, &tensor));
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    OrtError{ api }(api->GetTensorElementType(tensor, &type));
    api->ReleaseTypeInfo(info);
    return type;
}

bool IsHalf(ONNXTensorElementDataType type, const char* what)
{
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
        return true;
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        return false;
    throw std::runtime_error(std::string("The model ") + what + " is neither float32 nor float16.");
}
} // namespace

DepthModel::~DepthModel()
{
    if (api)
    {
        if (session)
            api->ReleaseSession(session);
        if (options)
            api->ReleaseSessionOptions(options);
        if (memory)
            api->ReleaseMemoryInfo(memory);
        if (env)
            api->ReleaseEnv(env);
    }
    if (queue)
        queue->Release();
    if (dml)
        dml->Release();
    if (library)
        FreeLibrary(static_cast<HMODULE>(library));
    if (dmlLibrary)
        FreeLibrary(static_cast<HMODULE>(dmlLibrary));
}

void DepthModel::Load(const std::wstring& directory, const std::wstring& modelFile, int width, int height, ID3D12Device* device)
{
    // DirectML.dll is an import of onnxruntime.dll and resolves from the exe directory as well.
    library = LoadLibraryW((directory + L"onnxruntime.dll").c_str());
    dmlLibrary = LoadLibraryW((directory + L"DirectML.dll").c_str());
    if (!library || !dmlLibrary)
        throw std::runtime_error("onnxruntime.dll or DirectML.dll could not be loaded.");

    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    auto getApiBase = reinterpret_cast<GetApiBase>(GetProcAddress(static_cast<HMODULE>(library), "OrtGetApiBase"));
    using CreateDml = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    auto createDml = reinterpret_cast<CreateDml>(GetProcAddress(static_cast<HMODULE>(dmlLibrary), "DMLCreateDevice"));
    if (!getApiBase || !createDml)
        throw std::runtime_error("onnxruntime.dll or DirectML.dll is not the expected build.");
    api = getApiBase()->GetApi(ORT_API_VERSION);
    if (!api)
        throw std::runtime_error("onnxruntime.dll is older than the version this host was built for.");
    const OrtError check{ api };
    const OrtDmlApi* dmlApi = nullptr;
    check(api->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));

    // Letting ONNX Runtime pick the GPU would have it call D3D12CreateDevice and get ReShade's proxy,
    // on which DirectML fails while uploading its fused graph. Build the DirectML device and queue on
    // the given device instead.
    if (FAILED(createDml(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml))))
        throw std::runtime_error("DirectML could not use this GPU.");
    const D3D12_COMMAND_QUEUE_DESC queueDesc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
        throw std::runtime_error("The GPU command queue could not be created.");

    check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "Unishade", &env));
    check(api->CreateSessionOptions(&options));
    // DirectML requires sequential execution without memory patterns.
    check(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
    check(api->DisableMemPattern(options));
    check(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
    // Otherwise idle worker threads spin and take CPU time from the game.
    check(api->AddSessionConfigEntry(options, "session.intra_op.allow_spinning", "0"));
    // DirectML compiles the graph for fixed shapes. Left dynamic, the model runs many times slower.
    check(api->AddFreeDimensionOverrideByName(options, "batch_size", 1));
    check(api->AddFreeDimensionOverrideByName(options, "height", height));
    check(api->AddFreeDimensionOverrideByName(options, "width", width));
    check(dmlApi->SessionOptionsAppendExecutionProvider_DML1(options, dml, queue));
    check(api->CreateSession(env, (directory + modelFile).c_str(), options, &session));
    check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));

    OrtAllocator* allocator = nullptr;
    check(api->GetAllocatorWithDefaultOptions(&allocator));
    size_t inputs = 0, outputs = 0;
    check(api->SessionGetInputCount(session, &inputs));
    check(api->SessionGetOutputCount(session, &outputs));
    if (inputs != 1 || outputs != 1)
        throw std::runtime_error("The model does not take one image and return one depth map.");
    char* name = nullptr;
    check(api->SessionGetInputName(session, 0, allocator, &name));
    inputName = name;
    check(api->AllocatorFree(allocator, name));
    check(api->SessionGetOutputName(session, 0, allocator, &name));
    outputName = name;
    check(api->AllocatorFree(allocator, name));

    OrtTypeInfo* info = nullptr;
    check(api->SessionGetInputTypeInfo(session, 0, &info));
    halfInput = IsHalf(ElementType(api, info), "input");
    check(api->SessionGetOutputTypeInfo(session, 0, &info));
    halfOutput = IsHalf(ElementType(api, info), "output");
}

void DepthModel::Run(const std::vector<float>& input, int width, int height, std::vector<float>& output)
{
    using namespace DirectX::PackedVector;
    const OrtError check{ api };
    const size_t count = static_cast<size_t>(width) * height;
    if (input.size() != 3 * count)
        throw std::logic_error("Depth input size mismatch.");

    const int64_t shape[4] = { 1, 3, height, width };
    const void* data = input.data();
    size_t bytes = input.size() * sizeof(float);
    if (halfInput)
    {
        halfBuffer.resize(input.size());
        XMConvertFloatToHalfStream(halfBuffer.data(), sizeof(HALF), input.data(), sizeof(float), input.size());
        data = halfBuffer.data();
        bytes = halfBuffer.size() * sizeof(HALF);
    }

    OrtValue* in = nullptr;
    check(api->CreateTensorWithDataAsOrtValue(memory, const_cast<void*>(data), bytes, shape, 4,
                                              halfInput ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
    OrtValue* out = nullptr;
    const char* inNames[] = { inputName.c_str() };
    const char* outNames[] = { outputName.c_str() };
    OrtStatus* status = api->Run(session, nullptr, inNames, &in, 1, outNames, 1, &out);
    api->ReleaseValue(in);
    if (status)
    {
        if (out)
            api->ReleaseValue(out);
        check(status);
    }

    OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
    size_t elements = 0;
    void* result = nullptr;
    OrtStatus* failure = api->GetTensorTypeAndShape(out, &shapeInfo);
    if (!failure)
    {
        failure = api->GetTensorShapeElementCount(shapeInfo, &elements);
        api->ReleaseTensorTypeAndShapeInfo(shapeInfo);
    }
    if (!failure)
        failure = api->GetTensorMutableData(out, &result);
    if (!failure && elements == count)
    {
        output.resize(count);
        if (halfOutput)
            XMConvertHalfToFloatStream(output.data(), sizeof(float), static_cast<const HALF*>(result), sizeof(HALF), count);
        else
            memcpy(output.data(), result, count * sizeof(float));
    }
    api->ReleaseValue(out);
    check(failure);
    if (elements != count)
        throw std::runtime_error("The model returned a depth map of unexpected size.");
}
