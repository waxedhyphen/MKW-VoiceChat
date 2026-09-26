using System.Text.Json;
using Microsoft.Win32;

namespace MKWVoiceChat.Patcher;

internal sealed record PatcherLayout(
    string WheelWizardRoot,
    string ConfigPath,
    string RecompRoot,
    string InstallRoot,
    string CacheRoot,
    string SetupPath,
    string InstallStatePath);

internal sealed class WiiCompiledInstallState
{
    public int SchemaVersion { get; set; }
    public string? SetupVersion { get; set; }
    public bool RetroRewindInstalled { get; set; }
    public string? RetroRewindRoot { get; set; }
    public string? RetroWfcPayloadMode { get; set; }
}

internal static class PatcherPaths
{
    private const string WheelWizardRegistryPath = @"Software\WheelWizard";
    private const string WheelWizardRegistryValue = "AppDataLocation";

    public static PatcherLayout Discover()
    {
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        if (string.IsNullOrWhiteSpace(roaming))
            throw new InvalidOperationException("Windows roaming AppData could not be resolved.");

        string? configuredRoot = null;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(WheelWizardRegistryPath, writable: false);
            configuredRoot = key?.GetValue(WheelWizardRegistryValue) as string;
        }
        catch
        {
            // Wheel Wizard uses the default root when its optional registry override
            // is absent or unreadable. Mirror that behavior exactly.
        }

        var wheelWizardRoot = string.IsNullOrWhiteSpace(configuredRoot)
            ? Path.Combine(roaming, "CT-MKWII")
            : Path.GetFullPath(configuredRoot);

        var recompRoot = Path.Combine(wheelWizardRoot, "Recomp");
        var installRoot = Path.Combine(recompRoot, "Install");
        return new(
            wheelWizardRoot,
            Path.Combine(wheelWizardRoot, "config.json"),
            recompRoot,
            installRoot,
            Path.Combine(recompRoot, "Cache"),
            Path.Combine(installRoot, "WiiCompiled-Setup.exe"),
            Path.Combine(installRoot, "install-state.json"));
    }

    public static WiiCompiledInstallState? ReadInstallState(PatcherLayout layout)
    {
        if (!File.Exists(layout.InstallStatePath))
            return null;

        try
        {
            return JsonSerializer.Deserialize<WiiCompiledInstallState>(
                File.ReadAllText(layout.InstallStatePath),
                JsonUtil.Options);
        }
        catch
        {
            return null;
        }
    }

    public static string ResolveRetroRoot(
        PatcherLayout layout,
        WiiCompiledInstallState? state = null)
    {
        if (!string.IsNullOrWhiteSpace(state?.RetroRewindRoot))
        {
            var recorded = NormalizeRetroRoot(state!.RetroRewindRoot!);
            if (LooksLikeRetroRewind(recorded) ||
                HasInterruptedRetroTransaction(recorded))
            {
                return recorded;
            }
            // The recorded path may be stale after the user moved Dolphin/RR.
            // Fall through to Wheel Wizard's live configuration rather than
            // recreating Retro Rewind at an obsolete location.
        }

        var candidates = new List<string>();

        var userFolder = ReadWheelWizardString(layout, "UserFolderPath");
        if (!string.IsNullOrWhiteSpace(userFolder))
        {
            var normalizedUser = Path.GetFullPath(userFolder);
            var loadPath = ReadDolphinLoadPath(normalizedUser);
            if (string.IsNullOrWhiteSpace(loadPath))
                loadPath = Path.Combine(normalizedUser, "Load");

            candidates.Add(Path.Combine(
                loadPath,
                "Riivolution",
                "WheelWizard",
                "RetroRewind6"));
        }

        // Wheel Wizard's recomp-only fallback when no Dolphin user folder is configured.
        candidates.Add(Path.Combine(
            layout.WheelWizardRoot,
            "RetroRewind",
            "RetroRewind6"));

        // Common Windows Dolphin default. This is only a fallback; Wheel Wizard's
        // configured user folder/load path above always wins when present.
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        if (!string.IsNullOrWhiteSpace(roaming))
        {
            candidates.Add(Path.Combine(
                roaming,
                "Dolphin Emulator",
                "Load",
                "Riivolution",
                "WheelWizard",
                "RetroRewind6"));
        }

        foreach (var candidate in candidates
                     .Select(NormalizeRetroRoot)
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (LooksLikeRetroRewind(candidate) ||
                HasInterruptedRetroTransaction(candidate))
                return candidate;
        }

        throw new DirectoryNotFoundException(
            "Retro Rewind could not be found from Wheel Wizard's configuration. " +
            "Install/launch Retro Rewind through Wheel Wizard once before running MKW Voice Chat.");
    }

    private static string NormalizeRetroRoot(string root)
    {
        var normalized = Path.GetFullPath(root)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        if (!string.Equals(
                Path.GetFileName(normalized),
                "RetroRewind6",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"Retro Rewind path is not a RetroRewind6 directory: {normalized}");
        }

        return normalized;
    }

    private static bool LooksLikeRetroRewind(string root) =>
        Directory.Exists(root) &&
        File.Exists(Path.Combine(root, "version.txt")) &&
        File.Exists(Path.Combine(root, "Binaries", "Code.pul"));

    private static bool HasInterruptedRetroTransaction(string retroRoot)
    {
        var parent = Directory.GetParent(retroRoot)?.FullName;
        return !string.IsNullOrWhiteSpace(parent) &&
               Directory.Exists(Path.Combine(parent, ".mkwvc-rr-transaction"));
    }

    private static string? ReadDolphinLoadPath(string userFolder)
    {
        var ini = Path.Combine(userFolder, "Config", "Dolphin.ini");
        if (!File.Exists(ini))
            return null;

        var inGeneral = false;
        foreach (var raw in File.ReadLines(ini))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('#'))
                continue;

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                inGeneral = line.Equals(
                    "[General]",
                    StringComparison.OrdinalIgnoreCase);
                continue;
            }

            if (!inGeneral)
                continue;

            var equals = line.IndexOf('=');
            if (equals <= 0)
                continue;

            var key = line[..equals].Trim();
            if (!key.Equals("LoadPath", StringComparison.OrdinalIgnoreCase))
                continue;

            var value = line[(equals + 1)..].Trim().Trim('"');
            if (string.IsNullOrWhiteSpace(value))
                return null;

            try
            {
                var full = Path.GetFullPath(value);
                return Directory.Exists(full) ? full : null;
            }
            catch
            {
                return null;
            }
        }

        return null;
    }

    public static string? ReadWheelWizardString(
        PatcherLayout layout,
        string key)
    {
        if (!File.Exists(layout.ConfigPath))
            return null;

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(layout.ConfigPath));
            if (!document.RootElement.TryGetProperty(key, out var value) ||
                value.ValueKind != JsonValueKind.String)
                return null;

            var text = value.GetString();
            return string.IsNullOrWhiteSpace(text) ? null : text.Trim();
        }
        catch
        {
            return null;
        }
    }

    public static string ReadWheelWizardGameLocation(PatcherLayout layout)
    {
        var path = ReadWheelWizardString(layout, "GameLocation");
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidDataException(
                "Wheel Wizard config.json does not contain a GameLocation setting.");

        if (!File.Exists(path))
            throw new FileNotFoundException(
                "Wheel Wizard's configured Mario Kart Wii image does not exist.", path);

        return Path.GetFullPath(path);
    }
}

internal static class JsonUtil
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true
    };
}
