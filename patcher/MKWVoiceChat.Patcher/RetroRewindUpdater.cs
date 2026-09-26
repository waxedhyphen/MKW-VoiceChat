using System.IO.Compression;

namespace MKWVoiceChat.Patcher;

internal static class RetroRewindUpdater
{
    private static readonly Version FullReinstallBefore = new(3, 2, 6);

    public static string? ReadInstalledVersion(string retroRoot)
    {
        var versionPath = Path.Combine(retroRoot, "version.txt");
        if (!File.Exists(versionPath))
            return null;
        return UpstreamCatalog.NormalizeVersion(File.ReadAllText(versionPath));
    }

    public static void RecoverInterruptedTransaction(string retroRoot)
    {
        var liveParent = Directory.GetParent(Path.GetFullPath(retroRoot))?.FullName
            ?? throw new InvalidOperationException("Retro Rewind has no parent directory.");
        var transactionRoot = Path.Combine(liveParent, ".mkwvc-rr-transaction");
        if (!Directory.Exists(transactionRoot))
        {
            CleanupStaleStages(liveParent);
            return;
        }

        var committedMarker = Path.Combine(transactionRoot, "committed");
        if (File.Exists(committedMarker))
        {
            // Publication had crossed its commit marker; the live directories
            // are authoritative and the transaction only contains old backups.
            TryDeleteDirectory(transactionRoot);
            CleanupStaleStages(liveParent);
            return;
        }

        ConsoleUi.WriteLine("Recovering Retro Rewind from an interrupted patcher update...");

        var originalsPath = Path.Combine(transactionRoot, "originals.json");
        if (!File.Exists(originalsPath))
            throw new InvalidDataException(
                $"Retro Rewind transaction metadata is missing: {originalsPath}");

        var originals = System.Text.Json.JsonSerializer.Deserialize<
            Dictionary<string, bool>>(File.ReadAllText(originalsPath), JsonUtil.Options)
            ?? throw new InvalidDataException(
                "Retro Rewind transaction metadata is empty.");

        foreach (var name in new[] { "RetroRewind6", "riivolution" })
        {
            if (!originals.TryGetValue(name, out var hadOriginal))
                throw new InvalidDataException(
                    $"Retro Rewind transaction metadata has no entry for {name}.");

            var backup = Path.Combine(transactionRoot, name);
            var live = Path.Combine(liveParent, name);

            if (hadOriginal)
            {
                if (Directory.Exists(backup))
                {
                    if (Directory.Exists(live))
                        Directory.Delete(live, recursive: true);
                    Directory.Move(backup, live);
                }
                // No backup means the process died before moving this original;
                // the still-live directory is already the correct rollback.
                else if (!Directory.Exists(live))
                {
                    throw new InvalidDataException(
                        $"Both live and backup copies of {name} are missing during recovery.");
                }
            }
            else if (Directory.Exists(live))
            {
                // This tree did not exist before the transaction, so any live
                // copy is a partially published new tree.
                Directory.Delete(live, recursive: true);
            }
        }

        File.Delete(originalsPath);

        if (Directory.EnumerateFileSystemEntries(transactionRoot).Any())
        {
            // Anything left here is unexpected and is kept rather than
            // silently discarded.
            throw new InvalidDataException(
                $"Retro Rewind recovery left unexpected data in {transactionRoot}.");
        }

        Directory.Delete(transactionRoot);
        CleanupStaleStages(liveParent);
    }

    public static async Task<bool> EnsureCurrentAsync(
        HttpClient http,
        string retroRoot,
        RetroCatalog catalog,
        CancellationToken cancellationToken = default)
    {
        var installedText = ReadInstalledVersion(retroRoot);
        var installedValid =
            UpstreamCatalog.TryVersion(installedText, out var installedVersion);

        // Match Wheel Wizard: a locally installed version at or beyond the
        // public feed head is not downgraded.
        if (installedValid && installedVersion >= catalog.LatestVersion)
        {
            ConsoleUi.Progress(18, "Retro Rewind is already current.");
            return false;
        }

        var liveParent = Directory.GetParent(retroRoot)?.FullName
            ?? throw new InvalidOperationException("Retro Rewind has no parent directory.");
        if (!string.Equals(
                Path.GetFileName(retroRoot),
                "RetroRewind6",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"Unexpected canonical Retro Rewind folder name: {retroRoot}");
        }

        ConsoleUi.Progress(
            18,
            $"Updating Retro Rewind {installedText ?? "unknown"} -> {catalog.LatestVersion}...");

        var stageParent = Path.Combine(
            liveParent,
            ".mkwvc-rr-stage-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(stageParent);

        try
        {
            // Preserve unrelated Wheel Wizard riivolution files in the staged
            // parent even when RR itself requires a full reinstall.
            var liveRiivolution = Path.Combine(liveParent, "riivolution");
            var stagedRiivolution = Path.Combine(stageParent, "riivolution");
            if (Directory.Exists(liveRiivolution))
                CopyDirectory(liveRiivolution, stagedRiivolution);

            Version stageVersion;
            if (installedValid &&
                installedVersion >= FullReinstallBefore &&
                Directory.Exists(retroRoot))
            {
                CopyDirectory(
                    retroRoot,
                    Path.Combine(stageParent, "RetroRewind6"));
                stageVersion = installedVersion;
            }
            else
            {
                ConsoleUi.Progress(18, "Preparing a clean Retro Rewind base...");
                await DownloadAndExtractAsync(
                    http,
                    catalog.InstallUrl,
                    stageParent,
                    cancellationToken);

                var stagedVersionText = ReadInstalledVersion(
                    Path.Combine(stageParent, "RetroRewind6"));
                if (!UpstreamCatalog.TryVersion(stagedVersionText, out stageVersion))
                    throw new InvalidDataException(
                        "Fresh Retro Rewind package has no usable version.txt.");
            }

            var targetVersion = catalog.LatestVersion;
            ApplyDeletions(
                stageParent,
                catalog.Deletions
                    .Where(item => item.Version > stageVersion &&
                                   item.Version <= targetVersion));

            foreach (var update in catalog.Updates.Where(
                         item => item.Version > stageVersion &&
                                 item.Version <= targetVersion))
            {
                ConsoleUi.Progress(
                    20,
                    $"Applying Retro Rewind {update.Version}: {update.Description}");
                await DownloadAndExtractAsync(
                    http,
                    update.Url,
                    stageParent,
                    cancellationToken);
                File.WriteAllText(
                    Path.Combine(stageParent, "RetroRewind6", "version.txt"),
                    update.Version.ToString());
            }

            ValidateStage(stageParent, targetVersion);
            CommitStage(stageParent, liveParent);
            ConsoleUi.Progress(25, "Retro Rewind is current.");
            return true;
        }
        finally
        {
            TryDeleteDirectory(stageParent);
        }
    }

    private static void ApplyDeletions(
        string stageParent,
        IEnumerable<RetroDeletion> deletions)
    {
        foreach (var deletion in deletions.OrderBy(item => item.Version))
        {
            var relative = deletion.Path.TrimStart('/', '\\');
            var target = ResolveAllowedPath(stageParent, relative);

            if (File.Exists(target))
                File.Delete(target);
            else if (Directory.Exists(target))
                Directory.Delete(target, recursive: true);
        }
    }

    private static async Task DownloadAndExtractAsync(
        HttpClient http,
        string url,
        string stageParent,
        CancellationToken cancellationToken)
    {
        var temp = Path.Combine(
            Path.GetTempPath(),
            "mkwvc-rr-" + Guid.NewGuid().ToString("N") + ".zip");

        try
        {
            await DownloadFileAsync(http, url, temp, cancellationToken);
            using var archive = ZipFile.OpenRead(temp);
            var entries = archive.Entries.ToList();
            for (var index = 0; index < entries.Count; index++)
            {
                var entry = entries[index];
                var normalized = entry.FullName.Replace('\\', '/').TrimStart('/');
                if (string.IsNullOrWhiteSpace(normalized) ||
                    normalized.EndsWith("desktop.ini", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                var destination = ResolveAllowedPath(stageParent, normalized);
                if (normalized.EndsWith('/'))
                {
                    Directory.CreateDirectory(destination);
                    continue;
                }

                var directory = Path.GetDirectoryName(destination);
                if (!string.IsNullOrWhiteSpace(directory))
                    Directory.CreateDirectory(directory);

                entry.ExtractToFile(destination, overwrite: true);

                if (entries.Count > 0)
                {
                    ConsoleUi.ProgressMeasured(
                        index + 1,
                        entries.Count,
                        "Extracting Retro Rewind update");
                }
            }
        }
        finally
        {
            TryDeleteFile(temp);
        }
    }

    private static async Task DownloadFileAsync(
        HttpClient http,
        string url,
        string destination,
        CancellationToken cancellationToken)
    {
        using var response = await UpstreamCatalog.GetWithRetroFallbackAsync(
            http,
            url,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();

        var total = response.Content.Headers.ContentLength;
        await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
        await using var output = File.Create(destination);

        var buffer = new byte[1024 * 128];
        long copied = 0;
        while (true)
        {
            var read = await input.ReadAsync(buffer, cancellationToken);
            if (read == 0)
                break;

            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            copied += read;
            if (total is > 0)
            {
                ConsoleUi.ProgressMeasured(
                    copied,
                    total.Value,
                    "Downloading Retro Rewind update");
            }
        }

        await output.FlushAsync(cancellationToken);
    }

    private static string ResolveAllowedPath(string stageParent, string relative)
    {
        var normalizedRelative = relative.Replace('/', Path.DirectorySeparatorChar);
        var top = normalizedRelative
            .Split(Path.DirectorySeparatorChar, 2, StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault();

        if (!string.Equals(top, "RetroRewind6", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(top, "riivolution", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"Retro Rewind update attempted to write outside its allowed trees: {relative}");
        }

        var root = Path.GetFullPath(stageParent)
            .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var result = Path.GetFullPath(Path.Combine(stageParent, normalizedRelative));
        if (!result.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(
                $"Retro Rewind update path escapes the staging directory: {relative}");

        return result;
    }

    private static void ValidateStage(string stageParent, Version expectedVersion)
    {
        var retro = Path.Combine(stageParent, "RetroRewind6");
        if (!Directory.Exists(retro))
            throw new InvalidDataException("Staged Retro Rewind folder is missing.");
        if (!File.Exists(Path.Combine(retro, "Binaries", "Code.pul")))
            throw new InvalidDataException("Staged Retro Rewind Code.pul is missing.");

        var versionText = ReadInstalledVersion(retro);
        if (!UpstreamCatalog.TryVersion(versionText, out var actual) ||
            actual != expectedVersion)
        {
            throw new InvalidDataException(
                $"Staged Retro Rewind version is {versionText ?? "unknown"}, expected {expectedVersion}.");
        }
    }

    private static void CommitStage(string stageParent, string liveParent)
    {
        var names = new[] { "RetroRewind6", "riivolution" };
        var transactionRoot = Path.Combine(
            liveParent,
            ".mkwvc-rr-transaction");

        if (Directory.Exists(transactionRoot))
            throw new InvalidOperationException(
                $"An unrecovered Retro Rewind transaction already exists: {transactionRoot}");

        Directory.CreateDirectory(transactionRoot);

        var originals = names.ToDictionary(
            name => name,
            name => Directory.Exists(Path.Combine(liveParent, name)),
            StringComparer.OrdinalIgnoreCase);
        File.WriteAllText(
            Path.Combine(transactionRoot, "originals.json"),
            System.Text.Json.JsonSerializer.Serialize(
                originals,
                new System.Text.Json.JsonSerializerOptions(JsonUtil.Options)
                {
                    WriteIndented = true
                }));

        var movedLive = new List<string>();
        var published = new List<string>();
        var committed = false;

        try
        {
            foreach (var name in names)
            {
                var live = Path.Combine(liveParent, name);
                if (!Directory.Exists(live))
                    continue;

                Directory.Move(live, Path.Combine(transactionRoot, name));
                movedLive.Add(name);
            }

            foreach (var name in names)
            {
                var staged = Path.Combine(stageParent, name);
                if (!Directory.Exists(staged))
                    continue;

                Directory.Move(staged, Path.Combine(liveParent, name));
                published.Add(name);
            }

            // The marker is the transaction commit point. If the process dies
            // before this write, the next run restores the old directories.
            // After this write, the new live tree wins and recovery only drops
            // the old backup.
            File.WriteAllText(
                Path.Combine(transactionRoot, "committed"),
                DateTimeOffset.UtcNow.ToString("O"));
            committed = true;
        }
        catch
        {
            foreach (var name in published.AsEnumerable().Reverse())
            {
                TryDeleteDirectory(Path.Combine(liveParent, name));
            }

            foreach (var name in movedLive.AsEnumerable().Reverse())
            {
                var backup = Path.Combine(transactionRoot, name);
                var live = Path.Combine(liveParent, name);
                if (Directory.Exists(backup) && !Directory.Exists(live))
                    Directory.Move(backup, live);
            }

            // If every original backup was restored, metadata is no longer
            // needed and the transaction can be removed. Otherwise keep both
            // metadata and remaining backups for next-run recovery.
            if (Directory.Exists(transactionRoot))
            {
                var remainingBackup = names.Any(
                    name => Directory.Exists(Path.Combine(transactionRoot, name)));
                if (!remainingBackup)
                {
                    TryDeleteFile(Path.Combine(transactionRoot, "originals.json"));
                    if (!Directory.EnumerateFileSystemEntries(transactionRoot).Any())
                        Directory.Delete(transactionRoot);
                }
            }
            throw;
        }
        finally
        {
            if (committed)
                TryDeleteDirectory(transactionRoot);
        }
    }

    private static void CleanupStaleStages(string liveParent)
    {
        try
        {
            foreach (var directory in Directory.EnumerateDirectories(
                         liveParent,
                         ".mkwvc-rr-stage-*",
                         SearchOption.TopDirectoryOnly))
            {
                TryDeleteDirectory(directory);
            }
        }
        catch
        {
            // Stale staging is harmless. Failure to clean it must not block an
            // otherwise recoverable/current installation.
        }
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);

        foreach (var file in Directory.EnumerateFiles(source))
        {
            File.Copy(
                file,
                Path.Combine(destination, Path.GetFileName(file)),
                overwrite: true);
        }

        foreach (var directory in Directory.EnumerateDirectories(source))
        {
            CopyDirectory(
                directory,
                Path.Combine(destination, Path.GetFileName(directory)));
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
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
        }
    }
}
