using System.Text.Json;
using System.Text.RegularExpressions;

namespace MKWVoiceChat.Patcher;

internal static class WiiCompiledRestorer
{
    private const string ToolkitStateFileName = "toolkit-state.json";
    private const string ForcedRestoreBackupName = "official-toolkit-state.before-mkwvc-restore.json";

    public static void RecoverInterruptedForcedRestore(PatcherLayout layout)
    {
        var backup = ForcedRestoreBackupPath(layout);
        if (!File.Exists(backup))
            return;

        var live = Path.Combine(layout.InstallRoot, ToolkitStateFileName);
        if (File.Exists(live))
        {
            // The official setup reached publication far enough to establish a
            // new toolkit state. The old state is no longer authoritative.
            TryDeleteFile(backup);
            return;
        }

        ConsoleUi.WriteLine(
            "Recovering WiiCompiled toolkit state from an interrupted clean-base restore...");
        Directory.CreateDirectory(layout.InstallRoot);
        File.Move(backup, live);
    }

    public static async Task<bool> EnsureOfficialBaseAsync(
        HttpClient http,
        PatcherLayout layout,
        string retroRoot,
        WiiCompiledRelease release,
        bool forceFullRestore,
        CancellationToken cancellationToken = default)
    {
        var state = PatcherPaths.ReadInstallState(layout);
        var installedVersion = UpstreamCatalog.NormalizeVersion(state?.SetupVersion);
        var versionCurrent =
            UpstreamCatalog.TryVersion(installedVersion, out var installed) &&
            installed == release.Version;

        if (!forceFullRestore && versionCurrent && File.Exists(layout.SetupPath))
        {
            var check = await CheckProductsAsync(layout, retroRoot, cancellationToken);
            if (check.ExitCode == 0)
                return false;

            if (check.ExitCode == 2)
            {
                ConsoleUi.BeginPhase(
                    "Repairing official WiiCompiled compile inputs...");
                var repair = await ProcessUtil.RunAsync(
                    layout.SetupPath,
                    [
                        "--repair-products",
                        "--install-dir", layout.InstallRoot,
                        "--retro-dir", retroRoot,
                        "--download-retro-wfc-payload",
                        "--progress-json"
                    ],
                    layout.InstallRoot,
                    echoOutput: false,
                    stdoutLine: ObserveSetupProgress,
                    stderrLine: ObserveSetupDiagnostic,
                    cancellationToken: cancellationToken);
                if (repair.ExitCode != 0)
                    throw new InvalidOperationException(
                        "WiiCompiled repair failed." + Environment.NewLine + repair.Combined);

                await VerifyAsync(layout, retroRoot, release.Version, cancellationToken);
                ConsoleUi.Progress(100, "Official WiiCompiled repair is ready.");
                return true;
            }
        }

        ConsoleUi.BeginPhase(state is null
            ? $"Installing official WiiCompiled {release.Version}..."
            : $"Restoring official WiiCompiled {release.Version}...");

        RecoverInterruptedForcedRestore(layout);

        // Download and validate every external prerequisite before mutating the
        // installed provenance state. A network/config failure must leave the
        // official installation completely untouched.
        Directory.CreateDirectory(layout.CacheRoot);
        var cachedSetup = Path.Combine(
            layout.CacheRoot,
            $"MKWVC-WiiCompiled-Setup-v{release.Version}.exe");

        if (!await SetupReportsVersionAsync(
                cachedSetup,
                release.Version,
                cancellationToken))
        {
            TryDeleteFile(cachedSetup);
            await DownloadAsync(
                http,
                release.SetupDownloadUrl,
                cachedSetup,
                cancellationToken);

            if (!await SetupReportsVersionAsync(
                    cachedSetup,
                    release.Version,
                    cancellationToken))
            {
                TryDeleteFile(cachedSetup);
                throw new InvalidDataException(
                    $"Downloaded WiiCompiled setup does not report version {release.Version}.");
            }
        }

        var gameLocation = PatcherPaths.ReadWheelWizardGameLocation(layout);
        ConsoleUi.BeginPhase(
            state is null
                ? "Installing official WiiCompiled..."
                : "Restoring official WiiCompiled workspace...");

        // The official installer deliberately trusts toolkit-state.json for its
        // cheap same-release path and does not hash the complete BuildWorkspace.
        // For a strict MKWVC clean-base rebuild, temporarily remove that
        // provenance record only after all downloads/config validation passed.
        // This makes the official setup republish its packaged Toolkit + source
        // Workspace instead of trusting arbitrary manual edits.
        var toolkitState = Path.Combine(layout.InstallRoot, ToolkitStateFileName);
        var toolkitStateBackup = ForcedRestoreBackupPath(layout);
        if (File.Exists(toolkitStateBackup))
            throw new InvalidOperationException(
                $"A WiiCompiled clean-base restore backup is already present: {toolkitStateBackup}");
        if (File.Exists(toolkitState))
        {
            Directory.CreateDirectory(PatchStateStore.Root(layout));
            File.Move(toolkitState, toolkitStateBackup);
        }

        try
        {
            var install = await ProcessUtil.RunAsync(
                cachedSetup,
                [
                    "--silent",
                    "--game", gameLocation,
                    "--install-dir", layout.InstallRoot,
                    "--portable",
                    "--progress-json",
                    "--retro-dir", retroRoot,
                    "--download-retro-wfc-payload"
                ],
                layout.RecompRoot,
                echoOutput: false,
                stdoutLine: ObserveSetupProgress,
                stderrLine: ObserveSetupDiagnostic,
                cancellationToken: cancellationToken);

            if (install.ExitCode != 0)
                throw new InvalidOperationException(
                    "Official WiiCompiled restore failed." +
                    Environment.NewLine +
                    install.Combined);

            await VerifyAsync(layout, retroRoot, release.Version, cancellationToken);
            TryDeleteFile(toolkitStateBackup);
            ConsoleUi.Progress(100, "Official WiiCompiled installation is ready.");
            return true;
        }
        catch
        {
            // The upstream installer publishes transactionally. If it did not
            // publish a replacement state, put the original record back so a
            // failed download/validation cannot leave Wheel Wizard needlessly
            // broken. If a new state exists, leave it; the next run will verify
            // and, if necessary, force another clean restore.
            if (!File.Exists(toolkitState) && File.Exists(toolkitStateBackup))
                File.Move(toolkitStateBackup, toolkitState);
            throw;
        }
    }

    public static Task<ProcessResult> CheckProductsAsync(
        PatcherLayout layout,
        string retroRoot,
        CancellationToken cancellationToken = default)
    {
        if (!File.Exists(layout.SetupPath))
            return Task.FromResult(new ProcessResult(
                1,
                "",
                "Installed WiiCompiled setup host is missing."));

        return ProcessUtil.RunAsync(
            layout.SetupPath,
            [
                "--check-products",
                "--install-dir", layout.InstallRoot,
                "--retro-dir", retroRoot
            ],
            layout.InstallRoot,
            echoOutput: false,
            cancellationToken: cancellationToken);
    }

    private static async Task VerifyAsync(
        PatcherLayout layout,
        string retroRoot,
        Version expectedVersion,
        CancellationToken cancellationToken)
    {
        var state = PatcherPaths.ReadInstallState(layout)
            ?? throw new InvalidDataException(
                "WiiCompiled restore completed without a readable install-state.json.");

        if (!UpstreamCatalog.TryVersion(state.SetupVersion, out var actual) ||
            actual != expectedVersion)
        {
            throw new InvalidDataException(
                $"Restored WiiCompiled reports {state.SetupVersion ?? "unknown"}, expected {expectedVersion}.");
        }

        var check = await CheckProductsAsync(layout, retroRoot, cancellationToken);
        if (check.ExitCode != 0)
            throw new InvalidDataException(
                "WiiCompiled is still not clean after official restore." +
                Environment.NewLine +
                check.Combined);
    }

    private static async Task<bool> SetupReportsVersionAsync(
        string path,
        Version expected,
        CancellationToken cancellationToken)
    {
        if (!File.Exists(path) || new FileInfo(path).Length == 0)
            return false;

        try
        {
            var result = await ProcessUtil.RunAsync(
                path,
                ["--version"],
                cancellationToken: cancellationToken);

            return result.ExitCode == 0 &&
                   UpstreamCatalog.TryVersion(result.Stdout.Trim(), out var actual) &&
                   actual == expected;
        }
        catch
        {
            return false;
        }
    }

    private static async Task DownloadAsync(
        HttpClient http,
        string url,
        string destination,
        CancellationToken cancellationToken)
    {
        var temp = destination + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using var response = await http.GetAsync(
                url,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken);
            response.EnsureSuccessStatusCode();

            var total = response.Content.Headers.ContentLength;
            await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
            await using var output = File.Create(temp);

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
                        "Downloading official WiiCompiled setup");
                }
            }

            await output.FlushAsync(cancellationToken);
            output.Close();

            File.Move(temp, destination, overwrite: true);
            ConsoleUi.ProgressMeasured(1, 1, "Official WiiCompiled setup downloaded");
        }
        finally
        {
            TryDeleteFile(temp);
        }
    }

    private static void ObserveSetupProgress(string line)
    {
        try
        {
            using var json = JsonDocument.Parse(line);
            var root = json.RootElement;
            if (!root.TryGetProperty("type", out var type) ||
                !type.ValueEquals("progress") ||
                !root.TryGetProperty("percent", out var percentElement) ||
                !percentElement.TryGetInt32(out var percent))
                return;

            var stage = root.TryGetProperty("stage", out var stageElement)
                ? stageElement.GetString()
                : null;

            // The official setup's BuildProgressWindow intentionally estimates
            // compile progress from log activity. We have the raw Ninja
            // "[done/total]" diagnostics as well, so ignore that estimate and
            // let ObserveSetupDiagnostic drive the visible compile bar from
            // completed build edges instead.
            if (string.Equals(stage, "build-base", StringComparison.Ordinal) ||
                string.Equals(stage, "build-retro", StringComparison.Ordinal) ||
                string.Equals(stage, "publish", StringComparison.Ordinal))
            {
                return;
            }

            var message = root.TryGetProperty("message", out var messageElement)
                ? messageElement.GetString()
                : null;
            ConsoleUi.Progress(
                Math.Clamp(percent, 0, 100),
                string.IsNullOrWhiteSpace(message)
                    ? "Installing WiiCompiled..."
                    : "WiiCompiled: " + message);
        }
        catch (JsonException)
        {
            // Diagnostics are captured in ProcessResult; non-protocol stdout
            // does not control the progress bar.
        }
    }

    private static void ObserveSetupDiagnostic(string line)
    {
        // During a real local compilation CMake/Ninja reports completed build
        // edges as "[done/total]". That is actual completed work and is more
        // useful than advancing on arbitrary log output.
        var match = Regex.Match(line, @"\[(\d+)\/(\d+)\]");
        if (!match.Success ||
            !int.TryParse(match.Groups[1].Value, out var done) ||
            !int.TryParse(match.Groups[2].Value, out var total) ||
            total <= 0)
        {
            return;
        }

        done = Math.Clamp(done, 0, total);
        ConsoleUi.ProgressMeasured(
            done,
            total,
            "Compiling official WiiCompiled");
    }

    private static string ForcedRestoreBackupPath(PatcherLayout layout) =>
        Path.Combine(PatchStateStore.Root(layout), ForcedRestoreBackupName);

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
}
