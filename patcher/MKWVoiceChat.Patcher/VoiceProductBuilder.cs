using System.Text.RegularExpressions;

namespace MKWVoiceChat.Patcher;

internal sealed record VoiceBuildResult(
    string ProductDirectory,
    string ExecutablePath,
    IReadOnlyDictionary<string, string> PatchedSourceSha256);

internal static class VoiceProductBuilder
{
    public static async Task<VoiceBuildResult> BuildAsync(
        HttpClient http,
        OfficialBaseStatus official,
        bool forceCleanBuild,
        CancellationToken cancellationToken = default)
    {
        var layout = official.Layout;
        var workspace = Path.Combine(layout.InstallRoot, "BuildWorkspace");
        var toolkit = Path.Combine(layout.InstallRoot, "Toolkit");
        var localBuild = Path.Combine(workspace, "LocalBuild.ps1");
        var payload = Path.Combine(workspace, "Assets", "OfflinePayload");

        RequireFile(localBuild, "WiiCompiled LocalBuild.ps1");
        RequireFile(
            Path.Combine(payload, "binary", "payload.RMCPD00.bin"),
            "cached Retro-WFC online payload");
        if (!Directory.Exists(toolkit))
            throw new DirectoryNotFoundException(
                $"WiiCompiled toolkit is missing: {toolkit}");

        var patchRoot = PatchStateStore.Root(layout);
        Directory.CreateDirectory(patchRoot);

        var voiceDependencies = await VoiceDependencyProvisioner.EnsureAsync(
            http,
            layout,
            cancellationToken);

        var buildOutput = Path.Combine(
            patchRoot,
            ".build-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(buildOutput);

        IReadOnlyDictionary<string, string> patchedHashes;
        try
        {
            // The patch exists in BuildWorkspace only for the duration of this
            // local build. The official workspace is restored in Dispose even
            // when compilation fails, so Wheel Wizard remains authoritative.
            using (var patch = VoicePatchApplier.Apply(
                       layout,
                       official.LatestWiiCompiled))
            {
                patchedHashes = patch.PatchedSourceSha256;

                ConsoleUi.BeginPhase("Preparing Wiicompiled (Voicechat) build...");

                var compiling = false;
                var lastBuildPercent = 0;
                void ObserveBuildLine(string line)
                {
                    var mapped = line switch
                    {
                        var value when value.Contains("MKWCBUILD:STEP:retranslate-base", StringComparison.Ordinal) => 10,
                        var value when value.Contains("MKWCBUILD:STEP:translate-base", StringComparison.Ordinal) => 15,
                        var value when value.Contains("MKWCBUILD:STEP:reuse-base-translation", StringComparison.Ordinal) => 20,
                        var value when value.Contains("MKWCBUILD:STEP:emit-base-manifest", StringComparison.Ordinal) => 30,
                        var value when value.Contains("MKWCBUILD:STEP:translate-mod", StringComparison.Ordinal) => 40,
                        var value when value.Contains("MKWCBUILD:STEP:generate-data-init", StringComparison.Ordinal) => 50,
                        var value when value.Contains("MKWCBUILD:STEP:emit-build-shards", StringComparison.Ordinal) => 65,
                        var value when value.Contains("MKWCBUILD:STEP:configure-native", StringComparison.Ordinal) => 80,
                        var value when value.Contains("MKWCBUILD:STEP:compile", StringComparison.Ordinal) => 100,
                        _ => lastBuildPercent
                    };

                    if (line.Contains("MKWCBUILD:STEP:compile", StringComparison.Ordinal))
                    {
                        if (!compiling)
                            ConsoleUi.BeginPhase("Compiling Wiicompiled (Voicechat)...");
                        compiling = true;
                    }

                    if (compiling)
                    {
                        // Ninja prints real completed/total build-edge counts as
                        // "[123/456]". Use that ratio directly instead of a
                        // timer/output-line heartbeat.
                        var match = Regex.Match(line, @"\[(\d+)\/(\d+)\]");
                        if (match.Success &&
                            int.TryParse(match.Groups[1].Value, out var done) &&
                            int.TryParse(match.Groups[2].Value, out var total) &&
                            total > 0)
                        {
                            done = Math.Clamp(done, 0, total);
                            ConsoleUi.ProgressMeasured(
                                done,
                                total,
                                "Compiling Wiicompiled (Voicechat)");
                            return;
                        }
                    }

                    if (mapped > lastBuildPercent)
                    {
                        lastBuildPercent = mapped;
                        ConsoleUi.Progress(
                            mapped,
                            compiling
                                ? "Compiling Wiicompiled (Voicechat)..."
                                : "Preparing Wiicompiled (Voicechat) build...");
                    }
                }

                var buildArgs = new List<string>
                {
                    "-NoProfile",
                    "-ExecutionPolicy", "Bypass",
                    "-File", localBuild,
                    "-Workspace", workspace,
                    "-Toolkit", toolkit,
                    "-Profile", "retro-rewind",
                    "-OutputDirectory", buildOutput,
                    "-RetroRewindPackageDirectory", official.RetroRoot,
                    "-RetroWfcOfflineDirectory", payload,
                    "-RetroWfcPayloadOrigin", "downloaded"
                };
                if (forceCleanBuild)
                    buildArgs.Add("-ForceCleanBuild");

                var result = await ProcessUtil.RunAsync(
                    "powershell.exe",
                    buildArgs,
                    workspace,
                    echoOutput: false,
                    stdoutLine: ObserveBuildLine,
                    stderrLine: ObserveBuildLine,
                    environment: new Dictionary<string, string>
                    {
                        ["MKWVC_DEP_ROOT"] = voiceDependencies.Root
                    },
                    cancellationToken: cancellationToken);

                if (result.ExitCode != 0)
                    throw new InvalidOperationException(
                        "Voice-patched WiiCompiled build failed." +
                        Environment.NewLine +
                        result.Combined);
            }

            ConsoleUi.Progress(100, "Wiicompiled (Voicechat) compile completed.");
            var builtExe = Path.Combine(buildOutput, "RetroRewind.exe");
            RequireFile(builtExe, "voice-patched RetroRewind.exe");

            var finalProduct = PatchStateStore.ProductDirectory(layout);
            PublishProduct(buildOutput, finalProduct);

            var finalExe = Path.Combine(finalProduct, "RetroRewind.exe");
            RequireFile(finalExe, "published voice-patched RetroRewind.exe");

            return new(finalProduct, finalExe, patchedHashes);
        }
        finally
        {
            TryDeleteDirectory(buildOutput);
        }
    }

    private static void PublishProduct(string staged, string destination)
    {
        var parent = Directory.GetParent(destination)?.FullName
            ?? throw new InvalidOperationException("Voice product has no parent directory.");
        Directory.CreateDirectory(parent);

        var backup = destination + ".backup-" + Guid.NewGuid().ToString("N");
        var hadOld = Directory.Exists(destination);
        var published = false;

        try
        {
            if (hadOld)
                Directory.Move(destination, backup);

            Directory.Move(staged, destination);
            published = true;

            if (hadOld)
                TryDeleteDirectory(backup);
        }
        catch
        {
            if (published)
                TryDeleteDirectory(destination);

            if (hadOld && Directory.Exists(backup) && !Directory.Exists(destination))
                Directory.Move(backup, destination);
            throw;
        }
    }

    private static void RequireFile(string path, string label)
    {
        if (!File.Exists(path) || new FileInfo(path).Length == 0)
            throw new FileNotFoundException($"{label} is missing.", path);
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
