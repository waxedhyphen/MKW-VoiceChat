using System.Text.Json;

namespace MKWVoiceChat.Patcher;

internal static class PatcherSelfTests
{
    public static void RunBootstrapDiscovery()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "mkwvc-bootstrap-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            var wheelWizard = Path.Combine(root, "WheelWizard");
            var dolphinUser = Path.Combine(root, "DolphinUser");
            var rr = Path.Combine(
                dolphinUser,
                "Load",
                "Riivolution",
                "WheelWizard",
                "RetroRewind6");

            Directory.CreateDirectory(Path.Combine(rr, "Binaries"));
            File.WriteAllText(Path.Combine(rr, "version.txt"), "6.12.8");
            File.WriteAllBytes(Path.Combine(rr, "Binaries", "Code.pul"), [1, 2, 3]);
            Directory.CreateDirectory(wheelWizard);
            File.WriteAllText(
                Path.Combine(wheelWizard, "config.json"),
                JsonSerializer.Serialize(new Dictionary<string, string>
                {
                    ["UserFolderPath"] = dolphinUser,
                    ["GameLocation"] = Path.Combine(root, "game.iso")
                }));

            var recomp = Path.Combine(wheelWizard, "Recomp");
            var install = Path.Combine(recomp, "Install");
            var layout = new PatcherLayout(
                wheelWizard,
                Path.Combine(wheelWizard, "config.json"),
                recomp,
                install,
                Path.Combine(recomp, "Cache"),
                Path.Combine(install, "WiiCompiled-Setup.exe"),
                Path.Combine(install, "install-state.json"));

            var resolved = PatcherPaths.ResolveRetroRoot(layout, state: null);
            if (!Path.GetFullPath(resolved).Equals(
                    Path.GetFullPath(rr),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new Exception(
                    $"Bootstrap RR discovery returned '{resolved}', expected '{rr}'.");
            }
        }
        finally
        {
            try
            {
                if (Directory.Exists(root))
                    Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }
    }
}