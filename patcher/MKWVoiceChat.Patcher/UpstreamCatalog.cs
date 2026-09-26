using System.Net.Http.Headers;
using System.Text.Json.Serialization;

namespace MKWVoiceChat.Patcher;

internal sealed record WiiCompiledRelease(
    string TagName,
    Version Version,
    string SetupDownloadUrl);

internal sealed record RetroUpdate(
    Version Version,
    string Url,
    string Description);

internal sealed record RetroDeletion(
    Version Version,
    string Path);

internal sealed record RetroCatalog(
    Version LatestVersion,
    IReadOnlyList<RetroUpdate> Updates,
    IReadOnlyList<RetroDeletion> Deletions,
    string InstallUrl);

internal static class UpstreamCatalog
{
    private const string WiiCompiledReleasesApi =
        "https://api.github.com/repos/patchzyy/Wiicompiled/releases?per_page=100";
    private const string RetroBase = "https://update.rwfc.net/";
    private const string OldRetroBase = "http://update.rwfc.net:8000/";
    private const string RetroVersionFeed =
        RetroBase + "RetroRewind/RetroRewindVersion.txt";
    private const string RetroDeletionFeed =
        RetroBase + "RetroRewind/RetroRewindDelete.txt";
    private const string RetroInstallFeed =
        RetroBase + "RetroRewind/RetroRewindInstall.txt";

    public static HttpClient CreateHttpClient()
    {
        var http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        http.DefaultRequestHeaders.UserAgent.Add(
            new ProductInfoHeaderValue("MKWVoiceChat-Patcher", "0.2"));
        return http;
    }

    public static async Task<WiiCompiledRelease> LatestWiiCompiledAsync(
        HttpClient http,
        CancellationToken cancellationToken = default)
    {
        using var response = await http.GetAsync(
            WiiCompiledReleasesApi,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();

        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        var releases = await System.Text.Json.JsonSerializer.DeserializeAsync<List<GitHubRelease>>(
            stream,
            JsonUtil.Options,
            cancellationToken) ?? [];

        WiiCompiledRelease? best = null;
        foreach (var release in releases)
        {
            if (release.Prerelease || !TryVersion(release.TagName, out var version))
                continue;

            var asset = release.Assets?.FirstOrDefault(candidate =>
                string.Equals(
                    candidate.Name,
                    "WiiCompiled-Setup.exe",
                    StringComparison.OrdinalIgnoreCase));
            if (asset is null || string.IsNullOrWhiteSpace(asset.BrowserDownloadUrl))
                continue;

            if (best is null || version > best.Version)
                best = new(release.TagName!, version, asset.BrowserDownloadUrl!);
        }

        return best ?? throw new InvalidDataException(
            "No usable official WiiCompiled Windows release was found.");
    }

    public static async Task<RetroCatalog> RetroRewindAsync(
        HttpClient http,
        CancellationToken cancellationToken = default)
    {
        var versionTextTask = ReadRetroTextAsync(
            http,
            RetroVersionFeed,
            cancellationToken);
        var deleteTextTask = ReadRetroTextAsync(
            http,
            RetroDeletionFeed,
            cancellationToken);
        var installTextTask = ReadRetroTextAsync(
            http,
            RetroInstallFeed,
            cancellationToken);
        await Task.WhenAll(versionTextTask, deleteTextTask, installTextTask);

        var updates = new List<RetroUpdate>();
        foreach (var raw in versionTextTask.Result.Split(
                     '\n',
                     StringSplitOptions.RemoveEmptyEntries))
        {
            var line = raw.Trim();
            var parts = line.Split(' ', 4, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 4 || !TryVersion(parts[0], out var version))
                continue;

            updates.Add(new(version, parts[1], parts[3].Trim()));
        }

        if (updates.Count == 0)
            throw new InvalidDataException("Retro Rewind update feed contained no usable versions.");

        updates.Sort((left, right) => left.Version.CompareTo(right.Version));

        var deletions = new List<RetroDeletion>();
        foreach (var raw in deleteTextTask.Result.Split(
                     '\n',
                     StringSplitOptions.RemoveEmptyEntries))
        {
            var line = raw.Trim();
            var parts = line.Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2 || !TryVersion(parts[0], out var version))
                continue;
            deletions.Add(new(version, parts[1].Trim()));
        }

        var installUrl = installTextTask.Result.Trim();
        if (!Uri.TryCreate(installUrl, UriKind.Absolute, out _))
            throw new InvalidDataException("Retro Rewind install feed returned an invalid URL.");

        return new(updates[^1].Version, updates, deletions, installUrl);
    }

    internal static async Task<HttpResponseMessage> GetWithRetroFallbackAsync(
        HttpClient http,
        string url,
        HttpCompletionOption completionOption,
        CancellationToken cancellationToken)
    {
        var candidates = new List<string> { url };
        if (TryRetroFallbackUrl(url, out var fallback) &&
            !string.Equals(fallback, url, StringComparison.OrdinalIgnoreCase))
        {
            candidates.Add(fallback);
        }

        var failures = new List<string>();
        Exception? lastException = null;

        foreach (var candidate in candidates)
        {
            for (var attempt = 1; attempt <= 2; ++attempt)
            {
                try
                {
                    var response = await http.GetAsync(
                        candidate,
                        completionOption,
                        cancellationToken);

                    if (response.IsSuccessStatusCode)
                        return response;

                    var status = (int)response.StatusCode;
                    var retryable =
                        status >= 500 ||
                        status == 408 ||
                        status == 425 ||
                        status == 429;

                    failures.Add($"{candidate} -> HTTP {status}");
                    if (!retryable)
                        return response;

                    response.Dispose();
                }
                catch (Exception ex) when (
                    !cancellationToken.IsCancellationRequested &&
                    (ex is HttpRequestException || ex is TaskCanceledException))
                {
                    lastException = ex;
                    failures.Add($"{candidate} -> {ex.Message}");
                }

                if (attempt < 2)
                    await Task.Delay(TimeSpan.FromMilliseconds(350), cancellationToken);
            }
        }

        var detail = failures.Count == 0
            ? url
            : string.Join("; ", failures.Distinct(StringComparer.OrdinalIgnoreCase));

        throw new HttpRequestException(
            "Retro Rewind update server could not be reached through its HTTPS endpoint " +
            "or direct HTTP fallback. " + detail,
            lastException);
    }

    public static string? NormalizeVersion(string? value)
    {
        if (!TryVersion(value, out var version))
            return null;
        return version.ToString();
    }

    public static bool TryVersion(string? value, out Version version)
    {
        version = new Version(0, 0);
        if (string.IsNullOrWhiteSpace(value))
            return false;

        var text = value.Trim().TrimStart('v', 'V');
        if (!Version.TryParse(text, out var parsed) || parsed is null)
            return false;

        version = parsed;
        return true;
    }

    private static async Task<string> ReadRetroTextAsync(
        HttpClient http,
        string url,
        CancellationToken cancellationToken)
    {
        using var response = await GetWithRetroFallbackAsync(
            http,
            url,
            HttpCompletionOption.ResponseContentRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();
        return await response.Content.ReadAsStringAsync(cancellationToken);
    }

    private static bool TryRetroFallbackUrl(string url, out string fallback)
    {
        fallback = "";
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) ||
            !string.Equals(uri.Host, "update.rwfc.net", StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(uri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var relative = uri.PathAndQuery.TrimStart('/');
        fallback = OldRetroBase + relative;
        return true;
    }

    private sealed class GitHubRelease
    {
        [JsonPropertyName("tag_name")]
        public string? TagName { get; set; }

        [JsonPropertyName("prerelease")]
        public bool Prerelease { get; set; }

        [JsonPropertyName("assets")]
        public List<GitHubAsset>? Assets { get; set; }
    }

    private sealed class GitHubAsset
    {
        [JsonPropertyName("name")]
        public string? Name { get; set; }

        [JsonPropertyName("browser_download_url")]
        public string? BrowserDownloadUrl { get; set; }
    }
}
