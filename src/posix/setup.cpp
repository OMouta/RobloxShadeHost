#include "setup.h"
#include "config.h"
#include "log.h"
#include "preset.h"

#include <miniz.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <set>
#include <stdexcept>

extern char** environ;

namespace
{
struct Cancelled
{
};

// Downloads with curl, which macOS and nearly every Linux system have. Throws on failure or when cancelled.
void Download(const std::string& url, const fs::path& path, const std::atomic<bool>& cancel)
{
    if (url.rfind("https://", 0) != 0 && url.rfind("file://", 0) != 0)
        throw std::runtime_error("Refusing to download from " + url + ".");
    fs::create_directories(path.parent_path());
    const std::string output = path.string();
    const char* argv[] = { "curl", "-fsSL", "--retry", "2", "--connect-timeout", "20", "-o", output.c_str(), url.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, "curl", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0)
        throw std::runtime_error("curl is needed to download effects. Install it and try again.");
    int status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0)
    {
        if (cancel)
        {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            throw Cancelled{};
        }
        usleep(50'000);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Could not download " + url + ".");
}

bool IsInside(const fs::path& path, const fs::path& folder)
{
    const fs::path relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

void ExtractZip(const fs::path& zipPath, const fs::path& destination, const std::string& name)
{
    const std::string data = ReadFile(zipPath);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
        throw std::runtime_error("The download of " + name + " is not a zip file.");
    struct Closer
    {
        mz_zip_archive* zip;
        ~Closer() { mz_zip_reader_end(zip); }
    } closer{ &zip };

    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_directory)
            continue;
        const fs::path target = (destination / stat.m_filename).lexically_normal();
        if (!IsInside(target, destination))
            throw std::runtime_error(name + " contains a file outside its own folder.");
        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
        if (!bytes)
            throw std::runtime_error("Could not extract " + name + ".");
        const bool written = WriteFile(target, std::string(static_cast<const char*>(bytes), size));
        mz_free(bytes);
        if (!written)
            throw std::runtime_error("Could not write " + target.string() + ".");
    }
}

bool SameName(const fs::path& path, const char* name)
{
    return Lowercase(path.filename().string()) == name;
}

// Matches ReShade's own installer: folders named Shaders and Textures first, otherwise the shallowest folder
// with effect or image files.
void FindPackageFolders(const fs::path& directory, fs::path& shaders, fs::path& textures, fs::path& shaderFallback, fs::path& textureFallback)
{
    for (const auto& entry : fs::directory_iterator(directory))
    {
        const fs::path& path = entry.path();
        if (entry.is_directory())
        {
            if (shaders.empty() && SameName(path, "shaders"))
                shaders = path;
            if (textures.empty() && SameName(path, "textures"))
                textures = path;
            FindPackageFolders(path, shaders, textures, shaderFallback, textureFallback);
            continue;
        }
        const std::string extension = Lowercase(path.extension().string());
        const auto shallower = [&](const fs::path& current) { return current.empty() || directory.native().size() < current.native().size(); };
        if (extension == ".fx" && shallower(shaderFallback))
            shaderFallback = directory;
        if ((extension == ".png" || extension == ".jpg" || extension == ".jpeg") && shallower(textureFallback))
            textureFallback = directory;
    }
}

void CopyPackageFiles(const fs::path& source, const fs::path& destination, const std::string& denied)
{
    fs::create_directories(destination);
    std::set<std::string> deniedNames;
    for (std::string name : PresetIni::Split(denied))
    {
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        deniedNames.insert(Lowercase(name));
    }
    for (const auto& entry : fs::directory_iterator(source))
    {
        const fs::path name = entry.path().filename();
        if (entry.is_directory())
        {
            CopyPackageFiles(entry.path(), destination / name, denied);
            continue;
        }
        const std::string extension = Lowercase(name.extension().string());
        if (extension == ".addon" || extension == ".addon32" || extension == ".addon64" || extension == ".dll" || extension == ".exe" ||
            deniedNames.count(Lowercase(name.string())))
            continue;
        fs::copy_file(entry.path(), destination / name, fs::copy_options::overwrite_existing);
    }
}

// EffectPackages.ini writes Windows paths such as .\reshade-shaders\Shaders\OtisFX.
fs::path PackageDestination(const std::string& relative, const char* kind, const std::string& package)
{
    std::string path = relative;
    std::replace(path.begin(), path.end(), '\\', '/');
    const fs::path destination = (DataDirectory() / path).lexically_normal();
    const fs::path root = (EffectsDirectory() / kind).lexically_normal();
    if (relative.empty() || (!IsInside(destination, root) && destination != root))
        throw std::runtime_error("The effect package " + package + " has an invalid install folder.");
    return destination;
}

// SHA-256, for the preset checksums.
struct Sha256State
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    void Block(const uint8_t* p)
    {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
            w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
};
} // namespace

std::string Sha256(const std::string& data)
{
    Sha256State state;
    size_t offset = 0;
    for (; offset + 64 <= data.size(); offset += 64)
        state.Block(reinterpret_cast<const uint8_t*>(data.data()) + offset);
    uint8_t tail[128]{};
    const size_t rest = data.size() - offset;
    std::memcpy(tail, data.data() + offset, rest);
    tail[rest] = 0x80;
    const size_t length = rest < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; ++i)
        tail[length - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));
    for (size_t block = 0; block < length; block += 64)
        state.Block(tail + block);
    char hex[65];
    for (int i = 0; i < 8; ++i)
        snprintf(hex + i * 8, 9, "%08x", state.h[i]);
    return hex;
}

EffectSetup::~EffectSetup()
{
    Cancel();
}

void EffectSetup::Start()
{
    if (thread.joinable())
    {
        if (Read().running)
            return;
        thread.join();
    }
    cancel = false;
    {
        std::lock_guard lock(mutex);
        state = {};
        state.running = true;
    }
    thread = std::thread([this] { Run(); });
}

void EffectSetup::Cancel()
{
    cancel = true;
    if (thread.joinable())
        thread.join();
}

EffectSetup::State EffectSetup::Read() const
{
    std::lock_guard lock(mutex);
    return state;
}

bool EffectSetup::TakeFinished()
{
    std::lock_guard lock(mutex);
    return std::exchange(finishedFlag, false);
}

void EffectSetup::Status(const std::string& status, const std::string& detail)
{
    std::lock_guard lock(mutex);
    state.status = status;
    state.detail = detail;
    Log(LogLevel::Info, "Setup: %s %s", status.c_str(), detail.c_str());
}

void EffectSetup::Fraction(float fraction)
{
    std::lock_guard lock(mutex);
    state.fraction = fraction;
}

void EffectSetup::Run()
{
    const fs::path work = fs::temp_directory_path() / ("unishade-setup-" + std::to_string(getpid()));
    std::vector<std::string> notes;
    std::string error;
    try
    {
        fs::remove_all(work);
        fs::create_directories(work);

        // Effects
        Status("Downloading the list of effects");
        const fs::path catalogPath = work / "EffectPackages.ini";
        Download(setupSources.effects, catalogPath, cancel);
        const PresetIni catalog(ReadFile(catalogPath));
        const std::vector<std::string> sections = catalog.SectionNames();
        if (sections.empty())
            throw std::runtime_error("The list of effects is empty.");
        for (size_t index = 0; index < sections.size(); ++index)
        {
            const std::string& section = sections[index];
            std::string name = section, url, installPath, texturePath, denied;
            catalog.Get(section, "PackageName", name);
            catalog.Get(section, "DownloadUrl", url);
            catalog.Get(section, "InstallPath", installPath);
            catalog.Get(section, "TextureInstallPath", texturePath);
            catalog.Get(section, "DenyEffectFiles", denied);
            Status("Downloading effects", std::to_string(index + 1) + " of " + std::to_string(sections.size()) + ": " + name);
            try
            {
                const fs::path zip = work / "package.zip";
                const fs::path extracted = work / "package";
                fs::remove_all(extracted);
                Download(url, zip, cancel);
                ExtractZip(zip, extracted, name);
                fs::path shaders, textures, shaderFallback, textureFallback;
                FindPackageFolders(extracted, shaders, textures, shaderFallback, textureFallback);
                if (shaders.empty())
                    shaders = shaderFallback;
                if (textures.empty())
                    textures = textureFallback;
                if (shaders.empty())
                    throw std::runtime_error("The effect package " + name + " contains no effects.");
                CopyPackageFiles(shaders, PackageDestination(installPath, "Shaders", name), denied);
                if (!textures.empty())
                    CopyPackageFiles(textures, PackageDestination(texturePath, "Textures", name), "");
                fs::remove_all(extracted);
                fs::remove(zip);
            }
            catch (const std::exception& e)
            {
                // One package that cannot be downloaded should not stop the rest.
                notes.push_back(name + ": " + e.what());
                Log(LogLevel::Warning, "Setup skipped %s: %s", name.c_str(), e.what());
            }
            Fraction(0.9f * float(index + 1) / float(sections.size()));
        }

        // Presets
        Status("Downloading presets");
        const fs::path presetList = work / "preset-downloads.ini";
        Download(setupSources.presets + "/downloads.ini", presetList, cancel);
        const PresetIni presets(ReadFile(presetList));
        fs::create_directories(PresetsDirectory());
        for (const std::string& file : presets.SectionNames())
        {
            std::string hash;
            if (fs::path(file).filename() != file || Lowercase(fs::path(file).extension().string()) != ".ini" || !presets.Get(file, "sha256", hash))
                throw std::runtime_error("The preset list contains an invalid entry.");
            const fs::path downloaded = work / file;
            Download(setupSources.presets + "/" + file, downloaded, cancel);
            const std::string contents = ReadFile(downloaded);
            if (Sha256(contents) != Lowercase(hash))
                throw std::runtime_error("The preset " + file + " did not match its checksum.");
            // Presets the user already has may have changes of theirs.
            if (!fs::exists(PresetsDirectory() / file) && !WriteFile(PresetsDirectory() / file, contents))
                throw std::runtime_error("Could not write the preset " + file + ".");
        }
        Fraction(1.0f);
        Status(notes.empty() ? "Effects and presets are installed." : "Effects and presets are installed, with some packages skipped.");
    }
    catch (const Cancelled&)
    {
        error = "Cancelled.";
    }
    catch (const std::exception& e)
    {
        error = e.what();
        Log(LogLevel::Error, "Setup failed: %s", e.what());
    }
    std::error_code ignored;
    fs::remove_all(work, ignored);
    std::lock_guard lock(mutex);
    state.running = false;
    state.finished = true;
    state.error = error;
    state.notes = notes;
    finishedFlag = true;
}

int EffectSetup::RunInTerminal()
{
    EffectSetup setup;
    setup.Start();
    std::string last;
    for (State state = setup.Read(); state.running; state = setup.Read())
    {
        const std::string line = state.status + (state.detail.empty() ? "" : " - " + state.detail);
        if (line != last)
            printf("%3d%%  %s\n", int(state.fraction * 100), line.c_str());
        last = line;
        usleep(200'000);
    }
    setup.thread.join();
    const State state = setup.Read();
    for (const std::string& note : state.notes)
        printf("Skipped %s\n", note.c_str());
    if (!state.error.empty())
    {
        fprintf(stderr, "%s\n", state.error.c_str());
        return 1;
    }
    printf("%s\n", state.status.c_str());
    return 0;
}
