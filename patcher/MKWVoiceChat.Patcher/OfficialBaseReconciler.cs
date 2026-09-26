namespace MKWVoiceChat.Patcher;

internal sealed record OfficialBaseStatus(
    PatcherLayout Layout,
    string RetroRoot,
    Version LatestWiiCompiled,
    Version LatestRetroRewind,
    string? InstalledWiiCompiled,
    string? InstalledRetroRewind,
    ProcessResult ProductCheck)
{
    public bool WiiCompiledVersionCurrent =>
        UpstreamCatalog.TryVersion(InstalledWiiCompiled, out var version) &&
        version == LatestWiiCompiled;

    public bool RetroRewindVersionCurrent =>
        UpstreamCatalog.TryVersion(InstalledRetroRewind, out var version) &&
        version >= LatestRetroRewind;

    public bool ProductCurrent => ProductCheck.ExitCode == 0;

    public bool IsClean =>
        WiiCompiledVersionCurrent &&
        RetroRewindVersionCurrent &&
        ProductCurrent;
}

internal static class OfficialBaseReconciler
{
    private static async Task<RetroCatalog> ResolveRetroCatalogAsync(
        HttpClient http,
        string retroRoot,
        CancellationToken cancellationToken)
    {
        try
        {
            return await UpstreamCatalog.RetroRewindAsync(http, cancellationToken);
        }
        catch (HttpRequestException error)
        {
            var installedText = RetroRewindUpdater.ReadInstalledVersion(retroRoot);
            var codePath = Path.Combine(retroRoot, "Binaries", "Code.pul");
            if (!UpstreamCatalog.TryVersion(installedText, out var installedVersion) ||
                !File.Exists(codePath))
            {
                throw new HttpRequestException(
                    "Retro Rewind update service is unavailable and no valid local Retro Rewind installation could be used as an offline fallback.",
                    error);
            }

            ConsoleUi.WriteLine(
                $"Retro Rewind update service unavailable; using installed Retro Rewind {installedVersion} without updating.");
            return new RetroCatalog(
                installedVersion,
                Array.Empty<RetroUpdate>(),
                Array.Empty<RetroDeletion>(),
                string.Empty);
        }
    }

    public static async Task<OfficialBaseStatus> CheckAsync(
        HttpClient http,
        CancellationToken cancellationToken = default)
    {
        var layout = PatcherPaths.Discover();
        var state = PatcherPaths.ReadInstallState(layout);
        var retroRoot = PatcherPaths.ResolveRetroRoot(layout, state);
        RetroRewindUpdater.RecoverInterruptedTransaction(retroRoot);

        ConsoleUi.Progress(35, "Checking current WiiCompiled and Retro Rewind releases...");
        var wiiTask = UpstreamCatalog.LatestWiiCompiledAsync(http, cancellationToken);
        var retroTask = ResolveRetroCatalogAsync(http, retroRoot, cancellationToken);
        await Task.WhenAll(wiiTask, retroTask);

        var wii = wiiTask.Result;
        var retro = retroTask.Result;
        var product = state is null
            ? new ProcessResult(
                2,
                "",
                "WiiCompiled is not installed yet; the installer will install it automatically.")
            : await WiiCompiledRestorer.CheckProductsAsync(
                layout,
                retroRoot,
                cancellationToken);

        ConsoleUi.Progress(100, "Official versions checked.");
        return new(
            layout,
            retroRoot,
            wii.Version,
            retro.LatestVersion,
            UpstreamCatalog.NormalizeVersion(state?.SetupVersion),
            RetroRewindUpdater.ReadInstalledVersion(retroRoot),
            product);
    }

    public static async Task<OfficialBaseStatus> ReconcileAsync(
        HttpClient http,
        bool forceFullWiiCompiledRestore = false,
        CancellationToken cancellationToken = default)
    {
        var layout = PatcherPaths.Discover();
        var initialState = PatcherPaths.ReadInstallState(layout);
        var retroRoot = PatcherPaths.ResolveRetroRoot(layout, initialState);
        RetroRewindUpdater.RecoverInterruptedTransaction(retroRoot);

        ConsoleUi.BeginPhase("Preparing official Retro Rewind / WiiCompiled base...");
        var wiiTask = UpstreamCatalog.LatestWiiCompiledAsync(http, cancellationToken);
        var retroTask = ResolveRetroCatalogAsync(http, retroRoot, cancellationToken);
        await Task.WhenAll(wiiTask, retroTask);

        var wiiRelease = wiiTask.Result;
        var retroCatalog = retroTask.Result;

        var productBefore = initialState is null
            ? new ProcessResult(2, "", "WiiCompiled is not installed.")
            : await WiiCompiledRestorer.CheckProductsAsync(
                layout,
                retroRoot,
                cancellationToken);
        var installedWiiBefore = UpstreamCatalog.NormalizeVersion(
            initialState?.SetupVersion);
        var installedRetroBefore = RetroRewindUpdater.ReadInstalledVersion(retroRoot);

        var setupWasCurrent =
            UpstreamCatalog.TryVersion(installedWiiBefore, out var parsedWii) &&
            parsedWii == wiiRelease.Version;
        var officialWorkspaceWasClean =
            initialState is not null &&
            setupWasCurrent &&
            productBefore.ExitCode == 0;

        var retroChanged = await RetroRewindUpdater.EnsureCurrentAsync(
            http,
            retroRoot,
            retroCatalog,
            cancellationToken);

        // If the workspace/product was already untrusted before an RR-only update,
        // never repair from that workspace: reinstall the official setup payload.
        var forceFullRestore =
            forceFullWiiCompiledRestore || !officialWorkspaceWasClean;

        // A clean official workspace whose only change is a newer RR Code.pul can
        // safely use the official setup host's targeted repair path.
        await WiiCompiledRestorer.EnsureOfficialBaseAsync(
            http,
            layout,
            retroRoot,
            wiiRelease,
            forceFullRestore,
            cancellationToken);

        var finalState = PatcherPaths.ReadInstallState(layout)
            ?? throw new InvalidDataException(
                "Official-base reconciliation ended without install-state.json.");
        var productAfter = await WiiCompiledRestorer.CheckProductsAsync(
            layout,
            retroRoot,
            cancellationToken);

        var result = new OfficialBaseStatus(
            layout,
            retroRoot,
            wiiRelease.Version,
            retroCatalog.LatestVersion,
            UpstreamCatalog.NormalizeVersion(finalState.SetupVersion),
            RetroRewindUpdater.ReadInstalledVersion(retroRoot),
            productAfter);

        if (!result.IsClean)
            throw new InvalidDataException(
                "Official-base reconciliation completed, but final verification is not clean.");

        ConsoleUi.Progress(
            100,
            retroChanged
                ? "Retro Rewind updated; official WiiCompiled base is ready."
                : "Official WiiCompiled / Retro Rewind base is ready.");

        return result;
    }

    public static void Print(OfficialBaseStatus status)
    {
        ConsoleUi.WriteLine($"Wheel Wizard data: {status.Layout.WheelWizardRoot}");
        ConsoleUi.WriteLine($"WiiCompiled install: {status.Layout.InstallRoot}");
        ConsoleUi.WriteLine($"Retro Rewind root: {status.RetroRoot}");
        ConsoleUi.WriteLine(
            $"WiiCompiled: installed={status.InstalledWiiCompiled ?? "not installed"} " +
            $"latest={status.LatestWiiCompiled} " +
            $"[{(status.WiiCompiledVersionCurrent ? "CURRENT" : "UPDATE REQUIRED")}]");
        ConsoleUi.WriteLine(
            $"Retro Rewind: installed={status.InstalledRetroRewind ?? "unknown"} " +
            $"latest={status.LatestRetroRewind} " +
            $"[{(status.RetroRewindVersionCurrent ? "CURRENT" : "UPDATE REQUIRED")}]");
        var productStatus = status.InstalledWiiCompiled is null
            ? "NOT INSTALLED - WILL INSTALL AUTOMATICALLY"
            : status.ProductCurrent
                ? "CURRENT"
                : status.ProductCheck.ExitCode == 2
                    ? "REPAIR REQUIRED"
                    : "CHECK FAILED";
        ConsoleUi.WriteLine(
            $"WiiCompiled products: exit={status.ProductCheck.ExitCode} [{productStatus}]");
        if (!string.IsNullOrWhiteSpace(status.ProductCheck.Combined))
            ConsoleUi.WriteLine(status.ProductCheck.Combined.Trim());
        ConsoleUi.WriteLine(status.IsClean
            ? "Official base: CLEAN"
            : "Official base: RECONCILIATION REQUIRED");
    }
}
