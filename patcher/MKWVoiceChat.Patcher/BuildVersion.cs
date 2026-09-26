using System.Text.Json;
using System.Text.Json.Serialization;

namespace MKWVoiceChat.Patcher;

internal sealed class BuildVersionDocument
{
    [JsonPropertyName("schemaVersion")]
    public int SchemaVersion { get; set; }

    [JsonPropertyName("productVersion")]
    public string ProductVersion { get; set; } = "";

    [JsonPropertyName("patchRevision")]
    public string PatchRevision { get; set; } = "";

    [JsonPropertyName("protocolVersion")]
    public int ProtocolVersion { get; set; }

    [JsonPropertyName("wiiCompiledVersion")]
    public string WiiCompiledVersion { get; set; } = "";
}

internal sealed record BuildVersionInfo(
    string ProductVersionText,
    Version ProductVersion,
    string PatchRevision,
    int ProtocolVersion,
    string WiiCompiledVersionText,
    Version WiiCompiledVersion);

internal static class BuildVersion
{
    private const string ResourceName = "MKWVoiceChat.Patcher.version.json";
    private static readonly Lazy<BuildVersionInfo> Current = new(Load);

    public static string ProductVersionText => Current.Value.ProductVersionText;
    public static Version ProductVersion => Current.Value.ProductVersion;
    public static string PatchRevision => Current.Value.PatchRevision;
    public static int ProtocolVersion => Current.Value.ProtocolVersion;
    public static string WiiCompiledVersionText => Current.Value.WiiCompiledVersionText;
    public static Version WiiCompiledVersion => Current.Value.WiiCompiledVersion;

    private static BuildVersionInfo Load()
    {
        using var stream = typeof(BuildVersion).Assembly.GetManifestResourceStream(ResourceName)
            ?? throw new InvalidDataException(
                "Embedded MKW Voice Chat version metadata is missing.");
        var document = JsonSerializer.Deserialize<BuildVersionDocument>(
            stream,
            JsonUtil.Options)
            ?? throw new InvalidDataException(
                "Embedded MKW Voice Chat version metadata is empty.");

        if (document.SchemaVersion != 1)
            throw new InvalidDataException(
                $"Unsupported MKW Voice Chat version metadata schema {document.SchemaVersion}.");

        var productVersion = ParseTriplet(
            document.ProductVersion,
            "productVersion");
        var wiiCompiledVersion = ParseTriplet(
            document.WiiCompiledVersion,
            "wiiCompiledVersion");

        if (document.ProtocolVersion < 1)
            throw new InvalidDataException(
                "MKW Voice Chat protocolVersion must be at least 1.");

        if (string.IsNullOrWhiteSpace(document.PatchRevision) ||
            document.PatchRevision.Any(ch =>
                !(char.IsLetterOrDigit(ch) ||
                  ch is '-' or '_' or '.')))
        {
            throw new InvalidDataException(
                "MKW Voice Chat patchRevision contains invalid characters.");
        }

        return new(
            document.ProductVersion,
            productVersion,
            document.PatchRevision,
            document.ProtocolVersion,
            document.WiiCompiledVersion,
            wiiCompiledVersion);
    }

    private static Version ParseTriplet(string value, string field)
    {
        var parts = value.Split('.');
        if (parts.Length != 3 ||
            parts.Any(part =>
                part.Length == 0 ||
                !int.TryParse(part, out var number) ||
                number < 0) ||
            !Version.TryParse(value, out var parsed))
        {
            throw new InvalidDataException(
                $"MKW Voice Chat {field} must be a numeric x.y.z version.");
        }

        return parsed;
    }
}
