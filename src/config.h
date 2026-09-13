#pragma once
#include <atomic>
#include <string>

namespace cfg {
    extern std::atomic<bool> g_autoUpdate;

    void load();
    void save();
    std::wstring config_path();
    std::wstring exe_version();
}
