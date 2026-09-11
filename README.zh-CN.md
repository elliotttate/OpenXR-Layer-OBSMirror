# OpenXR + OpenVR OBS Mirror

> 简体中文 | [English](README.md)

> **非官方汉化分支。** 本仓库是社区简体中文汉化版，**并非上游作者发布的版本**，也未获得其背书或审核。
> 上游项目：[elliotttate/OpenXR-Layer-OBSMirror](https://github.com/elliotttate/OpenXR-Layer-OBSMirror)
> （作者 Elliott Tate；其中的 OpenXR API 层源自
> [OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)，MIT 许可，版权与全文见 [LICENSE](LICENSE)）。
> 汉化内容与实现说明见 [docs/LOCALIZATION.md](docs/LOCALIZATION.md)，
> 第三方组件与 OBS 插件的许可说明见 [THIRD_PARTY](THIRD_PARTY)，
> 更完整的汉化版声明见 [docs/NOTICE-ZH-CN.md](docs/NOTICE-ZH-CN.md)。
>
> **语言切换**：界面中文随系统语言自动生效，也可用环境变量 `OBSMIRROR_LANG=zh-CN`（或 `en-US`）强制指定；
> 英文原文与英文界面完整保留，语言资源缺失时自动回退英文。

**在 OBS Studio 中直接采集原生 OpenXR 应用程序或 SteamVR/OpenVR 合成器。OpenXR 采集还可以使用更宽、更稳定的录制相机，同时头显继续正常观看和追踪。**

OpenXR OBS Mirror 将一个原生 OpenXR API 层、原生 OpenXR 与 OpenVR OBS 源，以及一个自包含的深色 WinUI 3 控制中心（Control Center）组合在一起。它支持 Direct3D 11 和 Direct3D 12 OpenXR 应用程序，以及 Windows x64 上的 SteamVR/OpenVR 合成器采集，同时将机器的常规头显运行时保留为默认值。

主要的录制控制包括：

- 仅录制生效的 FOV 过扫描，头显中心裁切保持不变；
- 实时相机平滑与裁切边距；
- 独立控制 OpenXR 合成四边形层的显示/隐藏；
- 跟随 OpenXR 或 OpenVR/SteamVR 采集的应用内预览；
- 实时显示运行时、层、插件、哈希值与诊断日志状态。
- 原生 SteamVR/OpenVR 左眼、右眼或立体镜像采集。

## 实际效果

| 更宽 FOV + 平滑相机 | 控制中心导览 |
| :---: | :---: |
| [![更宽 FOV 与平滑后的 VR 画面](https://img.youtube.com/vi/0Aa91IXBh3c/maxresdefault.jpg)](https://youtu.be/0Aa91IXBh3c) | [![OBS OpenXR 录制界面](https://img.youtube.com/vi/0CDRNip2I10/maxresdefault.jpg)](https://www.youtube.com/watch?v=0CDRNip2I10) |
| 仅录制生效的 FOV 过扫描与相机平滑的实际效果。 | 对安装、状态与录制控制的引导式演示。 |

点击任意预览图即可在 YouTube 上观看。

OpenXR 层模板基于
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)。

## 快速安装

1. 打开[最新的 GitHub 发行版](https://github.com/elliotttate/OpenXR-Layer-OBSMirror/releases/latest)。
2. 关闭 OBS Studio 以及任何正在运行的 OpenXR 应用程序。
3. 下载并运行 `OpenXR-OBSMirror-...-Setup.exe` 安装程序。
4. 打开 OBS Studio，添加 **VR 镜像采集（自动：OpenXR / SteamVR）**。
   它会自动选择正确的采集后端。
5. 通过头显软件正常启动 VR 应用程序。

安装程序会安装匹配的 OBS 源、为当前用户注册层、添加开始菜单集成，并打开控制中心。它**不会**选择模拟器，也不会替换系统 OpenXR 运行时。管理员提示仅用于将 OBS 源放入 OBS Studio 的共享插件文件夹。

更希望使用便携版安装？解压完整的便携版 ZIP，双击
`Launch OpenXR OBS Mirror.cmd`，然后在控制中心里使用 **安装 / 更新**。
该应用包含其所需的 .NET 和 Windows App SDK 运行时文件。

完整的安装、更新、卸载、录制控制与故障排查说明请见 [docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)（英文说明：[docs/INSTALL.md](docs/INSTALL.md)）。请用发行版中的 `SHA256SUMS.txt` 校验下载文件；当前构建未签名，可能会触发 Windows SmartScreen 警告。

也可以手动注销当前用户的层注册：

```powershell
pwsh -File .\scripts\Uninstall-Layer.ps1 -Scope CurrentUser
```

## 从源码构建并安装

初始化子模块并还原原生 NuGet 包：

```powershell
git submodule update --init --recursive
nuget restore .\OpenXR-Layer-OBSMirror.sln `
  -Source https://api.nuget.org/v3/index.json
```

使用 Visual Studio 2022 构建 x64 层：

```powershell
msbuild .\OpenXR-Layer-OBSMirror.sln /m `
  /p:Configuration=Release /p:Platform=x64
```

OBS 插件必须针对与已安装 OBS 版本匹配的源码进行编译。例如，对于 OBS 32.2.1：

```powershell
git clone --depth 1 --branch 32.2.1 `
  https://github.com/obsproject/obs-studio.git C:\src\obs-studio-32.2.1
pwsh -File .\scripts\Build-OBSPlugin.ps1 `
  -OBSSourcePath C:\src\obs-studio-32.2.1
```

关闭 OBS 后，为当前用户安装这两个刚构建好的组件：

```powershell
pwsh -File .\scripts\Setup-OBS.ps1
```

若要在不打断正在运行的 OBS 的情况下更新带哈希版本的层，请加上
`-AllowRunningOBS`。如果插件二进制文件也发生了变化，请关闭 OBS 并再次运行安装，以便安全地复制新的源。

这会把层放到 `%LOCALAPPDATA%\OpenXR-OBSMirror`，在
`HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit` 下注册其清单，并把插件安装到 OBS 的 Windows 发现路径
`%ProgramData%\obs-studio\plugins\win-openxr\bin\64bit`。

## 选择采集源

- **VR 镜像采集（自动：OpenXR / SteamVR）** 是推荐的源。当可用的 OpenXR 应用程序存在时，它通过本项目的 API 层读取该应用的图像，然后自动回退到 SteamVR 的原生合成器镜像以支持 OpenVR 应用程序。使用较旧的 **OpenXR 镜像采集** 名称保存的现有场景会被就地升级，因为源 ID 没有改变。支持 Direct3D 11 和 Direct3D 12 OpenXR 应用程序。
- **OpenVR / SteamVR 镜像采集** 直接读取 SteamVR 的原生合成器镜像，仍作为显式的高级源提供。它提供左眼、右眼和并排立体模式、百分比裁切控制、自动填充录制画布，以及按需重连。SteamVR 必须正在运行。

只有当 SteamVR 已在运行且没有可用的 OpenXR 镜像时，自动源才会从插件文件夹加载官方 Valve OpenVR API。它不会启动 SteamVR，也不会更改活动的 OpenXR 运行时。OpenXR 过扫描、平滑和四边形层控制
无法修改已经合成好的 SteamVR 镜像，因此这些控制仅在自动源使用其 OpenXR 后端时生效。

## 控制中心

深色 WinUI 3 控制中心提供一个集中的位置，用于查看层、插件、运行时和 OBS 状态；安装或更新这两个组件；注册层；预览活动的 OpenXR 或 OpenVR 镜像画面；配置录制过扫描；控制相机
平滑；在录制中显示或隐藏 OpenXR 四边形层 UI；以及查看实时日志。
它以头显为先：**仪表盘（Dashboard）** 会显示实际生效的运行时，在模拟器覆盖处于活动状态时发出警告，并提供 **使用头显运行时** 来清除每用户模拟器选择器并返回到机器级 OpenXR 运行时。

构建自包含的 x64 副本：

```powershell
pwsh -File .\scripts\Build-ControlCenter.ps1
```

用一条可复现的命令构建原生层、匹配的 OBS 32.2.1 源码、控制中心、安装程序、便携版 ZIP 和校验和：

```powershell
pwsh -File .\scripts\Build-Release.ps1 `
  -Version 0.3.0-beta.4 `
  -OBSSourcePath E:\Github\obs-studio
```

运行 `bin\x64\Release\ControlCenter\OBSMirror.ControlCenter.exe`。过扫描
更改会在 OpenXR 应用程序下次启动时生效。相机平滑更改
会被活动的 OBS Mirror 源实时采用。四边形层可见性在更新后的 OpenXR 层
被加载过一次之后会被实时采用。仪表盘预览直接连接到 OBS 所使用的活动 OpenXR 共享图像或
SteamVR 合成器镜像，并在选中另一个控制中心
页面时暂停。打开控制中心从不会启动 SteamVR 或更改
活动的 OpenXR 运行时。

## 运行时注意事项

- 安装层之后再启动 OpenXR 应用程序。
- OBS 可以在 VR 应用程序启动之前就加载该源；一旦应用程序创建了共享镜像表面，该源就会重试
  其 IPC 连接。
- 在某些系统上，以提升的权限运行 OBS 可能会改善 GPU 调度优先级，但
  插件本身不需要管理员权限。
- OpenXR 应用程序和 OBS 必须运行在同一个 Windows 桌面上，并使用
  兼容的 D3D11 适配器，共享纹理才能打开。
- OpenVR 源还要求 OBS 和 SteamVR 使用同一块 GPU。它的 OBS
  互操作路径是 D3D11，因为这是 SteamVR 暴露的镜像接口；
  它不会更改 VR 应用程序所使用的渲染 API。
- 由头显软件选择的机器级 OpenXR 运行时是正常的
  默认值。控制中心从不会仅仅因为打开了其可选的测试工具就选择模拟器，并且它会剥离它所启动的应用程序继承来的 `XR_RUNTIME_JSON` 覆盖。
- 模拟器可能会残留每用户的 `XR_RUNTIME_JSON` 或 `ActiveRuntime` 覆盖。
  使用控制中心里的 **使用头显运行时** 可清除 64 位
  和 32 位的每用户选择器。请重启在旧的环境变量覆盖处于活动状态时
  已经启动的任何启动器。
- 某些模拟器版本会在测试时刷新 OpenXR API 层注册。
  如果层状态在模拟器会话之后发生变化，请在下次启动 OpenXR 应用程序之前把 **层**
  开关重新打开。

## 录制过扫描（实验性）

录制通常只显示头显的精确视场角，因此头部运动
会紧贴画面边缘。录制过扫描会要求 OpenXR 应用程序渲染
更宽的视场角和按比例更大的图像，把完整的宽
图像送入 OBS，并只把原始的中心裁切提交给 OpenXR 运行时
——头显视图保持不变，包括其每度像素数。

```powershell
# Enable with the defaults (115% horizontal, 108% vertical, ~24% more pixels)
pwsh -File .\scripts\Set-RecordingOverscan.ps1 -Enable

# Custom scale
pwsh -File .\scripts\Set-RecordingOverscan.ps1 -Enable -HorizontalPercent 120 -VerticalPercent 110

# Turn it off again
pwsh -File .\scripts\Set-RecordingOverscan.ps1 -Disable
```

### 为什么录制画面看起来仍是方形

采集的是头显的其中一只眼睛，而头显对每只眼睛的渲染比例接近
1:1 —— 例如，SteamVR 在 Index 级别的头显上要求 3344 × 3344。
这两个扩展百分比会对该方形进行缩放，因此决定录制画面形状的是它们之间的*比例*，而不是其中任何一个单独的值：130% × 115%
的结果仍然是 1.13:1。要得到宽屏录制，水平
扩展需要比垂直扩展多出目标宽高比的量——16:9 对应 178% × 100%。

控制中心的过扫描页面会完成这项计算。它的 **录制画面比例**（Recording shape）
按钮会根据目标宽高比设置两个滑块，而 **录制帧尺寸**（Recording frame）卡片会
报告当前设置产生的像素尺寸和形状，使用的是层从最近一次 VR 会话中学到的每眼尺寸。

垂直扩展无法让录制画面变得更宽，所以当目标是
宽屏画面时，它只会把 GPU 时间花在画面不会显示的像素上。
这就是形状按钮把它保持在 100% 的原因。

### 画面两侧的黑边

当把接近方形的镜像放到 16:9 画布上时就会出现黑边：场景
项会保持源的宽高比，因此画布会在两侧露出来。
有两种方法可以消除它们。

- OBS 源上的 **填满录制画布（按镜像形状裁切）** 会把镜像以居中方式裁切到
  画布的形状，使源填满画面、没有黑边、也不需要
  手动裁切值。它没有额外开销且立即生效，但在 16:9 画布上的方形
  镜像会损失顶部和底部各约 22%。
- **178% 水平过扫描**（即 **16:9** 形状按钮）会让镜像
  本身变成 16:9，因此没有需要裁切的内容，也没有损失——录制
  会保留完整的头显视图并在此基础上增加内容。代价是渲染像素增加 78%，
  并且需要重启 VR 应用程序。

两者可以结合使用：在任何 178% 或更高的水平扩展下，填充画布
已经没有可以从头显视图中裁掉的部分。低于该值时，裁切会先来自
过扫描保护带，然后才来自头显视图。

该设置在 VR 应用程序启动时读取一次，因此更改后请重启应用程序。注意事项：

- 渲染开销会随额外像素增加（`horizontal × vertical` 缩放）。
- 当运行时的最大交换链尺寸没有余量时，缩放会自动减小（或过扫描被禁用），
  因此头显永远不会降级。
- 过扫描处于活动状态时会抑制隐藏区域遮罩，以免应用程序
  把额外的外围区域模板化剔除；这会带来少量 GPU 开销。
- 忽略 `xrLocateViews` FOV 或推荐渲染分辨率的应用程序
  会自动回退到正常行为（它们提交的内容会原样通过）。
- 由投影烘焙的全屏模糊、色调、暗角或淡入淡出可能只覆盖
  头显原生 FOV，从而在新增的录制外围区域露出硬边。
  对于以这种方式渲染这些效果的作品，请减小或禁用过扫描。

## 相机平滑（实验性）

原始 VR 画面带有头部的每一次微动。相机平滑会在镜像中运行一台
低通滤波的虚拟相机，并从该相机对每一帧进行重投影，
使用一个小的 tan 空间裁切作为吸收抖动的平移边距。
头显完全不受影响——平滑只存在于 OBS 图像中。

这两个控制都位于 OBS 源上并且实时生效，无需重启。控制
中心也可以全局管理它们；随时关闭它的覆盖即可
返回到保存在各个 OBS 源上的值：

- **相机平滑**（0-100）：滤波强度，从关闭到非常飘忽
  （约 40 毫秒到 800 毫秒的时间常数）。建议从 30-50 左右开始。
- **平滑裁切比例**（0-25，默认 8）：平滑器可以在图像边缘
  内平移的范围。裁切越多，在相机不得不跟上之前能实现的平滑就越强；
  输出会相应地上采样。

说明：

- 平滑相机会被钳制，使裁切窗口永远不会超出已渲染的
  图像——快速运动会退化为跟随头部，而不是显示黑色
  边缘。急转和瞬移按设计会立即跟随。
- 与录制过扫描搭配良好：启用过扫描后，裁切边距可以
  来自过扫描外围区域，因此录制会保留完整的头显
  视场。
- 位置平滑使用位于 2 m 的平面重投影平面；在强烈的
  位置运动期间，非常近的几何体可能会出现轻微闪烁。
- 开销是在镜像设备上每眼一次带纹理的四边形绘制——可以忽略不计。

## OpenXR 四边形层 UI

控制中心的 **UI 层**（UI layers）页面控制单独提交的
OpenXR 四边形层是否出现在 OBS 镜像中。**在录制中显示**（Show in recording）保留
默认的合成结果。**从录制中隐藏**（Hide from recording）录制不含
`XR_COMPOSITION_LAYER_QUAD` 内容的投影图像。层会实时轮询该偏好，
并且头显提交的内容永远不会被修改，因此头显会继续显示其
所有原始层。

此过滤器只能分离作为真正的 OpenXR 合成四边形
层提交的 UI。绘制到投影眼纹理中的 UI，包括世界空间 UI 和
后处理叠加层，已经是投影图像的一部分，无法
被独立移除。安装包含此功能的构建后，
请重启 OpenXR 应用程序一次，以便它加载更新后的层；之后的
显示/隐藏更改会实时生效。

层更新以带哈希版本的二进制文件安装。这使控制
中心即使在上一个 DLL 已加载时也能暂存新构建；正在运行的
会话会保留其现有代码，而下一次 OpenXR 启动会自动
遵循更新后的清单。
