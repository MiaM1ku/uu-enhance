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
    // 4.40 进入桌面区域：platform 在 +0x60；+0x69 或 +0x90 非 0 则不出「进入桌面」。
    uintptr_t deviceDesktopBlockOff, deviceActionBlockOff;
    uintptr_t imageSize;
};

inline const VerSet kVer[] = {
    // GameViewer 4.40.0.1780  SizeOfImage=0x4570000
    // isControlled @ 0x2C95E0  (movzx eax, [rcx+0FAh]; ret)
    // dispatcher ret @ 0x2D270D (sub_1402D26C0)
    // DeviceDesktopScene::render @ 0x3F96B0
    { L"4.40.0.1780", 0x2C95E0, 0x2D270D, 0x2D270D, 0x3F96B0, 0x69, 0x90, 0x4570000 },
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
