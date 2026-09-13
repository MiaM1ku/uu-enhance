#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct HookStat { std::string name; void* addr; std::string how; bool ok; };

struct DbgLine { const wchar_t* role; std::string name; std::string how; unsigned long long off; bool ok; };

struct DebugInfo {
    std::wstring gvVersion;
    bool gvKnown;
    std::vector<DbgLine> hooks;
};
DebugInfo debug_snapshot();
