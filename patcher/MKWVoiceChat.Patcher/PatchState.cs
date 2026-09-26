using System.Security.Cryptography;
using System.Text.Json;

namespace MKWVoiceChat.Patcher;

internal sealed class PatchState
{
    public int SchemaVersion { get; set; } = 1;
    public string PatchRevision { get; set; } = VoicePatchApplier.PatchRevision;
    public string ProductVersion { get; set; } = "";
    public int ProtocolVersion { get; set; } = MkwvcReleaseCatalog.CurrentProtocol;
    public string PatcherVersion { get; set; } = "";
    public string WiiCompiledVersion { get; set; } = "";
    public string RetroRewindVersion { get; set; } = "";
    public string RetroRewindRoot { get; set; } = "";
    public Dictionary<string, string> PatchedSourceSha256 { get; set; } =
        new(StringComparer.OrdinalIgnoreCase);
    public string PatchedExecutableSha256 { get; set; } = "";
    public DateTimeOffset InstalledUtc { get; set; } = DateTimeOffset.UtcNow;
}

internal static class PatchStateStore
{
    public static string Root(PatcherLayout layout) =>
        Path.Combine(layout.RecompRoot, "MKWVoiceChat");

    public static string ProductDirectory(PatcherLayout layout) =>
        Path.Combine(Root(layout), "RetroRewind");

    public static string StatePath(PatcherLayout layout) =>
        Path.Combine(Root(layout), "patcher-state.json");

    public static PatchState? Read(PatcherLayout layout)
    {
        var path = StatePath(layout);
        if (!File.Exists(path))
            return null;
        try
        {
            return JsonSerializer.Deserialize<PatchState>(
                File.ReadAllText(path),
                JsonUtil.Options);
        }
        catch
        {
            return null;
        }
    }

    public static void Write(PatcherLayout layout, PatchState state)
    {
        Directory.CreateDirectory(Root(layout));
        var path = StatePath(layout);
        var temp = path + ".tmp-" + Guid.NewGuid().ToString("N");
        File.WriteAllText(
            temp,
            JsonSerializer.Serialize(
                state,
                new JsonSerializerOptions(JsonUtil.Options)
                {
                    WriteIndented = true
                }));
        File.Move(temp, path, overwrite: true);
    }

    public static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }
}
