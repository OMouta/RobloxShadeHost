#include "effects.h"
#include "log.h"
#include "preset_ini.h"

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>

#include <stb_image.h>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <tuple>

// A hash of ReShade's compiler sources, from posix.cmake, so a new or changed compiler starts a new cache.
#ifndef UNISHADE_COMPILER_ID
#define UNISHADE_COMPILER_ID "unknown"
#endif

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

// A new size waits this long for the next before effects are compiled for it.
constexpr auto kResizeDelay = std::chrono::milliseconds(250);
// The compile cache drops the entries used longest ago beyond this.
constexpr uintmax_t kCacheLimit = 256 * 1024 * 1024;

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

void BuildUniforms(Effect& effect)
{
    effect.uniforms.clear();
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

// The compile cache

uint64_t Hash(std::string_view data, uint64_t hash = 0xcbf29ce484222325)
{
    for (const unsigned char c : data)
        hash = (hash ^ c) * 0x100000001b3;
    return hash;
}

// Also hashes where each piece ends, so "ab" + "c" and "a" + "bc" differ.
uint64_t HashPiece(std::string_view data, uint64_t hash)
{
    const uint64_t size = data.size();
    return Hash(data, Hash(std::string_view(reinterpret_cast<const char*>(&size), sizeof(size)), hash));
}

// Bump when what the cache stores changes.
constexpr uint32_t kCacheFormat = 1;
constexpr char kCacheMagic[8] = "UNIFXC1";

bool ReadText(const fs::path& path, std::string& text)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    text.assign(std::istreambuf_iterator<char>(input), {});
    return !input.bad();
}

// Writes through a file of its own, so two processes or threads writing the same entry cannot mix their data.
bool WriteCacheFile(const fs::path& path, const std::string& contents)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path temporary = path.string() + "." + std::to_string(getpid()) + "-" +
                               std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id())) + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output.flush())
        {
            output.close();
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, path, error);
    if (error)
        fs::remove(temporary, error);
    return !error;
}

// Writes an effect for the cache and reads it back with the same Visit functions, so the two cannot drift apart.
struct Writer
{
    std::string data;
    template <class T>
    void Raw(T& value) { data.append(reinterpret_cast<const char*>(&value), sizeof(T)); }
    void Text(std::string& value)
    {
        uint64_t size = value.size();
        Raw(size);
        data += value;
    }
    void Bytes(const void* bytes, size_t size) { data.append(static_cast<const char*>(bytes), size); }
    template <class T>
    bool Count(std::vector<T>& values, size_t = 1)
    {
        uint64_t size = values.size();
        Raw(size);
        return true;
    }
};

struct Reader
{
    std::string_view data;
    size_t at = 0;
    bool ok = true;

    template <class T>
    void Raw(T& value)
    {
        if (!ok || data.size() - at < sizeof(T))
        {
            ok = false;
            value = T();
            return;
        }
        std::memcpy(&value, data.data() + at, sizeof(T));
        at += sizeof(T);
    }
    void Text(std::string& value)
    {
        uint64_t size = 0;
        Raw(size);
        if (!ok || data.size() - at < size)
        {
            ok = false;
            return;
        }
        value.assign(data.substr(at, size));
        at += size;
    }
    void Bytes(void* bytes, size_t size)
    {
        if (!ok || data.size() - at < size)
        {
            ok = false;
            return;
        }
        std::memcpy(bytes, data.data() + at, size);
        at += size;
    }
    // Every element takes at least a byte, which stops a damaged count from asking for too much memory.
    template <class T>
    bool Count(std::vector<T>& values, size_t elementSize = 1)
    {
        uint64_t size = 0;
        Raw(size);
        if (!ok || size > (data.size() - at) / elementSize)
            return ok = false;
        values.resize(size);
        return true;
    }
};

template <class A, class T>
    requires std::is_arithmetic_v<T> || std::is_enum_v<T>
void Visit(A& archive, T& value)
{
    archive.Raw(value);
}

template <class A>
void Visit(A& archive, std::string& value)
{
    archive.Text(value);
}

template <class A, class T, size_t N>
void Visit(A& archive, T (&values)[N])
{
    for (T& value : values)
        Visit(archive, value);
}

template <class A, class T, class U>
void Visit(A& archive, std::pair<T, U>& value)
{
    Visit(archive, value.first);
    Visit(archive, value.second);
}

template <class A, class T>
void Visit(A& archive, std::vector<T>& values)
{
    if (archive.Count(values))
        for (T& value : values)
            Visit(archive, value);
}

// Numbers in one piece, which SPIR-V is mostly made of.
template <class A, class T>
    requires std::is_arithmetic_v<T>
void Visit(A& archive, std::vector<T>& values)
{
    if (archive.Count(values, sizeof(T)))
        archive.Bytes(values.data(), values.size() * sizeof(T));
}

template <class A>
void Visit(A& archive, reshadefx::type& type)
{
    uint32_t base = type.base, rows = type.rows, cols = type.cols, qualifiers = type.qualifiers;
    Visit(archive, base);
    Visit(archive, rows);
    Visit(archive, cols);
    Visit(archive, qualifiers);
    type.base = static_cast<reshadefx::type::datatype>(base);
    type.rows = rows;
    type.cols = cols;
    type.qualifiers = qualifiers;
    Visit(archive, type.array_length);
    Visit(archive, type.struct_definition);
}

template <class A>
void Visit(A& archive, reshadefx::constant& value)
{
    Visit(archive, value.as_uint);
    Visit(archive, value.string_data);
    Visit(archive, value.array_data);
}

template <class A>
void Visit(A& archive, reshadefx::annotation& annotation)
{
    Visit(archive, annotation.type);
    Visit(archive, annotation.name);
    Visit(archive, annotation.value);
}

template <class A>
void Visit(A& archive, reshadefx::texture& texture)
{
    Visit(archive, texture.width);
    Visit(archive, texture.height);
    Visit(archive, texture.depth);
    Visit(archive, texture.levels);
    Visit(archive, texture.type);
    Visit(archive, texture.format);
    Visit(archive, texture.id);
    Visit(archive, texture.name);
    Visit(archive, texture.unique_name);
    Visit(archive, texture.semantic);
    Visit(archive, texture.annotations);
    Visit(archive, texture.render_target);
    Visit(archive, texture.storage_access);
    Visit(archive, texture.semantic_binding);
}

template <class A>
void Visit(A& archive, reshadefx::sampler& sampler)
{
    Visit(archive, sampler.filter);
    Visit(archive, sampler.address_u);
    Visit(archive, sampler.address_v);
    Visit(archive, sampler.address_w);
    Visit(archive, sampler.min_lod);
    Visit(archive, sampler.max_lod);
    Visit(archive, sampler.lod_bias);
    Visit(archive, sampler.type);
    Visit(archive, sampler.id);
    Visit(archive, sampler.name);
    Visit(archive, sampler.unique_name);
    Visit(archive, sampler.texture_name);
    Visit(archive, sampler.annotations);
    Visit(archive, sampler.srgb);
}

template <class A>
void Visit(A& archive, reshadefx::storage& storage)
{
    Visit(archive, storage.level);
    Visit(archive, storage.type);
    Visit(archive, storage.id);
    Visit(archive, storage.name);
    Visit(archive, storage.unique_name);
    Visit(archive, storage.texture_name);
}

template <class A>
void Visit(A& archive, reshadefx::uniform& uniform)
{
    Visit(archive, uniform.type);
    Visit(archive, uniform.name);
    Visit(archive, uniform.unique_name);
    Visit(archive, uniform.size);
    Visit(archive, uniform.offset);
    Visit(archive, uniform.annotations);
    Visit(archive, uniform.has_initializer_value);
    Visit(archive, uniform.initializer_value);
}

template <class A>
void Visit(A& archive, reshadefx::texture_binding& binding)
{
    Visit(archive, binding.index);
    Visit(archive, binding.entry_point_binding);
    Visit(archive, binding.srgb);
}

template <class A>
void Visit(A& archive, reshadefx::sampler_binding& binding)
{
    Visit(archive, binding.index);
    Visit(archive, binding.entry_point_binding);
}

template <class A>
void Visit(A& archive, reshadefx::storage_binding& binding)
{
    Visit(archive, binding.index);
    Visit(archive, binding.entry_point_binding);
}

template <class A>
void Visit(A& archive, reshadefx::pass& pass)
{
    Visit(archive, pass.name);
    Visit(archive, pass.render_target_names);
    Visit(archive, pass.vs_entry_point);
    Visit(archive, pass.ps_entry_point);
    Visit(archive, pass.cs_entry_point);
    Visit(archive, pass.generate_mipmaps);
    Visit(archive, pass.clear_render_targets);
    Visit(archive, pass.blend_enable);
    Visit(archive, pass.source_color_blend_factor);
    Visit(archive, pass.dest_color_blend_factor);
    Visit(archive, pass.color_blend_op);
    Visit(archive, pass.source_alpha_blend_factor);
    Visit(archive, pass.dest_alpha_blend_factor);
    Visit(archive, pass.alpha_blend_op);
    Visit(archive, pass.srgb_write_enable);
    Visit(archive, pass.render_target_write_mask);
    Visit(archive, pass.stencil_enable);
    Visit(archive, pass.stencil_read_mask);
    Visit(archive, pass.stencil_write_mask);
    Visit(archive, pass.stencil_reference_value);
    Visit(archive, pass.stencil_comparison_func);
    Visit(archive, pass.stencil_pass_op);
    Visit(archive, pass.stencil_fail_op);
    Visit(archive, pass.stencil_depth_fail_op);
    Visit(archive, pass.topology);
    Visit(archive, pass.num_vertices);
    Visit(archive, pass.viewport_width);
    Visit(archive, pass.viewport_height);
    Visit(archive, pass.viewport_dispatch_z);
    Visit(archive, pass.texture_bindings);
    Visit(archive, pass.sampler_bindings);
    Visit(archive, pass.storage_bindings);
}

template <class A>
void Visit(A& archive, reshadefx::technique& technique)
{
    Visit(archive, technique.name);
    Visit(archive, technique.passes);
    Visit(archive, technique.annotations);
}

template <class A>
void Visit(A& archive, reshadefx::effect_module& module)
{
    Visit(archive, module.textures);
    Visit(archive, module.samplers);
    Visit(archive, module.storages);
    Visit(archive, module.uniforms);
    Visit(archive, module.spec_constants);
    Visit(archive, module.total_uniform_size);
    Visit(archive, module.techniques);
    Visit(archive, module.entry_points);
}

// The compiled part of an effect: its module, SPIR-V and warnings.
template <class A>
void VisitCompiled(A& archive, Effect& effect)
{
    Visit(archive, effect.module);
    std::vector<std::pair<std::string, std::vector<uint32_t>>> spirv(effect.spirv.begin(), effect.spirv.end());
    Visit(archive, spirv);
    effect.spirv = { spirv.begin(), spirv.end() };
    Visit(archive, effect.errors);
}

// One entry per effect, set of macros and include folders. It lists the files the effect was compiled from,
// which must still have the text they had then.
fs::path CacheEntry(const CompileOptions& options, const fs::path& path, const Definitions& macros)
{
    uint64_t hash = HashPiece(UNISHADE_COMPILER_ID, Hash({}));
    hash = HashPiece(path.string(), hash);
    for (const auto& [name, value] : macros)
        hash = HashPiece(value, HashPiece(name, hash));
    for (const fs::path& include : options.includePaths)
        hash = HashPiece(include.string(), hash);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(hash));
    return options.cacheDirectory / (path.stem().string() + "-" + name);
}

bool ReadCache(Effect& effect, const fs::path& entry, const Definitions& macros)
{
    std::string data;
    if (!ReadText(entry, data))
        return false;
    Reader in{ data };
    char magic[sizeof(kCacheMagic)]{};
    uint32_t format = 0;
    uint64_t key = 0, checksum = 0;
    std::vector<std::string> paths;
    Visit(in, magic);
    Visit(in, format);
    Visit(in, key);
    Visit(in, paths);
    Visit(in, checksum);
    if (!in.ok || std::memcmp(magic, kCacheMagic, sizeof(magic)) != 0 || format != kCacheFormat || paths.empty() ||
        Hash(std::string_view(data).substr(in.at)) != checksum)
        return false;
    std::vector<std::string> files(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
        if (!ReadText(paths[i], files[i]))
            return false;
    if (CompileKey(macros, files) != key)
        return false;
    VisitCompiled(in, effect);
    if (!in.ok || in.at != data.size())
        return false;
    // The entry was used now, which keeps it when the cache is pruned.
    std::error_code error;
    fs::last_write_time(entry, fs::file_time_type::clock::now(), error);
    return true;
}

void WriteCache(Effect& effect, const fs::path& entry, const Definitions& macros, std::vector<std::string> paths)
{
    std::vector<std::string> files(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
        if (!ReadText(paths[i], files[i]))
            return;
    Writer payload;
    VisitCompiled(payload, effect);
    Writer out;
    char magic[sizeof(kCacheMagic)];
    std::memcpy(magic, kCacheMagic, sizeof(magic));
    uint32_t format = kCacheFormat;
    uint64_t key = CompileKey(macros, files), checksum = Hash(payload.data);
    Visit(out, magic);
    Visit(out, format);
    Visit(out, key);
    Visit(out, paths);
    Visit(out, checksum);
    WriteCacheFile(entry, out.data + payload.data);
}

// Keeps the compile cache under its limit, dropping the entries used longest ago.
void PruneCache(const fs::path& directory)
{
    std::vector<std::tuple<fs::file_time_type, uintmax_t, fs::path>> entries;
    uintmax_t total = 0;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error))
    {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".bin")
            continue;
        const uintmax_t size = entry.file_size(error);
        entries.emplace_back(entry.last_write_time(error), size, entry.path());
        total += size;
    }
    if (total <= kCacheLimit)
        return;
    std::sort(entries.begin(), entries.end());
    for (const auto& [time, size, path] : entries)
    {
        if (total <= kCacheLimit / 4 * 3)
            break;
        if (fs::remove(path, error))
            total -= size;
    }
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

fs::path CacheDirectory()
{
    return DataDirectory() / "cache";
}
} // namespace

std::string TechniqueKey(const Technique& technique, const Effect& effect)
{
    return technique.name + "@" + effect.file;
}

Definitions EffectMacros(const Definitions& definitions, const CompileOptions& options)
{
    // ReShade 6.8, running on Vulkan. The first definition of a name wins, so these cannot be replaced.
    Definitions macros = {
        { "__RESHADE__", "60800" },
        { "__RESHADE_PERMUTATION__", "0" },
        { "__RESHADE_PERFORMANCE_MODE__", "0" },
        { "__VENDOR__", std::to_string(options.vendor) },
        { "__DEVICE__", std::to_string(options.device) },
        { "__RENDERER__", "131072" }, // 0x20000, Vulkan
        { "__APPLICATION__", "0" },
        { "BUFFER_WIDTH", std::to_string(options.width) },
        { "BUFFER_HEIGHT", std::to_string(options.height) },
        { "BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)" },
        { "BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)" },
        { "BUFFER_COLOR_SPACE", "1" },   // sRGB
        { "BUFFER_COLOR_FORMAT", "87" }, // B8G8R8A8_UNORM, in ReShade's numbering
        { "BUFFER_COLOR_BIT_DEPTH", "8" },
    };
    for (const auto& [name, value] : definitions)
        if (!name.empty())
            macros.emplace_back(name, value.empty() ? "1" : value);
    return macros;
}

uint64_t CompileKey(const Definitions& macros, const std::vector<std::string>& files)
{
    uint64_t hash = HashPiece(UNISHADE_COMPILER_ID, Hash({}));
    const uint32_t format = kCacheFormat;
    hash = Hash(std::string_view(reinterpret_cast<const char*>(&format), sizeof(format)), hash);
    for (const auto& [name, value] : macros)
        hash = HashPiece(value, HashPiece(name, hash));
    for (const std::string& file : files)
        hash = HashPiece(file, hash);
    return hash;
}

bool CompileEffect(Effect& effect, const fs::path& path, const Definitions& definitions, const CompileOptions& options,
                   const std::function<bool()>& cancelled)
{
    effect.path = path;
    effect.file = path.filename().string();
    const Definitions macros = EffectMacros(definitions, options);
    fs::path entry;
    if (!options.cacheDirectory.empty())
    {
        entry = CacheEntry(options, path, macros);
        if (ReadCache(effect, entry, macros))
        {
            BuildUniforms(effect);
            effect.compiled = effect.cached = true;
            return true;
        }
        effect = {};
        effect.path = path;
        effect.file = path.filename().string();
    }

    reshadefx::preprocessor pp;
    for (const auto& [name, value] : macros)
        pp.add_macro_definition(name, value);
    pp.add_include_path(path.parent_path());
    for (const fs::path& include : options.includePaths)
        pp.add_include_path(include);
    pp.append_string(kCompatibilityMacros);

    const bool preprocessed = pp.append_file(path);
    effect.errors = pp.errors();
    if (!preprocessed || (cancelled && cancelled()))
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
        if (cancelled && cancelled())
            return false;
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

    if (!entry.empty())
    {
        std::vector<std::string> paths{ path.string() };
        for (const fs::path& included : pp.included_files())
            paths.push_back(included.string());
        std::sort(paths.begin() + 1, paths.end());
        WriteCache(effect, entry, macros, std::move(paths));
    }
    return true;
}

Runtime::~Runtime()
{
    StopLoader();
    JoinLoaders(true);
}

bool Runtime::Init(const Settings& initial)
{
    settings = initial;
    // Pipelines made before, so effects start faster. Data from another driver or graphics card is left out:
    // drivers should ignore it, but not all do.
    std::string data;
    ReadText(CacheDirectory() / "pipelines.bin", data);
    uint32_t header[4]{};
    if (data.size() >= sizeof(header) + VK_UUID_SIZE)
        std::memcpy(header, data.data(), sizeof(header));
    if (header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header[2] != gpu.properties.vendorID || header[3] != gpu.properties.deviceID ||
        std::memcmp(data.data() + sizeof(header), gpu.properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        data.clear();
    VkPipelineCacheCreateInfo cacheInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    cacheInfo.initialDataSize = data.size();
    cacheInfo.pInitialData = data.data();
    if (vkCreatePipelineCache(gpu.device, &cacheInfo, nullptr, &pipelineCache) != VK_SUCCESS)
        pipelineCache = VK_NULL_HANDLE;

    if (!gpu.CreateImage(depth, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !gpu.CreateImage(blank, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    VkCommandBuffer commands = gpu.BeginCommands();
    if (!commands)
        return false;
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (const GpuImage* image : { &depth, &blank })
    {
        InitLayout(commands, *image);
        vkCmdClearColorImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    return gpu.SubmitAndWait(commands);
}

void Runtime::Shutdown()
{
    StopLoader();
    JoinLoaders(true);
    if (!gpu.device)
        return;
    vkDeviceWaitIdle(gpu.device);
    DestroyAllGpu();
    SavePipelineCache();
    if (pipelineCache)
        vkDestroyPipelineCache(gpu.device, pipelineCache, nullptr);
    pipelineCache = VK_NULL_HANDLE;
    for (auto& [desc, sampler] : samplers)
        vkDestroySampler(gpu.device, sampler, nullptr);
    samplers.clear();
    for (GpuImage* image : { &backbuffer, &color, &depth, &blank })
        gpu.DestroyImage(*image);
    gpu.DestroyBuffer(staging);
    gpu.DestroyBuffer(readback);
}

void Runtime::SavePipelineCache()
{
    size_t size = 0;
    if (!pipelineCache || vkGetPipelineCacheData(gpu.device, pipelineCache, &size, nullptr) != VK_SUCCESS || !size)
        return;
    std::string data(size, '\0');
    if (vkGetPipelineCacheData(gpu.device, pipelineCache, &size, data.data()) == VK_SUCCESS)
        WriteCacheFile(CacheDirectory() / "pipelines.bin", data.substr(0, size));
}

// Does not wait for the loader: it stops after the step it is on, and Update joins it once it has.
void Runtime::StopLoader()
{
    std::lock_guard lock(finishedMutex);
    ++generation;
    loaderRunning = false;
    summaryPending = false;
    finished.clear();
}

void Runtime::JoinLoaders(bool wait)
{
    std::erase_if(loaders, [wait](const std::unique_ptr<Loader>& loader) {
        if (!wait && !loader->done)
            return false;
        loader->thread.join();
        return true;
    });
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
    if (!commands)
        return false;
    InitLayout(commands, backbuffer);
    InitLayout(commands, color);
    return gpu.SubmitAndWait(commands);
}

void Runtime::SetSize(uint32_t newWidth, uint32_t newHeight)
{
    const auto now = std::chrono::steady_clock::now();
    if (newWidth == width && newHeight == height)
    {
        if (resizePending && now - resizeTime >= kResizeDelay)
            ReloadNow();
        return;
    }
    // The first size applies right away. After that the picture goes on without effects until the size has
    // stayed the same for a moment, so dragging a window's corner does not compile every effect for every size
    // on the way.
    const bool first = !width || !height;
    DestroyAllGpu();
    width = newWidth;
    height = newHeight;
    if (!CreateTargets())
    {
        StopLoader();
        width = height = compiledWidth = compiledHeight = 0;
        resizePending = false;
        return;
    }
    if (first)
        ReloadNow();
    else if (width == compiledWidth && height == compiledHeight)
    {
        // Back at the size the effects have.
        resizePending = false;
        loadedCount = totalCount.load();
    }
    else
    {
        // A load for the old size is no use anymore, and the effects it left out make that size need a new one.
        if (loaderRunning)
        {
            StopLoader();
            compiledWidth = compiledHeight = 0;
        }
        resizePending = true;
        resizeTime = now;
        loadedCount = 0;
    }
}

void Runtime::ReloadNow()
{
    reloadRequested = false;
    resizePending = false;
    StopLoader();
    if (!width || !height)
        return;
    vkDeviceWaitIdle(gpu.device);
    DestroyAllGpu();
    SavePipelineCache();
    effects.clear();
    techniques.clear();
    textureFilesScanned = false;
    compiledWidth = width;
    compiledHeight = height;

    // Effects the preset uses compile first, so the picture has its effects as soon as possible.
    std::set<std::string> used;
    for (const std::string& file : PresetEffectFiles(PresetIni(ReadFile(presetPath))))
        used.insert(Lowercase(file));

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
            const std::string file = it->path().filename().string();
            // Presets name effects by file, so the first file of a name wins, as in ReShade.
            if (!seen.insert(Lowercase(file)).second)
                continue;
            CompileJob job{ it->path(), {} };
            // The first definition of a name wins, so the effect's own come first.
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
    options.cacheDirectory = CacheDirectory() / "effects";

    loadedCount = 0;
    totalCount = jobs.size();
    if (jobs.empty())
    {
        Log(LogLevel::Warning, "No effects were found. Install them from the launcher.");
        return;
    }
    Log(LogLevel::Info, "Compiling %zu effects for %ux%u...", jobs.size(), width, height);
    const uint64_t current = generation;
    loaderRunning = true;
    summaryPending = true;
    Loader& loader = *loaders.emplace_back(std::make_unique<Loader>());
    loader.thread = std::thread([this, &loader, jobs = std::move(jobs), options, current] {
        const auto cancelled = [this, current] { return generation != current; };
        std::atomic<size_t> next = 0;
        std::vector<std::thread> workers;
        const unsigned count = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
        for (unsigned i = 0; i < count; ++i)
            workers.emplace_back([&] {
                for (size_t index; !cancelled() && (index = next++) < jobs.size();)
                {
                    Effect effect;
                    CompileEffect(effect, jobs[index].path, jobs[index].definitions, options, cancelled);
                    std::lock_guard lock(finishedMutex);
                    if (cancelled())
                        return;
                    ++loadedCount;
                    finished.push_back(std::move(effect));
                }
            });
        for (std::thread& worker : workers)
            worker.join();
        if (!cancelled())
            PruneCache(options.cacheDirectory);
        {
            std::lock_guard lock(finishedMutex);
            if (!cancelled())
                loaderRunning = false;
        }
        loader.done = true;
    });
}

void Runtime::Update()
{
    JoinLoaders(false);
    if (resizePending && std::chrono::steady_clock::now() - resizeTime >= kResizeDelay)
        ReloadNow();
    if (reloadRequested)
        ReloadNow();
    std::deque<Effect> arrived;
    bool running;
    {
        std::lock_guard lock(finishedMutex);
        arrived.swap(finished);
        running = loaderRunning;
    }
    for (Effect& effect : arrived)
        AddEffect(std::move(effect));
    if (!arrived.empty())
        SortTechniques();
    // The loader is done once it stops running and everything it finished has been taken.
    if (summaryPending && !running)
    {
        summaryPending = false;
        size_t failed = 0, cached = 0;
        for (const Effect& effect : effects)
        {
            failed += !effect.compiled;
            cached += effect.cached;
        }
        Log(LogLevel::Info, "Effects loaded: %zu compiled (%zu from the cache), %zu failed.", effects.size() - failed, cached, failed);
        for (const std::string& missing : MissingTechniques())
            Report(LogLevel::Warning, "The preset uses %s, which is not installed.", missing.c_str());
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
                if (vkCreateComputePipelines(gpu.device, pipelineCache, 1, &info, nullptr, &p.pipeline) != VK_SUCCESS)
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
            if (vkCreateGraphicsPipelines(gpu.device, pipelineCache, 1, &info, nullptr, &p.pipeline) != VK_SUCCESS)
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

namespace
{
// Images written outside Vulkan, such as dma-bufs the X server draws to, change hands with a queue family
// ownership transfer around each use.
void TransferForeign(VkCommandBuffer commands, VkImage image, bool acquire)
{
    const uint32_t outside = gpu.foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = acquire ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = acquire ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = acquire ? outside : gpu.queueFamily;
    barrier.dstQueueFamilyIndex = acquire ? gpu.queueFamily : outside;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
}

void CopySource(VkCommandBuffer commands, const Runtime::Source& source, VkImage target, uint32_t width, uint32_t height)
{
    if (source.foreign)
        TransferForeign(commands, source.image, true);
    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffset = { int32_t(source.x), int32_t(source.y), 0 };
    region.dstSubresource = region.srcSubresource;
    region.extent = { width, height, 1 };
    vkCmdCopyImage(commands, source.image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    if (source.foreign)
        TransferForeign(commands, source.image, false);
}
} // namespace

void Runtime::Render(VkCommandBuffer commands, const Source& source, bool enabled)
{
    if (!width || !height)
        return;
    FullBarrier(commands);
    if (source.image)
        // Straight from the capture on the graphics card. Alpha is whatever the window has, as in games where
        // ReShade runs.
        CopySource(commands, source, backbuffer.image, width, height);
    else
    {
        std::memcpy(staging.mapped, source.pixels, size_t(width) * height * 4);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(commands, staging.buffer, backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    FullBarrier(commands);

    const auto now = std::chrono::steady_clock::now();
    frameTime = std::chrono::duration<float, std::milli>(now - lastFrame).count();
    lastFrame = now;
    ++frameCount;
    // Effects compiled for another size wait for the new one.
    if (!enabled || resizePending)
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
    return ReadImage(backbuffer.image, 0, 0, false);
}

std::vector<uint8_t> Runtime::ReadSource(const Source& source)
{
    return source.image ? ReadImage(source.image, source.x, source.y, source.foreign) : std::vector<uint8_t>();
}

// Copies width by height pixels at (x, y) of a BGRA image into memory, as RGBA. Waits for this copy only: the
// barrier before it orders it after the frames submitted before.
std::vector<uint8_t> Runtime::ReadImage(VkImage image, uint32_t x, uint32_t y, bool foreign)
{
    std::vector<uint8_t> pixels;
    if (!width || !height || !image)
        return pixels;
    const VkDeviceSize size = VkDeviceSize(width) * height * 4;
    if (readback.size != size)
    {
        gpu.DestroyBuffer(readback);
        if (!gpu.CreateBuffer(readback, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
            return pixels;
    }
    VkCommandBuffer commands = gpu.BeginCommands();
    if (!commands)
        return pixels;
    FullBarrier(commands);
    if (foreign)
        TransferForeign(commands, image, true);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageOffset = { int32_t(x), int32_t(y), 0 };
    copy.imageExtent = { width, height, 1 };
    vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
    if (foreign)
        TransferForeign(commands, image, false);
    VkMemoryBarrier toHost{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
    if (!gpu.SubmitAndWait(commands))
        return pixels;
    gpu.Invalidate(readback);
    pixels.resize(size);
    std::memcpy(pixels.data(), readback.mapped, size);
    for (size_t i = 0; i < size; i += 4)
    {
        std::swap(pixels[i], pixels[i + 2]);
        pixels[i + 3] = 255;
    }
    return pixels;
}
} // namespace fx
