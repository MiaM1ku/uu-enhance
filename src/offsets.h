#pragma once
#include <cstdint>
#include <wchar.h>

namespace ver {

// 4.40 只保留「被控期间仍可远控其他主机」。观看/仅浏览走官方能力。
struct VerSet {
    const wchar_t* version;
    // HomePageContent 次对象上的 isControlled()：读 this+0xFA 的一字节。
    // 虚表槽 0xE0，实现经 thunk 0xB8F1B 跳到这里。
    uintptr_t isCtrlNarrowRva;
    // 命令分发器 HomePageContent 包装（startRemoteAssist 等）里
    // call [vtable+0xE0] 之后的返回地址。只在这两处撒谎，收起被控页仍用真值。
    uintptr_t isCtrlConnectRetRva, isCtrlGuardRetRva;
    // DeviceDesktopScene::render(DeviceDetailViewData const&)
    uintptr_t deviceSceneRenderRva;
    // DeviceDetailViewData：platform 在 +0x60。
    // desktopAllowed=1 才不会画「该设备不允许被控」；desktopControlled=1 会藏进入桌面按钮。
    uintptr_t deviceDesktopAllowedOff, deviceDesktopControlledOff;
    uintptr_t deviceActionAllowedOff, deviceActionControlledOff;
    uintptr_t imageSize;
};

inline const VerSet kVer[] = {
    // GameViewer 4.40.0.1780  SizeOfImage=0x4570000
    { L"4.40.0.1780", 0x2C95E0, 0x2D270D, 0x2D270D, 0x3F96B0, 0x69, 0x6a, 0x8f, 0x90, 0x4570000 },
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
