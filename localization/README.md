# 本地化工具链（ControlCenter）

`ControlCenter` 的中文（及其他语言）文本全部来自两个来源：

```
localization/control-center-strings.tsv   ← 主表：XAML 界面文案（x:Uid）
localization/fragments/*.tsv              ← 分片：C# 代码文案（Loc.S / Loc.F）
```

拆成主表 + 分片是为了让多个文件/多人可以同时添加键而不必争抢同一个文件。
两份表会被脚本合并处理。

**不要直接手改 `ControlCenter/Strings/*/Resources.resw`** —— 它们是生成产物，
会被 `New-ControlCenterResources.ps1` 覆盖。

**值里的首尾空格是有意义的**：有些文案本身就以前导空格开头（例如追加在状态行后面的
`  •  CPU fallback`），去掉就会改变英文界面的显示。因此表由
`localization/tools/TranslationTable.ps1` 手工解析，而**不用 `Import-Csv`**
（它会把每个字段的首尾空格吃掉）；生成器、校验器、XAML 注入器共用这一份实现。

## 表格列

| 列 | 含义 |
| --- | --- |
| `key` | 资源键基名。XAML 元素的键会被拼成 `键.属性`（如 `Ui_Nav_Dashboard.Text`）。 |
| `target` | 承载文本的文件：`MainWindow.xaml`、`MirrorPreviewWindow.xaml`，或某个 `.cs` 文件。 |
| `prop` | XAML 属性名（`Text`/`Content`/`Header`/`OnContent`/`OffContent`/`ToolTipService.ToolTip`），或 `code` 表示由 C# 调用 `Loc`。 |
| `english` | 英文原文。XAML/C# 里依然保留它，作为资源缺失时的回退值。 |
| `zh-CN` | 简体中文译文。留空表示暂不翻译（界面显示英文）。 |

`prop` 的两种修饰：`Text#2` 指定第 2 处匹配，`Text*` 给所有匹配处打同一个 `x:Uid`
（用于同一文案在多处出现的元素）。

## 脚本

```powershell
# 1. 合并主表与分片，生成两份 resw（覆盖生成产物）
pwsh -File localization/tools/New-ControlCenterResources.ps1

# 2. 依据主表给 XAML 元素补 x:Uid（一次性迁移工具，幂等；-Reset 可撤销本工具注入的 x:Uid）
pwsh -File localization/tools/Add-XamlUids.ps1

# 3. 一致性校验（不需要编译器）
pwsh -File localization/tools/Test-Localization.ps1
```

没用 PowerShell 7 的话，把 `pwsh` 换成 `powershell` 即可（脚本在 5.1 与 7 上都能跑）。
`localization/tools/TranslationTable.ps1` 不是可直接执行的脚本，它是上面三个脚本共用的
表格读取函数，保证解析规则只有一份。

## 两个必须记住的约定

1. **`Loc` 接收的是资源名，不是 `x:Uid`。**
   XAML 元素的资源名是 `键.属性`，所以代码里复用 XAML 文案必须写全：
   `Loc.S("Ui_Nav_Dashboard.Text", "Dashboard")`。
   只写 `Ui_Nav_Dashboard` 不会有编译错误，但查找会失败并静默回退英文；
   `Test-Localization.ps1` 会报出这种错误。
2. **一个 XAML 元素只能有一个 `x:Uid`，而且不要把一个 `x:Uid` 挂到多个元素上。**
   同一元素上多条文案（例如 InfoBar 的 `Title` 与 `Message`、ToggleSwitch 的
   `OnContent` 与 `OffContent`）必须共用同一个键基名，资源名为 `键.Title`、`键.Message`、
   `键.OnContent`、`键.OffContent`。
   反过来，**不同元素不要共用同一个键基名**：MRT 会把该键下的**所有**资源套到**每个**
   带该 Uid 的元素上；元素缺少其中某个属性时会在**运行时**抛 `XamlParseException`，
   窗口根本不会出现（**编译期不报错**，只在实际启动时才炸）。
   唯一例外：多个**同类型**元素共用一个 Uid，且该 Uid 只有一个属性——例如 6 个
   `TextBlock` 共用 `Ui_Status_Checking.Text`，这是安全的。
   `Test-Localization.ps1` 会强制检查这条规则（第 6 项）。

## 新增一种语言

1. 在主表与各分片里加一列（例如 `ja-JP`）并填写译文；
2. 在 `New-ControlCenterResources.ps1` 的 `$Languages` 里加上该语言；
3. 运行脚本 1 与 3；
4. OBS 插件另在 `OBSPlugin/win-openxr/data/locale/` 复制一份 `ja-JP.ini`，
   键必须与 `en-US.ini` 完全一致（脚本 3 会检查）。

## 需要重新编译才能生效的部分

- **OBS 插件**：语言文件随插件一起安装，改完 `.ini` 后重新构建插件并重新安装。
- **ControlCenter**：`Resources.resw` 参与 `resources.pri` 生成，必须重新构建。
- **安装程序**：`installer/OpenXR-OBSMirror.iss` 需用 Inno Setup 6 重新编译。

## 安装程序的中文语言文件

`installer/languages/ChineseSimplified.isl` 来自社区翻译项目
<https://github.com/kira-96/Inno-Setup-Chinese-Simplified-Translation>，
内容未改动，仅重新保存为 UTF-8 with BOM（该文件声明 `LanguageCodePage=936`，
Inno Setup 要求此类文件为 Unicode）。文件面向 Inno Setup 6.5+；
版本不匹配时请替换为对应版本，或删除该文件——`.iss` 里的 `#if HasChineseIsl`
会让编译器自动跳过中文，构建仍会成功。
