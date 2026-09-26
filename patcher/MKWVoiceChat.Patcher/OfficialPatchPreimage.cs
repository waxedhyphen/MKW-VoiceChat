using System.Security.Cryptography;
using System.Text;

namespace MKWVoiceChat.Patcher;

internal static class OfficialPatchPreimage
{
    private sealed record ExpectedFile(string RelativePath, string GitBlobSha1);

    // Exact upstream blobs at patchzyy/Wiicompiled v0.2.32. These are the
    // upstream files the Voicechat integration edits. Voice-owned bridge/core
    // files must be absent on a clean official base.
    private static readonly ExpectedFile[] ExpectedV0232 =
    [
        new(
            @"runtime\CMakeLists.txt",
            "0bee6f1add223f51732f549406c4f11f8c3323d9"),
        new(
            @"runtime\cmake\PublicProducts.cmake",
            "ad2a535b68bb3cb6c59b7bbfb702cd6ea6fc0674"),
        new(
            @"runtime\src\hle\net\network_socket.cpp",
            "95c6634f0b1794b3962646f1c5e4781d54803e93"),
        new(
            @"runtime\src\settings_overlay.cpp",
            "8127eaeb451b0a698506f90c62f44fa66de35b11")
    ];

    private static readonly string[] MustBeAbsentV0232 =
    [
        @"runtime\include\retro_rewind_voice_bridge.h",
        @"runtime\src\retro_rewind_voice_bridge.cpp",
        @"runtime\voicechat\CMakeLists.txt",
        @"runtime\voicechat\include\mkwvc\EmbeddedCore.hpp",
        @"runtime\voicechat\include\mkwvc\AdaptiveJitterBuffer.hpp",
        @"runtime\voicechat\include\mkwvc\AudioEngine.hpp",
        @"runtime\voicechat\include\mkwvc\AudioProcessor.hpp",
        @"runtime\voicechat\include\mkwvc\NetworkSimulator.hpp",
        @"runtime\voicechat\include\mkwvc\OpusCodec.hpp",
        @"runtime\voicechat\include\mkwvc\SpscRingBuffer.hpp",
        @"runtime\voicechat\include\mkwvc\UdpVoiceTransport.hpp",
        @"runtime\voicechat\include\mkwvc\VoiceClient.hpp",
        @"runtime\voicechat\include\mkwvc\VoiceFormat.hpp",
        @"runtime\voicechat\include\mkwvc\VoiceTransport.hpp",
        @"runtime\voicechat\include\mkwvc\IcePeerTransport.hpp",
        @"runtime\voicechat\include\mkwvc\IceSignal.hpp",
        @"runtime\voicechat\include\mkwvc\SignalingClient.hpp",
        @"runtime\voicechat\src\AdaptiveJitterBuffer.cpp",
        @"runtime\voicechat\src\AudioEngine.cpp",
        @"runtime\voicechat\src\AudioProcessor.cpp",
        @"runtime\voicechat\src\EmbeddedCore.cpp",
        @"runtime\voicechat\src\IcePeerTransport.cpp",
        @"runtime\voicechat\src\IceSignal.cpp",
        @"runtime\voicechat\src\NetworkSimulator.cpp",
        @"runtime\voicechat\src\OpusCodec.cpp",
        @"runtime\voicechat\src\SignalingClient.cpp",
        @"runtime\voicechat\src\UdpVoiceTransport.cpp",
        @"runtime\voicechat\src\VoiceClient.cpp"
    ];

    public static bool IsClean(PatcherLayout layout, Version version, out string detail) =>
        IsCleanWorkspace(
            Path.Combine(layout.InstallRoot, "BuildWorkspace"),
            version,
            out detail);

    internal static bool IsCleanWorkspace(
        string workspace,
        Version version,
        out string detail)
    {
        if (version != VoicePatchApplier.SupportedWiiCompiledVersion)
        {
            detail = $"No MKWVC preimage is defined for WiiCompiled {version}.";
            return false;
        }

        workspace = Path.GetFullPath(workspace);
        if (!Directory.Exists(workspace))
        {
            detail = "BuildWorkspace is missing.";
            return false;
        }

        foreach (var expected in ExpectedV0232)
        {
            var path = Path.Combine(workspace, expected.RelativePath);
            if (!File.Exists(path))
            {
                detail = $"Official patch source is missing: {expected.RelativePath}";
                return false;
            }

            var actual = GitBlobSha1(path);
            if (!actual.Equals(expected.GitBlobSha1, StringComparison.OrdinalIgnoreCase))
            {
                detail =
                    $"Official patch source differs from v0.2.32: {expected.RelativePath}";
                return false;
            }
        }

        foreach (var relative in MustBeAbsentV0232)
        {
            if (File.Exists(Path.Combine(workspace, relative)))
            {
                detail = $"Voicechat integration source already exists in official workspace: {relative}";
                return false;
            }
        }

        detail = "MKWVC patch preimage matches official WiiCompiled v0.2.32.";
        return true;
    }

    private static string GitBlobSha1(string path)
    {
        var bytes = File.ReadAllBytes(path);
        var header = Encoding.ASCII.GetBytes($"blob {bytes.Length}\0");

        using var sha1 = SHA1.Create();
        sha1.TransformBlock(header, 0, header.Length, null, 0);
        sha1.TransformFinalBlock(bytes, 0, bytes.Length);
        return Convert.ToHexString(sha1.Hash!).ToLowerInvariant();
    }
}
