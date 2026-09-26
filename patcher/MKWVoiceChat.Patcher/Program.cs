namespace MKWVoiceChat.Patcher;

internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        if (!OperatingSystem.IsWindows())
        {
            ConsoleUi.WriteError("Wiicompiled (Voicechat) Build Installer currently supports Windows only.");
            return 1;
        }

        var command = args.Length == 0
            ? ConsoleUi.ChooseInstallerAction()
            : args[0].Trim().ToLowerInvariant();

        if (command == "exit")
            return 0;

        if (command == "selftest-bootstrap")
        {
            try
            {
                PatcherSelfTests.RunBootstrapDiscovery();
                ConsoleUi.WriteLine("Bootstrap discovery self-test OK.");
                return 0;
            }
            catch (Exception ex)
            {
                ConsoleUi.WriteError("Bootstrap discovery self-test failed: " + ex.Message);
                return 1;
            }
        }

        if (command == "selftest-version")
        {
            try
            {
                var assemblyVersion =
                    typeof(Program).Assembly.GetName().Version
                    ?? throw new InvalidDataException(
                        "Installer assembly version is missing.");
                var expected = BuildVersion.ProductVersion;
                if (assemblyVersion.Major != expected.Major ||
                    assemblyVersion.Minor != expected.Minor ||
                    assemblyVersion.Build != expected.Build)
                {
                    throw new InvalidDataException(
                        $"Installer assembly version {assemblyVersion} does not match " +
                        $"version.json productVersion {BuildVersion.ProductVersionText}.");
                }

                UserEntryPointInstaller.ValidateEmbeddedUpdaterResource();

                ConsoleUi.WriteLine(
                    $"Version metadata self-test OK: " +
                    $"MKWVC {BuildVersion.ProductVersionText}, " +
                    $"patch {BuildVersion.PatchRevision}, " +
                    $"protocol {BuildVersion.ProtocolVersion}, " +
                    $"WiiCompiled {BuildVersion.WiiCompiledVersionText}, " +
                    "bootstrap updater embedded.");
                return 0;
            }
            catch (Exception ex)
            {
                ConsoleUi.WriteError(
                    "Version metadata self-test failed: " + ex.Message);
                return 1;
            }
        }

        if (command == "selftest-patch")
        {
            if (args.Length != 2)
            {
                ConsoleUi.WriteError("selftest-patch requires an official WiiCompiled source directory.");
                return 1;
            }

            try
            {
                if (!OfficialPatchPreimage.IsCleanWorkspace(
                        args[1],
                        VoicePatchApplier.SupportedWiiCompiledVersion,
                        out var preimageDetail))
                {
                    throw new InvalidDataException(
                        "Official preimage validation failed: " + preimageDetail);
                }

                using var patch = VoicePatchApplier.ApplyWorkspace(
                    args[1],
                    VoicePatchApplier.SupportedWiiCompiledVersion);
                Console.WriteLine(
                    $"Patch compatibility OK ({patch.PatchedSourceSha256.Count} source files).");
                return 0;
            }
            catch (Exception ex)
            {
                ConsoleUi.WriteError("Patch compatibility test failed: " + ex.Message);
                return 1;
            }
        }

        if (command is "--run") command = "run";
        if (command is "--check") command = "check";
        if (command is "--reconcile") command = "reconcile";
        if (command is "--patch") command = "patch";
        if (command is "--repair" or "--repatch") command = "repair";
        if (command is "--update") command = "update";
        if (command is "--launch") command = "launch";
        if (command is "--unpatch") command = "unpatch";

        if (command != "run" &&
            command != "check" &&
            command != "reconcile" &&
            command != "patch" &&
            command != "repair" &&
            command != "repatch" &&
            command != "update" &&
            command != "launch" &&
            command != "unpatch")
        {
            ConsoleUi.WriteError(
                "Usage: WiiCompiled-VoiceChat-Installer [run|check|reconcile|patch|repair|repatch|update|launch|unpatch] [--wait-pid PID]");
            return 1;
        }

        int waitPid;
        try
        {
            waitPid = ParseWaitPid(args);
        }
        catch (Exception ex)
        {
            ConsoleUi.WriteError("Installer failed: " + ex.Message);
            return 1;
        }

        try
        {
            // Self-update is intentionally resolved before taking the installation
            // lock. A newer installer can then take over and wait for the running
            // game process without the old installer holding any files or locks.
            if (command is "run" or "patch" or "repair" or "repatch" or "update")
            {
                using var releaseHttp = UpstreamCatalog.CreateHttpClient();
                var handedOff = await TryHandOffToLatestInstallerAsync(
                    releaseHttp,
                    command == "update" ? "patch" : command,
                    waitPid,
                    requireSuccessfulCheck: command == "update");
                if (handedOff)
                    return 0;
            }

            if (waitPid > 0)
                MkwvcReleaseCatalog.WaitForProcessExit(waitPid);

            var layout = PatcherPaths.Discover();

            if (command == "unpatch" &&
                MkwvcReleaseCatalog.RelaunchUnpatchOutsideInstallRoot(layout))
            {
                return 0;
            }

            ConsoleUi.Banner();
            ConsoleUi.PrintInstallPlan(layout);
            ConsoleUi.ResetProgress();
            using var operationLock = PatcherOperationLock.Acquire(layout);

            WiiCompiledRestorer.RecoverInterruptedForcedRestore(layout);
            VoicePatchApplier.RecoverInterruptedPatch(layout);

            if (command == "launch")
                return VoicePatchCoordinator.Launch();

            if (command == "unpatch")
            {
                RunningGameGuard.EnsureRetroRewindNotRunning();
                VoicePatchCoordinator.Uninstall();
                return 0;
            }

            using var http = UpstreamCatalog.CreateHttpClient();

            if (command == "run")
            {
                RunningGameGuard.EnsureRetroRewindNotRunning();
                await VoicePatchCoordinator.InstallOrUpdateAsync(http);
                return ConsoleUi.AskYesNo("Start Wiicompiled (Voicechat) now?")
                    ? VoicePatchCoordinator.Launch()
                    : 0;
            }

            if (command is "patch" or "reconcile" or "repair" or "repatch" or "update")
                RunningGameGuard.EnsureRetroRewindNotRunning();

            if (command == "check")
            {
                await PrintMkwvcReleaseStatusAsync(http);
                var status = await OfficialBaseReconciler.CheckAsync(http);
                OfficialBaseReconciler.Print(status);
                ConsoleUi.Progress(
                    100,
                    status.IsClean
                        ? "Check complete."
                        : "Check complete; installation/update is required.");
                return status.IsClean ? 0 : 2;
            }

            if (command == "reconcile")
            {
                var reconciled = await OfficialBaseReconciler.ReconcileAsync(http);
                OfficialBaseReconciler.Print(reconciled);
                return 0;
            }

            var forceRepair = command is "repair" or "repatch";
            await VoicePatchCoordinator.InstallOrUpdateAsync(
                http,
                forceRebuild: forceRepair);

            return ConsoleUi.AskYesNo("Start Wiicompiled (Voicechat) now?")
                ? VoicePatchCoordinator.Launch()
                : 0;
        }
        catch (Exception ex)
        {
            ConsoleUi.WriteError("Installer failed: " + ex.Message);
            return 1;
        }
    }

    private static int ParseWaitPid(string[] args)
    {
        for (var i = 1; i < args.Length; ++i)
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
                throw new ArgumentException("--wait-pid requires a positive process ID.");
            }

            return pid;
        }

        return 0;
    }

    private static async Task<bool> TryHandOffToLatestInstallerAsync(
        HttpClient http,
        string command,
        int waitPid,
        bool requireSuccessfulCheck)
    {
        MkwvcReleaseStatus release;
        try
        {
            release = await MkwvcReleaseCatalog.CheckAsync(http);
        }
        catch (Exception ex) when (!requireSuccessfulCheck)
        {
            ConsoleUi.WriteLine(
                "MKW Voice Chat update check unavailable; continuing with this installer. " +
                ex.Message);
            return false;
        }

        ConsoleUi.WriteLine(
            $"MKW Voice Chat: installer={release.CurrentVersion.ToString(3)} " +
            $"latest={release.LatestVersion.ToString(3)}");

        if (!release.UpdateAvailable)
        {
            if (release.Manifest.MinimumProtocol > MkwvcReleaseCatalog.CurrentProtocol)
            {
                throw new InvalidOperationException(
                    $"This installer supports protocol {MkwvcReleaseCatalog.CurrentProtocol}, " +
                    $"but the current MKW Voice Chat release requires protocol " +
                    $"{release.Manifest.MinimumProtocol}. The release metadata does not " +
                    "advertise a newer installer that can satisfy this requirement.");
            }
            return false;
        }

        ConsoleUi.BeginPhase(
            $"Downloading MKW Voice Chat {release.LatestVersion.ToString(3)}...");
        var downloaded = await MkwvcReleaseCatalog.DownloadInstallerAsync(
            http,
            release.Manifest);

        ConsoleUi.Progress(
            100,
            $"Downloaded MKW Voice Chat {release.LatestVersion.ToString(3)}.");
        ConsoleUi.WriteLine("Handing installation to the newer installer...");

        MkwvcReleaseCatalog.LaunchInstaller(
            downloaded,
            command,
            waitPid);
        return true;
    }

    private static async Task PrintMkwvcReleaseStatusAsync(HttpClient http)
    {
        try
        {
            var release = await MkwvcReleaseCatalog.CheckAsync(http);
            ConsoleUi.WriteLine(
                $"MKW Voice Chat: installed-installer={release.CurrentVersion.ToString(3)} " +
                $"latest={release.LatestVersion.ToString(3)} " +
                $"[{(release.UpdateAvailable ? "UPDATE AVAILABLE" : "CURRENT")}]");
            ConsoleUi.WriteLine(
                $"MKWVC protocol: local={MkwvcReleaseCatalog.CurrentProtocol} " +
                $"minimum={release.Manifest.MinimumProtocol}");
            ConsoleUi.WriteLine(
                $"WiiCompiled base: local={BuildVersion.WiiCompiledVersionText} " +
                $"required={release.Manifest.WiiCompiledVersion}");
        }
        catch (Exception ex)
        {
            ConsoleUi.WriteLine(
                "MKW Voice Chat release status could not be checked: " + ex.Message);
        }
    }
}
