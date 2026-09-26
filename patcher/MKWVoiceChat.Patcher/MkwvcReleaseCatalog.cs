using System.Diagnostics;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MKWVoiceChat.Patcher;

internal sealed class MkwvcReleaseManifest
{
    [JsonPropertyName("schemaVersion")]
    public int SchemaVersion { get; set; }

    [JsonPropertyName("version")]
    public string Version { get; set; } = "";

    [JsonPropertyName("patchRevision")]
    public string PatchRevision { get; set; } = "";

    [JsonPropertyName("minimumProtocol")]
    public int MinimumProtocol { get; set; }

    [JsonPropertyName("wiiCompiledVersion")]
    public string WiiCompiledVersion { get; set; } = "";

    [JsonPropertyName("installerUrl")]
    public string InstallerUrl { get; set; } = "";

    [JsonPropertyName("installerSha256")]
    public string InstallerSha256 { get; set; } = "";

    [JsonPropertyName("releasePageUrl")]
    public string ReleasePageUrl { get; set; } = "";
}

internal sealed record MkwvcReleaseStatus(
    Version CurrentVersion,
    Version LatestVersion,
    Version RequiredWiiCompiledVersion,
    MkwvcReleaseManifest Manifest)
{
    public bool IntegrationMismatch =>
        LatestVersion == CurrentVersion &&
        (!string.Equals(
             Manifest.PatchRevision,
             BuildVersion.PatchRevision,
             StringComparison.Ordinal) ||
         RequiredWiiCompiledVersion != BuildVersion.WiiCompiledVersion);

    public bool ProtocolUpdateRequired =>
        LatestVersion >= CurrentVersion &&
        Manifest.MinimumProtocol > BuildVersion.ProtocolVersion;

    public bool UpdateAvailable =>
        LatestVersion > CurrentVersion ||
        IntegrationMismatch ||
        ProtocolUpdateRequired;
}

internal static class MkwvcReleaseCatalog
{
    public const string ManifestUrl =
        "https://github.com/waxedhyphen/MKW-VoiceChat/releases/latest/download/mkwvc-release.json";

    public static int CurrentProtocol =>
        BuildVersion.ProtocolVersion;

    public static Version CurrentVersion =>
        BuildVersion.ProductVersion;

    public static Version CurrentWiiCompiledVersion =>
        BuildVersion.WiiCompiledVersion;

    public static async Task<MkwvcReleaseStatus> CheckAsync(
        HttpClient http,
        CancellationToken cancellationToken = default)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, ManifestUrl);
        request.Headers.CacheControl =
            new CacheControlHeaderValue { NoCache = true };

        using var response = await http.SendAsync(
            request,
            HttpCompletionOption.ResponseContentRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();

        var json = await response.Content.ReadAsStringAsync(cancellationToken);
        var manifest = JsonSerializer.Deserialize<MkwvcReleaseManifest>(
            json,
            JsonUtil.Options)
            ?? throw new InvalidDataException(
                "MKW Voice Chat release manifest is empty.");

        Validate(manifest);

        if (!UpstreamCatalog.TryVersion(manifest.Version, out var latest))
            throw new InvalidDataException(
                "MKW Voice Chat release manifest contains an invalid version.");
        if (!UpstreamCatalog.TryVersion(
                manifest.WiiCompiledVersion,
                out var requiredWiiCompiled))
        {
            throw new InvalidDataException(
                "MKW Voice Chat release manifest contains an invalid WiiCompiled version.");
        }

        return new(
            CurrentVersion,
            latest,
            requiredWiiCompiled,
            manifest);
    }

    public static async Task<string> DownloadInstallerAsync(
        HttpClient http,
        MkwvcReleaseManifest manifest,
        CancellationToken cancellationToken = default)
    {
        Validate(manifest);

        if (string.IsNullOrWhiteSpace(manifest.InstallerSha256))
        {
            throw new InvalidDataException(
                "The advertised MKW Voice Chat update has no installer SHA-256. " +
                "The release is incomplete and will not be installed.");
        }

        var root = Path.Combine(
            Path.GetTempPath(),
            "MKWVoiceChat",
            "updates",
            manifest.Version,
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        // Keep the canonical filename in the unique temporary directory so
        // update handoffs and diagnostics stay predictable.
        var destination = Path.Combine(
            root,
            "WiiCompiled-VoiceChat-Installer.exe");

        using var response = await http.GetAsync(
            manifest.InstallerUrl,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();

        await using (var input =
            await response.Content.ReadAsStreamAsync(cancellationToken))
        await using (var output = new FileStream(
            destination,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            1024 * 128,
            useAsync: true))
        {
            await input.CopyToAsync(output, cancellationToken);
        }

        var info = new FileInfo(destination);
        if (info.Length < 1024 * 1024)
        {
            File.Delete(destination);
            throw new InvalidDataException(
                "Downloaded MKW Voice Chat installer is unexpectedly small.");
        }

        if (!string.IsNullOrWhiteSpace(manifest.InstallerSha256))
        {
            var actual = PatchStateStore.Sha256File(destination);
            if (!string.Equals(
                    actual,
                    manifest.InstallerSha256.Trim(),
                    StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(destination);
                throw new InvalidDataException(
                    "Downloaded MKW Voice Chat installer failed SHA-256 verification.");
            }
        }

        CleanupPreviousUpdateDownloads(destination);
        return destination;
    }

    private static void CleanupPreviousUpdateDownloads(string keepInstallerPath)
    {
        try
        {
            var updateRoot = Path.Combine(
                Path.GetTempPath(),
                "MKWVoiceChat",
                "updates");
            var keepDirectory = Path.GetDirectoryName(
                Path.GetFullPath(keepInstallerPath));
            if (string.IsNullOrWhiteSpace(keepDirectory) ||
                !Directory.Exists(updateRoot))
            {
                return;
            }

            foreach (var versionDirectory in Directory.EnumerateDirectories(updateRoot))
            {
                var fullVersionDirectory = Path.GetFullPath(versionDirectory);
                var versionPrefix = fullVersionDirectory.TrimEnd(
                        Path.DirectorySeparatorChar,
                        Path.AltDirectorySeparatorChar)
                    + Path.DirectorySeparatorChar;

                if (!keepDirectory.StartsWith(
                        versionPrefix,
                        StringComparison.OrdinalIgnoreCase))
                {
                    TryDeleteDirectory(versionDirectory);
                    continue;
                }

                foreach (var downloadDirectory in Directory.EnumerateDirectories(versionDirectory))
                {
                    if (Path.GetFullPath(downloadDirectory).Equals(
                            keepDirectory,
                            StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    TryDeleteDirectory(downloadDirectory);
                }
            }
        }
        catch
        {
            // Temp cleanup must never block an otherwise valid update.
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
                Directory.Delete(path, recursive: true);
        }
        catch
        {
            // A file can still be locked briefly by an older process.
            // The next successful update will try again.
        }
    }

    public static int LaunchInstaller(
        string installerPath,
        string command,
        int waitPid = 0)
    {
        var arguments = command;
        if (waitPid > 0)
            arguments += $" --wait-pid {waitPid}";

        var start = new ProcessStartInfo
        {
            FileName = installerPath,
            Arguments = arguments,
            WorkingDirectory =
                Path.GetDirectoryName(installerPath) ?? Environment.CurrentDirectory,
            UseShellExecute = true
        };

        _ = Process.Start(start)
            ?? throw new InvalidOperationException(
                "Could not start the MKW Voice Chat installer.");
        return 0;
    }

    public static void WaitForProcessExit(int pid)
    {
        if (pid <= 0 || pid == Environment.ProcessId)
            return;

        try
        {
            using var process = Process.GetProcessById(pid);
            if (process.HasExited)
                return;

            ConsoleUi.BeginPhase(
                $"Waiting for process {pid} to close...");
            process.WaitForExit();
        }
        catch (ArgumentException)
        {
            // Process already exited.
        }
    }

    public static bool RelaunchUnpatchOutsideInstallRoot(PatcherLayout layout)
    {
        var source = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(source) || !File.Exists(source))
            return false;

        var installRoot = Path.GetFullPath(PatchStateStore.Root(layout))
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        var sourceFull = Path.GetFullPath(source);

        if (!sourceFull.StartsWith(
                installRoot,
                StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var tempRoot = Path.Combine(
            Path.GetTempPath(),
            "MKWVoiceChat",
            "unpatch",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tempRoot);

        var tempInstaller = Path.Combine(
            tempRoot,
            "WiiCompiled-VoiceChat-Installer.exe");
        File.Copy(sourceFull, tempInstaller, overwrite: false);

        LaunchInstaller(
            tempInstaller,
            "unpatch",
            Environment.ProcessId);
        return true;
    }

    private static void Validate(MkwvcReleaseManifest manifest)
    {
        if (manifest.SchemaVersion != 1)
            throw new InvalidDataException(
                $"Unsupported MKW Voice Chat release manifest schema {manifest.SchemaVersion}.");

        if (string.IsNullOrWhiteSpace(manifest.Version) ||
            string.IsNullOrWhiteSpace(manifest.PatchRevision) ||
            string.IsNullOrWhiteSpace(manifest.WiiCompiledVersion))
        {
            throw new InvalidDataException(
                "MKW Voice Chat release manifest is incomplete.");
        }

        if (manifest.MinimumProtocol < 1)
            throw new InvalidDataException(
                "MKW Voice Chat release manifest contains an invalid protocol requirement.");

        if (!Uri.TryCreate(
                manifest.InstallerUrl,
                UriKind.Absolute,
                out var installerUri) ||
            installerUri.Scheme != Uri.UriSchemeHttps ||
            !string.Equals(
                installerUri.Host,
                "github.com",
                StringComparison.OrdinalIgnoreCase) ||
            !installerUri.AbsolutePath.StartsWith(
                "/waxedhyphen/MKW-VoiceChat/releases/",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "MKW Voice Chat release manifest contains an invalid installer URL.");
        }

        if (!string.IsNullOrWhiteSpace(manifest.InstallerSha256) &&
            (manifest.InstallerSha256.Length != 64 ||
             manifest.InstallerSha256.Any(ch =>
                 !Uri.IsHexDigit(ch))))
        {
            throw new InvalidDataException(
                "MKW Voice Chat release manifest contains an invalid installer SHA-256.");
        }
    }
}
