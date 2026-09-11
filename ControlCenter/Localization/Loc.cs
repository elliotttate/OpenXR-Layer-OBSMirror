using System.Collections.Concurrent;
using System.Globalization;
using Microsoft.Windows.ApplicationModel.Resources;

namespace OBSMirror.ControlCenter.Localization;

/// <summary>
/// 本地化入口。
///
/// 设计约定（见 docs/LOCALIZATION.md）：
/// 1. 英文原文继续保留在 XAML 与 C# 代码中，同时写入 Strings/en-US/Resources.resw；
/// 2. 其他语言（如 Strings/zh-CN/Resources.resw）只提供覆盖值；
/// 3. 任何资源查找失败都静默回退到调用点传入的英文原文，因此缺少语言包绝不会产生空白界面；
/// 4. 语言默认跟随系统，可用环境变量 OBSMIRROR_LANG（例如 zh-CN / en-US）强制指定。
///
/// 未打包 WinUI 3 应用的关键点：<see cref="ResourceLoader"/> 使用"中性默认上下文"，
/// 在没有包语言列表时永远解析到 pri 的默认限定符（en-US），因此代码查找必须自己带上
/// 显式设置了 Language 的 <see cref="ResourceContext"/>。<see cref="ResourceManager"/> 在
/// 本版本 WindowsAppSDK 中也没有 DefaultContext 属性，所以这里直接持有 ResourceMap + Context。
/// </summary>
internal static class Loc
{
    private const string LanguageOverrideVariable = "OBSMIRROR_LANG";
    private const string ResourcesPrefix = "Resources/";

    // S/F are called from the UI thread and from the background preview loop,
    // so the cache must tolerate concurrent reads while entries are filled in.
    private static readonly ConcurrentDictionary<string, string> Cache = new(StringComparer.Ordinal);

    private static ResourceManager? _manager;
    private static ResourceMap? _map;
    private static ResourceContext? _context;

    /// <summary>当前生效的资源语言标记，仅用于日志诊断。</summary>
    public static string CurrentLanguage { get; private set; } = "en-US";

    /// <summary>语言覆盖是否来自环境变量。</summary>
    public static bool IsOverridden { get; private set; }

    /// <summary>资源索引的可用性诊断，仅用于日志。</summary>
    public static string ResourceDiagnostic { get; private set; } = "not attempted";

    /// <summary>语言应用过程的诊断信息，仅用于日志。</summary>
    public static string LanguageDiagnostic { get; private set; } = "not attempted";

    /// <summary>
    /// 在创建任何窗口之前调用。设定语言并准备好资源索引。
    /// </summary>
    public static void Initialize()
    {
        var requested = Environment.GetEnvironmentVariable(LanguageOverrideVariable);
        IsOverridden = !string.IsNullOrWhiteSpace(requested);
        var language = IsOverridden ? requested!.Trim() : ResolveSystemLanguage();

        ApplyLanguage(language);
        Cache.Clear();

        try
        {
            _manager = new ResourceManager();
            _map = _manager.MainResourceMap;
            _context = _manager.CreateResourceContext();
            _context.QualifierValues["Language"] = language;
            ResourceDiagnostic = "ok";
        }
        catch (Exception exception)
        {
            _manager = null;
            _map = null;
            _context = null;
            ResourceDiagnostic = $"{exception.GetType().Name}: {exception.Message}";
        }

        CurrentLanguage = language;
    }

    /// <summary>按键取本地化文本；缺失或查找失败时返回 english。</summary>
    public static string S(string key, string english)
    {
        if (Cache.TryGetValue(key, out var cached))
        {
            return cached;
        }

        var resolved = english;
        try
        {
            if (_map != null && _context != null)
            {
                var candidate = _map.TryGetValue(ResourcesPrefix + key.Replace('.', '/'), _context);
                if (candidate != null && candidate.Kind == ResourceCandidateKind.String)
                {
                    var value = candidate.ValueAsString;
                    if (!string.IsNullOrEmpty(value))
                    {
                        resolved = value;
                    }
                }
            }
        }
        catch
        {
            // 资源键缺失或资源索引不可读时保持英文原文。
            resolved = english;
        }

        Cache[key] = resolved;
        return resolved;
    }

    /// <summary>按键取带占位符的本地化格式串并填充参数。</summary>
    public static string F(string key, string english, params object?[] args)
    {
        var format = S(key, english);
        try
        {
            return string.Format(CultureInfo.CurrentCulture, format, args);
        }
        catch (FormatException)
        {
            // 译文占位符写错时退回英文格式串，避免抛异常打断界面刷新。
            return string.Format(CultureInfo.CurrentCulture, english, args);
        }
    }

    /// <summary>
    /// 按英文原文填充参数，用于必须保持英文的输出（诊断日志与诊断报告）。
    /// </summary>
    public static string E(string english, params object?[] args)
    {
        return string.Format(CultureInfo.InvariantCulture, english, args);
    }

    private static string ResolveSystemLanguage()
    {
        try
        {
            var languages = Windows.Globalization.ApplicationLanguages.Languages;
            if (languages.Count > 0)
            {
                return languages[0];
            }
        }
        catch
        {
            // 退回到 .NET 的当前 UI 区域性。
        }

        return CultureInfo.CurrentUICulture.Name;
    }

    private static void ApplyLanguage(string language)
    {
        var notes = new List<string>();

        // WindowsAppSDK 版：未打包应用可用（UWP 版在未打包应用上会抛异常）。
        try
        {
            Microsoft.Windows.Globalization.ApplicationLanguages.PrimaryLanguageOverride = language;
            notes.Add("winappsdk=ok");
        }
        catch (Exception exception)
        {
            notes.Add($"winappsdk={exception.GetType().Name}");
        }

        // UWP 版：仅在打包应用上有效，失败即忽略。
        try
        {
            Windows.Globalization.ApplicationLanguages.PrimaryLanguageOverride = language;
            notes.Add("uwp=ok");
        }
        catch (Exception exception)
        {
            notes.Add($"uwp={exception.GetType().Name}");
        }

        LanguageDiagnostic = string.Join(", ", notes);
    }
}
