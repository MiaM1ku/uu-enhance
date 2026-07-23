#pragma once
#include <cstdint>
#include <cwchar>

namespace ver {

struct VerSet {
    const wchar_t* version;
    uintptr_t deviceIdOff;
    uintptr_t vmwDevIdOff, vmwTitleOff;
    uintptr_t isCtrlWrapperRva, isCtrlDirectRva;
    uintptr_t isCtrlMemberOff;
    bool isCtrlMemberEmbedded;
};

inline const VerSet kVer[] = {
    { L"4.33.0.8907", 4296, 344, 352, 0x73fe10, 0x4ea310, 0x30, true },
    { L"4.31.1.8795", 4296, 344, 352, 0x73fe20, 0x4e4d30, 0x30, true },
    { L"4.29.0.8620", 4296, 344, 352, 0x32489, 0, 0x130, false },
    { L"4.26.0.8259", 3984, 344, 352, 0, 0, 0, false },   // vmw offsets unverified
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
