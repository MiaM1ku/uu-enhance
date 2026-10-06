# UU远程增强 开发指南

## 构建

需要 Visual Studio 2022（C++ 桌面开发 + CMake）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物在 `build/Release/`：
- `version.dll` — 补丁本体
- `uu-enhance-installer.exe` — 一键安装器（内嵌完整 DLL）

**注意**：源码含 UTF-8 中文注释，CMakeLists.txt 里的 `/utf-8` 不能去掉，否则 MSVC 按 936 码页误读会编译失败。`src/app.h` 只能写 ASCII（rc.exe 用系统 ANSI 码页读它），中文显示名放在 `tray.cpp` 和 `installer.cpp` 里。

## 版本号

版本号**唯一定义**在 `src/app.h`，所有地方从这里取：

```c
#define UURE_VERSION     "0.1.0"        // C 字符串
#define UURE_VERSION_W   L"0.1.0"       // 宽字符串
#define UURE_VERSION_RC  0,1,0,0        // VERSIONINFO 资源用的四段逗号格式
```

改版本号**只改这一个文件**，DLL（app.rc）和安装器（installer.rc）的 VERSIONINFO 都从这里 include。

GitHub 仓库地址也在这里（`UURE_GITHUB` / `UURE_GITHUB_W`），安装器的更新检查和托盘菜单的"项目主页"链接都用它。

## 发布流程

1. **改版本号**：编辑 `src/app.h`，同时改 `UURE_VERSION`、`UURE_VERSION_W`、`UURE_VERSION_RC` 三个宏。
2. **构建**：`cmake --build build --config Release`。确认 `build/Release/` 下 DLL 和安装器都更新了。
3. **测试**：把产出的 `version.dll` 丢进 GameViewer 的 `bin\` 目录，启动 GameViewer，确认日志写出、各功能正常（默认不显示托盘图标）。用安装器测一遍安装/卸载/更新流程。
4. **提交 + 打 tag**：
   ```powershell
   git add -A
   git commit -m "release: v0.2.0"
   git tag v0.2.0
   git push origin main --tags
   ```
   tag 格式固定为 `vX.Y.Z`，安装器的更新检查从 GitHub Release 的 `tag_name` 取这个值，去掉 `v` 前缀后跟本地版本比较。
5. **创建 GitHub Release**（只建空壳，产物交给 Action）：
   ```powershell
   gh release create v0.2.0 --title "v0.2.0" --notes "改动说明"
   ```
   发布后 `.github/workflows/release.yml` 会自动在 windows-latest 上构建 Release，并把 `uu-enhance-installer.exe` 和 `version.dll` 传回这个 Release。**不用再本地附加附件**。
   - workflow 会先校验 tag（去 `v`）等于 `app.h` 的 `UURE_VERSION`，不一致直接失败——防止更新检查对不上。
   - 想手动重跑：Actions 页面选 `build-release` → Run workflow，填已存在的 tag。
   - 产物里 `uu-enhance-installer.exe` 是用户下载的主体（DLL 已内嵌），`version.dll` 附上方便手动安装的用户。

## 安装器更新检查

安装器启动时在后台线程用 WinHTTP 请求 `https://api.github.com/repos/MiaM1ku/uu-enhance/releases/latest`，从返回的 JSON 里取 `tag_name` 字段，去掉 `v` 前缀后和内嵌的 `UURE_VERSION` 比较。如果 Release 版本更新，在底部链接栏显示"新版本 vX.Y.Z 可用"并指向 Release 页面。请求失败（没网、API 限流、仓库不存在）静默忽略。

所以**发了 Release 就等于推送了更新通知**，不需要额外的更新服务器。

## 项目结构

```
src/           补丁本体
  app.h        版本号、GitHub URL（唯一定义处，ASCII only）
  app.rc       DLL 的 VERSIONINFO 资源
  dllmain.cpp  入口：代理转发 + 后台线程
  proxy.cpp    version.dll 17 个导出转发
  hooks.cpp    被控期间仍可远控：isControlled 窄绕过 + 设备卡片渲染
  resolver.cpp 抗更新定位器（字符串 + .pdata）
  tray.cpp     系统托盘菜单（默认不启动）
  config.cpp   ini 读写
  offsets.h    4.42.1 / 4.40.1 / 4.40.0 isControlled / DeviceDesktopScene RVA

installer/     一键安装器
  installer.cpp  GUI + 自动查找 + 释放/卸载 + 更新检查
  installer.rc   内嵌 version.dll + manifest + VERSIONINFO
  resource.h     资源和控件 ID
  installer.manifest  管理员权限 + 现代控件 + 高 DPI

vendor/minhook/ MinHook 源码
```

## 适配 GameViewer 新版本

4.40 起只保留「被控期间仍可远控其他主机」。版本表在 `src/offsets.h`（当前支持 4.42.1.2835 / 4.40.1.2090 / 4.40.0.1780）。未知版本或 SizeOfImage 对不上会拒绝安装 RVA hook。

1. 拿到新版 GameViewer.exe，用 IDA 打开
2. HomePageContent 构造里 `this+0x30` 是次虚表（`??_7HomePageContent@home@client_ui@@6B@_1`）。槽 `+0xE0` 是 `isControlled()`，4.40 实现为 `movzx eax, [rcx+0FAh]; ret`
3. 只在「提交连接 / 发起远控保护」两处对 `isControlled()` 撒谎。这两处都走命令分发器：4.40 是 `sub_1402D26C0`（ret `0x2D270D`），4.42.1 是 `sub_1402D7B90`（`startRemoteAssist` / `startCloudDeviceAdd` / `startCloudDeviceMarket` 都经它，ret `0x2D7BDD`）；返回地址取 `call [rax+0E0h]` 的下一条。分发器被控分支只做「收起被控窗口 + 不跑回调」，真正收起被控页的其它调用点仍拿真值
4. `DeviceDesktopScene::render`：`DeviceDetailViewData+0x60` 是 platform（1=Win，4=Mac）。`+0x69=0` 会画「该设备不允许被控」，`+0x6a=1` 会藏进入桌面。hook 只在 render 期间改成允许=1、被控=0，返回后恢复。4.40 render @ `0x3F96B0`，4.42.1 @ `0x3FF0B0`。操作区：4.40 是 `+0x8f`（允许）/ `+0x90`（被控）；4.42.1 外层门是 `+0x8C`（show/hide a1+120），`+0x8D` 是启用样式需置 1，`+0x8E` 是能力位不能清零（`deviceActionControlledOff=0` 跳过）。4.42.1 底部工具栏（观看模式/文件传输等）另走 `+0xD8` 的 8 字节数组：每项 `DWORD toolId`、`+4` 启用、`+5` 是否上底栏。被控时 `+4` 被清成 0（按钮还在，但是灰的）。render hook 只置 `+4=1`：出栏集合跟官方一致，布局不动，灰按钮变可用。**不要置 `+5`**——`DesktopBottomWidget::update` 拿 `+5` 做集合 diff，集合一变就整套重建按钮，官方收在「更多工具」里的项会被一起拉到 680px 宽的底栏上，7 个 137px 的按钮加一个 52px 的「更多」放不下，全挤成一排

### 被控时底栏工具点了没反应

拦截点不在信号层也不在 model——点击从按钮经四跳信号（toolId 直传）到 `DeviceDesktopView::onDeviceTool`（RVA `0x409620`），交给 presenter 后，`DeviceDetailPresenter::onDeviceTool`（RVA `0xA8C720`）开头先现场构建「已发布工具」列表再查表：

- **启用判定 `isToolEnabled`（4.42.1 RVA `0xA88B90`）**：签名 `(presenter, toolId, hasSession)`。toolId 0-3（观看/文件传输/端口/终端）在 `hasSession=1` 时判 0；本机被控时 presenter+0x18 有入站会话，`hasSession=1`，这四个工具就全被压掉。toolId>3（重启/电源）不受影响——所以被控时日志里 id=5,6 的 `en=1`、id=0-3 的 `en=0`，数据层 `+0xD8` 数组的 `+4` 与之同源。查表没过就 `reason=not_published` 拒绝（Warning 级日志，默认看不见，界面毫无提示）。
- **按钮 state 被禁用**：`DesktopButton` 的启用状态设置（RVA `0x6B6D80`）把 state 置 3，`DesktopButton::event`（RVA `0x6B6920`）在 state==3 时直接丢鼠标事件。

修法两处 hook：`isToolEnabled` 转发时把 `hasSession` 强制当 0（设备能力判定 `sub_140A85BA0` 和云设备 toolId=4 的平台检查原样保留）；`DesktopButton` 启用状态恒按启用处理。**不要**用 `startRemoteAssist` 直连观看模式——那是"开始协助"的入口，会跳到协助流程而不是直接以仅观看模式连上主机；观看模式的正确路径是 presenter case 0 → `home_frame_model`（presenter+0x28，仅 NULL 检查，被控时非空）→ dispatch `"view_mode"` 请求，修好启用判定后这条原路就能走通。
5. 布局守卫：`SizeOfImage` + 字符串 `startRemoteAssist: device data is not init, return` 和 `control_mode_switch`
6. 重新构建、测试、发版
