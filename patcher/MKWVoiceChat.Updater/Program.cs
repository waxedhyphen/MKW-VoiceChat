using System.Diagnostics;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;

namespace MKWVoiceChat.Updater;

internal sealed record ReleaseManifest(
    string Version,
    string InstallerUrl,
    string InstallerSha256);

internal static class Program
{
    private const string ManifestUrl =
        "https://github.com/zurasaaa/MKW-VoiceChat/releases/latest/download/mkwvc-release.json";
    private const long MinimumInstallerBytes = 1024 * 1024;
    private const long MaximumInstallerBytes = 512L * 1024 * 1024;

    public static async Task<int> Main(string[] args)
    {
        UpdaterLog.Start(args);
        try
        {
            if (args.Any(value =>
                    string.Equals(value, "--selftest", StringComparison.OrdinalIgnoreCase)))
            {
                RunSelfTest();
                Console.WriteLine("Updater self-test OK.");
                return 0;
            }

            var waitPid = ParseWaitPid(args);
            if (waitPid > 0)
            {
                UpdaterLog.Write("WAIT", $"Waiting for game process {waitPid} to close.");
                Console.WriteLine($"Waiting for game process {waitPid} to close...");
                WaitForProcessExit(waitPid);
            }

            using var http = new HttpClient
            {
                Timeout = TimeSpan.FromMinutes(10)
            };
            http.DefaultRequestHeaders.UserAgent.ParseAdd(
                "MKWVoiceChat-Updater/1.0");
            http.DefaultRequestHeaders.Accept.Add(
                new MediaTypeWithQualityHeaderValue("application/json"));

            UpdaterLog.Write("CHECK", "Checking latest MKW VoiceChat release.");
            Console.WriteLine("Checking latest MKW VoiceChat release...");
            var manifest = await FetchManifestAsync(http);
            UpdaterLog.Write("CHECK", $"Latest version={manifest.Version}");

            UpdaterLog.Write("DOWNLOAD", $"Downloading MKW VoiceChat {manifest.Version}.");
            Console.WriteLine(
                $"Downloading MKW VoiceChat {manifest.Version}...");
            var installer = await DownloadInstallerAsync(http, manifest);
            UpdaterLog.Write("DOWNLOAD", $"Verified installer={installer}");
            CleanupPreviousUpdateDownloads(installer);

            UpdaterLog.Write("HANDOFF", "Starting MKW VoiceChat installer.");
            Console.WriteLine("Starting MKW VoiceChat installer...");
            var start = new ProcessStartInfo
            {
                FileName = installer,
                WorkingDirectory =
                    Path.GetDirectoryName(installer)
                    ?? Environment.CurrentDirectory,
                UseShellExecute = true
            };
            start.ArgumentList.Add("update");

            _ = Process.Start(start)
                ?? throw new InvalidOperationException(
                    "Could not start the MKW VoiceChat installer.");

            return 0;
        }
        catch (Exception ex)
        {
            UpdaterLog.Exception("UPDATER", ex);
            var logSuffix = string.IsNullOrWhiteSpace(UpdaterLog.CurrentPath)
                ? ""
                : "\n\nLog: " + UpdaterLog.CurrentPath;
            var message =
                "MKW VoiceChat updater failed.\n\n" + ex.Message + logSuffix;
            Console.Error.WriteLine(message);
            ShowError(message);
            return 1;
        }
    }

    private static int ParseWaitPid(string[] args)
    {
        for (var i = 0; i < args.Length; ++i)
        {
            if (!string.Equals(
                    args[i],
                    "--wait-pid",
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (i + 1 >= args.Length ||
                !int.TryParse(args[i + 1], out var pid) ||
                pid <= 0)
            {
                throw new ArgumentException(
                    "--wait-pid requires a positive process ID.");
            }

            return pid;
        }

        return 0;
    }

    private static void WaitForProcessExit(int pid)
    {
        if (pid <= 0 || pid == Environment.ProcessId)
            return;

        try
        {
            using var process = Process.GetProcessById(pid);
            if (!process.HasExited)
                process.WaitForExit();
        }
        catch (ArgumentException)
        {
        }
    }

    private static async Task<ReleaseManifest> FetchManifestAsync(
        HttpClient http)
    {
        using var request = new HttpRequestMessage(
            HttpMethod.Get,
            ManifestUrl);
        request.Headers.CacheControl =
            new CacheControlHeaderValue { NoCache = true };

        using var timeout = new CancellationTokenSource(
            TimeSpan.FromSeconds(30));
        using var response = await http.SendAsync(
            request,
            HttpCompletionOption.ResponseContentRead,
            timeout.Token);
        response.EnsureSuccessStatusCode();

        var json = await response.Content.ReadAsStringAsync(
            timeout.Token);
        return ParseManifest(json);
    }

    private static ReleaseManifest ParseManifest(string json)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;

        if (!root.TryGetProperty("schemaVersion", out var schemaValue) ||
            schemaValue.ValueKind != JsonValueKind.Number ||
            schemaValue.GetInt32() != 1)
        {
            throw new InvalidDataException(
                "Release manifest schema is unsupported.");
        }

        var version = RequiredString(root, "version");
        var installerUrl = RequiredString(root, "installerUrl");
        var installerSha256 =
            RequiredString(root, "installerSha256").Trim();

        if (!IsVersionTriplet(version))
            throw new InvalidDataException(
                "Release manifest version is invalid.");

        ValidateInstallerUrl(installerUrl);

        if (installerSha256.Length != 64 ||
            installerSha256.Any(ch =>
                !Uri.IsHexDigit(ch)))
        {
            throw new InvalidDataException(
                "Release manifest installer SHA-256 is invalid.");
        }

        return new(version, installerUrl, installerSha256);
    }

    private static string RequiredString(
        JsonElement root,
        string name)
    {
        if (!root.TryGetProperty(name, out var value) ||
            value.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException(
                $"Release manifest field '{name}' is missing.");
        }

        var result = value.GetString();
        if (string.IsNullOrWhiteSpace(result))
        {
            throw new InvalidDataException(
                $"Release manifest field '{name}' is empty.");
        }

        return result;
    }

    private static bool IsVersionTriplet(string value)
    {
        var parts = value.Split('.');
        return parts.Length == 3 &&
               parts.All(part =>
                   part.Length > 0 &&
                   int.TryParse(part, out var number) &&
                   number >= 0);
    }

    private static void ValidateInstallerUrl(string value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) ||
            !string.Equals(
                uri.Scheme,
                Uri.UriSchemeHttps,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                uri.Host,
                "github.com",
                StringComparison.OrdinalIgnoreCase) ||
            !uri.AbsolutePath.StartsWith(
                "/zurasaaa/MKW-VoiceChat/releases/",
                StringComparison.OrdinalIgnoreCase) ||
            !uri.AbsolutePath.EndsWith(
                "/WiiCompiled-VoiceChat-Installer.exe",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Release manifest installer URL is not trusted.");
        }
    }

    private static async Task<string> DownloadInstallerAsync(
        HttpClient http,
        ReleaseManifest manifest)
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "MKWVoiceChat",
            "updates",
            manifest.Version,
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);

        var destination = Path.Combine(
            root,
            "WiiCompiled-VoiceChat-Installer.exe");

        using var response = await http.GetAsync(
            manifest.InstallerUrl,
            HttpCompletionOption.ResponseHeadersRead);
        response.EnsureSuccessStatusCode();

        if (response.Content.Headers.ContentLength is long advertised &&
            (advertised < MinimumInstallerBytes ||
             advertised > MaximumInstallerBytes))
        {
            throw new InvalidDataException(
                "Downloaded installer size is outside the allowed range.");
        }

        await using var input =
            await response.Content.ReadAsStreamAsync();
        await using var output = new FileStream(
            destination,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            128 * 1024,
            useAsync: true);

        var buffer = new byte[128 * 1024];
        long total = 0;
        while (true)
        {
            var read = await input.ReadAsync(buffer);
            if (read == 0)
                break;

            total += read;
            if (total > MaximumInstallerBytes)
                throw new InvalidDataException(
                    "Downloaded installer exceeds the allowed size.");

            await output.WriteAsync(buffer.AsMemory(0, read));
        }

        await output.FlushAsync();

        if (total < MinimumInstallerBytes)
            throw new InvalidDataException(
                "Downloaded installer is unexpectedly small.");

        output.Close();

        var actual = Sha256File(destination);
        if (!string.Equals(
                actual,
                manifest.InstallerSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(destination);
            throw new InvalidDataException(
                "Downloaded installer failed SHA-256 verification.");
        }

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

    private static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(
                SHA256.HashData(stream))
            .ToLowerInvariant();
    }

    private static void RunSelfTest()
    {
        const string good =
            """
            {
              "schemaVersion": 1,
              "version": "1.2.3",
              "installerUrl": "https://github.com/zurasaaa/MKW-VoiceChat/releases/latest/download/WiiCompiled-VoiceChat-Installer.exe",
              "installerSha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            }
            """;

        var parsed = ParseManifest(good);
        if (parsed.Version != "1.2.3")
            throw new Exception(
                "Manifest version self-test failed.");

        const string bad =
            """
            {
              "schemaVersion": 1,
              "version": "1.2.3",
              "installerUrl": "https://example.com/WiiCompiled-VoiceChat-Installer.exe",
              "installerSha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            }
            """;

        try
        {
            _ = ParseManifest(bad);
            throw new Exception(
                "Untrusted updater URL was accepted.");
        }
        catch (InvalidDataException)
        {
        }
    }

    private static void ShowError(string message)
    {
        try
        {
            _ = MessageBoxW(
                IntPtr.Zero,
                message,
                "MKW VoiceChat Updater",
                0x00000010u | 0x00000000u);
        }
        catch
        {
        }
    }

    [DllImport(
        "user32.dll",
        CharSet = CharSet.Unicode,
        SetLastError = false)]
    private static extern int MessageBoxW(
        IntPtr hWnd,
        string text,
        string caption,
        uint type);
}
