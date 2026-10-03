#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct IDMLDevice;
struct OrtApi;
struct OrtDmlApi;
struct OrtEnv;
struct OrtSessionOptions;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtValue;

// Depth Anything V2 through ONNX Runtime on DirectML. onnxruntime.dll is loaded at runtime from the
// given directory, so the host still runs when the depth files are not installed.
class DepthModel
{
public:
    ~DepthModel();

    // Builds a session for one input size, since DirectML needs fixed shapes to run fast. device must be
    // the native D3D12 device rather than ReShade's proxy, which DirectML cannot run on. width and height
    // must be multiples of 14.
    // Throws std::runtime_error with a printable reason.
    void Load(const std::wstring& directory, const std::wstring& modelFile, int width, int height, ID3D12Device* device);

    // Whether the model takes and returns float16 rather than float32.
    bool HalfInput() const { return halfInput; }
    bool HalfOutput() const { return halfOutput; }
    // Where the model runs. Work for its input and output goes on the same queue.
    ID3D12CommandQueue* Queue() const { return queue; }

    // The buffers Run reads and writes from now on, on the device given to Load. input holds planar RGB,
    // ImageNet-normalized, 3 * width * height values. output receives width * height values of relative
    // inverse depth, larger being closer. Only call while the GPU is not running the model.
    void Bind(ID3D12Resource* input, ID3D12Resource* output);

    // Puts the model on the queue behind what is already there. Returns before the GPU is done.
    void Run();

private:
    void Unbind();

    void* library = nullptr;
    void* dmlLibrary = nullptr;
    IDMLDevice* dml = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    const OrtApi* api = nullptr;
    const OrtDmlApi* dmlApi = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::string inputName;
    std::string outputName;
    bool halfInput = false;
    bool halfOutput = false;
    std::vector<int64_t> inputShape;
    std::vector<int64_t> outputShape;
    // Held until other buffers are bound, or the model is gone.
    ID3D12Resource* inputBuffer = nullptr;
    ID3D12Resource* outputBuffer = nullptr;
    void* inputAllocation = nullptr;
    void* outputAllocation = nullptr;
    OrtValue* inputValue = nullptr;
    OrtValue* outputValue = nullptr;
};
