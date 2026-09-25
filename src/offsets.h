#pragma once
#include <cstdint>
#include <wchar.h>

namespace ver {

// 4.40 只保留「被控期间仍可远控其他主机」。观看/仅浏览走官方能力。

// 4.42.1.2835 定位（IDA，SizeOfImage=0x4636000）：
//   HomePageContent 次虚表 0x143B53670，槽 +0xE0 是 thunk 0x1400BA9F6 -> sub_1402CEA70
//   （movzx eax,[rcx+0FAh]; ret，8 字节，可安全挂钩）
//   命令分发器 sub_1402D7B90(this, logname, callback)：被控时不跑 callback，转去收起被控窗口。
//   call [rax+0E0h] @ 0x2D7BD7，返回地址 0x2D7BDD。startRemoteAssist / startCloudDeviceAdd /
//   startCloudDeviceMarket 三个命令都走它，所以 connect / guard 两个返回值相同。
//   DeviceDesktopScene::render @ 0x3FF0B0：内部分别渲染桌面区（读 +0x69 / +0x6A）
//   和操作区（读 +0x8D / +0x8E）。
// 4.40 定位（旧版保留）：isControlled @ 0x2C95E0，dispatcher ret @ 0x2D270D，render @ 0x3F96B0。
struct VerSet {
    const wchar_t* version;
    // HomePageContent 次对象上的 isControlled()：读 this+0xFA 的一字节。
    // 虚表槽 0xE0，实现经 thunk 跳到这里。
    uintptr_t isCtrlNarrowRva;
    // 命令分发器 HomePageContent 包装（startRemoteAssist 等）里
    // call [vtable+0xE0] 之后的返回地址。只在这两处撒谎，收起被控页仍用真值。
    uintptr_t isCtrlConnectRetRva, isCtrlGuardRetRva;
    // DeviceDesktopScene::render(DeviceDetailViewData const&)
    uintptr_t deviceSceneRenderRva;
    // DeviceDetailViewData：platform 在 +0x60。
    // desktopAllowed=1 才不会画「该设备不允许被控」；desktopControlled=1 会藏进入桌面按钮。
    // actionAllowed=0 时操作区不可用（4.42.1 起该位 = 可用 && !被控，本机被控时会变 0）。
    // 4.42.1 没有独立的 actionControlled 字节：+0x8E 是「能力可用」位，清零会把操作区关掉，
    // 所以置 0 表示跳过这一项。
    uintptr_t deviceDesktopAllowedOff, deviceDesktopControlledOff;
    uintptr_t deviceActionAllowedOff, deviceActionControlledOff;
    uintptr_t imageSize;
};

inline const VerSet kVer[] = {
    // GameViewer 4.42.1.2835  SizeOfImage=0x4636000
    { L"4.42.1.2835", 0x2CEA70, 0x2D7BDD, 0x2D7BDD, 0x3FF0B0, 0x69, 0x6a, 0x8d, 0, 0x4636000 },
    // GameViewer 4.40.1.2090  SizeOfImage=0x4570000
    // RVA 与 4.40.0.1780 相同：isControlled @ 0x2C95E0，dispatcher ret @ 0x2D270D，render @ 0x3F96B0
    { L"4.40.1.2090", 0x2C95E0, 0x2D270D, 0x2D270D, 0x3F96B0, 0x69, 0x6a, 0x8f, 0x90, 0x4570000 },
    // GameViewer 4.40.0.1780  SizeOfImage=0x4570000
    { L"4.40.0.1780", 0x2C95E0, 0x2D270D, 0x2D270D, 0x3F96B0, 0x69, 0x6a, 0x8f, 0x90, 0x4570000 },
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
