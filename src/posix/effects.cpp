#include "effects.h"
#include "log.h"
#include "preset_ini.h"

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <random>
#include <set>

namespace fx
{
// Everything a compiled effect needs on the graphics card.
struct PassGpu
{
    bool compute = false;
    bool toBackbuffer = false;
    bool clear = false;
    bool mipmaps = false;
    uint32_t targetCount = 0;
    std::vector<const GpuImage*> written; // render targets and storage, for their mipmaps
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[3]{};
    VkDescriptorSet sets[3]{};
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t vertices = 3;
    uint32_t dispatch[3]{ 1, 1, 1 };
};

struct EffectGpu
{
    GpuBuffer uniforms;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkShaderModule> modules;
    std::vector<std::vector<PassGpu>> techniques; // by the module's technique index
    uint32_t updatedFrame = UINT32_MAX;
};

Effect::Effect() = default;
Effect::Effect(Effect&&) noexcept = default;
Effect& Effect::operator=(Effect&&) noexcept = default;
Effect::~Effect() = default;

namespace
{
constexpr const char* kCompatibilityMacros =
    // The conversions ReShade adds for effects written for older versions.
    "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
    "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
    "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
    "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
    "#define tex2Dgather0 tex2DgatherR\n"
    "#define tex2Dgather1 tex2DgatherG\n"
    "#define tex2Dgather2 tex2DgatherB\n"
    "#define tex2Dgather3 tex2DgatherA\n";

const reshadefx::annotation* FindAnnotation(const std::vector<reshadefx::annotation>& annotations, std::string_view name)
{
    for (const reshadefx::annotation& annotation : annotations)
        if (annotation.name == name)
            return &annotation;
    return nullptr;
}

std::string AnnotationString(const std::vector<reshadefx::annotation>& annotations, std::string_view name)
{
    const reshadefx::annotation* annotation = FindAnnotation(annotations, name);
    return annotation && annotation->type.base == reshadefx::type::t_string ? annotation->value.string_data : std::string();
}

float AnnotationFloat(const std::vector<reshadefx::annotation>& annotations, std::string_view name, float fallback, size_t index = 0)
{
    const reshadefx::annotation* annotation = FindAnnotation(annotations, name);
    if (!annotation || index >= 16)
        return fallback;
    if (annotation->type.is_floating_point())
        return annotation->value.as_float[index];
    if (annotation->type.is_signed())
        return static_cast<float>(annotation->value.as_int[index]);
    if (annotation->type.is_numeric())
        return static_cast<float>(annotation->value.as_uint[index]);
    return fallback;
}

int AnnotationInt(const std::vector<reshadefx::annotation>& annotations, std::string_view name, int fallback)
{
    const reshadefx::annotation* annotation = FindAnnotation(annotations, name);
    if (!annotation)
        return fallback;
    if (annotation->type.is_floating_point())
        return static_cast<int>(annotation->value.as_float[0]);
    return annotation->type.is_numeric() ? annotation->value.as_int[0] : fallback;
}

VkFormat TextureFormat(reshadefx::texture_format format)
{
    using reshadefx::texture_format;
    switch (format)
    {
    case texture_format::r8: return VK_FORMAT_R8_UNORM;
    case texture_format::r16: return VK_FORMAT_R16_UNORM;
    case texture_format::r16f: return VK_FORMAT_R16_SFLOAT;
    case texture_format::r32f: return VK_FORMAT_R32_SFLOAT;
    case texture_format::r32u: return VK_FORMAT_R32_UINT;
    case texture_format::r32i: return VK_FORMAT_R32_SINT;
    case texture_format::rg8: return VK_FORMAT_R8G8_UNORM;
    case texture_format::rg16: return VK_FORMAT_R16G16_UNORM;
    case texture_format::rg16f: return VK_FORMAT_R16G16_SFLOAT;
    case texture_format::rg32f: return VK_FORMAT_R32G32_SFLOAT;
    case texture_format::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
    case texture_format::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
    case texture_format::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case texture_format::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case texture_format::rgba32u: return VK_FORMAT_R32G32B32A32_UINT;
    case texture_format::rgba32i: return VK_FORMAT_R32G32B32A32_SINT;
    case texture_format::rgb10a2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case texture_format::rg11b10f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkBlendFactor BlendFactor(reshadefx::blend_factor factor)
{
    using reshadefx::blend_factor;
    switch (factor)
    {
    case blend_factor::zero: return VK_BLEND_FACTOR_ZERO;
    case blend_factor::source_color: return VK_BLEND_FACTOR_SRC_COLOR;
    case blend_factor::one_minus_source_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case blend_factor::dest_color: return VK_BLEND_FACTOR_DST_COLOR;
    case blend_factor::one_minus_dest_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case blend_factor::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case blend_factor::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case blend_factor::dest_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case blend_factor::one_minus_dest_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(reshadefx::blend_op op)
{
    using reshadefx::blend_op;
    switch (op)
    {
    case blend_op::subtract: return VK_BLEND_OP_SUBTRACT;
    case blend_op::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case blend_op::min: return VK_BLEND_OP_MIN;
    case blend_op::max: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkPrimitiveTopology Topology(reshadefx::primitive_topology topology)
{
    using reshadefx::primitive_topology;
    switch (topology)
    {
    case primitive_topology::point_list: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case primitive_topology::line_list: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case primitive_topology::line_strip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case primitive_topology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkSamplerAddressMode AddressMode(reshadefx::texture_address_mode mode)
{
    using reshadefx::texture_address_mode;
    switch (mode)
    {
    case texture_address_mode::wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case texture_address_mode::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case texture_address_mode::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

struct CompileJob
{
    fs::path path;
    Definitions definitions;
};

struct CompileOptions
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vendor = 0;
    uint32_t device = 0;
    std::vector<fs::path> includePaths;
};

void BuildUniforms(Effect& effect)
{
    effect.uniformData.assign((effect.module.total_uniform_size + 15) & ~15u, 0);
    for (size_t i = 0; i < effect.module.uniforms.size(); ++i)
    {
        const reshadefx::uniform& declared = effect.module.uniforms[i];
        const auto& annotations = declared.annotations;
        Uniform uniform;
        uniform.name = declared.name;
        uniform.type = declared.type;
        uniform.offset = declared.offset;
        uniform.size = declared.size;
        uniform.declaration = i;
        uniform.label = AnnotationString(annotations, "ui_label");
        if (uniform.label.empty())
            uniform.label = declared.name;
        uniform.tooltip = AnnotationString(annotations, "ui_tooltip");
        uniform.uiType = AnnotationString(annotations, "ui_type");
        uniform.category = AnnotationString(annotations, "ui_category");
        uniform.text = AnnotationString(annotations, "ui_text");
        uniform.source = AnnotationString(annotations, "source");
        uniform.items = AnnotationString(annotations, "ui_items");
        if (!uniform.items.empty())
        {
            // Combo boxes need the list to end with an empty item.
            if (uniform.items.back() != '\0')
                uniform.items.push_back('\0');
            uniform.items.push_back('\0');
        }
        // Without a range, drags are unbounded, as in ReShade.
        const bool floating = declared.type.is_floating_point();
        uniform.min = AnnotationFloat(annotations, "ui_min", std::numeric_limits<float>::lowest());
        uniform.max = AnnotationFloat(annotations, "ui_max", std::numeric_limits<float>::max());
        uniform.step = AnnotationFloat(annotations, "ui_step", floating ? 0.001f : 1.0f);
        uniform.hidden = AnnotationInt(annotations, "hidden", 0) != 0 || !uniform.source.empty() || !declared.type.is_numeric();
        effect.uniforms.push_back(std::move(uniform));
    }
}

bool Compile(Effect& effect, const CompileJob& job, const CompileOptions& options)
{
    effect.path = job.path;
    effect.file = job.path.filename().string();

    reshadefx::preprocessor pp;
    // ReShade 6.8, running on Vulkan.
    pp.add_macro_definition("__RESHADE__", "60800");
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", std::to_string(options.vendor));
    pp.add_macro_definition("__DEVICE__", std::to_string(options.device));
    pp.add_macro_definition("__RENDERER__", "131072"); // 0x20000, Vulkan
    pp.add_macro_definition("__APPLICATION__", "0");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(options.width));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(options.height));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1"); // sRGB
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", "87"); // B8G8R8A8_UNORM, in ReShade's numbering
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
    // The first definition of a name wins, so the effect's own come first.
    for (const auto& [name, value] : job.definitions)
        if (!name.empty())
            pp.add_macro_definition(name, value.empty() ? "1" : value);
    pp.add_include_path(job.path.parent_path());
    for (const fs::path& include : options.includePaths)
        pp.add_include_path(include);
    pp.append_string(kCompatibilityMacros);

    const bool preprocessed = pp.append_file(job.path);
    effect.errors = pp.errors();
    if (!preprocessed)
        return false;

    // Vulkan's clip space is upside down compared to Direct3D's, which effects are written for.
    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(true, false, false, false, true));
    reshadefx::parser parser;
    const bool parsed = parser.parse(pp.output(), codegen.get());
    effect.errors += parser.errors();
    if (!parsed)
        return false;
    effect.module = codegen->module();

    for (const auto& [name, type] : effect.module.entry_points)
    {
        std::string binary, assembly, errors;
        if (!codegen->assemble_code_for_entry_point(name, binary, assembly, errors))
        {
            effect.errors += errors;
            return false;
        }
        std::vector<uint32_t>& code = effect.spirv[name];
        code.resize(binary.size() / 4);
        std::memcpy(code.data(), binary.data(), code.size() * 4);
    }
    BuildUniforms(effect);
    effect.compiled = true;
    return true;
}

// Reads the values of a uniform in ReShade's layout: every array element and every matrix row starts on
// 16 bytes.
size_t ComponentOffset(const Uniform& uniform, size_t i)
{
    const reshadefx::type& type = uniform.type;
    if (type.is_matrix())
    {
        const size_t element = i / type.components(), rest = i % type.components();
        return uniform.offset + (element * type.rows * 4 + (rest / type.cols) * 4 + rest % type.cols) * 4;
    }
    if (type.is_array())
        return uniform.offset + ((i / type.rows) * 4 + i % type.rows) * 4;
    return uniform.offset + i * 4;
}

size_t ComponentCount(const Uniform& uniform)
{
    return uniform.type.components() * (uniform.type.is_array() ? uniform.type.array_length : 1u);
}

std::string FormatFloat(float value)
{
    return std::to_string(value);
}
} // namespace

std::string TechniqueKey(const Technique& technique, const Effect& effect)
{
    return technique.name + "@" + effect.file;
}

bool Runtime::Init(const Settings& initial)
{
    settings = initial;
    if (!gpu.CreateImage(depth, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !gpu.CreateImage(blank, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    VkCommandBuffer commands = gpu.BeginCommands();
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (const GpuImage* image : { &depth, &blank })
    {
        InitLayout(commands, *image);
        vkCmdClearColorImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    gpu.SubmitAndWait(commands);
    return true;
}

void Runtime::Shutdown()
{
    StopLoader();
    if (!gpu.device)
        return;
    vkDeviceWaitIdle(gpu.device);
    DestroyAllGpu();
    for (auto& [desc, sampler] : samplers)
        vkDestroySampler(gpu.device, sampler, nullptr);
    samplers.clear();
    for (GpuImage* image : { &backbuffer, &color, &depth, &blank })
        gpu.DestroyImage(*image);
    gpu.DestroyBuffer(staging);
    gpu.DestroyBuffer(readback);
}

void Runtime::StopLoader()
{
    cancel = true;
    if (loader.joinable())
        loader.join();
    cancel = false;
    loaderRunning = false;
    std::lock_guard lock(finishedMutex);
    finished.clear();
}

bool Runtime::CreateTargets()
{
    vkDeviceWaitIdle(gpu.device);
    for (GpuImage* image : { &backbuffer, &color })
        gpu.DestroyImage(*image);
    gpu.DestroyBuffer(staging);
    const VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!gpu.CreateImage(backbuffer, width, height, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | transfer) ||
        !gpu.CreateImage(color, width, height, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | transfer) ||
        !gpu.CreateBuffer(staging, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
    {
        Log(LogLevel::Error, "Could not create %ux%u images for effects.", width, height);
        return false;
    }
    VkCommandBuffer commands = gpu.BeginCommands();
    InitLayout(commands, backbuffer);
    InitLayout(commands, color);
    gpu.SubmitAndWait(commands);
    return true;
}

void Runtime::SetSize(uint32_t newWidth, uint32_t newHeight)
{
    if (newWidth == width && newHeight == height)
        return;
    StopLoader();
    DestroyAllGpu();
    width = newWidth;
    height = newHeight;
    if (!CreateTargets())
    {
        width = height = 0;
        return;
    }
    // Right away: the size changes before the frame records any pass, so nothing uses the old effects yet.
    ReloadNow();
}

void Runtime::ReloadNow()
{
    reloadRequested = false;
    StopLoader();
    if (!width || !height)
        return;
    vkDeviceWaitIdle(gpu.device);
    DestroyAllGpu();
    effects.clear();
    techniques.clear();
    textureFilesScanned = false;

    // Effects the preset uses compile first, so the picture has its effects as soon as possible.
    const PresetIni preset(ReadFile(presetPath));
    std::string value;
    std::set<std::string> used;
    if (preset.Get("", "Techniques", value))
        for (const std::string& technique : PresetIni::Split(value))
            if (const size_t at = technique.find('@'); at != std::string::npos)
                used.insert(Lowercase(technique.substr(at + 1)));

    std::vector<CompileJob> jobs;
    std::set<std::string> seen;
    for (const fs::path& root : settings.effectPaths)
    {
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error))
        {
            if (error)
                break;
            if (!it->is_regular_file(error) || Lowercase(it->path().extension().string()) != ".fx")
                continue;
            // Presets name effects by file, so the first file of a name wins, as in ReShade.
            if (!seen.insert(Lowercase(it->path().filename().string())).second)
                continue;
            CompileJob job{ it->path(), {} };
            const std::string file = it->path().filename().string();
            if (const auto found = presetDefinitions.effects.find(file); found != presetDefinitions.effects.end())
                job.definitions = found->second;
            job.definitions.insert(job.definitions.end(), presetDefinitions.global.begin(), presetDefinitions.global.end());
            job.definitions.insert(job.definitions.end(), settings.definitions.begin(), settings.definitions.end());
            jobs.push_back(std::move(job));
        }
    }
    std::stable_sort(jobs.begin(), jobs.end(), [&used](const CompileJob& a, const CompileJob& b) {
        return used.count(Lowercase(a.path.filename().string())) > used.count(Lowercase(b.path.filename().string()));
    });

    CompileOptions options;
    options.width = width;
    options.height = height;
    options.vendor = gpu.properties.vendorID;
    options.device = gpu.properties.deviceID;
    options.includePaths = settings.effectPaths;

    loadedCount = 0;
    totalCount = jobs.size();
    if (jobs.empty())
    {
        Log(LogLevel::Warning, "No effects were found. Install them from the launcher.");
        return;
    }
    Log(LogLevel::Info, "Compiling %zu effects for %ux%u...", jobs.size(), width, height);
    loaderRunning = true;
    loader = std::thread([this, jobs = std::move(jobs), options] {
        std::atomic<size_t> next = 0;
        std::vector<std::thread> workers;
        const unsigned count = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
        for (unsigned i = 0; i < count; ++i)
            workers.emplace_back([&] {
                for (size_t index; !cancel && (index = next++) < jobs.size();)
                {
                    Effect effect;
                    Compile(effect, jobs[index], options);
                    ++loadedCount;
                    std::lock_guard lock(finishedMutex);
                    finished.push_back(std::move(effect));
                }
            });
        for (std::thread& worker : workers)
            worker.join();
        loaderRunning = false;
    });
}

void Runtime::Update()
{
    if (reloadRequested)
        ReloadNow();
    std::deque<Effect> arrived;
    {
        std::lock_guard lock(finishedMutex);
        arrived.swap(finished);
    }
    if (arrived.empty())
        return;
    for (Effect& effect : arrived)
        AddEffect(std::move(effect));
    SortTechniques();
    if (!loaderRunning)
    {
        std::lock_guard lock(finishedMutex);
        if (finished.empty())
        {
            size_t failed = 0;
            for (const Effect& effect : effects)
                failed += !effect.compiled;
            Log(LogLevel::Info, "Effects loaded: %zu compiled, %zu failed.", effects.size() - failed, failed);
            for (const std::string& missing : MissingTechniques())
                Report(LogLevel::Warning, "The preset uses %s, which is not installed.", missing.c_str());
        }
    }
}

void Runtime::AddEffect(Effect&& effect)
{
    if (!effect.compiled)
    {
        // Only the first lines: a broken effect can report hundreds.
        std::string first = effect.errors.substr(0, effect.errors.find('\n'));
        Log(LogLevel::Info, "%s did not compile: %s", effect.file.c_str(), first.c_str());
    }
    const size_t index = effects.size();
    effects.push_back(std::move(effect));
    Effect& added = effects.back();
    if (!added.compiled)
        return;
    for (size_t i = 0; i < added.module.techniques.size(); ++i)
    {
        const reshadefx::technique& declared = added.module.techniques[i];
        Technique technique;
        technique.name = declared.name;
        technique.label = AnnotationString(declared.annotations, "ui_label");
        technique.tooltip = AnnotationString(declared.annotations, "ui_tooltip");
        technique.hidden = AnnotationInt(declared.annotations, "hidden", 0) != 0;
        technique.enabledByDefault = AnnotationInt(declared.annotations, "enabled", 0) != 0;
        technique.effect = index;
        technique.index = i;
        techniques.push_back(std::move(technique));
    }
    ApplyPreset(added, index);
}

// Presets list the techniques that are on and, optionally, the order of all of them. Techniques neither lists
// keep their file's order and otherwise go by name, as in ReShade.
void Runtime::SortTechniques()
{
    const auto rank = [this](const Technique& technique) {
        const std::string key = TechniqueKey(technique, effects[technique.effect]);
        auto it = std::find(sorting.begin(), sorting.end(), key);
        if (it == sorting.end())
            it = std::find(sorting.begin(), sorting.end(), technique.name);
        return static_cast<size_t>(it - sorting.begin());
    };
    std::stable_sort(techniques.begin(), techniques.end(), [&](const Technique& a, const Technique& b) {
        const size_t left = rank(a), right = rank(b);
        if (left != right)
            return left < right;
        if (a.effect == b.effect)
            return a.index < b.index;
        return Lowercase(a.label.empty() ? a.name : a.label) < Lowercase(b.label.empty() ? b.name : b.label);
    });
}

void Runtime::ApplyPreset(Effect& effect, size_t effectIndex)
{
    const PresetIni preset(ReadFile(presetPath));
    std::string value;
    std::vector<std::string> enabled;
    if (preset.Get("", "Techniques", value))
        enabled = PresetIni::Split(value);
    for (Technique& technique : techniques)
        if (technique.effect == effectIndex)
        {
            const std::string key = TechniqueKey(technique, effect);
            technique.enabled = std::find(enabled.begin(), enabled.end(), key) != enabled.end() ||
                                std::find(enabled.begin(), enabled.end(), technique.name) != enabled.end() ||
                                (technique.hidden && technique.enabledByDefault);
        }

    for (const Uniform& uniform : effect.uniforms)
    {
        ResetValue(effect, uniform);
        if (!uniform.source.empty() || !preset.Get(effect.file, uniform.name, value))
            continue;
        const std::vector<std::string> items = PresetIni::Split(value);
        const size_t count = std::min(items.size(), ComponentCount(uniform));
        if (uniform.type.is_floating_point())
        {
            std::vector<float> values(count);
            for (size_t i = 0; i < count; ++i)
                values[i] = std::strtof(items[i].c_str(), nullptr);
            SetValue(effect, uniform, values.data(), count);
        }
        else
        {
            std::vector<int> values(count);
            for (size_t i = 0; i < count; ++i)
                values[i] = items[i] == "true" ? 1 : items[i] == "false" ? 0 : static_cast<int>(std::strtol(items[i].c_str(), nullptr, 10));
            SetValue(effect, uniform, values.data(), count);
        }
    }
}

bool Runtime::LoadPreset(const fs::path& path)
{
    presetPath = path;
    dirty = false;
    const PresetIni preset(ReadFile(path));
    PresetDefinitions definitions;
    std::string value;
    if (preset.Get("", "PreprocessorDefinitions", value))
        definitions.global = ParseDefinitions(value);
    std::vector<std::string> sortingList;
    if (preset.Get("", "TechniqueSorting", value))
        sortingList = PresetIni::Split(value);
    if (sortingList.empty() && preset.Get("", "Techniques", value))
        sortingList = PresetIni::Split(value);
    sorting = sortingList;

    for (const std::string& file : preset.SectionNames())
        if (preset.Get(file, "PreprocessorDefinitions", value))
            definitions.effects[file] = ParseDefinitions(value);

    if (definitions != presetDefinitions || effects.empty())
    {
        presetDefinitions = std::move(definitions);
        Reload();
        return true;
    }
    for (size_t i = 0; i < effects.size(); ++i)
        if (effects[i].compiled)
            ApplyPreset(effects[i], i);
    SortTechniques();
    return true;
}

bool Runtime::SavePreset()
{
    if (presetPath.empty())
        return false;
    PresetIni preset(ReadFile(presetPath));
    std::vector<std::string> enabled, order;
    std::set<size_t> used;
    for (const Technique& technique : techniques)
    {
        const std::string key = TechniqueKey(technique, effects[technique.effect]);
        order.push_back(key);
        if (technique.enabled && !(technique.hidden && technique.enabledByDefault))
        {
            enabled.push_back(key);
            used.insert(technique.effect);
        }
    }
    // Techniques whose effect did not load stay in the preset, so it still works where they are installed.
    std::string value;
    if (preset.Get("", "Techniques", value))
        for (const std::string& key : PresetIni::Split(value))
            if (std::find(order.begin(), order.end(), key) == order.end())
                enabled.push_back(key);
    preset.Set("", "Techniques", PresetIni::Join(enabled));
    preset.Set("", "TechniqueSorting", PresetIni::Join(order));
    sorting = order;

    for (size_t index = 0; index < effects.size(); ++index)
    {
        const Effect& effect = effects[index];
        if (!effect.compiled || (!used.count(index) && !preset.HasSection(effect.file)))
            continue;
        for (const Uniform& uniform : effect.uniforms)
        {
            if (!uniform.source.empty() || !uniform.type.is_numeric())
                continue;
            const size_t count = ComponentCount(uniform);
            std::string text;
            if (uniform.type.is_floating_point())
            {
                std::vector<float> values(count);
                GetValue(effect, uniform, values.data(), count);
                for (float v : values)
                    text += (text.empty() ? "" : ",") + FormatFloat(v);
            }
            else
            {
                std::vector<int> values(count);
                GetValue(effect, uniform, values.data(), count);
                for (int v : values)
                    text += (text.empty() ? "" : ",") + std::to_string(v);
            }
            preset.Set(effect.file, uniform.name, text);
        }
    }
    if (!WriteFile(presetPath, preset.Text()))
    {
        Log(LogLevel::Warning, "Could not save the preset %s.", presetPath.c_str());
        return false;
    }
    dirty = false;
    return true;
}

bool Runtime::SavePresetAs(const fs::path& path)
{
    const fs::path previous = presetPath;
    presetPath = path;
    if (SavePreset())
        return true;
    presetPath = previous;
    return false;
}

std::vector<std::string> Runtime::MissingTechniques() const
{
    const PresetIni preset(ReadFile(presetPath));
    std::string value;
    std::vector<std::string> missing;
    if (!preset.Get("", "Techniques", value))
        return missing;
    for (const std::string& key : PresetIni::Split(value))
    {
        const size_t at = key.find('@');
        const std::string file = at == std::string::npos ? std::string() : Lowercase(key.substr(at + 1));
        const bool found = std::any_of(effects.begin(), effects.end(), [&](const Effect& effect) {
            return effect.compiled && (file.empty() || Lowercase(effect.file) == file);
        });
        if (!found)
            missing.push_back(key);
    }
    return missing;
}

void Runtime::SetEnabled(size_t index, bool enabled)
{
    if (index < techniques.size() && techniques[index].enabled != enabled)
    {
        techniques[index].enabled = enabled;
        dirty = true;
    }
}

void Runtime::MoveTechnique(size_t from, size_t to)
{
    if (from >= techniques.size() || to >= techniques.size() || from == to)
        return;
    Technique moved = std::move(techniques[from]);
    techniques.erase(techniques.begin() + from);
    techniques.insert(techniques.begin() + to, std::move(moved));
    sorting.clear();
    for (const Technique& technique : techniques)
        sorting.push_back(TechniqueKey(technique, effects[technique.effect]));
    dirty = true;
}

// Uniform values

void Runtime::GetValue(const Effect& effect, const Uniform& uniform, float* values, size_t count) const
{
    count = std::min(count, ComponentCount(uniform));
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = ComponentOffset(uniform, i);
        uint32_t raw = 0;
        if (offset + 4 <= effect.uniformData.size())
            std::memcpy(&raw, effect.uniformData.data() + offset, 4);
        if (uniform.type.is_floating_point())
            std::memcpy(&values[i], &raw, 4);
        else if (uniform.type.is_signed())
            values[i] = static_cast<float>(static_cast<int32_t>(raw));
        else
            values[i] = static_cast<float>(raw);
    }
}

void Runtime::GetValue(const Effect& effect, const Uniform& uniform, int* values, size_t count) const
{
    count = std::min(count, ComponentCount(uniform));
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = ComponentOffset(uniform, i);
        uint32_t raw = 0;
        if (offset + 4 <= effect.uniformData.size())
            std::memcpy(&raw, effect.uniformData.data() + offset, 4);
        if (uniform.type.is_floating_point())
        {
            float value;
            std::memcpy(&value, &raw, 4);
            values[i] = static_cast<int>(value);
        }
        else
            values[i] = static_cast<int32_t>(raw);
    }
}

void Runtime::SetValue(Effect& effect, const Uniform& uniform, const float* values, size_t count)
{
    count = std::min(count, ComponentCount(uniform));
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = ComponentOffset(uniform, i);
        if (offset + 4 > effect.uniformData.size())
            break;
        uint32_t raw;
        if (uniform.type.is_floating_point())
            std::memcpy(&raw, &values[i], 4);
        else if (uniform.type.is_boolean())
            raw = values[i] != 0.0f;
        else if (uniform.type.is_signed())
            raw = static_cast<uint32_t>(static_cast<int32_t>(values[i]));
        else
            raw = static_cast<uint32_t>(std::max(values[i], 0.0f));
        std::memcpy(effect.uniformData.data() + offset, &raw, 4);
    }
}

void Runtime::SetValue(Effect& effect, const Uniform& uniform, const int* values, size_t count)
{
    count = std::min(count, ComponentCount(uniform));
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = ComponentOffset(uniform, i);
        if (offset + 4 > effect.uniformData.size())
            break;
        uint32_t raw;
        if (uniform.type.is_floating_point())
        {
            const float value = static_cast<float>(values[i]);
            std::memcpy(&raw, &value, 4);
        }
        else if (uniform.type.is_boolean())
            raw = values[i] != 0;
        else
            raw = static_cast<uint32_t>(values[i]);
        std::memcpy(effect.uniformData.data() + offset, &raw, 4);
    }
}

void Runtime::ResetValue(Effect& effect, const Uniform& uniform)
{
    const reshadefx::uniform& declared = effect.module.uniforms[uniform.declaration];
    const size_t components = uniform.type.components();
    const size_t length = uniform.type.is_array() ? uniform.type.array_length : 1;
    std::vector<uint32_t> raw(components * length, 0);
    if (uniform.source.empty() && declared.has_initializer_value)
        for (size_t element = 0; element < length; ++element)
        {
            const reshadefx::constant& value = uniform.type.is_array()
                ? (element < declared.initializer_value.array_data.size() ? declared.initializer_value.array_data[element] : reshadefx::constant{})
                : declared.initializer_value;
            for (size_t i = 0; i < components && i < 16; ++i)
                raw[element * components + i] = value.as_uint[i];
        }
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const size_t offset = ComponentOffset(uniform, i);
        if (offset + 4 <= effect.uniformData.size())
            std::memcpy(effect.uniformData.data() + offset, &raw[i], 4);
    }
}

// Values the host sets every frame, named by the source annotation.
void Runtime::UpdateSpecialUniforms(Effect& effect)
{
    static std::mt19937 random(std::random_device{}());
    for (const Uniform& uniform : effect.uniforms)
    {
        if (uniform.source.empty())
            continue;
        const auto& annotations = effect.module.uniforms[uniform.declaration].annotations;
        const std::string& source = uniform.source;
        if (source == "frametime")
        {
            SetValue(effect, uniform, &frameTime, 1);
        }
        else if (source == "framecount")
        {
            if (uniform.type.is_floating_point())
            {
                const float value = static_cast<float>(frameCount);
                SetValue(effect, uniform, &value, 1);
            }
            else
            {
                const int value = static_cast<int>(frameCount);
                SetValue(effect, uniform, &value, 1);
            }
        }
        else if (source == "timer")
        {
            const float value = std::chrono::duration<float, std::milli>(lastFrame - startTime).count();
            SetValue(effect, uniform, &value, 1);
        }
        else if (source == "date")
        {
            const time_t now = time(nullptr);
            tm local{};
            localtime_r(&now, &local);
            const int value[4] = { local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec };
            SetValue(effect, uniform, value, 4);
        }
        else if (source == "random")
        {
            const int low = AnnotationInt(annotations, "min", 0), high = AnnotationInt(annotations, "max", 32767);
            const int value = low + static_cast<int>(random() % static_cast<unsigned>(std::max(high - low + 1, 1)));
            SetValue(effect, uniform, &value, 1);
        }
        else if (source == "pingpong")
        {
            const float low = AnnotationFloat(annotations, "min", 0.0f), high = AnnotationFloat(annotations, "max", 1.0f);
            const float stepLow = AnnotationFloat(annotations, "step", 1.0f, 0), stepHigh = AnnotationFloat(annotations, "step", 0.0f, 1);
            const float smoothing = AnnotationFloat(annotations, "smoothing", 0.0f);
            float value[2];
            GetValue(effect, uniform, value, 2);
            float increment = stepHigh == 0 ? stepLow : stepLow + std::fmod(static_cast<float>(random()), stepHigh - stepLow + 1);
            const float seconds = frameTime / 1000.0f;
            if (value[1] >= 0)
            {
                increment = std::max(increment - std::max(0.0f, smoothing - (high - value[0])), 0.05f) * seconds;
                if ((value[0] += increment) >= high)
                    value[0] = high, value[1] = -1;
            }
            else
            {
                increment = std::max(increment - std::max(0.0f, smoothing - (value[0] - low)), 0.05f) * seconds;
                if ((value[0] -= increment) <= low)
                    value[0] = low, value[1] = 1;
            }
            SetValue(effect, uniform, value, 2);
        }
        else if (source == "overlay_open")
        {
            const int value = menuOpen;
            SetValue(effect, uniform, &value, 1);
        }
        else if (source == "mousepoint")
        {
            const float value[2] = { mouseX, mouseY };
            SetValue(effect, uniform, value, 2);
        }
        else
        {
            // Keys, the mouse buttons and wheel, and depth, which the host cannot provide, stay zero.
            ResetValue(effect, uniform);
        }
    }
}

// GPU resources

VkSampler Runtime::Sampler(const reshadefx::sampler_desc& desc)
{
    std::vector<uint8_t> key(sizeof(desc));
    std::memcpy(key.data(), &desc, sizeof(desc));
    for (const auto& [existing, sampler] : samplers)
        if (existing == key)
            return sampler;

    const unsigned filter = static_cast<unsigned>(desc.filter);
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.minFilter = (filter & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.magFilter = (filter & 0x4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = (filter & 0x1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (desc.filter == reshadefx::filter_mode::anisotropic && gpu.anisotropy)
    {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(16.0f, gpu.properties.limits.maxSamplerAnisotropy);
    }
    info.addressModeU = AddressMode(desc.address_u);
    info.addressModeV = AddressMode(desc.address_v);
    info.addressModeW = AddressMode(desc.address_w);
    const float bias = gpu.properties.limits.maxSamplerLodBias;
    info.mipLodBias = std::clamp(desc.lod_bias, -bias, bias);
    info.minLod = std::clamp(desc.min_lod, 0.0f, 1000.0f);
    info.maxLod = std::clamp(desc.max_lod, info.minLod, 1000.0f);
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(gpu.device, &info, nullptr, &sampler);
    samplers.emplace_back(std::move(key), sampler);
    return sampler;
}

fs::path Runtime::FindTexture(const std::string& source)
{
    if (!textureFilesScanned)
    {
        textureFiles.clear();
        for (const fs::path& root : settings.texturePaths)
        {
            std::error_code error;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error))
                if (!error && it->is_regular_file(error))
                    textureFiles.emplace_back(Lowercase(it->path().lexically_relative(root).generic_string()), it->path());
        }
        textureFilesScanned = true;
    }
    std::string wanted = Lowercase(source);
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    for (const auto& [relative, path] : textureFiles)
        if (relative == wanted)
            return path;
    // Effects in subfolders often name textures without the folder they are in.
    const std::string name = fs::path(wanted).filename().string();
    for (const auto& [relative, path] : textureFiles)
        if (fs::path(relative).filename() == name)
            return path;
    return {};
}

GpuImage* Runtime::Texture(const reshadefx::texture& texture, Effect& effect)
{
    if (texture.semantic == "COLOR")
        return &color;
    if (texture.semantic == "DEPTH")
        return &depth;
    if (!texture.semantic.empty())
        return &blank;

    std::string key = texture.unique_name;
    if (const auto found = sharedTextures.find(key); found != sharedTextures.end())
    {
        const reshadefx::texture_desc& desc = found->second.desc;
        if (desc.width == texture.width && desc.height == texture.height && desc.depth == texture.depth && desc.levels == texture.levels &&
            desc.format == texture.format && desc.type == texture.type)
            return &found->second.image;
        // Another effect has a texture of the same name that differs, so this one gets its own.
        key += "@" + effect.file;
        if (const auto own = sharedTextures.find(key); own != sharedTextures.end())
            return &own->second.image;
    }

    const VkFormat format = TextureFormat(texture.format);
    const VkImageType type = texture.type == reshadefx::texture_type::texture_1d ? VK_IMAGE_TYPE_1D
                           : texture.type == reshadefx::texture_type::texture_3d ? VK_IMAGE_TYPE_3D
                                                                                 : VK_IMAGE_TYPE_2D;
    // Render targets are 2D in ReShade too.
    if (format == VK_FORMAT_UNDEFINED || (texture.render_target && type != VK_IMAGE_TYPE_2D))
    {
        Log(LogLevel::Warning, "%s: texture %s has a format or shape the host does not support.", effect.file.c_str(), texture.name.c_str());
        return nullptr;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatFeatureFlags features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (texture.render_target)
    {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if (texture.storage_access)
    {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
    if (!gpu.Supports(format, features))
    {
        Log(LogLevel::Warning, "%s: the graphics card cannot use texture %s's format this way.", effect.file.c_str(), texture.name.c_str());
        return nullptr;
    }

    SharedTexture& shared = sharedTextures[key];
    shared.desc = texture;
    const uint32_t levels = std::max<uint32_t>(1, texture.levels);
    if (!gpu.CreateImage(shared.image, texture.width, texture.height, levels, format, usage, type, texture.depth))
    {
        sharedTextures.erase(key);
        Log(LogLevel::Warning, "%s: could not create texture %s.", effect.file.c_str(), texture.name.c_str());
        return nullptr;
    }

    VkCommandBuffer commands = gpu.BeginCommands();
    InitLayout(commands, shared.image);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    vkCmdClearColorImage(commands, shared.image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);

    GpuBuffer upload;
    for (const reshadefx::annotation& annotation : texture.annotations)
    {
        if (annotation.name != "source" || annotation.type.base != reshadefx::type::t_string || type != VK_IMAGE_TYPE_2D)
            continue;
        const fs::path file = FindTexture(annotation.value.string_data);
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = file.empty() ? nullptr : stbi_load(file.c_str(), &w, &h, &channels, 4);
        if (!pixels)
        {
            Log(LogLevel::Warning, "%s: could not load texture %s.", effect.file.c_str(), annotation.value.string_data.c_str());
            break;
        }
        // Resized to the size the effect declares, like ReShade does.
        const uint32_t tw = texture.width, th = texture.height;
        const uint32_t pixelSize = texture.format == reshadefx::texture_format::r8 ? 1 : texture.format == reshadefx::texture_format::rg8 ? 2 : 4;
        if (texture.format != reshadefx::texture_format::rgba8 && pixelSize == 4)
        {
            Log(LogLevel::Warning, "%s: texture %s must be RGBA8 to load an image.", effect.file.c_str(), texture.name.c_str());
            stbi_image_free(pixels);
            break;
        }
        std::vector<uint8_t> data(size_t(tw) * th * pixelSize);
        for (uint32_t y = 0; y < th; ++y)
            for (uint32_t x = 0; x < tw; ++x)
            {
                const uint32_t sx = std::min<uint32_t>(x * w / tw, w - 1), sy = std::min<uint32_t>(y * h / th, h - 1);
                std::memcpy(&data[(size_t(y) * tw + x) * pixelSize], &pixels[(size_t(sy) * w + sx) * 4], pixelSize);
            }
        stbi_image_free(pixels);
        if (!gpu.CreateBuffer(upload, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
            break;
        std::memcpy(upload.mapped, data.data(), data.size());
        FullBarrier(commands);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { tw, th, 1 };
        vkCmdCopyBufferToImage(commands, upload.buffer, shared.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        FullBarrier(commands);
        GenerateMipmaps(commands, shared.image);
        break;
    }
    gpu.SubmitAndWait(commands);
    gpu.DestroyBuffer(upload);
    return &shared.image;
}

void Runtime::GenerateMipmaps(VkCommandBuffer commands, const GpuImage& image)
{
    if (image.levels <= 1 || !gpu.Supports(image.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return;
    for (uint32_t level = 1; level < image.levels; ++level)
    {
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
        blit.srcOffsets[1] = { std::max(1, int(image.width >> (level - 1))), std::max(1, int(image.height >> (level - 1))),
                               std::max(1, int(image.depth >> (level - 1))) };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        blit.dstOffsets[1] = { std::max(1, int(image.width >> level)), std::max(1, int(image.height >> level)), std::max(1, int(image.depth >> level)) };
        vkCmdBlitImage(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        FullBarrier(commands);
    }
}

bool Runtime::CreateGpu(Effect& effect)
{
    auto result = std::make_shared<EffectGpu>();
    EffectGpu& g = *result;
    const reshadefx::effect_module& module = effect.module;
    const auto fail = [&](const char* what, const std::string& where) {
        Report(LogLevel::Warning, "%s could not start: %s (%s).", effect.file.c_str(), what, where.c_str());
        effect.gpu = result;
        DestroyGpu(effect);
        effect.gpuFailed = true;
        return false;
    };

    if (!gpu.CreateBuffer(g.uniforms, std::max<VkDeviceSize>(16, effect.uniformData.size()), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
        return fail("no memory for its variables", effect.file);

    for (const auto& [name, code] : effect.spirv)
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = code.size() * 4;
        info.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(gpu.device, &info, nullptr, &shader) != VK_SUCCESS)
            return fail("the graphics card rejected a shader", name);
        g.modules[name] = shader;
    }

    uint32_t passCount = 0, samplerCount = 0, storageCount = 0;
    for (const reshadefx::technique& technique : module.techniques)
        for (const reshadefx::pass& pass : technique.passes)
        {
            ++passCount;
            samplerCount += static_cast<uint32_t>(pass.texture_bindings.size());
            storageCount += static_cast<uint32_t>(pass.storage_bindings.size());
        }
    std::vector<VkDescriptorPoolSize> sizes = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, std::max(1u, passCount) } };
    if (samplerCount)
        sizes.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount });
    if (storageCount)
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageCount });
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = std::max(1u, passCount * 3);
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &g.pool);

    const auto findTexture = [&module](const std::string& uniqueName) -> const reshadefx::texture* {
        for (const reshadefx::texture& texture : module.textures)
            if (texture.unique_name == uniqueName)
                return &texture;
        return nullptr;
    };

    g.techniques.resize(module.techniques.size());
    for (size_t t = 0; t < module.techniques.size(); ++t)
        for (const reshadefx::pass& pass : module.techniques[t].passes)
        {
            PassGpu& p = g.techniques[t].emplace_back();
            p.compute = !pass.cs_entry_point.empty();
            p.clear = pass.clear_render_targets;
            p.mipmaps = pass.generate_mipmaps;
            p.vertices = pass.num_vertices;
            const std::string where = module.techniques[t].name + (pass.name.empty() ? "" : "/" + pass.name);

            // Render targets. A pass without any writes to the picture itself.
            std::vector<const GpuImage*> targets;
            std::vector<VkImageView> targetViews;
            std::vector<VkFormat> targetFormats;
            if (!p.compute)
            {
                for (int i = 0; i < 8; ++i)
                {
                    const std::string& name = pass.render_target_names[i];
                    const GpuImage* image = nullptr;
                    if (name.empty())
                    {
                        if (i != 0)
                            break;
                        image = &backbuffer;
                        p.toBackbuffer = true;
                    }
                    else
                    {
                        const reshadefx::texture* texture = findTexture(name);
                        if (texture && texture->semantic == "COLOR")
                        {
                            image = &backbuffer;
                            p.toBackbuffer = true;
                        }
                        else if (texture)
                            image = Texture(*texture, effect);
                        if (!image || image == &blank || image == &depth)
                            return fail("a render target is missing", where);
                    }
                    targets.push_back(image);
                    const bool srgb = pass.srgb_write_enable && SrgbFormat(image->format) != image->format;
                    targetViews.push_back(srgb ? image->srgbTarget : image->target);
                    targetFormats.push_back(srgb ? SrgbFormat(image->format) : image->format);
                    if (image != &backbuffer)
                        p.written.push_back(image);
                }
                p.targetCount = static_cast<uint32_t>(targets.size());
                p.extent = { targets[0]->width, targets[0]->height };
            }

            // Set 0 holds the variables, set 1 the textures with their samplers and set 2 storage, as the SPIR-V
            // ReShade's compiler writes expects.
            std::vector<VkDescriptorSetLayoutBinding> bindings[3];
            bindings[0].push_back({ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr });
            for (const reshadefx::texture_binding& binding : pass.texture_bindings)
                bindings[1].push_back({ binding.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr });
            for (const reshadefx::storage_binding& binding : pass.storage_bindings)
                bindings[2].push_back({ binding.entry_point_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr });
            for (int set = 0; set < 3; ++set)
            {
                VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                layoutInfo.bindingCount = static_cast<uint32_t>(bindings[set].size());
                layoutInfo.pBindings = bindings[set].data();
                vkCreateDescriptorSetLayout(gpu.device, &layoutInfo, nullptr, &p.setLayouts[set]);
            }
            VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 3;
            layoutInfo.pSetLayouts = p.setLayouts;
            vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &p.layout);

            VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocation.descriptorPool = g.pool;
            allocation.descriptorSetCount = 3;
            allocation.pSetLayouts = p.setLayouts;
            if (vkAllocateDescriptorSets(gpu.device, &allocation, p.sets) != VK_SUCCESS)
                return fail("out of descriptors", where);

            std::vector<VkDescriptorImageInfo> images;
            images.reserve(pass.texture_bindings.size() + pass.storage_bindings.size());
            std::vector<VkWriteDescriptorSet> writes;
            const VkDescriptorBufferInfo bufferInfo{ g.uniforms.buffer, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet uniformWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            uniformWrite.dstSet = p.sets[0];
            uniformWrite.descriptorCount = 1;
            uniformWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uniformWrite.pBufferInfo = &bufferInfo;
            writes.push_back(uniformWrite);

            for (size_t i = 0; i < pass.texture_bindings.size(); ++i)
            {
                const reshadefx::texture_binding& binding = pass.texture_bindings[i];
                const reshadefx::sampler& sampler = module.samplers[binding.index];
                const reshadefx::texture* texture = findTexture(sampler.texture_name);
                const GpuImage* image = texture ? Texture(*texture, effect) : nullptr;
                if (!image)
                    return fail("a texture is missing", where);
                // A texture cannot be read while this pass writes it. Direct3D reads zero then, and so does this.
                if (std::find(targets.begin(), targets.end(), image) != targets.end())
                    image = &blank;
                VkDescriptorImageInfo info{};
                info.sampler = Sampler(sampler);
                info.imageView = binding.srgb ? image->srgbView : image->view;
                info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                images.push_back(info);
                VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet = p.sets[1];
                write.dstBinding = binding.entry_point_binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &images.back();
                writes.push_back(write);
            }
            for (const reshadefx::storage_binding& binding : pass.storage_bindings)
            {
                const reshadefx::storage& storage = module.storages[binding.index];
                const reshadefx::texture* texture = findTexture(storage.texture_name);
                const GpuImage* image = texture ? Texture(*texture, effect) : nullptr;
                if (!image || storage.level >= image->storage.size())
                    return fail("a storage texture is missing", where);
                p.written.push_back(image);
                VkDescriptorImageInfo info{};
                info.imageView = image->storage[storage.level];
                info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                images.push_back(info);
                VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet = p.sets[2];
                write.dstBinding = binding.entry_point_binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write.pImageInfo = &images.back();
                writes.push_back(write);
            }
            vkUpdateDescriptorSets(gpu.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            if (p.compute)
            {
                const auto shader = g.modules.find(pass.cs_entry_point);
                if (shader == g.modules.end())
                    return fail("its compute shader is missing", where);
                VkComputePipelineCreateInfo info{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                info.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                info.stage.module = shader->second;
                info.stage.pName = pass.cs_entry_point.c_str();
                info.layout = p.layout;
                if (vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline) != VK_SUCCESS)
                    return fail("the graphics card rejected a compute shader", where);
                p.dispatch[0] = std::max(1u, pass.viewport_width);
                p.dispatch[1] = std::max(1u, pass.viewport_height);
                p.dispatch[2] = std::max(1u, pass.viewport_dispatch_z);
                continue;
            }

            if (pass.stencil_enable)
                Log(LogLevel::Info, "%s (%s) uses stencil, which the host ignores.", effect.file.c_str(), where.c_str());

            std::vector<VkAttachmentDescription> attachments;
            std::vector<VkAttachmentReference> references;
            for (uint32_t i = 0; i < p.targetCount; ++i)
            {
                VkAttachmentDescription attachment{};
                attachment.format = targetFormats[i];
                attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = p.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
                attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
                attachments.push_back(attachment);
                references.push_back({ i, VK_IMAGE_LAYOUT_GENERAL });
            }
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = p.targetCount;
            subpass.pColorAttachments = references.data();
            VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            passInfo.attachmentCount = p.targetCount;
            passInfo.pAttachments = attachments.data();
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            vkCreateRenderPass(gpu.device, &passInfo, nullptr, &p.renderPass);

            VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebufferInfo.renderPass = p.renderPass;
            framebufferInfo.attachmentCount = p.targetCount;
            framebufferInfo.pAttachments = targetViews.data();
            framebufferInfo.width = p.extent.width;
            framebufferInfo.height = p.extent.height;
            framebufferInfo.layers = 1;
            for (const GpuImage* target : targets)
            {
                framebufferInfo.width = std::min(framebufferInfo.width, target->width);
                framebufferInfo.height = std::min(framebufferInfo.height, target->height);
            }
            vkCreateFramebuffer(gpu.device, &framebufferInfo, nullptr, &p.framebuffer);

            const auto vs = g.modules.find(pass.vs_entry_point);
            const auto ps = pass.ps_entry_point.empty() ? g.modules.end() : g.modules.find(pass.ps_entry_point);
            if (vs == g.modules.end())
                return fail("its vertex shader is missing", where);
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vs->second;
            stages[0].pName = pass.vs_entry_point.c_str();
            uint32_t stageCount = 1;
            if (ps != g.modules.end())
            {
                stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = ps->second;
                stages[1].pName = pass.ps_entry_point.c_str();
                stageCount = 2;
            }

            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            assembly.topology = Topology(pass.topology);
            const VkViewport viewport{ 0, 0, float(pass.viewport_width ? pass.viewport_width : framebufferInfo.width),
                                       float(pass.viewport_height ? pass.viewport_height : framebufferInfo.height), 0, 1 };
            const VkRect2D scissor{ { 0, 0 }, { framebufferInfo.width, framebufferInfo.height } };
            VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewportState.viewportCount = 1;
            viewportState.pViewports = &viewport;
            viewportState.scissorCount = 1;
            viewportState.pScissors = &scissor;
            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blends[8]{};
            for (uint32_t i = 0; i < p.targetCount; ++i)
            {
                const uint32_t s = gpu.independentBlend ? i : 0;
                blends[i].blendEnable = pass.blend_enable[s];
                blends[i].srcColorBlendFactor = BlendFactor(pass.source_color_blend_factor[s]);
                blends[i].dstColorBlendFactor = BlendFactor(pass.dest_color_blend_factor[s]);
                blends[i].colorBlendOp = BlendOp(pass.color_blend_op[s]);
                blends[i].srcAlphaBlendFactor = BlendFactor(pass.source_alpha_blend_factor[s]);
                blends[i].dstAlphaBlendFactor = BlendFactor(pass.dest_alpha_blend_factor[s]);
                blends[i].alphaBlendOp = BlendOp(pass.alpha_blend_op[s]);
                blends[i].colorWriteMask = pass.render_target_write_mask[s] & 0xF;
            }
            VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            blend.attachmentCount = p.targetCount;
            blend.pAttachments = blends;

            VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            info.stageCount = stageCount;
            info.pStages = stages;
            info.pVertexInputState = &vertexInput;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewportState;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pColorBlendState = &blend;
            info.layout = p.layout;
            info.renderPass = p.renderPass;
            if (vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline) != VK_SUCCESS)
                return fail("the graphics card rejected a shader", where);
        }

    effect.gpu = std::move(result);
    Log(LogLevel::Info, "%s is ready.", effect.file.c_str());
    return true;
}

void Runtime::DestroyGpu(Effect& effect)
{
    if (!effect.gpu)
        return;
    EffectGpu& g = *effect.gpu;
    for (auto& passes : g.techniques)
        for (PassGpu& p : passes)
        {
            vkDestroyPipeline(gpu.device, p.pipeline, nullptr);
            vkDestroyFramebuffer(gpu.device, p.framebuffer, nullptr);
            vkDestroyRenderPass(gpu.device, p.renderPass, nullptr);
            vkDestroyPipelineLayout(gpu.device, p.layout, nullptr);
            for (VkDescriptorSetLayout layout : p.setLayouts)
                vkDestroyDescriptorSetLayout(gpu.device, layout, nullptr);
        }
    for (auto& [name, shader] : g.modules)
        vkDestroyShaderModule(gpu.device, shader, nullptr);
    if (g.pool)
        vkDestroyDescriptorPool(gpu.device, g.pool, nullptr);
    gpu.DestroyBuffer(g.uniforms);
    effect.gpu.reset();
}

void Runtime::DestroyAllGpu()
{
    if (!gpu.device)
        return;
    vkDeviceWaitIdle(gpu.device);
    for (Effect& effect : effects)
    {
        DestroyGpu(effect);
        effect.gpuFailed = false;
    }
    for (auto& [name, texture] : sharedTextures)
        gpu.DestroyImage(texture.image);
    sharedTextures.clear();
}

// Rendering

void Runtime::Render(VkCommandBuffer commands, const uint32_t* pixels, bool enabled)
{
    if (!width || !height)
        return;
    std::memcpy(staging.mapped, pixels, size_t(width) * height * 4);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    FullBarrier(commands);
    vkCmdCopyBufferToImage(commands, staging.buffer, backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    FullBarrier(commands);

    const auto now = std::chrono::steady_clock::now();
    frameTime = std::chrono::duration<float, std::milli>(now - lastFrame).count();
    lastFrame = now;
    ++frameCount;
    if (!enabled)
        return;

    bool colorStale = true;
    for (const Technique& technique : techniques)
    {
        if (!technique.enabled)
            continue;
        Effect& effect = effects[technique.effect];
        if (!effect.compiled || effect.gpuFailed)
            continue;
        if (!effect.gpu && !CreateGpu(effect))
            continue;
        EffectGpu& g = *effect.gpu;
        if (g.updatedFrame != frameCount)
        {
            UpdateSpecialUniforms(effect);
            std::memcpy(g.uniforms.mapped, effect.uniformData.data(), std::min<size_t>(effect.uniformData.size(), g.uniforms.size));
            g.updatedFrame = frameCount;
        }

        for (const PassGpu& p : g.techniques[technique.index])
        {
            if (colorStale)
            {
                VkImageCopy region{};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = region.srcSubresource;
                region.extent = { width, height, 1 };
                vkCmdCopyImage(commands, backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, color.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                FullBarrier(commands);
                colorStale = false;
            }

            const VkPipelineBindPoint bindPoint = p.compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
            vkCmdBindPipeline(commands, bindPoint, p.pipeline);
            vkCmdBindDescriptorSets(commands, bindPoint, p.layout, 0, 3, p.sets, 0, nullptr);
            if (p.compute)
                vkCmdDispatch(commands, p.dispatch[0], p.dispatch[1], p.dispatch[2]);
            else
            {
                VkClearValue clears[8]{};
                VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                begin.renderPass = p.renderPass;
                begin.framebuffer = p.framebuffer;
                begin.renderArea.extent = p.extent;
                begin.clearValueCount = p.clear ? p.targetCount : 0;
                begin.pClearValues = clears;
                vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdDraw(commands, p.vertices, 1, 0, 0);
                vkCmdEndRenderPass(commands);
            }
            FullBarrier(commands);
            if (p.toBackbuffer)
                colorStale = true;
            if (p.mipmaps)
                for (const GpuImage* image : p.written)
                    GenerateMipmaps(commands, *image);
        }
    }
}

std::vector<uint8_t> Runtime::ReadOutput()
{
    std::vector<uint8_t> pixels;
    if (!width || !height)
        return pixels;
    const VkDeviceSize size = VkDeviceSize(width) * height * 4;
    if (readback.size != size)
    {
        gpu.DestroyBuffer(readback);
        if (!gpu.CreateBuffer(readback, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
            return pixels;
    }
    vkDeviceWaitIdle(gpu.device);
    VkCommandBuffer commands = gpu.BeginCommands();
    FullBarrier(commands);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    vkCmdCopyImageToBuffer(commands, backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
    gpu.SubmitAndWait(commands);
    pixels.resize(size);
    const uint8_t* source = static_cast<const uint8_t*>(readback.mapped);
    for (size_t i = 0; i < size; i += 4)
    {
        pixels[i] = source[i + 2];
        pixels[i + 1] = source[i + 1];
        pixels[i + 2] = source[i];
        pixels[i + 3] = 255;
    }
    return pixels;
}
} // namespace fx
