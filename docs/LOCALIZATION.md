# 中文本地化说明（LOCALIZATION）

本分支（`zh-cn-localization`）为 OpenXR-Layer-OBSMirror 增加了简体中文界面与文档。
所有改动都是**叠加式**的：英文原文与英文行为完整保留，任何语言资源缺失都会回退英文，
因此不会出现空白界面或功能异常。

## 1. 设计原则

1. **英文原文不动。** XAML 元素的 `Text`/`Content` 等属性、C# 里的字符串字面量都保留英文，
   同时作为资源缺失时的回退值。
2. **新增语言包，而不是替换。** 中文只以资源形式提供，按语言选择生效。
3. **不改逻辑。** 只用 `Loc.S` / `Loc.F` 包住用户可见文案，判断条件、比较用的值一律保持英文
   （例如服务层返回的运行时名称仍用于 `Equals("Not configured")` 这类判断，显示时才映射中文）。
4. **日志与诊断保持英文。** 写日志、诊断报告、上传的诊断包、异常消息全部不翻译，
   以免破坏排障与上游 issue 的对照。界面文案与日志文案在代码里是分开的两条路径。
5. **专有名词不译。** OpenXR、OpenVR、SteamVR、OBS Studio、Direct3D 11/12、WinUI 3、
   XR_COMPOSITION_LAYER_QUAD、filebin.net、设备名等保持原文。

## 2. 各组件怎么本地化的

### OBS 插件（`OBSPlugin/win-openxr`）

- 使用 OBS 自带机制：`data/locale/en-US.ini` + **新增** `data/locale/zh-CN.ini`，
  键与英文文件一一对应；OBS 的语言设为"简体中文"时自动生效。
- 裁切预设名原先写死在 `data/win_openxrmirror-presets.ini` 里且会直接显示在下拉框中，
  因此约定：**预设名以 `@` 开头时按语言键解析**（`@CropPresetNoCropping` →
  `obs_module_text("CropPresetNoCropping")`），设备名（如 `Reverb G2`）仍按字面显示。
  该改动在 `win-openxr.cpp` 的 `get_properties()` 内（在构建属性面板时才解析语言键，
  避免 `obs_module_load` 阶段语言尚未就绪导致预设名退化成键名），需要重新编译插件。

### ControlCenter（WinUI 3，C#）

- 资源：`ControlCenter/Strings/en-US/Resources.resw` 与 `Strings/zh-CN/Resources.resw`，
  由 MakePri 编译进 `OBSMirror.ControlCenter.pri`（SDK 默认收录 `Strings/**/*.resw`，无需改 csproj）。
- XAML：元素加 `x:Uid="键"`，对应资源名 `键.属性`（如 `Ui_Nav_Dashboard.Text`）。
  一个元素只能有一个 `x:Uid`，所以同一元素的多条文案共用同一个键基名
  （如 `Ui_Smoothing_Availability.Title` 与 `.Message`）。
  ⚠️ **不同元素不要共用同一个键基名**：MRT 会把该键下的所有资源套到每个带该 Uid 的
  元素上，元素缺少其中某个属性时会在**运行时**抛 `XamlParseException`，窗口不会出现
  （编译期不报错）。多个同类型元素共用一个只有单属性的 Uid（如 6 个 `TextBlock`
  共用 `Ui_Status_Checking.Text`）是安全的。`Test-Localization.ps1` 会检查这条规则。
- C#：`ControlCenter/Localization/Loc.cs`
  - `Loc.S("资源名", "English fallback")`
  - `Loc.F("资源名", "English {0} fallback", arg)`（带占位符）
  - **注意**：`Loc` 接收的是**资源名**而不是 `x:Uid`。复用 XAML 文案时必须带属性后缀，
    例如 `Loc.S("Ui_Nav_Dashboard.Text", "Dashboard")`；写成 `Ui_Nav_Dashboard` 会静默回退英文。
    `localization/tools/Test-Localization.ps1` 会检出这种错误。
- 语言选择：默认跟随系统语言；设置环境变量 `OBSMIRROR_LANG` 可强制指定，例如
  `set OBSMIRROR_LANG=zh-CN`（或 `en-US`）。
- **未打包应用必须显式设定语言**（否则界面全是英文）：未打包应用没有"包语言列表"，
  MRT 的默认资源上下文会一直解析到 pri 的默认限定符（en-US）。因此 `Loc.Initialize()`
  会在加载任何界面之前显式设定语言：
  - `Microsoft.Windows.Globalization.ApplicationLanguages.PrimaryLanguageOverride` —— 影响
    XAML 的 `x:Uid` 解析。注意 UWP 版 `Windows.Globalization.ApplicationLanguages` 在未打包
    应用上会直接抛异常，所以两者都尝试、失败即忽略；
  - 代码查找走 `ResourceManager.MainResourceMap.TryGetValue("Resources/<键路径>", context)`，
    其中 `context` 的 `Language` 限定符被显式设定。`ResourceLoader` 在未打包应用上恒用
    中性上下文（等于永远取 en-US），而本版本 WindowsAppSDK 的 `ResourceManager` 也没有
    `DefaultContext` 属性，因此不能依赖它们。
  实测（用 UI 自动化读取真实界面文字）：系统语言中文时界面为中文；
  `OBSMIRROR_LANG=en-US` 时整个界面切回英文。
- 启动日志会写明解析结果，便于快速确认：
  `%LOCALAPPDATA%\OpenXR-OBSMirror\ControlCenter-startup.log` 中的
  `Localization: language=..., overridden=..., dashboardNav=...` 一行。

### 安装程序（Inno Setup）

- `installer/languages/ChineseSimplified.isl`：社区简体中文语言文件（保留原文件内容，
  仅重新保存为 **UTF-8 with BOM**，以配合文件中声明的 `LanguageCodePage=936`）。
- `installer/OpenXR-OBSMirror.iss`：
  - `[Languages]` 增加 `chinesesimplified`，并为其单独指定 `InfoBeforeFile: "..\docs\INSTALL.zh-CN.md"`，
    使中文向导显示中文安装说明；
  - `[CustomMessages]` 提供英文/中文两套（桌面快捷方式、附加快捷方式、注册层状态、完成页"打开控制中心"）；
  - 该语言项与中文自定义消息都被 `#if HasChineseIsl` 包住：**语言文件缺失时自动跳过中文，
    构建不会失败**（退回英文安装界面）。
  - 语言文件面向 Inno Setup 6.5+；若本机 Inno 版本更低，请从同一来源获取对应版本替换，
    或直接删除该文件（安装程序仍可构建）。

### 文档

- `README.zh-CN.md`、`docs/INSTALL.zh-CN.md` 为中文版，顶部有语言互链；
  文档里提到的界面元素名称与**汉化后的实际界面文案**保持一致
  （依据 `localization/control-center-strings.tsv` 与 `OBSPlugin/win-openxr/data/locale/zh-CN.ini`）。
- `scripts/Build-Release.ps1` 会把中文文档一并放进发布产物（便携版与安装程序）。

## 3. 翻译数据的唯一来源

```
localization/control-center-strings.tsv     ← 主表（XAML 界面）
localization/fragments/*.tsv                ← 分片（C# 代码文案）
```

两份 resw 是**生成产物**，不要手改：

```powershell
pwsh -File localization/tools/New-ControlCenterResources.ps1   # 生成 resw
pwsh -File localization/tools/Test-Localization.ps1            # 一致性校验
```

`Test-Localization.ps1` 会在无编译器的情况下检查：资源名是否重复、各语言键集合是否一致、
有无未翻译项、resw 里**每个值**是否与表格逐条一致（键名对得上但内容过期同样会被检出）、
resw 是否残留表格里已删掉的条目、XAML 的 `x:Uid` 是否都有对应资源、C# 的 `Loc` 调用是否
都指向**存在的资源名**、每个 `code` 型资源是否真的被某个 `.cs` 引用（防止"英文写死在代码里、
译文躺在 resw 里没人用"）、以及 OBS 插件各语言 ini 的键是否与 `en-US.ini` 完全对齐
（含重复键检测）。

## 4. 构建与验证

```powershell
# ControlCenter（可只用 .NET SDK，不需要 Visual Studio）
dotnet build .\ControlCenter\OBSMirror.ControlCenter.csproj -c Release -p:Platform=x64

# OBS 插件（需要 OBS 源码树）
pwsh -File scripts\Build-OBSPlugin.ps1 -OBSSourcePath <obs-studio 路径> -OBSInstallPath <OBS 安装路径>

# 安装程序与发布产物（需要 VS2022 + Inno Setup 6）
pwsh -File scripts\Build-Release.ps1
```

验证中文资源确实进了资源索引（可选）：

```powershell
makepri dump /if .\ControlCenter\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64\OBSMirror.ControlCenter.pri /of pri.xml /o
# 期望看到：
#   <Candidate qualifiers="Language-ZH-CN" ...><Value>仪表盘</Value></Candidate>
#   <Candidate qualifiers="Language-EN-US" isDefault="true" ...><Value>Dashboard</Value></Candidate>
```

## 5. 术语表

| 英文 | 中文 |
| --- | --- |
| Mirror | 镜像 |
| Capture | 采集 |
| Recording | 录制 |
| Overscan | 过扫描 |
| Layer | 层（OpenXR API 层） |
| Runtime | 运行时 |
| OBS source | OBS 源 |
| Crop | 裁切 |
| Preview | 预览 |
| Headset | 头显 |
| Swapchain | 交换链 |
| Compositor | 合成器 |
| Quad layer | 四边形层 |
| Dashboard | 仪表盘 |
| Diagnostics | 诊断 |
| Simulator | 模拟器 |
| Plugin | 插件 |
| Installer | 安装程序 |
| Portable install | 便携版安装 |

## 6. 已知限制

- **日志、诊断报告、上传的诊断包、异常消息保持英文**（有意为之，见设计原则 4）。
- 服务层返回的运行时名称（如 `Not configured`、`System headset runtime`、`Meta XR Simulator`）
  在服务内部仍是英文以便逻辑判断；界面显示处由 `MainWindow` 映射为中文，
  未知的第三方运行时/设备名按原文显示。
- 裁切预设 `Reverb G2` 属于设备名，保持原文。
- 安装程序的中文界面已在本机用 Inno Setup 6.7.3 实际编译通过（0 错误 / 0 警告），
  并确认删除 `installer/languages/ChineseSimplified.isl` 后仍能编译（此时只有英文界面）。
- OBS 插件的中文语言文件与 `win-openxr.cpp` 的改动**未在本机编译验证**
  （本机没有 OBS 源码树），已通过键集合一致性脚本校验。
- `ControlCenter` 已用 .NET SDK 实际编译通过，并用 `makepri` 确认
  `zh-CN` / `en-US` 两份资源已按语言正确分离；也已在本机实际运行，用 UI 自动化读取
  界面文字确认"系统中文→中文界面、`OBSMIRROR_LANG=en-US`→英文界面"。
