using System.Reflection;
using System.Runtime.InteropServices;

namespace MKWVoiceChat.Patcher;

internal static class UserEntryPointInstaller
{
    private const string InstalledInstallerFileName = "WiiCompiled-VoiceChat-Installer.exe";
    private const string InstalledUpdaterFileName = "MKWVoiceChat-Updater.exe";
    private const string UpdaterResourceName =
        "MKWVoiceChat.Patcher.Updater.MKWVoiceChat-Updater.exe";
    private const string ShortcutFileName = "Wiicompiled (Voicechat).lnk";
    private const string LegacyShortcutFileName = "MKW Voice Chat.lnk";
    private const string LegacyInstallerFileName = "MKWVoiceChat.Patcher.exe";

    public static string InstalledInstallerPath(PatcherLayout layout) =>
        Path.Combine(PatchStateStore.Root(layout), InstalledInstallerFileName);

    public static string InstalledUpdaterPath(PatcherLayout layout) =>
        Path.Combine(PatchStateStore.Root(layout), InstalledUpdaterFileName);

    public static string DesktopShortcutPath()
    {
        var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        if (string.IsNullOrWhiteSpace(desktop))
            throw new InvalidOperationException("Windows desktop directory could not be resolved.");
        return Path.Combine(desktop, ShortcutFileName);
    }

    public static void InstallCurrentInstallerAndShortcut(PatcherLayout layout)
    {
        var source = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(source) || !File.Exists(source))
            throw new InvalidOperationException(
                "The running WiiCompiled Voicechat installer executable could not be located.");

        var sourceName = Path.GetFileName(source);
        if (!sourceName.Equals(InstalledInstallerFileName, StringComparison.OrdinalIgnoreCase) &&
            !sourceName.Equals(LegacyInstallerFileName, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                "Run the published WiiCompiled-VoiceChat-Installer.exe to install this build.");
        }

        var destination = InstalledInstallerPath(layout);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);

        if (!Path.GetFullPath(source).Equals(
                Path.GetFullPath(destination),
                StringComparison.OrdinalIgnoreCase))
        {
            var temp = destination + ".tmp-" + Guid.NewGuid().ToString("N");
            try
            {
                File.Copy(source, temp, overwrite: true);
                File.Move(temp, destination, overwrite: true);
            }
            finally
            {
                TryDeleteFile(temp);
            }
        }

        InstallBootstrapUpdater(layout);

        var productExe = Path.Combine(
            PatchStateStore.ProductDirectory(layout),
            "RetroRewind.exe");
        if (!File.Exists(productExe))
            throw new FileNotFoundException(
                "The compiled WiiCompiled Voicechat build is missing.",
                productExe);

        RemoveLegacyShortcut();

        var legacyInstalled = Path.Combine(
            PatchStateStore.Root(layout),
            LegacyInstallerFileName);
        if (!Path.GetFullPath(legacyInstalled).Equals(
                Path.GetFullPath(source),
                StringComparison.OrdinalIgnoreCase))
        {
            TryDeleteFile(legacyInstalled);
        }

        CreateDesktopShortcut(productExe, DesktopShortcutPath());
    }

    public static void ValidateEmbeddedUpdaterResource()
    {
        using var stream = Assembly.GetExecutingAssembly()
            .GetManifestResourceStream(UpdaterResourceName)
            ?? throw new InvalidDataException(
                "Embedded MKW Voice Chat updater is missing.");

        if (stream.Length < 64 * 1024)
            throw new InvalidDataException(
                "Embedded MKW Voice Chat updater is unexpectedly small.");

        Span<byte> header = stackalloc byte[2];
        if (stream.Read(header) != header.Length ||
            header[0] != (byte)'M' ||
            header[1] != (byte)'Z')
        {
            throw new InvalidDataException(
                "Embedded MKW Voice Chat updater is not a Windows executable.");
        }
    }

    private static void InstallBootstrapUpdater(PatcherLayout layout)
    {
        ValidateEmbeddedUpdaterResource();

        var destination = InstalledUpdaterPath(layout);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        var temp = destination + ".tmp-" + Guid.NewGuid().ToString("N");

        try
        {
            using var input = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream(UpdaterResourceName)
                ?? throw new InvalidDataException(
                    "Embedded MKW Voice Chat updater is missing.");
            using (var output = new FileStream(
                temp,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None))
            {
                input.CopyTo(output);
                output.Flush(flushToDisk: true);
            }

            File.Move(temp, destination, overwrite: true);
        }
        finally
        {
            TryDeleteFile(temp);
        }
    }

    public static void RemoveDesktopShortcut()
    {
        TryDeleteFile(DesktopShortcutPath());
        RemoveLegacyShortcut();
    }

    private static void RemoveLegacyShortcut()
    {
        var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        if (!string.IsNullOrWhiteSpace(desktop))
            TryDeleteFile(Path.Combine(desktop, LegacyShortcutFileName));
    }

    private static void CreateDesktopShortcut(string target, string shortcutPath)
    {
        var shellType = Type.GetTypeFromProgID("WScript.Shell")
            ?? throw new InvalidOperationException(
                "Windows Script Host is unavailable; desktop shortcut could not be created.");

        object? shell = null;
        object? shortcut = null;
        try
        {
            shell = Activator.CreateInstance(shellType)
                ?? throw new InvalidOperationException(
                    "Windows Script Host could not be created.");

            shortcut = shellType.InvokeMember(
                "CreateShortcut",
                BindingFlags.InvokeMethod,
                binder: null,
                target: shell,
                args: [shortcutPath])
                ?? throw new InvalidOperationException(
                    "Desktop shortcut object could not be created.");

            var shortcutType = shortcut.GetType();
            shortcutType.InvokeMember(
                "TargetPath",
                BindingFlags.SetProperty,
                binder: null,
                target: shortcut,
                args: [target]);
            shortcutType.InvokeMember(
                "WorkingDirectory",
                BindingFlags.SetProperty,
                binder: null,
                target: shortcut,
                args: [Path.GetDirectoryName(target)!]);
            shortcutType.InvokeMember(
                "Description",
                BindingFlags.SetProperty,
                binder: null,
                target: shortcut,
                args: ["Launch Wiicompiled (Voicechat)"]);
            shortcutType.InvokeMember(
                "IconLocation",
                BindingFlags.SetProperty,
                binder: null,
                target: shortcut,
                args: [target + ",0"]);
            shortcutType.InvokeMember(
                "Save",
                BindingFlags.InvokeMethod,
                binder: null,
                target: shortcut,
                args: null);
        }
        finally
        {
            if (shortcut is not null && Marshal.IsComObject(shortcut))
                Marshal.FinalReleaseComObject(shortcut);
            if (shell is not null && Marshal.IsComObject(shell))
                Marshal.FinalReleaseComObject(shell);
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
}