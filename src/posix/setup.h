#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Downloads effects and presets into the data folder, like Setup does on Windows: every package in ReShade's
// own EffectPackages.ini, then the presets listed in this repository's presets/downloads.ini. Files download
// with the system's curl and unpack with miniz.
class EffectSetup
{
public:
    struct State
    {
        bool running = false;
        bool finished = false; // set once, when a run ends
        std::string status;
        std::string detail;
        float fraction = 0;
        std::string error;
        std::vector<std::string> notes; // packages that were skipped
    };

    ~EffectSetup();
    void Start();
    void Cancel();
    State Read() const;
    // Returns true once after a run finished, so the caller can reload effects.
    bool TakeFinished();

    // Runs in the calling thread and prints progress, for --install-effects.
    static int RunInTerminal();

private:
    void Run();
    void Status(const std::string& status, const std::string& detail = {});
    void Fraction(float fraction);

    std::thread thread;
    std::atomic<bool> cancel = false;
    mutable std::mutex mutex;
    State state;
    bool finishedFlag = false;
};

// Lowercase hex. Exposed for tests.
std::string Sha256(const std::string& data);

// Where the effect packages and presets come from. Tests point them at local files.
struct SetupSources
{
    std::string effects = "https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini";
    std::string presets = "https://raw.githubusercontent.com/OMouta/Unishade/main/presets";
};
inline SetupSources setupSources;
