# UU远程增强

给 [UU远程](https://uuyc.163.com/)（GameViewer）加官方没有的功能。DLL 代理（伪装 `version.dll`，和 ReShade 一个套路）：丢进安装目录生效，删掉恢复。

> 个人逆向研究，与网易无关，不涉及破解或绕过计费。风险自负。

## 功能

当前适配 **GameViewer 4.40.1.2090**（兼容 4.40.0.1780）。4.40 已自带观看模式，本补丁只保留一项：

- **被控期间仍可远控其他主机** — 本机正在被别人控制时，设备列表仍可点「进入桌面」，并继续发起对其它主机的远控。被控页的收起/展开不受影响。

## 安装

从 [Releases](https://github.com/MiaM1ku/uu-enhance/releases) 下载 `uu-enhance-installer-<版本>.exe`，双击安装/卸载，自动定位 GameViewer；DLL 被占用时会列出进程并可一键关闭/重启。也可手动把 `version.dll` 放进 `GameViewer\bin\`。

## 构建

Visual Studio 2022 + CMake：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物在 `build/Release/`：`version.dll` 和 `uu-enhance-installer-<版本>.exe`（DLL 已内嵌）。

## License

[MIT](LICENSE) · 依赖 [MinHook](https://github.com/TsudaKageyu/minhook)
