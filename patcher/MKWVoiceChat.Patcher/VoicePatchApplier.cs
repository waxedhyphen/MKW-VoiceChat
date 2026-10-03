using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace MKWVoiceChat.Patcher;

internal sealed record VoicePatchSession(
    string Workspace,
    IReadOnlyDictionary<string, string> PatchedSourceSha256,
    Action Restore) : IDisposable
{
    private int _restored;

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _restored, 1) == 0)
            Restore();
    }
}

internal static class VoicePatchApplier
{
    // This patch was rebased and runtime-tested against the current official
    // Windows release. If upstream changes, fail closed until the integration
    // is explicitly rebased instead of guessing that old anchors remain
    // semantically safe.
    public static Version SupportedWiiCompiledVersion =>
        BuildVersion.WiiCompiledVersion;
    public static string PatchRevision =>
        BuildVersion.PatchRevision;

    private static readonly (string RelativePath, string ResourceName)[] VoiceCoreResources =
    [
        (@"runtime\voicechat\third_party\rnnoise\src\os_support.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.os_support.h"),
        (@"runtime\voicechat\third_party\rnnoise\AUTHORS", "MKWVoiceChat.Patcher.Payload.rnnoise.AUTHORS"),
        (@"runtime\voicechat\third_party\rnnoise\CMakeLists.txt", "MKWVoiceChat.Patcher.Payload.rnnoise.CMakeLists.txt"),
        (@"runtime\voicechat\third_party\rnnoise\COPYING", "MKWVoiceChat.Patcher.Payload.rnnoise.COPYING"),
        (@"runtime\voicechat\third_party\rnnoise\README.mkwvc.md", "MKWVoiceChat.Patcher.Payload.rnnoise.README.mkwvc.md"),
        (@"runtime\voicechat\third_party\rnnoise\include\rnnoise.h", "MKWVoiceChat.Patcher.Payload.rnnoise.include.rnnoise.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\_kiss_fft_guts.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src._kiss_fft_guts.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\arch.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.arch.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\celt_lpc.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.celt_lpc.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\celt_lpc.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.celt_lpc.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\common.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.common.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\cpu_support.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.cpu_support.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\denoise.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.denoise.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\denoise.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.denoise.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\kiss_fft.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.kiss_fft.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\kiss_fft.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.kiss_fft.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\nnet.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.nnet.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\nnet.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.nnet.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\nnet_arch.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.nnet_arch.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\nnet_default.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.nnet_default.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\opus_types.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.opus_types.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\parse_lpcnet_weights.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.parse_lpcnet_weights.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\pitch.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.pitch.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\pitch.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.pitch.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\rnn.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.rnn.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\rnn.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.rnn.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\rnnoise_data.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.rnnoise_data.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\rnnoise_data.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.rnnoise_data.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\rnnoise_data_little.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.rnnoise_data_little.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\rnnoise_tables.c", "MKWVoiceChat.Patcher.Payload.rnnoise.src.rnnoise_tables.c"),
        (@"runtime\voicechat\third_party\rnnoise\src\vec.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.vec.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\vec_avx.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.vec_avx.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\vec_neon.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.vec_neon.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\x86\dnn_x86.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.x86.dnn_x86.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\x86\x86_arch_macros.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.x86.x86_arch_macros.h"),
        (@"runtime\voicechat\third_party\rnnoise\src\x86\x86cpu.h", "MKWVoiceChat.Patcher.Payload.rnnoise.src.x86.x86cpu.h"),
        (@"runtime\voicechat\CMakeLists.txt",
            "MKWVoiceChat.Patcher.Payload.voicechat.CMakeLists.txt"),
        (@"runtime\voicechat\include\mkwvc\EmbeddedCore.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.EmbeddedCore.hpp"),
        (@"runtime\voicechat\include\mkwvc\AdaptiveJitterBuffer.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.AdaptiveJitterBuffer.hpp"),
        (@"runtime\voicechat\include\mkwvc\AudioEngine.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.AudioEngine.hpp"),
        (@"runtime\voicechat\include\mkwvc\AudioProcessor.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.AudioProcessor.hpp"),
        (@"runtime\voicechat\include\mkwvc\MicrophoneTest.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.MicrophoneTest.hpp"),
        (@"runtime\voicechat\include\mkwvc\NetworkSimulator.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.NetworkSimulator.hpp"),
        (@"runtime\voicechat\include\mkwvc\OpusCodec.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.OpusCodec.hpp"),
        (@"runtime\voicechat\include\mkwvc\SpscRingBuffer.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.SpscRingBuffer.hpp"),
        (@"runtime\voicechat\include\mkwvc\UdpVoiceTransport.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.UdpVoiceTransport.hpp"),
        (@"runtime\voicechat\include\mkwvc\VoiceClient.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.VoiceClient.hpp"),
        (@"runtime\voicechat\include\mkwvc\VoiceFormat.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.VoiceFormat.hpp"),
        (@"runtime\voicechat\include\mkwvc\VoiceTransport.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.VoiceTransport.hpp"),
        (@"runtime\voicechat\include\mkwvc\IcePeerTransport.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.IcePeerTransport.hpp"),
        (@"runtime\voicechat\include\mkwvc\IceSignal.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.IceSignal.hpp"),
        (@"runtime\voicechat\include\mkwvc\SignalingClient.hpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.include.SignalingClient.hpp"),
        (@"runtime\voicechat\src\AdaptiveJitterBuffer.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.AdaptiveJitterBuffer.cpp"),
        (@"runtime\voicechat\src\AudioEngine.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.AudioEngine.cpp"),
        (@"runtime\voicechat\src\AudioProcessor.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.AudioProcessor.cpp"),
        (@"runtime\voicechat\src\MicrophoneTest.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.MicrophoneTest.cpp"),
        (@"runtime\voicechat\src\EmbeddedCore.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.EmbeddedCore.cpp"),
        (@"runtime\voicechat\src\IcePeerTransport.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.IcePeerTransport.cpp"),
        (@"runtime\voicechat\src\IceSignal.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.IceSignal.cpp"),
        (@"runtime\voicechat\src\NetworkSimulator.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.NetworkSimulator.cpp"),
        (@"runtime\voicechat\src\OpusCodec.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.OpusCodec.cpp"),
        (@"runtime\voicechat\src\SignalingClient.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.SignalingClient.cpp"),
        (@"runtime\voicechat\src\UdpVoiceTransport.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.UdpVoiceTransport.cpp"),
        (@"runtime\voicechat\src\VoiceClient.cpp",
            "MKWVoiceChat.Patcher.Payload.voicechat.src.VoiceClient.cpp")
    ];

    private static readonly string[] PatchedPaths =
    [
        @"runtime\CMakeLists.txt",
        @"runtime\cmake\PublicProducts.cmake",
        @"runtime\include\retro_rewind_voice_bridge.h",
        @"runtime\src\hle\net\network_socket.cpp",
        @"runtime\src\retro_rewind_voice_bridge.cpp",
        @"runtime\src\settings_overlay.cpp",
        .. VoiceCoreResources.Select(entry => entry.RelativePath)
    ];

    private const string TransactionDirectoryName = ".workspace-patch-transaction";
    private const string TransactionManifestName = "manifest.json";

    public static VoicePatchSession Apply(
        PatcherLayout layout,
        Version officialWiiCompiledVersion)
    {
        var workspace = Path.GetFullPath(
            Path.Combine(layout.InstallRoot, "BuildWorkspace"));
        ValidateWorkspace(workspace, officialWiiCompiledVersion);

        RecoverInterruptedPatch(layout);

        var transactionRoot = Path.Combine(
            PatchStateStore.Root(layout),
            TransactionDirectoryName);
        if (Directory.Exists(transactionRoot))
            throw new InvalidOperationException(
                $"A previous voice patch workspace transaction could not be cleared: {transactionRoot}");

        Directory.CreateDirectory(transactionRoot);
        var manifest = CreateDiskBackup(workspace, transactionRoot);
        WriteTransactionManifest(transactionRoot, manifest);

        try
        {
            var hashes = PatchWorkspace(workspace);
            return new(
                workspace,
                hashes,
                () => RestoreDiskTransaction(transactionRoot, manifest));
        }
        catch
        {
            RestoreDiskTransaction(transactionRoot, manifest);
            throw;
        }
    }

    internal static VoicePatchSession ApplyWorkspace(
        string workspace,
        Version officialWiiCompiledVersion)
    {
        workspace = Path.GetFullPath(workspace);
        ValidateWorkspace(workspace, officialWiiCompiledVersion);

        var backups = new Dictionary<string, byte[]?>(
            StringComparer.OrdinalIgnoreCase);
        try
        {
            foreach (var relative in PatchedPaths)
            {
                var full = UnderWorkspace(workspace, relative);
                backups[relative] =
                    File.Exists(full) ? File.ReadAllBytes(full) : null;
            }

            var hashes = PatchWorkspace(workspace);
            return new(
                workspace,
                hashes,
                () => RestoreMemoryBackup(workspace, backups));
        }
        catch
        {
            RestoreMemoryBackup(workspace, backups);
            throw;
        }
    }

    public static void RecoverInterruptedPatch(PatcherLayout layout)
    {
        var transactionRoot = Path.Combine(
            PatchStateStore.Root(layout),
            TransactionDirectoryName);
        if (!Directory.Exists(transactionRoot))
            return;

        var manifestPath = Path.Combine(
            transactionRoot,
            TransactionManifestName);
        if (!File.Exists(manifestPath))
        {
            // Backups and manifest are written before source mutation begins.
            // A transaction directory without a manifest therefore never
            // reached the patching phase.
            Directory.Delete(transactionRoot, recursive: true);
            return;
        }

        WorkspaceTransactionManifest manifest;
        try
        {
            manifest = JsonSerializer.Deserialize<WorkspaceTransactionManifest>(
                File.ReadAllText(manifestPath),
                JsonUtil.Options)
                ?? throw new InvalidDataException(
                    "Workspace patch transaction manifest is empty.");
        }
        catch (Exception ex)
        {
            throw new InvalidDataException(
                $"Interrupted voice patch transaction cannot be read: {manifestPath}",
                ex);
        }

        var expectedWorkspace = Path.GetFullPath(
            Path.Combine(layout.InstallRoot, "BuildWorkspace"));
        if (!string.Equals(
                Path.GetFullPath(manifest.Workspace),
                expectedWorkspace,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Interrupted voice patch transaction belongs to a different WiiCompiled workspace.");
        }

        Console.WriteLine(
            "Recovering WiiCompiled workspace from an interrupted voice patch build...");
        RestoreDiskTransaction(transactionRoot, manifest);
    }

    private static void ValidateWorkspace(
        string workspace,
        Version officialWiiCompiledVersion)
    {
        if (officialWiiCompiledVersion != SupportedWiiCompiledVersion)
        {
            throw new InvalidOperationException(
                $"MKW Voice Chat patch {PatchRevision} supports WiiCompiled " +
                $"{SupportedWiiCompiledVersion}, but the requested base is " +
                $"{officialWiiCompiledVersion}.");
        }

        if (!Directory.Exists(workspace))
            throw new DirectoryNotFoundException(
                $"WiiCompiled source workspace is missing: {workspace}");
    }

    private static IReadOnlyDictionary<string, string> PatchWorkspace(
        string workspace)
    {
        PatchRuntimeCMake(UnderWorkspace(workspace, PatchedPaths[0]));
        PatchPublicProducts(UnderWorkspace(workspace, PatchedPaths[1]));
        WriteEmbedded(
            UnderWorkspace(workspace, PatchedPaths[2]),
            "MKWVoiceChat.Patcher.Payload.retro_rewind_voice_bridge.h");
        PatchNetworkSocket(UnderWorkspace(workspace, PatchedPaths[3]));
        WriteEmbedded(
            UnderWorkspace(workspace, PatchedPaths[4]),
            "MKWVoiceChat.Patcher.Payload.retro_rewind_voice_bridge.cpp");
        PatchSettingsOverlay(UnderWorkspace(workspace, PatchedPaths[5]));

        foreach (var (relativePath, resourceName) in VoiceCoreResources)
        {
            WriteEmbedded(
                UnderWorkspace(workspace, relativePath),
                resourceName);
        }

        var hashes = new Dictionary<string, string>(
            StringComparer.OrdinalIgnoreCase);
        foreach (var relative in PatchedPaths)
        {
            var full = UnderWorkspace(workspace, relative);
            hashes[relative.Replace('\\', '/')] =
                PatchStateStore.Sha256File(full);
        }
        return hashes;
    }

    private static WorkspaceTransactionManifest CreateDiskBackup(
        string workspace,
        string transactionRoot)
    {
        var backupRoot = Path.Combine(transactionRoot, "files");
        Directory.CreateDirectory(backupRoot);

        var entries = new List<WorkspaceTransactionEntry>();
        for (var index = 0; index < PatchedPaths.Length; ++index)
        {
            var relative = PatchedPaths[index];
            var source = UnderWorkspace(workspace, relative);
            var existed = File.Exists(source);
            string? backupName = null;
            if (existed)
            {
                backupName = index.ToString("D2") + ".bak";
                File.Copy(
                    source,
                    Path.Combine(backupRoot, backupName),
                    overwrite: false);
            }

            entries.Add(new(relative, existed, backupName));
        }

        return new(workspace, entries);
    }

    private static void WriteTransactionManifest(
        string transactionRoot,
        WorkspaceTransactionManifest manifest)
    {
        var finalPath = Path.Combine(
            transactionRoot,
            TransactionManifestName);
        var tempPath = finalPath + ".tmp";
        File.WriteAllText(
            tempPath,
            JsonSerializer.Serialize(
                manifest,
                new JsonSerializerOptions(JsonUtil.Options)
                {
                    WriteIndented = true
                }));
        File.Move(tempPath, finalPath, overwrite: false);
    }

    private static void RestoreDiskTransaction(
        string transactionRoot,
        WorkspaceTransactionManifest manifest)
    {
        var backupRoot = Path.Combine(transactionRoot, "files");
        var failures = new List<Exception>();

        foreach (var entry in manifest.Entries)
        {
            try
            {
                var destination = UnderWorkspace(
                    manifest.Workspace,
                    entry.RelativePath);
                if (!entry.Existed)
                {
                    if (File.Exists(destination))
                        File.Delete(destination);
                    continue;
                }

                if (string.IsNullOrWhiteSpace(entry.BackupName))
                    throw new InvalidDataException(
                        $"Backup name missing for {entry.RelativePath}.");

                var backup = Path.Combine(backupRoot, entry.BackupName);
                if (!File.Exists(backup))
                    throw new FileNotFoundException(
                        $"Workspace backup is missing for {entry.RelativePath}.",
                        backup);

                var directory = Path.GetDirectoryName(destination);
                if (!string.IsNullOrWhiteSpace(directory))
                    Directory.CreateDirectory(directory);
                File.Copy(backup, destination, overwrite: true);
                // LocalBuild may have produced patched objects after the
                // original source timestamp. Touch the restored official file
                // so any later official incremental build must recompile it
                // instead of reusing a newer patched object.
                File.SetLastWriteTimeUtc(destination, DateTime.UtcNow);
            }
            catch (Exception ex)
            {
                failures.Add(ex);
            }
        }

        if (failures.Count != 0)
        {
            // Keep the transaction directory and its backups intact for the
            // next run / explicit recovery.
            throw new AggregateException(
                "Could not fully restore the official WiiCompiled workspace after patching.",
                failures);
        }

        Directory.Delete(transactionRoot, recursive: true);
    }

    private static void RestoreMemoryBackup(
        string workspace,
        IReadOnlyDictionary<string, byte[]?> backups)
    {
        List<Exception>? failures = null;

        foreach (var pair in backups)
        {
            try
            {
                var full = UnderWorkspace(workspace, pair.Key);
                if (pair.Value is null)
                {
                    if (File.Exists(full))
                        File.Delete(full);
                    continue;
                }

                var directory = Path.GetDirectoryName(full);
                if (!string.IsNullOrWhiteSpace(directory))
                    Directory.CreateDirectory(directory);
                File.WriteAllBytes(full, pair.Value);
            }
            catch (Exception ex)
            {
                (failures ??= []).Add(ex);
            }
        }

        if (failures is { Count: > 0 })
            throw new AggregateException(
                "Could not fully restore the WiiCompiled source tree after compatibility testing.",
                failures);
    }

    private sealed record WorkspaceTransactionEntry(
        string RelativePath,
        bool Existed,
        string? BackupName);

    private sealed record WorkspaceTransactionManifest(
        string Workspace,
        List<WorkspaceTransactionEntry> Entries);

    private static void PatchRuntimeCMake(string path)
    {
        var text = ReadUtf8(path);
        text = ReplaceOnce(
            text,
            "if(MKW_PROJECT_COMPILE_DEFINITIONS)\n" +
            "    add_compile_definitions(${MKW_PROJECT_COMPILE_DEFINITIONS})\n" +
            "endif()\n\n" +
            "# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build",
            "if(MKW_PROJECT_COMPILE_DEFINITIONS)\n" +
            "    add_compile_definitions(${MKW_PROJECT_COMPILE_DEFINITIONS})\n" +
            "endif()\n\n" +
            "# MKW VoiceChat integration sources are kept outside runtime/src so the\n" +
            "# translator/native-source scan never mistakes host-only voice code for guest\n" +
            "# native overrides. Stage 1 compiles VoiceClient.cpp with the shipped toolchain\n" +
            "# and links a small status object into the products; external audio/ICE\n" +
            "# dependencies are added in the next stage.\n" +
            "if(EXISTS \"${CMAKE_CURRENT_LIST_DIR}/voicechat/CMakeLists.txt\")\n" +
            "    add_subdirectory(voicechat)\n" +
            "endif()\n\n" +
            "# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build",
            "runtime VoiceChat subdirectory");
        WriteUtf8(path, text);
    }

    private static void PatchPublicProducts(string path)
    {
        var text = ReadUtf8(path);
        text = ReplaceOnce(
            text,
            "            dbghelp user32 winmm ws2_32 iphlpapi secur32 crypt32 windowsapp)",
            "            dbghelp user32 winmm ws2_32 iphlpapi secur32 crypt32 windowsapp winhttp)",
            "PublicProducts Windows libraries");
        text = ReplaceOnce(
            text,
            "target_link_libraries(mkw_runtime_common PRIVATE\n" +
            "    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)\n",
            "target_link_libraries(mkw_runtime_common PRIVATE\n" +
            "    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)\n" +
            "if(TARGET mkw_voicechat_core_stage3)\n" +
            "    target_link_libraries(mkw_runtime_common PRIVATE mkw_voicechat_core_stage3)\n" +
            "endif()\n",
            "runtime VoiceChat core include surface");
        text = ReplaceOnce(
            text,
            "    target_link_libraries(${target} PRIVATE\n" +
            "        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)\n",
            "    target_link_libraries(${target} PRIVATE\n" +
            "        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)\n" +
            "    if(TARGET mkw_voicechat_core_stage3)\n" +
            "        target_link_libraries(${target} PRIVATE mkw_voicechat_core_stage3)\n" +
            "    endif()\n",
            "PublicProducts VoiceChat core link");
        WriteUtf8(path, text);
    }

    private static void PatchNetworkSocket(string path)
    {
        var text = ReadUtf8(path);

        text = ReplaceOnce(
            text,
            "#include \"network_internal.h\"\n",
            "#include \"network_internal.h\"\n#include \"retro_rewind_voice_bridge.h\"\n",
            "network_socket include");

        text = ReplaceOnce(
            text,
            "    ClearSslSessionsForSocket(fd);\n    CloseNativeSocket(s->native);",
            "    ClearSslSessionsForSocket(fd);\n" +
            "    RetroRewindVoiceBridge::OnSocketClosed(fd, s->peerPort);\n" +
            "    CloseNativeSocket(s->native);",
            "network_socket close observer");

        text = ReplaceOnce(
            text,
            "        const uint32_t sendSize = patchedWrite ? static_cast<uint32_t>(patched.size()) : in[0].size;\n" +
            "        const int ret = sendto(",
            "        uint32_t sendSize = patchedWrite ? static_cast<uint32_t>(patched.size()) : in[0].size;\n" +
            "        RetroRewindVoiceBridge::ObserveGpcmSend(fd, s->peerPort, sendData, sendSize);\n" +
            "        std::vector<uint8_t> voicePatched;\n" +
            "        const bool voicePatchedWrite = RetroRewindVoiceBridge::RewriteGpcmSend(\n" +
            "            fd, s->peerPort, sendData, sendSize, voicePatched);\n" +
            "        if (voicePatchedWrite) {\n" +
            "            sendData = voicePatched.data();\n" +
            "            sendSize = static_cast<uint32_t>(voicePatched.size());\n" +
            "        }\n" +
            "        const int ret = sendto(",
            "network_socket send observer");

        text = ReplaceOnce(
            text,
            "        if (patchedWrite && ret == static_cast<int>(sendSize)) {\n",
            "        if ((patchedWrite || voicePatchedWrite) && ret == static_cast<int>(sendSize)) {\n",
            "network_socket rewritten send result");

        text = ReplaceOnce(
            text,
            "        const int hostError = ret < 0 ? NativeLastError() : 0;\n",
            "        const int hostError = ret < 0 ? NativeLastError() : 0;\n" +
            "        if (voicePatchedWrite && ret == static_cast<int>(sendSize)) {\n" +
            "            RetroRewindVoiceBridge::ConfirmGpcmRewriteSent(fd, s->peerPort);\n" +
            "        }\n",
            "network_socket Open Host send confirmation");

        text = ReplaceOnce(
            text,
            "        if (ret >= 0 && fromPtr) {\n",
            "        if (ret > 0) {\n" +
            "            RetroRewindVoiceBridge::ObserveGpcmReceive(\n" +
            "                fd, s->peerPort, reinterpret_cast<const uint8_t*>(data),\n" +
            "                static_cast<std::size_t>(ret));\n" +
            "        }\n" +
            "        if (ret >= 0 && fromPtr) {\n",
            "network_socket receive observer");

        WriteUtf8(path, text);
    }

    private static void PatchSettingsOverlay(string path)
    {
        var text = ReadUtf8(path);

        text = ReplaceOnce(
            text,
            "#include \"runtime_log.h\"\n",
            "#include \"runtime_log.h\"\n" +
            "#include \"runtime_product.h\"\n" +
            "#include \"retro_rewind_voice_bridge.h\"\n" +
            "#include \"mkwvc/EmbeddedCore.hpp\"\n",
            "settings overlay includes");

        if (!text.Contains("#include <vector>\n", StringComparison.Ordinal))
        {
            text = ReplaceOnce(
                text,
                "#include <utility>\n",
                "#include <utility>\n#include <vector>\n",
                "settings overlay vector include");
        }

        const string voiceUi =
@"bool VoiceKeyboardBindingActive(int32_t scancode) {
    if(scancode<0 || scancode>=SDL_SCANCODE_COUNT || SDL_GetKeyboardFocus()==nullptr) return false;
    int count=0;
    const bool* keys=SDL_GetKeyboardState(&count);
    return keys && scancode<count && keys[scancode];
}

bool VoiceControllerBindingActive(const std::string& configName) {
    if(configName.empty()) return false;
    const NativeButtonItem* item=ControllerNames::FindNativeButton(configName);
    if(!item ||
       item->nativeButton==PAD_NATIVE_BUTTON_DISABLED ||
       item->nativeButton==PAD_NATIVE_BUTTON_INVALID) {
        return false;
    }

    const s32 controllerIndex=PADGetIndexForPort(0);
    if(controllerIndex<0) return false;
    SDL_Gamepad* gamepad=PADGetSDLGamepadForIndex(static_cast<u32>(controllerIndex));
    if(!gamepad) return false;

    if(PADIsAxisButton(item->nativeButton)) {
        const auto axis=static_cast<SDL_GamepadAxis>(PADAxisButtonAxis(item->nativeButton));
        const int value=static_cast<int>(SDL_GetGamepadAxis(gamepad,axis));
        const int signedValue=PADAxisButtonNegative(item->nativeButton) ? -value : value;
        const int threshold=32767*static_cast<int>(PADAxisButtonThreshold(item->nativeButton))/100;
        return signedValue>=threshold;
    }

    if(item->nativeButton>=SDL_GAMEPAD_BUTTON_COUNT) return false;
    return SDL_GetGamepadButton(gamepad,static_cast<SDL_GamepadButton>(item->nativeButton));
}

bool VoiceBindingActive(const mkwvc::EmbeddedVoiceBinding& binding) {
    return VoiceKeyboardBindingActive(binding.keyboard) ||
           VoiceControllerBindingActive(binding.controller);
}

const char* VoiceKeyboardBindingName(int32_t scancode) {
    if(scancode<0 || scancode>=SDL_SCANCODE_COUNT) return ""None"";
    const char* name=SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name && *name ? name : ""Unknown"";
}

std::string VoiceControllerBindingName(const std::string& configName) {
    if(configName.empty()) return ""None"";
    const NativeButtonItem* item=ControllerNames::FindNativeButton(configName);
    return item ? std::string(item->label) : std::string(""Unknown"");
}

void DrawVoiceBindingRow(
    const char* label,
    mkwvc::EmbeddedVoiceBindingAction action,
    const mkwvc::EmbeddedVoiceBinding& binding) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    ImGui::SetNextItemWidth(150.0f);
    if(ImGui::BeginCombo(""##KeyboardBinding"",VoiceKeyboardBindingName(binding.keyboard))) {
        if(ImGui::Selectable(""None"",binding.keyboard<0)) {
            mkwvc::setEmbeddedVoiceBinding(action,-1,binding.controller);
        }
        for(int scancode=0;scancode<SDL_SCANCODE_COUNT;++scancode) {
            const char* name=SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
            if(!name || !*name) continue;
            const bool selected=binding.keyboard==scancode;
            if(ImGui::Selectable(name,selected)) {
                mkwvc::setEmbeddedVoiceBinding(action,scancode,binding.controller);
            }
            if(selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const std::string controllerPreview=VoiceControllerBindingName(binding.controller);
    ImGui::SetNextItemWidth(190.0f);
    if(ImGui::BeginCombo(""##ControllerBinding"",controllerPreview.c_str())) {
        if(ImGui::Selectable(""None"",binding.controller.empty())) {
            mkwvc::setEmbeddedVoiceBinding(action,binding.keyboard,{});
        }
        for(const auto& item:kNativeButtons) {
            if(std::string_view(item.configName)==""disabled"" ||
               std::string_view(item.configName)==""unmapped"") {
                continue;
            }
            const bool selected=binding.controller==item.configName;
            if(ImGui::Selectable(item.label,selected)) {
                mkwvc::setEmbeddedVoiceBinding(action,binding.keyboard,item.configName);
            }
            if(selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if(binding.keyboard>=0 || !binding.controller.empty()) {
        ImGui::SameLine();
        if(ImGui::SmallButton(""Clear"")) {
            mkwvc::setEmbeddedVoiceBinding(action,-1,{});
        }
    }
    ImGui::PopID();
}

bool VoiceBindingConflict(
    const mkwvc::EmbeddedVoiceBinding& a,
    const mkwvc::EmbeddedVoiceBinding& b) {
    return (a.keyboard>=0 && a.keyboard==b.keyboard) ||
           (!a.controller.empty() && a.controller==b.controller);
}

void ServiceVoiceHotkeys() {
    auto controls=mkwvc::embeddedVoiceControls();
    static bool previousMute=false;
    static bool previousDeafen=false;

    if(!controls.enabled || controls.runtimeBlocked) {
        previousMute=false;
        previousDeafen=false;
        mkwvc::setEmbeddedVoiceHotkeyState(false,false);
        return;
    }

    const bool ptt=VoiceBindingActive(controls.pushToTalkBinding);
    const bool pushMute=VoiceBindingActive(controls.pushToMuteBinding);
    mkwvc::setEmbeddedVoiceHotkeyState(ptt,pushMute);

    const bool mute=VoiceBindingActive(controls.muteBinding);
    const bool deafen=VoiceBindingActive(controls.deafenBinding);
    if(!controls.pushToTalk && mute && !previousMute) {
        mkwvc::setEmbeddedVoiceMicrophoneMuted(!controls.microphoneMuted);
    }
    if(deafen && !previousDeafen) {
        mkwvc::setEmbeddedVoiceDeafened(!controls.deafened);
    }
    previousMute=mute;
    previousDeafen=deafen;
}

void DrawVoiceChatOverlay() {
    auto controls=mkwvc::embeddedVoiceControls();
    const auto release=RetroRewindVoiceBridge::Release();
    const bool updateRequired=
        release.checkComplete &&
        release.updateAvailable;

    if(!updateRequired &&
       (!controls.enabled ||
        (!controls.localStatusOverlayVisible &&
         !controls.playerSpeakersOverlayVisible))) {
        return;
    }

    const auto session=mkwvc::embeddedVoiceSessionStatus();
    ImGuiViewport* viewport=ImGui::GetMainViewport();
    if(!viewport) return;

    if(!updateRequired &&
       controls.playerSpeakersOverlayVisible &&
       !controls.peers.empty()) {
        ImGui::SetNextWindowPos(
            ImVec2(
                viewport->WorkPos.x+viewport->WorkSize.x-12.0f,
                viewport->WorkPos.y+viewport->WorkSize.y*0.5f),
            ImGuiCond_Always,
            ImVec2(1.0f,0.5f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(5.0f,5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(4.0f,3.0f));
        const ImGuiWindowFlags flags=
            ImGuiWindowFlags_NoDecoration|
            ImGuiWindowFlags_AlwaysAutoResize|
            ImGuiWindowFlags_NoSavedSettings|
            ImGuiWindowFlags_NoFocusOnAppearing|
            ImGuiWindowFlags_NoNav|
            ImGuiWindowFlags_NoInputs;
        if(ImGui::Begin(""##VoiceChatParticipants"",nullptr,flags)) {
            const float backgroundAlpha=std::clamp(
                1.0f-controls.playerSpeakerBackgroundTransparency,
                0.05f,
                1.0f);
            const bool useTwoColumns=
                controls.peers.size()>6 &&
                viewport->WorkSize.x>=460.0f;
            std::size_t peerIndex=0;
            for(const auto& peer:controls.peers) {
                ImGui::PushID(peer.memberId.c_str());
                const ImVec4 speakingBg=peer.speaking
                    ? ImVec4(0.12f,0.48f,0.19f,backgroundAlpha)
                    : (peer.isFriend
                        ? ImVec4(0.04f,0.10f,0.24f,backgroundAlpha)
                        : ImVec4(0.07f,0.07f,0.07f,backgroundAlpha));
                ImGui::PushStyleColor(ImGuiCol_ChildBg,speakingBg);
                ImGui::BeginChild(
                    ""##VoicePeerOverlay"",
                    ImVec2(210.0f,42.0f),
                    true,
                    ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);

                ImGui::SetWindowFontScale(0.90f);
                const std::string name=peer.displayName.empty()
                    ? (peer.participantId.empty() ? std::string(""Player"") : peer.participantId)
                    : peer.displayName;
                ImGui::TextUnformatted(name.c_str());
                if(!peer.friendCode.empty()) {
                    ImGui::SameLine(0.0f,4.0f);
                    ImGui::TextDisabled(""[%s]"",peer.friendCode.c_str());
                }

                if(peer.speaking) ImGui::TextUnformatted(""SPEAKING"");
                else ImGui::TextDisabled(""Silent"");
                if(peer.isFriend) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(""| FRIEND"");
                }
                if(peer.openHost) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.82f,0.18f,1.0f));
                    ImGui::TextUnformatted(""| OPEN HOST"");
                    ImGui::PopStyleColor();
                }
                if(peer.policyMuted) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(""| AUTO MUTED"");
                } else if(peer.remoteDeafened) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(""| DEAFENED"");
                } else if(peer.remoteMuted) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(""| MUTED"");
                }
                ImGui::SetWindowFontScale(1.0f);

                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();

                if(useTwoColumns &&
                   peerIndex%2==0 &&
                   peerIndex+1<controls.peers.size()) {
                    ImGui::SameLine(0.0f,4.0f);
                }
                ++peerIndex;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    if(!updateRequired && !controls.localStatusOverlayVisible) return;

    std::string status;
    if(updateRequired) status=""UPDATE REQUIRED"";
    else if(controls.deafened) status=""DEAFENED"";
    else if(controls.microphoneMuted) status=""MUTED"";
    else if(controls.pushToMute && controls.pushToMuteHeld) status=""PUSH-TO-MUTE"";
    else if(controls.localSpeaking) status=""Speaking"";
    else if(controls.pushToTalk) status=controls.pushToTalkHeld ? ""PTT ACTIVE"" : ""PTT READY"";
    else if(controls.voiceActivation) status=""VOICE ACTIVATION"";
    else status=session.voiceClientRunning ? ""VOICE READY"" : ""VOICE IDLE"";

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x+viewport->WorkSize.x-18.0f,
               viewport->WorkPos.y+viewport->WorkSize.y-18.0f),
        ImGuiCond_Always,
        ImVec2(1.0f,1.0f));
    ImGui::SetNextWindowBgAlpha(updateRequired ? 1.0f : 0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12.0f,8.0f));

    const bool alert=controls.microphoneMuted || controls.deafened;
    const bool speaking=
        controls.localSpeaking &&
        !updateRequired &&
        !alert &&
        !(controls.pushToMute && controls.pushToMuteHeld);
    if(updateRequired) {
        ImGui::PushStyleColor(
            ImGuiCol_WindowBg,
            ImVec4(0.62f,0.43f,0.02f,1.0f));
    } else if(alert) {
        ImGui::PushStyleColor(
            ImGuiCol_WindowBg,
            ImVec4(0.55f,0.05f,0.05f,0.92f));
    } else if(speaking) {
        ImGui::PushStyleColor(
            ImGuiCol_WindowBg,
            ImVec4(0.04f,0.48f,0.12f,0.94f));
    }

    const ImGuiWindowFlags localFlags=
        ImGuiWindowFlags_NoDecoration|
        ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|
        ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoInputs;
    if(ImGui::Begin(""##VoiceChatLocalStatus"",nullptr,localFlags)) {
        ImGui::SetWindowFontScale(1.55f);
        ImGui::TextUnformatted(status.c_str());
        ImGui::SetWindowFontScale(1.05f);
        if(updateRequired) {
            ImGui::TextUnformatted(""Voice Chat disabled until updated"");
        } else {
            ImGui::TextDisabled(""%s"",
                session.signalingConnected ? ""Voice connected"" : ""Voice waiting"");
        }
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    if(updateRequired || alert || speaking) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawVoiceChatSettings() {
    auto controls=mkwvc::embeddedVoiceControls();
    const auto release=RetroRewindVoiceBridge::Release();
    const auto session=mkwvc::embeddedVoiceSessionStatus();
    const bool updateRequired=
        release.checkComplete &&
        release.updateAvailable;

    bool enabled=controls.enabled;
    ImGui::BeginDisabled(controls.runtimeBlocked);
    if(ImGui::Checkbox(""Enabled"",&enabled)) {
        mkwvc::setEmbeddedVoiceEnabled(enabled);
        controls=mkwvc::embeddedVoiceControls();
    }
    ImGui::EndDisabled();

    ImGui::TextUnformatted(""Retro Rewind voice integration"");
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        ""MKW Voice Chat %s | Integration: %s"",
        RetroRewindVoiceBridge::kMkwVoiceChatVersion,
        RetroRewindVoiceBridge::kMkwVoiceChatPatchRevision);
    ImGui::PopStyleColor();
    if(session.onlineUserCount>0) {
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(1.0f,0.82f,0.18f,1.0f));
        ImGui::TextWrapped(
            ""You are now online. Voice chat users online: %u"",
            session.onlineUserCount);
        ImGui::PopStyleColor();
    }
    ImGui::TextWrapped(
        ""Update status: %s"",
        release.status.empty() ? ""Checking..."" : release.status.c_str());

    if(updateRequired) {
        ImGui::SeparatorText(""Update required"");
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(1.0f,0.82f,0.18f,1.0f));

        if(release.productUpdateRequired) {
            ImGui::TextUnformatted(""MKW VOICE CHAT UPDATE REQUIRED"");
            ImGui::Text(
                ""Installed: %s"",
                RetroRewindVoiceBridge::kMkwVoiceChatVersion);
            ImGui::Text(
                ""Latest: %s"",
                release.latestVersion.empty()
                    ? ""unknown""
                    : release.latestVersion.c_str());
        }

        if(release.integrationUpdateRequired) {
            ImGui::TextUnformatted(""MKW VOICE CHAT INTEGRATION UPDATE REQUIRED"");
            ImGui::TextWrapped(
                ""Installed integration: %s"",
                RetroRewindVoiceBridge::kMkwVoiceChatPatchRevision);
            ImGui::TextWrapped(
                ""Latest integration: %s"",
                release.latestPatchRevision.empty()
                    ? ""unknown""
                    : release.latestPatchRevision.c_str());
        }

        if(release.wiiCompiledUpdateRequired) {
            ImGui::TextUnformatted(""WIICOMPILED UPDATE REQUIRED"");
            ImGui::Text(
                ""Installed base: %s"",
                RetroRewindVoiceBridge::kMkwVoiceChatWiiCompiledVersion);
            ImGui::Text(
                ""Voice Chat base: %s"",
                RetroRewindVoiceBridge::kMkwVoiceChatWiiCompiledVersion);
            ImGui::Text(
                ""Official base: %s"",
                release.latestOfficialWiiCompiledVersion.empty()
                    ? (release.requiredWiiCompiledVersion.empty()
                        ? ""unknown""
                        : release.requiredWiiCompiledVersion.c_str())
                    : release.latestOfficialWiiCompiledVersion.c_str());
        }

        if(release.protocolUpdateRequired) {
            ImGui::TextUnformatted(""VOICE CHAT PROTOCOL UPDATE REQUIRED"");
            ImGui::Text(
                ""Installed protocol: %u"",
                RetroRewindVoiceBridge::kMkwVoiceChatProtocolVersion);
            ImGui::Text(
                ""Required protocol: %u"",
                release.minimumProtocol);
        }

        ImGui::TextWrapped(
            ""Voice Chat has been disabled for this process until MKW Voice Chat is updated. Your saved Enabled setting was not changed."");
        ImGui::PopStyleColor();

        const bool matchingVoiceChatUpdateUnavailable=
            release.wiiCompiledUpdateRequired &&
            !release.latestOfficialWiiCompiledVersion.empty() &&
            !release.requiredWiiCompiledVersion.empty() &&
            release.requiredWiiCompiledVersion!=
                release.latestOfficialWiiCompiledVersion;

        ImGui::BeginDisabled(matchingVoiceChatUpdateUnavailable);
        const bool updateClicked=
            ImGui::Button(""Update MKW Voice Chat"");
        ImGui::EndDisabled();

        if(matchingVoiceChatUpdateUnavailable &&
           ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                ""A matching MKW Voice Chat update for this WiiCompiled version is not available yet. Please check again later."");
        }

        if(updateClicked &&
           RetroRewindVoiceBridge::LaunchInstalledUpdater()) {
            ExitForAuroraWindowClose();
        }
        return;
    }

    if(controls.runtimeBlocked) {
        ImGui::TextWrapped(
            ""%s"",
            controls.runtimeBlockReason.empty()
                ? ""Voice Chat is temporarily unavailable.""
                : controls.runtimeBlockReason.c_str());
        return;
    }

    if(!controls.enabled) {
        ImGui::TextWrapped(
            ""Voice Chat is disabled. Enable it to connect to other Voice Chat users in your current Retro Rewind room."");
        return;
    }

    const RetroRewindVoiceBridge::IdentitySnapshot identity =
        RetroRewindVoiceBridge::Snapshot();
    const RetroRewindVoiceBridge::RoomSnapshot room =
        RetroRewindVoiceBridge::Room();

    const auto playerNameFor=[&](const std::string& profileId) {
        for(const auto& player:room.players) {
            if(player.profileId==profileId) return player.name;
        }
        return profileId.empty() ? std::string(""-"") : profileId;
    };
    const auto playerFriendCodeFor=[&](const std::string& profileId) {
        for(const auto& player:room.players) {
            if(player.profileId==profileId) return player.friendCode;
        }
        return std::string();
    };

    const std::string localName=playerNameFor(identity.profileId);
    const std::string localFriendCode=playerFriendCodeFor(identity.profileId);
    if(localFriendCode.empty()) ImGui::Text(""Display name: %s"",localName.c_str());
    else ImGui::Text(""Display name: %s [%s]"",localName.c_str(),localFriendCode.c_str());

    const auto& style=ImGui::GetStyle();

    ImGui::TextUnformatted(""Audio input"");
    const std::string inputPreview=controls.inputDevice.empty() ? ""System default"" : controls.inputDevice;
    const float inputRefreshWidth=ImGui::CalcTextSize(""Refresh##VoiceInput"").x+style.FramePadding.x*2.0f;
    ImGui::SetNextItemWidth(std::max(120.0f,ImGui::GetContentRegionAvail().x-inputRefreshWidth-style.ItemSpacing.x));
    if(ImGui::BeginCombo(""##VoiceInput"",inputPreview.c_str())) {
        if(ImGui::Selectable(""System default"",controls.inputDevice.empty())) {
            mkwvc::setEmbeddedVoiceInputDevice({});
            controls=mkwvc::embeddedVoiceControls();
        }
        for(std::size_t deviceIndex=0;deviceIndex<controls.inputDevices.size();++deviceIndex) {
            const auto& device=controls.inputDevices[deviceIndex];
            const bool selected=controls.inputDevice==device;
            ImGui::PushID(static_cast<int>(deviceIndex));
            if(ImGui::Selectable(device.c_str(),selected)) {
                mkwvc::setEmbeddedVoiceInputDevice(device);
                controls=mkwvc::embeddedVoiceControls();
            }
            if(selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if(ImGui::Button(""Refresh##VoiceInput"")) {
        mkwvc::refreshEmbeddedVoiceDevices();
        controls=mkwvc::embeddedVoiceControls();
    }

    ImGui::TextUnformatted(""Audio output"");
    const std::string outputPreview=controls.outputDevice.empty() ? ""System default"" : controls.outputDevice;
    const float outputRefreshWidth=ImGui::CalcTextSize(""Refresh##VoiceOutput"").x+style.FramePadding.x*2.0f;
    ImGui::SetNextItemWidth(std::max(120.0f,ImGui::GetContentRegionAvail().x-outputRefreshWidth-style.ItemSpacing.x));
    if(ImGui::BeginCombo(""##VoiceOutput"",outputPreview.c_str())) {
        if(ImGui::Selectable(""System default"",controls.outputDevice.empty())) {
            mkwvc::setEmbeddedVoiceOutputDevice({});
            controls=mkwvc::embeddedVoiceControls();
        }
        for(std::size_t deviceIndex=0;deviceIndex<controls.outputDevices.size();++deviceIndex) {
            const auto& device=controls.outputDevices[deviceIndex];
            const bool selected=controls.outputDevice==device;
            ImGui::PushID(static_cast<int>(deviceIndex));
            if(ImGui::Selectable(device.c_str(),selected)) {
                mkwvc::setEmbeddedVoiceOutputDevice(device);
                controls=mkwvc::embeddedVoiceControls();
            }
            if(selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if(ImGui::Button(""Refresh##VoiceOutput"")) {
        mkwvc::refreshEmbeddedVoiceDevices();
        controls=mkwvc::embeddedVoiceControls();
    }

    ImGui::TextUnformatted(""Output volume"");
    int outputVolume=static_cast<int>(controls.playbackVolume*100.0f+0.5f);
    ImGui::SetNextItemWidth(-1.0f);
    if(ImGui::SliderInt(""##VoiceOutputVolume"",&outputVolume,0,300,""%d%%"")) {
        mkwvc::setEmbeddedVoicePlaybackVolume(static_cast<float>(outputVolume)/100.0f);
        controls=mkwvc::embeddedVoiceControls();
    }

    if(ImGui::Button(""Test output tone"")) mkwvc::playEmbeddedVoiceTestTone();
    ImGui::SameLine();
    if(ImGui::Button(""Reset audio settings"")) {
        mkwvc::resetEmbeddedVoiceAudioSettings();
        controls=mkwvc::embeddedVoiceControls();
    }

    ImGui::TextUnformatted(""Microphone test"");
    bool microphoneTest=controls.microphoneTest;
    if(ImGui::Checkbox(""Hear my processed microphone"",&microphoneTest)) {
        mkwvc::setEmbeddedVoiceMicrophoneTest(microphoneTest);
        controls=mkwvc::embeddedVoiceControls();
    }
    if(controls.microphoneTest) {
        ImGui::ProgressBar(
            std::clamp(static_cast<float>(controls.micPeak)/32768.0f,0.0f,1.0f),
            ImVec2(-1.0f,20.0f),"""");
        ImGui::TextDisabled(""Test mode never transmits your microphone."");
    }

    ImGui::SeparatorText(""Status"");
    ImGui::BeginDisabled(controls.pushToTalk);
    if(controls.microphoneMuted) {
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.68f,0.12f,0.12f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.82f,0.16f,0.16f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.55f,0.08f,0.08f,1.0f));
    }
    if(ImGui::Button(controls.microphoneMuted ? ""Unmute"" : ""Mute"")) {
        mkwvc::setEmbeddedVoiceMicrophoneMuted(!controls.microphoneMuted);
        controls=mkwvc::embeddedVoiceControls();
    }
    if(controls.microphoneMuted) ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    ImGui::SameLine();
    if(controls.deafened) {
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.68f,0.12f,0.12f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.82f,0.16f,0.16f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.55f,0.08f,0.08f,1.0f));
    }
    if(ImGui::Button(controls.deafened ? ""Undeafen"" : ""Deafen"")) {
        mkwvc::setEmbeddedVoiceDeafened(!controls.deafened);
        controls=mkwvc::embeddedVoiceControls();
    }
    if(controls.deafened) ImGui::PopStyleColor(3);

    ImGui::TextUnformatted(""Voice activity"");
    ImGui::ProgressBar(
        std::clamp(static_cast<float>(controls.micPeak)/32768.0f,0.0f,1.0f),
        ImVec2(-1.0f,20.0f),
        controls.localSpeaking ? ""Speaking"" : """");

    std::string overlayOptionsPreview=""Off"";
    if(controls.localStatusOverlayVisible && controls.playerSpeakersOverlayVisible) {
        overlayOptionsPreview=""Local status + player speakers"";
    } else if(controls.localStatusOverlayVisible) {
        overlayOptionsPreview=""Local status"";
    } else if(controls.playerSpeakersOverlayVisible) {
        overlayOptionsPreview=""Player speakers"";
    }

    ImGui::TextUnformatted(""Overlay options"");
    ImGui::SetNextItemWidth(-1.0f);
    if(ImGui::BeginCombo(""##VoiceOverlayOptions"",overlayOptionsPreview.c_str())) {
        bool localStatusVisible=controls.localStatusOverlayVisible;
        if(ImGui::Checkbox(""Bottom-right local status"",&localStatusVisible)) {
            mkwvc::setEmbeddedVoiceOverlayOptions(
                localStatusVisible,
                controls.playerSpeakersOverlayVisible,
                controls.playerSpeakerBackgroundTransparency);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool playerSpeakersVisible=controls.playerSpeakersOverlayVisible;
        if(ImGui::Checkbox(""Right-side player speakers"",&playerSpeakersVisible)) {
            mkwvc::setEmbeddedVoiceOverlayOptions(
                controls.localStatusOverlayVisible,
                playerSpeakersVisible,
                controls.playerSpeakerBackgroundTransparency);
            controls=mkwvc::embeddedVoiceControls();
        }

        int backgroundTransparency=static_cast<int>(
            controls.playerSpeakerBackgroundTransparency*100.0f+0.5f);
        ImGui::BeginDisabled(!controls.playerSpeakersOverlayVisible);
        ImGui::SetNextItemWidth(250.0f);
        if(ImGui::SliderInt(
               ""Player speaker BG transparency"",
               &backgroundTransparency,
               0,
               95,
               ""%d%%"")) {
            mkwvc::setEmbeddedVoiceOverlayOptions(
                controls.localStatusOverlayVisible,
                controls.playerSpeakersOverlayVisible,
                static_cast<float>(backgroundTransparency)/100.0f);
            controls=mkwvc::embeddedVoiceControls();
        }
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }

    std::string muteOptionsPreview=""None"";
    if(controls.muteEveryone) {
        muteOptionsPreview=""Mute everyone"";
    } else {
        std::vector<std::string> muteParts;
        if(controls.muteOnlyFriends) muteParts.emplace_back(""friends"");
        else if(controls.muteEveryoneButFriends) muteParts.emplace_back(""everyone but friends"");
        if(controls.muteTeammates) muteParts.emplace_back(""teammates"");
        else if(controls.muteEveryoneButTeammates) muteParts.emplace_back(""everyone but teammates"");
        if(controls.muteNewPlayers) muteParts.emplace_back(""new players"");

        if(!muteParts.empty()) {
            muteOptionsPreview=""Mute "";
            for(std::size_t i=0;i<muteParts.size();++i) {
                if(i>0) muteOptionsPreview+="" + "";
                muteOptionsPreview+=muteParts[i];
            }
        }
    }

    ImGui::TextUnformatted(""Mute options"");
    ImGui::SetNextItemWidth(-1.0f);
    if(ImGui::BeginCombo(""##VoiceMuteOptions"",muteOptionsPreview.c_str())) {
        bool muteEveryone=controls.muteEveryone;
        if(ImGui::Checkbox(""Mute everyone"",&muteEveryone)) {
            mkwvc::setEmbeddedVoiceMutePolicy(
                muteEveryone,
                false,
                false,
                false,
                false,
                muteEveryone ? false : controls.muteNewPlayers);
            controls=mkwvc::embeddedVoiceControls();
        }

        ImGui::BeginDisabled(controls.muteEveryone);

        bool muteOnlyFriends=controls.muteOnlyFriends;
        if(ImGui::Checkbox(""Mute only friends"",&muteOnlyFriends)) {
            mkwvc::setEmbeddedVoiceMutePolicy(
                false,
                muteOnlyFriends,
                muteOnlyFriends ? false : controls.muteEveryoneButFriends,
                controls.muteTeammates,
                controls.muteEveryoneButTeammates,
                controls.muteNewPlayers);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool muteEveryoneButFriends=controls.muteEveryoneButFriends;
        if(ImGui::Checkbox(""Mute everyone but friends"",&muteEveryoneButFriends)) {
            mkwvc::setEmbeddedVoiceMutePolicy(
                false,
                muteEveryoneButFriends ? false : controls.muteOnlyFriends,
                muteEveryoneButFriends,
                controls.muteTeammates,
                controls.muteEveryoneButTeammates,
                controls.muteNewPlayers);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool muteTeammates=controls.muteTeammates;
        if(ImGui::Checkbox(""Mute teammates"",&muteTeammates)) {
            mkwvc::setEmbeddedVoiceMutePolicy(
                false,
                controls.muteOnlyFriends,
                controls.muteEveryoneButFriends,
                muteTeammates,
                muteTeammates ? false : controls.muteEveryoneButTeammates,
                controls.muteNewPlayers);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool muteEveryoneButTeammates=controls.muteEveryoneButTeammates;
        if(ImGui::Checkbox(""Mute everyone but teammates"",&muteEveryoneButTeammates)) {
            mkwvc::setEmbeddedVoiceMutePolicy(
                false,
                controls.muteOnlyFriends,
                controls.muteEveryoneButFriends,
                muteEveryoneButTeammates ? false : controls.muteTeammates,
                muteEveryoneButTeammates,
                controls.muteNewPlayers);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool muteNewPlayers=controls.muteNewPlayers;
        if(ImGui::Checkbox(""Mute new players joining"",&muteNewPlayers)) {
            mkwvc::setEmbeddedVoiceMutePolicy(
                false,
                controls.muteOnlyFriends,
                controls.muteEveryoneButFriends,
                controls.muteTeammates,
                controls.muteEveryoneButTeammates,
                muteNewPlayers);
            controls=mkwvc::embeddedVoiceControls();
        }

        if(!controls.teamModeActive) {
            ImGui::TextDisabled(""Team mute options activate automatically in team modes."");
        }

        ImGui::EndDisabled();
        ImGui::EndCombo();
    }

    if(ImGui::CollapsingHeader(""Advanced"")) {
        ImGui::Indent();

        bool normalization=controls.automaticNormalization;
        if(ImGui::Checkbox(""Automatic normalization"",&normalization)) {
            mkwvc::setEmbeddedVoiceProcessing(normalization,controls.noiseSuppression,controls.noiseSuppressionStrength);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool noiseSuppression=controls.noiseSuppression;
        if(ImGui::Checkbox(""Noise suppression"",&noiseSuppression)) {
            mkwvc::setEmbeddedVoiceProcessing(controls.automaticNormalization,noiseSuppression,controls.noiseSuppressionStrength);
            controls=mkwvc::embeddedVoiceControls();
        }
        if(controls.noiseSuppression) {
            int noiseStrength=controls.noiseSuppressionStrength;
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::SliderInt(""##VoiceNoiseStrength"",&noiseStrength,0,100,""Noise suppression %d%%"")) {
                mkwvc::setEmbeddedVoiceProcessing(controls.automaticNormalization,controls.noiseSuppression,noiseStrength);
                controls=mkwvc::embeddedVoiceControls();
            }
        }

        bool noiseGate=controls.noiseGate;
        if(ImGui::Checkbox(""Noise gate"",&noiseGate)) {
            mkwvc::setEmbeddedVoiceAdvancedProcessing(
                noiseGate,controls.noiseGateThreshold,controls.microphoneBoost,
                controls.compressor,controls.compressorStrength);
            controls=mkwvc::embeddedVoiceControls();
        }
        if(controls.noiseGate) {
            int gateThreshold=controls.noiseGateThreshold;
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::SliderInt(""##VoiceGateThreshold"",&gateThreshold,0,100,""Gate threshold %d%%"")) {
                mkwvc::setEmbeddedVoiceAdvancedProcessing(
                    controls.noiseGate,gateThreshold,controls.microphoneBoost,
                    controls.compressor,controls.compressorStrength);
                controls=mkwvc::embeddedVoiceControls();
            }
        }

        int microphoneBoost=static_cast<int>(controls.microphoneBoost*100.0f+0.5f);
        ImGui::SetNextItemWidth(-1.0f);
        if(ImGui::SliderInt(""##VoiceMicBoost"",&microphoneBoost,100,300,""Mic boost %d%%"")) {
            mkwvc::setEmbeddedVoiceAdvancedProcessing(
                controls.noiseGate,controls.noiseGateThreshold,
                static_cast<float>(microphoneBoost)/100.0f,
                controls.compressor,controls.compressorStrength);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool compressor=controls.compressor;
        if(ImGui::Checkbox(""Compressor"",&compressor)) {
            mkwvc::setEmbeddedVoiceAdvancedProcessing(
                controls.noiseGate,controls.noiseGateThreshold,controls.microphoneBoost,
                compressor,controls.compressorStrength);
            controls=mkwvc::embeddedVoiceControls();
        }
        if(controls.compressor) {
            int compressorStrength=controls.compressorStrength;
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::SliderInt(""##VoiceCompressorStrength"",&compressorStrength,0,100,""Compression %d%%"")) {
                mkwvc::setEmbeddedVoiceAdvancedProcessing(
                    controls.noiseGate,controls.noiseGateThreshold,controls.microphoneBoost,
                    controls.compressor,compressorStrength);
                controls=mkwvc::embeddedVoiceControls();
            }
        }

        ImGui::TextUnformatted(""Microphone gain"");
        int microphoneGain=static_cast<int>(controls.microphoneGain*100.0f+0.5f);
        ImGui::SetNextItemWidth(-1.0f);
        if(ImGui::SliderInt(""##VoiceMicrophoneGain"",&microphoneGain,25,500,""%d%%"")) {
            mkwvc::setEmbeddedVoiceMicrophoneGain(static_cast<float>(microphoneGain)/100.0f);
            controls=mkwvc::embeddedVoiceControls();
        }

        bool voiceActivation=controls.voiceActivation;
        if(ImGui::Checkbox(""Voice activation"",&voiceActivation)) {
            mkwvc::setEmbeddedVoiceVoiceActivation(voiceActivation,controls.voiceActivationThreshold);
            controls=mkwvc::embeddedVoiceControls();
        }
        if(controls.voiceActivation) {
            int activationThreshold=controls.voiceActivationThreshold;
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::SliderInt(""##VoiceActivationThreshold"",&activationThreshold,1,100,""Activation threshold %d%%"")) {
                mkwvc::setEmbeddedVoiceVoiceActivation(true,activationThreshold);
                controls=mkwvc::embeddedVoiceControls();
            }
        }

        const bool pttBound=
            controls.pushToTalkBinding.keyboard>=0 ||
            !controls.pushToTalkBinding.controller.empty();
        bool pushToTalk=controls.pushToTalk;
        ImGui::BeginDisabled(!pttBound && !controls.pushToTalk);
        if(ImGui::Checkbox(""Push-to-talk"",&pushToTalk)) {
            mkwvc::setEmbeddedVoicePushToTalk(pushToTalk);
            controls=mkwvc::embeddedVoiceControls();
        }
        ImGui::EndDisabled();
        if(!pttBound) ImGui::TextDisabled(""Bind Push-to-talk in Hotkeys before enabling it."");

        bool pushToMute=controls.pushToMute;
        if(ImGui::Checkbox(""Push-to-mute"",&pushToMute)) {
            mkwvc::setEmbeddedVoicePushToMute(pushToMute);
            controls=mkwvc::embeddedVoiceControls();
        }

        ImGui::Unindent();
    }

    if(ImGui::CollapsingHeader(""Hotkeys"")) {
        ImGui::Indent();
        DrawVoiceBindingRow(""Push-to-talk"",mkwvc::EmbeddedVoiceBindingAction::PushToTalk,controls.pushToTalkBinding);
        DrawVoiceBindingRow(""Push-to-mute"",mkwvc::EmbeddedVoiceBindingAction::PushToMute,controls.pushToMuteBinding);
        DrawVoiceBindingRow(""Mute"",mkwvc::EmbeddedVoiceBindingAction::ToggleMute,controls.muteBinding);
        DrawVoiceBindingRow(""Deafen"",mkwvc::EmbeddedVoiceBindingAction::ToggleDeafen,controls.deafenBinding);

        const std::array<mkwvc::EmbeddedVoiceBinding,4> bindings={
            controls.pushToTalkBinding,controls.pushToMuteBinding,controls.muteBinding,controls.deafenBinding};
        bool conflict=false;
        for(std::size_t i=0;i<bindings.size();++i) {
            for(std::size_t j=i+1;j<bindings.size();++j) {
                if(VoiceBindingConflict(bindings[i],bindings[j])) conflict=true;
            }
        }
        if(conflict) ImGui::TextWrapped(""Warning: two or more Voice Chat actions use the same binding."");
        ImGui::Unindent();
    }

    ImGui::SeparatorText(""Voice room"");
    if(controls.peers.empty()) {
        ImGui::TextDisabled(session.voiceClientRunning ? ""No Voice Chat peer connected."" : ""Not connected."");
    } else if(ImGui::BeginTable(""EmbeddedVoicePeerList"",4,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(""Player"",ImGuiTableColumnFlags_WidthStretch,1.5f);
        ImGui::TableSetupColumn(""Status"",ImGuiTableColumnFlags_WidthStretch,1.0f);
        ImGui::TableSetupColumn(""Volume"",ImGuiTableColumnFlags_WidthStretch,1.5f);
        ImGui::TableSetupColumn(""##VoiceMute"",ImGuiTableColumnFlags_WidthFixed,72.0f);
        for(const auto& peer:controls.peers) {
            ImGui::PushID(peer.memberId.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string peerName=peer.displayName.empty() ? playerNameFor(peer.participantId) : peer.displayName;
            const std::string peerFriendCode=peer.friendCode.empty() ? playerFriendCodeFor(peer.participantId) : peer.friendCode;
            ImGui::TextUnformatted(peerName.c_str());
            if(!peerFriendCode.empty()) {
                ImGui::SameLine(0.0f,4.0f);
                ImGui::TextDisabled(""[%s]"",peerFriendCode.c_str());
            }
            if(peer.openHost) {
                ImGui::SameLine(0.0f,4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.82f,0.18f,1.0f));
                ImGui::TextUnformatted(""[OPEN HOST]"");
                ImGui::PopStyleColor();
            }

            ImGui::TableNextColumn();
            if(peer.policyMuted) ImGui::TextDisabled(""Auto-muted"");
            else if(peer.remoteDeafened) ImGui::TextUnformatted(""Deafened"");
            else if(peer.remoteMuted) ImGui::TextUnformatted(""Muted"");
            else if(peer.speaking) ImGui::TextUnformatted(""Speaking"");
            else ImGui::TextDisabled(""Silent"");

            ImGui::TableNextColumn();
            int peerVolume=peer.policyMuted
                ? 0
                : static_cast<int>(peer.volume*100.0f+0.5f);
            ImGui::BeginDisabled(peer.policyMuted);
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::SliderInt(""##VoicePeerVolume"",&peerVolume,0,300,""%d%%"")) {
                mkwvc::setEmbeddedVoicePeerVolume(peer.memberId,static_cast<float>(peerVolume)/100.0f);
            }
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            if(peer.policyMuted) {
                ImGui::BeginDisabled();
                ImGui::Button(""Auto"",ImVec2(-1.0f,0.0f));
                ImGui::EndDisabled();
            } else {
                const bool peerMuted=peer.volume<=0.0f;
                if(ImGui::Button(peerMuted ? ""Unmute"" : ""Mute"",ImVec2(-1.0f,0.0f))) {
                    mkwvc::setEmbeddedVoicePeerVolume(peer.memberId,peerMuted ? 1.0f : 0.0f);
                    controls=mkwvc::embeddedVoiceControls();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if(room.roomFound) {
        ImGui::TextDisabled(""Retro Rewind room: %zu player%s"",room.players.size(),room.players.size()==1 ? """" : ""s"");
        for(const auto& player:room.players) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(player.name.c_str());
            if(!player.friendCode.empty()) {
                ImGui::SameLine(0.0f,4.0f);
                ImGui::TextDisabled(""[%s]"",player.friendCode.c_str());
            }
            if(player.openHost) {
                ImGui::SameLine(0.0f,4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.82f,0.18f,1.0f));
                ImGui::TextUnformatted(""[OPEN HOST]"");
                ImGui::PopStyleColor();
            }
            if(player.voiceChat) {
                ImGui::SameLine(0.0f,4.0f);
                ImGui::TextDisabled(""[Voice Chat]"");
            }
        }
    }

    bool openHost=controls.openHost;
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.82f,0.18f,1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(1.0f,0.82f,0.18f,1.0f));
    if(ImGui::Checkbox(""Open Host"",&openHost)) {
        mkwvc::setEmbeddedVoiceOpenHost(openHost);
        RetroRewindVoiceBridge::RequestOpenHostOverrideRefresh();
        controls=mkwvc::embeddedVoiceControls();
    }
    const bool openHostHovered=ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    if(openHostHovered) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize()*32.0f);
        ImGui::TextUnformatted(
            ""Overrides your native Open Host setting while the MKW Voice Chat build is running. ""
            ""Your original setting is preserved separately and is not overwritten. ""
            ""This helps make Voice Chat more accessible by making it easier for Voice Chat users ""
            ""who don't know each other to meet and join each other's rooms."");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImVec4(1.0f,0.82f,0.18f,1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_Header,
        ImVec4(0.48f,0.33f,0.03f,0.78f));
    ImGui::PushStyleColor(
        ImGuiCol_HeaderHovered,
        ImVec4(0.62f,0.43f,0.04f,0.88f));
    ImGui::PushStyleColor(
        ImGuiCol_HeaderActive,
        ImVec4(0.74f,0.52f,0.05f,0.94f));
    const bool onlineUsersOpen=
        ImGui::CollapsingHeader(""Online Voice Chat users"");
    ImGui::PopStyleColor(4);

    if(onlineUsersOpen) {
        ImGui::Indent();
        if(session.onlineUsers.empty()) {
            ImGui::TextDisabled(
                ""No online users currently have a resolved license and Friend Code."");
        } else {
            for(const auto& user:session.onlineUsers) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextUnformatted(user.displayName.c_str());
                ImGui::SameLine(0.0f,4.0f);
                ImGui::TextDisabled(""[%s]"",user.friendCode.c_str());
                if(user.openHost) {
                    ImGui::SameLine(0.0f,4.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.82f,0.18f,1.0f));
                    ImGui::TextUnformatted(""[OPEN HOST]"");
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::Unindent();
    }

    if(ImGui::CollapsingHeader(""Debug"")) {
        ImGui::Indent();

        if(!controls.error.empty()) ImGui::TextWrapped(""Voice error: %s"",controls.error.c_str());

        ImGui::SeparatorText(""Live GPCM identity"");
        ImGui::Text(""Online: %s"",identity.online ? ""Yes"" : ""No"");
        ImGui::Text(""Profile ID: %s"",identity.profileId.empty() ? ""-"" : identity.profileId.c_str());
        ImGui::Text(""Session key: %s"",identity.sessionKey.empty() ? ""Not captured"" : ""Captured (hidden)"");
        ImGui::Text(""Game name: %s"",identity.gameName.empty() ? ""-"" : identity.gameName.c_str());

        const mkwvc::EmbeddedCoreStatus core=mkwvc::embeddedCoreStatus();
        ImGui::SeparatorText(""Embedded voice core"");
        ImGui::Text(""VoiceClient source: %s"",core.voiceClientCompiled ? ""Compiled"" : ""Missing"");
        ImGui::Text(""Audio / Opus deps: %s"",core.audioDependenciesLinked ? ""Linked"" : ""Pending"");
        ImGui::Text(""ICE / libdatachannel deps: %s"",core.iceDependenciesLinked ? ""Linked"" : ""Pending"");
        ImGui::TextDisabled(""%u Hz mono, %u ms frames (%u samples)"",
                            core.sampleRate,core.frameDurationMs,core.frameSamples);

        ImGui::SeparatorText(""In-process voice session"");
        ImGui::Text(""Lifecycle: %s"",session.lifecycleActive ? ""Active"" : ""Inactive"");
        ImGui::Text(""Core signaling: %s"",session.signalingConnected ? ""Connected"" : ""Not connected"");
        ImGui::Text(""Dev roster admission: %s"",
                    session.developmentAdmitted ? ""Admitted (UNVERIFIED)"" :
                    (session.developmentAdmissionPending ? ""Waiting"" : ""Blocked""));
        ImGui::Text(""VoiceClient: %s"",session.voiceClientRunning ? ""Running"" : ""Stopped"");
        ImGui::Text(""Voice peers: %u"",session.peerCount);
        ImGui::Text(""Session: %s"",session.status.empty() ? ""-"" : session.status.c_str());

        ImGui::SeparatorText(""Room diagnostics"");
        ImGui::Text(""Local RKNet room: %s"",room.localRoomActive ? ""Active"" : ""Inactive"");
        ImGui::Text(""Signaling socket: %s"",room.signalingConnected ? ""Connected"" : ""Not connected"");
        ImGui::Text(""Lookup: %s"",room.status.empty() ? ""-"" : room.status.c_str());
        std::string localRoomPids;
        for(std::size_t i=0;i<room.localRoomProfileIds.size();++i) {
            if(i>0) localRoomPids+="", "";
            localRoomPids+=room.localRoomProfileIds[i];
        }
        ImGui::TextWrapped(
            ""Local RKNet PIDs: %s"",
            localRoomPids.empty() ? ""-"" : localRoomPids.c_str());
        ImGui::Text(""Room ID: %s"",room.roomId.empty() ? ""-"" : room.roomId.c_str());
        ImGui::Text(""Room instance: %s"",room.roomInstanceId.empty() ? ""-"" : room.roomInstanceId.c_str());

        ImGui::Unindent();
    }
}


";

        text = ReplaceOnce(
            text,
            "void DrawAudioSettings() {\n",
            voiceUi + "void DrawAudioSettings() {\n",
            "settings voice UI");

        text = ReplaceOnce(
            text,
            "    const std::string audioLabel = g_audioMuted\n",
            "    if (RuntimeProduct::IsRetroRewind()) {\n" +
            "        const float voiceMenuAvailable=ImGui::GetMainViewport()->WorkSize.x-24.0f;\n" +
            "        const float voiceMenuWidth=voiceMenuAvailable<280.0f ? 280.0f : (voiceMenuAvailable>560.0f ? 560.0f : voiceMenuAvailable);\n" +
            "        ImGui::SetNextWindowSizeConstraints(\n" +
            "            ImVec2(voiceMenuWidth, 0.0f),\n" +
            "            ImVec2(voiceMenuWidth, ImGui::GetMainViewport()->WorkSize.y));\n" +
            "        if (ImGui::BeginMenu(\"Voice Chat\")) {\n" +
            "            DrawVoiceChatSettings();\n" +
            "            ImGui::EndMenu();\n" +
            "        }\n" +
            "    }\n\n" +
            "    const std::string audioLabel = g_audioMuted\n",
            "settings Voice Chat menu");

        text = ReplaceOnce(
            text,
            "    UpdateCursorAutoHide();\n    UpdateBootShaderState();\n    if (!StartupScreenVisible()) {\n",
            "    UpdateCursorAutoHide();\n" +
            "    UpdateBootShaderState();\n" +
            "    if (RuntimeProduct::IsRetroRewind()) {\n" +
            "        RetroRewindVoiceBridge::ServiceRoomLookup();\n" +
            "        ServiceVoiceHotkeys();\n" +
            "    }\n" +
            "    if (!StartupScreenVisible()) {\n",
            "settings room service");

        text = ReplaceOnce(
            text,
            "    DrawFpsOverlay();\n    DrawTopBar();\n",
            "    DrawFpsOverlay();\n" +
            "    if (RuntimeProduct::IsRetroRewind()) {\n" +
            "        DrawVoiceChatOverlay();\n" +
            "    }\n" +
            "    DrawTopBar();\n",
            "settings voice overlay draw");

        WriteUtf8(path, text);
    }

    private static string ReplaceOnce(
        string text,
        string oldValue,
        string newValue,
        string label)
    {
        var first = text.IndexOf(oldValue, StringComparison.Ordinal);
        if (first < 0)
            throw new InvalidDataException(
                $"Voice patch anchor '{label}' does not match this WiiCompiled workspace.");

        var second = text.IndexOf(
            oldValue,
            first + oldValue.Length,
            StringComparison.Ordinal);
        if (second >= 0)
            throw new InvalidDataException(
                $"Voice patch anchor '{label}' is ambiguous in this WiiCompiled workspace.");

        return text[..first] + newValue + text[(first + oldValue.Length)..];
    }

    private static string ReadUtf8(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("Required WiiCompiled source file is missing.", path);

        return File.ReadAllText(path).Replace("\r\n", "\n");
    }

    private static void WriteUtf8(string path, string text)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrWhiteSpace(directory))
            Directory.CreateDirectory(directory);

        // The upstream workspace is text source; use UTF-8 without a BOM for
        // deterministic payload hashes. CMake/C++ accept this identically.
        File.WriteAllText(path, text, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private static void WriteEmbedded(string path, string resourceName)
    {
        var assembly = Assembly.GetExecutingAssembly();
        using var input = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidDataException(
                $"Embedded voice patch payload is missing: {resourceName}");
        using var reader = new StreamReader(input, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        var text = reader.ReadToEnd().Replace("\r\n", "\n")
            .Replace("@MKWVC_PRODUCT_VERSION@", BuildVersion.ProductVersionText, StringComparison.Ordinal)
            .Replace("@MKWVC_PATCH_REVISION@", BuildVersion.PatchRevision, StringComparison.Ordinal)
            .Replace("@MKWVC_PROTOCOL_VERSION@", BuildVersion.ProtocolVersion.ToString(), StringComparison.Ordinal)
            .Replace("@MKWVC_WIICOMPILED_VERSION@", BuildVersion.WiiCompiledVersionText, StringComparison.Ordinal);

        if (text.Contains("@MKWVC_", StringComparison.Ordinal))
            throw new InvalidDataException(
                $"Embedded voice patch payload contains an unresolved version token: {resourceName}");

        WriteUtf8(path, text);
    }

    private static string UnderWorkspace(string workspace, string relative)
    {
        var root = Path.GetFullPath(workspace)
            .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var full = Path.GetFullPath(Path.Combine(workspace, relative));
        if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"Patch path escapes BuildWorkspace: {relative}");
        return full;
    }

}
