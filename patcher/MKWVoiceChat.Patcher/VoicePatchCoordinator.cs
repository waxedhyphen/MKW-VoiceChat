namespace MKWVoiceChat.Patcher;

internal static class VoicePatchCoordinator
{
    public static async Task<PatchState> InstallOrUpdateAsync(
        HttpClient http,
        bool forceRebuild = false,
        CancellationToken cancellationToken = default)
    {
        ConsoleUi.BeginPhase("Detecting Wheel Wizard and Retro Rewind...");
        // First do the cheap/read-only upstream check. A valid already-built
        // voice product needs no source mutation or recompilation.
        var observed = await OfficialBaseReconciler.CheckAsync(
            http,
            cancellationToken);

        if (observed.LatestWiiCompiled != VoicePatchApplier.SupportedWiiCompiledVersion)
        {
            throw new InvalidOperationException(
                $"A new official WiiCompiled release was detected. " +
                $"Installed: {observed.InstalledWiiCompiled ?? "not installed"}, " +
                $"latest: {observed.LatestWiiCompiled}, " +
                $"this Voicechat build integration supports: {VoicePatchApplier.SupportedWiiCompiledVersion}. " +
                $"For safety, this installer will NOT build against an unverified WiiCompiled release. " +
                $"Install a newer WiiCompiled Voicechat installer after support for the new WiiCompiled release is published.");
        }

        var patchPreimageClean = OfficialPatchPreimage.IsClean(
            observed.Layout,
            observed.LatestWiiCompiled,
            out var patchPreimageDetail);
        if (!patchPreimageClean && observed.InstalledWiiCompiled is not null)
            ConsoleUi.BeginPhase("Modified WiiCompiled source detected; restoring official base...");

        var existing = PatchStateStore.Read(observed.Layout);
        var existingExe = Path.Combine(
            PatchStateStore.ProductDirectory(observed.Layout),
            "RetroRewind.exe");

        // If this exact patch was already built from the same official bases and
        // the product hash still matches, there is nothing to rebuild.
        if (!forceRebuild &&
            observed.IsClean &&
            patchPreimageClean &&
            existing is not null &&
            existing.SchemaVersion == 1 &&
            existing.PatchRevision == VoicePatchApplier.PatchRevision &&
            UpstreamCatalog.TryVersion(existing.WiiCompiledVersion, out var oldWii) &&
            oldWii == observed.LatestWiiCompiled &&
            UpstreamCatalog.TryVersion(existing.RetroRewindVersion, out var oldRr) &&
            UpstreamCatalog.TryVersion(observed.InstalledRetroRewind, out var currentRr) &&
            oldRr == currentRr &&
            !string.IsNullOrWhiteSpace(existing.RetroRewindRoot) &&
            string.Equals(
                Path.GetFullPath(existing.RetroRewindRoot),
                Path.GetFullPath(observed.RetroRoot),
                StringComparison.OrdinalIgnoreCase) &&
            File.Exists(existingExe) &&
            string.Equals(
                PatchStateStore.Sha256File(existingExe),
                existing.PatchedExecutableSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            ConsoleUi.Progress(100, "Wiicompiled (Voicechat) build is already current.");
            ConsoleUi.BeginPhase("Refreshing installer and desktop shortcut...");
            UserEntryPointInstaller.InstallCurrentInstallerAndShortcut(observed.Layout);
            existing.ProductVersion =
                MkwvcReleaseCatalog.CurrentVersion.ToString(3);
            existing.ProtocolVersion = MkwvcReleaseCatalog.CurrentProtocol;
            existing.PatcherVersion =
                BuildVersion.ProductVersionText;
            PatchStateStore.Write(observed.Layout, existing);
            ConsoleUi.Progress(100, "Wiicompiled (Voicechat) is ready.");
            PrintLaunchInfo(observed.Layout);
            return existing;
        }

        // --check-products deliberately avoids hashing the complete source
        // workspace. The exact three upstream patch targets plus absence of the
        // two MKWVC-owned bridge files provide a cheap deterministic preimage
        // check. A clean normal Wheel Wizard install can therefore build MKWVC
        // once without first compiling an unnecessary official copy. Dirty,
        // double-patched or unknown patch targets force the official setup to
        // republish a clean workspace first.
        var forceCleanWiiCompiledRestore =
            !observed.WiiCompiledVersionCurrent ||
            !observed.ProductCurrent ||
            !patchPreimageClean;

        ConsoleUi.BeginPhase(observed.InstalledWiiCompiled is null
            ? "WiiCompiled is missing; installing it automatically..."
            : "Checking / repairing official WiiCompiled base...");
        var official = await OfficialBaseReconciler.ReconcileAsync(
            http,
            forceFullWiiCompiledRestore: forceCleanWiiCompiledRestore,
            cancellationToken);

        var forceCleanVoiceBuild =
            forceRebuild ||
            (existing is not null &&
             existing.PatchRevision != VoicePatchApplier.PatchRevision);

        var build = await VoiceProductBuilder.BuildAsync(
            http,
            official,
            forceCleanVoiceBuild,
            cancellationToken);

        var state = new PatchState
        {
            SchemaVersion = 1,
            PatchRevision = VoicePatchApplier.PatchRevision,
            ProductVersion = MkwvcReleaseCatalog.CurrentVersion.ToString(3),
            ProtocolVersion = MkwvcReleaseCatalog.CurrentProtocol,
            PatcherVersion =
                BuildVersion.ProductVersionText,
            WiiCompiledVersion = official.LatestWiiCompiled.ToString(),
            RetroRewindVersion =
                official.InstalledRetroRewind ?? official.LatestRetroRewind.ToString(),
            RetroRewindRoot = official.RetroRoot,
            PatchedSourceSha256 = build.PatchedSourceSha256.ToDictionary(
                pair => pair.Key,
                pair => pair.Value,
                StringComparer.OrdinalIgnoreCase),
            PatchedExecutableSha256 =
                PatchStateStore.Sha256File(build.ExecutablePath),
            InstalledUtc = DateTimeOffset.UtcNow
        };

        PatchStateStore.Write(official.Layout, state);
        ConsoleUi.BeginPhase("Creating Wiicompiled (Voicechat) desktop shortcut...");
        UserEntryPointInstaller.InstallCurrentInstallerAndShortcut(official.Layout);
        ConsoleUi.Progress(100, "Wiicompiled (Voicechat) is ready.");
        ConsoleUi.WriteLine($"Wiicompiled (Voicechat) build ready: {build.ExecutablePath}");
        PrintLaunchInfo(official.Layout);
        return state;
    }

    public static int Launch()
    {
        var layout = PatcherPaths.Discover();
        var state = PatchStateStore.Read(layout)
            ?? throw new InvalidOperationException(
                "Wiicompiled (Voicechat) is not installed yet. Run the installer first.");

        if (state.PatchRevision != VoicePatchApplier.PatchRevision)
            throw new InvalidOperationException(
                "The installed Wiicompiled (Voicechat) build belongs to another integration revision. Update it first.");

        var product = PatchStateStore.ProductDirectory(layout);
        var exe = Path.Combine(product, "RetroRewind.exe");
        if (!File.Exists(exe))
            throw new FileNotFoundException(
                "The Wiicompiled (Voicechat) RetroRewind.exe is missing.", exe);

        var actualHash = PatchStateStore.Sha256File(exe);
        if (!string.Equals(
                actualHash,
                state.PatchedExecutableSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The Wiicompiled (Voicechat) executable was modified after installation. Run the installer again.");
        }

        var start = new System.Diagnostics.ProcessStartInfo
        {
            FileName = exe,
            WorkingDirectory = product,
            UseShellExecute = false
        };

        using var process = System.Diagnostics.Process.Start(start)
            ?? throw new InvalidOperationException("Could not start Wiicompiled (Voicechat).");
        return 0;
    }

    public static void PrintLaunchInfo(PatcherLayout layout)
    {
        ConsoleUi.WriteLine("");
        ConsoleUi.WriteLine("Wiicompiled (Voicechat) installation complete.");
        ConsoleUi.WriteLine($"  Build files:    {PatchStateStore.ProductDirectory(layout)}");
        ConsoleUi.WriteLine($"  Installer:      {UserEntryPointInstaller.InstalledInstallerPath(layout)}");
        ConsoleUi.WriteLine($"  Updater:        {UserEntryPointInstaller.InstalledUpdaterPath(layout)}");
        ConsoleUi.WriteLine($"  Desktop:        {UserEntryPointInstaller.DesktopShortcutPath()}");
        ConsoleUi.WriteLine("");
        ConsoleUi.WriteLine("Do NOT launch this build with Wheel Wizard's normal Retro Rewind button.");
        ConsoleUi.WriteLine("Use the \"Wiicompiled (Voicechat)\" desktop shortcut. It points directly to the compiled build.");
        ConsoleUi.WriteLine("");
    }

    public static void Uninstall()
    {
        var layout = PatcherPaths.Discover();
        var root = PatchStateStore.Root(layout);
        if (Directory.Exists(root))
            Directory.Delete(root, recursive: true);
        UserEntryPointInstaller.RemoveDesktopShortcut();
        ConsoleUi.WriteLine("Wiicompiled (Voicechat) build removed. Official WiiCompiled/Retro Rewind were left untouched.");
    }
}
