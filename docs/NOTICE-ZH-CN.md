# 关于本汉化版（请阅读）

本仓库是 OpenXR OBS Mirror 的**社区简体中文汉化版**，并非原作者发布的版本，也未获得
原作者的背书或审核。安装、使用与再分发本汉化版时，请注意以下几点。

## 声明

- 原项目：OpenXR-Layer-OBSMirror，作者 Elliott Tate
  https://github.com/elliotttate/OpenXR-Layer-OBSMirror
- 其中的 OpenXR API 层源自 OpenXR-Layer-Template（作者 Matthieu Bucchianeri）
  https://github.com/mbucchia/OpenXR-Layer-Template
- 本项目按 MIT 许可分发，版权声明与许可全文见仓库根目录的 `LICENSE`。
  MIT 允许修改与再分发，条件是保留版权声明与许可声明。
- 汉化部分（中文语言资源与中文文档）由 idanfang 提供。
- 本汉化版**完整保留英文原文**：界面语言默认跟随系统，也可用环境变量
  `OBSMIRROR_LANG` 强制指定（`zh-CN` 或 `en-US`）；任何中文资源缺失都会回退英文。

## 关于 OBS 插件与 GPL

OBS Studio 本体按 GNU GPL v2 或更高版本授权。本仓库中的 OBS 插件
（`OBSPlugin/win-openxr`）会包含 OBS Studio 的头文件并链接其 libobs，因此其
**编译后的二进制按 GPL-2.0-or-later 分发**。该插件自身的源码在本仓库内；
它需要配合 OBS Studio 源码树才能编译（接口头文件与 libobs 由后者提供）。

OBS Studio 源码：https://github.com/obsproject/obs-studio

## 安装程序的中文界面

安装向导的简体中文消息来自 Inno Setup 的社区翻译文件
（`installer/languages/ChineseSimplified.isl`，来源 https://jrsoftware.org/files/istrans/ ，
维护者 Zhenghan Yang (Kira)），文件内容未作改动，仅保存为 UTF-8 with BOM。

## 免责

本汉化版按"现状"提供，不附带任何形式的担保。若遇到问题，建议先在上游项目确认是否为
原版既有问题，再反馈给本汉化分支。

本汉化版沿用上游的安装标识（AppId），因此会与上游版本视作同一软件并就地升级/覆盖。
