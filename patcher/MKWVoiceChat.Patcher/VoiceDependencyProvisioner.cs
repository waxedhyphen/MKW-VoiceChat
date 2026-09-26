using System.Formats.Tar;
using System.IO.Compression;
using System.Security.Cryptography;

namespace MKWVoiceChat.Patcher;

internal sealed record VoiceDependencyLayout(string Root);

internal static class VoiceDependencyProvisioner
{
    private sealed record DependencySpec(
        string Key,
        string Version,
        string Url,
        string Sha512,
        string DestinationRelative,
        string RequiredPath);

    // Versions and archive hashes are pinned to the same upstream artifacts
    // currently used by vcpkg. Ordinary users do not need vcpkg: the installer
    // downloads and verifies the source archives itself, then WiiCompiled's
    // bundled LLVM-MinGW toolchain compiles them locally.
    private static readonly DependencySpec[] Specs =
    [
        new(
            "opus",
            "1.6.1",
            "https://github.com/xiph/opus/archive/v1.6.1.tar.gz",
            "b9a504f8576c977df57caee808412ab95e7424e9a64e3dd4045f05616c3ed58706bc3549195b61e25721e299547509d15206d246d96f1a307b82562b703b412c",
            "opus",
            "CMakeLists.txt"),
        new(
            "speexdsp",
            "1.2.1",
            "https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz",
            "41b5f37b48db5cb8c5a0f6437a4a8266d2627a5b7c1088de8549fe0bf0bb3105b7df8024fe207eef194096e0726ea73e2b53e0a4293d8db8e133baa0f8a3bad3",
            "speexdsp",
            @"libspeexdsp\preprocess.c"),
        new(
            "miniaudio",
            "0.11.25",
            "https://github.com/mackron/miniaudio/archive/0.11.25.tar.gz",
            "8cdfe5cd66dd84628430a24026b307c21158b4776492eec234c2ce3cf0da3ae26fe8162f3ed285502f6002fdf252ccb660f7c216e044e3c306b75b0997700b45",
            "miniaudio",
            "miniaudio.h"),

        // Stage 3: libdatachannel plus the exact dependency family required for
        // WebSocket signaling + WebRTC DataChannels, but NO_MEDIA=ON so we do
        // not pull libSRTP. All versions/hashes match current vcpkg sources.
        new(
            "libdatachannel",
            "0.24.5",
            "https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.5.tar.gz",
            "694561ba5b3e08ed35e7e167330d97455ee2ef8d298c9c41e12de4d07032bbe1bb2ebec1e35126187c57ef9492e7d1c82ffd0fa3511eabcdfb77efabfd4b7d9a",
            "libdatachannel",
            "CMakeLists.txt"),
        new(
            "libjuice",
            "1.7.2",
            "https://github.com/paullouisageneau/libjuice/archive/refs/tags/v1.7.2.tar.gz",
            "770b7123949a644ab8df01020abb2c8744496e1e486c91292252bb309a8c38239ae2a6c369b4eb0c028f6ac0ea78e5da09922d0ddf8446e50b4eb299cf1dabca",
            @"libdatachannel\deps\libjuice",
            "CMakeLists.txt"),
        new(
            "usrsctp",
            "0.9.5.0",
            "https://github.com/sctplab/usrsctp/archive/refs/tags/0.9.5.0.tar.gz",
            "7b28706449f9365ba9750fd39925e7171516a1e3145d123ec69a12486637ae2393ad4c587b056403298dc13c149f0b01a262cbe4852abca42e425d7680c77ee3",
            @"libdatachannel\deps\usrsctp",
            "CMakeLists.txt"),
        new(
            "plog",
            "1.1.11",
            "https://github.com/SergiusTheBest/plog/archive/refs/tags/1.1.11.tar.gz",
            "b51b83a2b478a54d83333590a4f157e3fdeea08903486249d537811afef370ce9968197efb534f2b4084a5a7a7253e5e2d7e191d602451ea625d645a39f195dc",
            @"libdatachannel\deps\plog",
            "CMakeLists.txt"),
        new(
            "mbedtls",
            "3.6.5",
            "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.5.tar.gz",
            "d7a1e0098fed7b000ac2e4de31d43f427e8a046aeace91719f58222e1289470e15af5ed2a5390cf3693cf93a1efd79f34de9a6a960dc63cc0fd135072809e6e4",
            "mbedtls",
            "CMakeLists.txt")
    ];

    public static async Task<VoiceDependencyLayout> EnsureAsync(
        HttpClient http,
        PatcherLayout layout,
        CancellationToken cancellationToken = default)
    {
        var root = Path.Combine(PatchStateStore.Root(layout), "Dependencies");
        var cache = Path.Combine(layout.CacheRoot, "MKWVoiceChat", "VoiceDependencies");
        Directory.CreateDirectory(root);
        Directory.CreateDirectory(cache);

        foreach (var spec in Specs)
        {
            var destination = Path.Combine(
                root,
                spec.DestinationRelative);
            if (IsReady(destination, spec))
            {
                // Preparation fixes are intentionally idempotent so an
                // installer update can normalize already-cached source trees
                // without forcing another network download.
                PrepareExtractedSource(destination, spec);
                continue;
            }

            var archive = Path.Combine(
                cache,
                $"{spec.Key}-{spec.Version}.tar.gz");

            if (!File.Exists(archive) ||
                !HashMatches(archive, spec.Sha512))
            {
                if (File.Exists(archive))
                    File.Delete(archive);

                await DownloadAsync(
                    http,
                    spec,
                    archive,
                    cancellationToken);
            }

            if (!HashMatches(archive, spec.Sha512))
            {
                File.Delete(archive);
                throw new InvalidDataException(
                    $"Downloaded {spec.Key} {spec.Version} archive failed SHA-512 verification.");
            }

            await ExtractAsync(
                archive,
                destination,
                spec,
                cancellationToken);
        }

        return new(Path.GetFullPath(root));
    }

    private static bool IsReady(string destination, DependencySpec spec)
    {
        var marker = Path.Combine(destination, ".mkwvc-source-sha512");
        var required = Path.Combine(destination, spec.RequiredPath);
        if (!File.Exists(marker) || !File.Exists(required))
            return false;

        try
        {
            return string.Equals(
                File.ReadAllText(marker).Trim(),
                spec.Sha512,
                StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private static async Task DownloadAsync(
        HttpClient http,
        DependencySpec spec,
        string destination,
        CancellationToken cancellationToken)
    {
        ConsoleUi.BeginPhase(
            $"Downloading {spec.Key} {spec.Version} voice dependency...");

        var temp = destination + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using var response = await http.GetAsync(
                spec.Url,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken);
            response.EnsureSuccessStatusCode();

            var total = response.Content.Headers.ContentLength;
            await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);

            var buffer = new byte[1024 * 128];
            long copied = 0;
            await using (var output = new FileStream(
                temp,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                1024 * 128,
                useAsync: true))
            {
                while (true)
                {
                    var read = await input.ReadAsync(
                        buffer.AsMemory(0, buffer.Length),
                        cancellationToken);
                    if (read == 0)
                        break;

                    await output.WriteAsync(
                        buffer.AsMemory(0, read),
                        cancellationToken);
                    copied += read;

                    if (total is > 0)
                    {
                        ConsoleUi.ProgressMeasured(
                            copied,
                            total.Value,
                            $"Downloading {spec.Key} {spec.Version}");
                    }
                }

                await output.FlushAsync(cancellationToken);
            }

            // Windows will not rename/replace a file while the destination
            // temp handle is still open. Dispose the download stream first.
            File.Move(temp, destination, overwrite: true);
            ConsoleUi.Progress(
                100,
                $"Downloaded {spec.Key} {spec.Version}.");
        }
        finally
        {
            TryDeleteFile(temp);
        }
    }

    private static async Task ExtractAsync(
        string archivePath,
        string destination,
        DependencySpec spec,
        CancellationToken cancellationToken)
    {
        ConsoleUi.BeginPhase(
            $"Extracting {spec.Key} {spec.Version} voice dependency...");

        var staging = destination + ".extract-" + Guid.NewGuid().ToString("N");
        TryDeleteDirectory(staging);
        Directory.CreateDirectory(staging);

        try
        {
            await using var archive = new FileStream(
                archivePath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                1024 * 128,
                useAsync: true);
            var archiveLength = archive.Length;

            using var gzip = new GZipStream(
                archive,
                CompressionMode.Decompress,
                leaveOpen: true);
            using var tar = new TarReader(gzip, leaveOpen: true);

            TarEntry? entry;
            while ((entry = tar.GetNextEntry()) is not null)
            {
                cancellationToken.ThrowIfCancellationRequested();

                var name = entry.Name.Replace('\\', '/');
                var firstSlash = name.IndexOf('/');
                if (firstSlash < 0 || firstSlash == name.Length - 1)
                {
                    ReportExtractProgress(archive, archiveLength, spec);
                    continue;
                }

                var relative = name[(firstSlash + 1)..];
                if (string.IsNullOrWhiteSpace(relative))
                {
                    ReportExtractProgress(archive, archiveLength, spec);
                    continue;
                }

                var target = SafeUnder(staging, relative);
                if (entry.EntryType == TarEntryType.Directory)
                {
                    Directory.CreateDirectory(target);
                }
                else if (entry.DataStream is not null)
                {
                    var parent = Path.GetDirectoryName(target);
                    if (!string.IsNullOrWhiteSpace(parent))
                        Directory.CreateDirectory(parent);

                    await using var output = new FileStream(
                        target,
                        FileMode.Create,
                        FileAccess.Write,
                        FileShare.None,
                        1024 * 128,
                        useAsync: true);
                    await entry.DataStream.CopyToAsync(
                        output,
                        1024 * 128,
                        cancellationToken);
                }

                ReportExtractProgress(archive, archiveLength, spec);
            }

            var required = Path.Combine(staging, spec.RequiredPath);
            if (!File.Exists(required))
                throw new InvalidDataException(
                    $"Extracted {spec.Key} {spec.Version} source is missing {spec.RequiredPath}.");

            File.WriteAllText(
                Path.Combine(staging, ".mkwvc-source-sha512"),
                spec.Sha512 + Environment.NewLine);

            PrepareExtractedSource(staging, spec);

            TryDeleteDirectory(destination);
            var destinationParent = Path.GetDirectoryName(destination);
            if (!string.IsNullOrWhiteSpace(destinationParent))
                Directory.CreateDirectory(destinationParent);
            Directory.Move(staging, destination);
            ConsoleUi.Progress(
                100,
                $"Extracted {spec.Key} {spec.Version}.");
        }
        catch
        {
            TryDeleteDirectory(staging);
            throw;
        }
    }

    private static void PrepareExtractedSource(
        string staging,
        DependencySpec spec)
    {
        if (spec.Key.Equals("mbedtls", StringComparison.OrdinalIgnoreCase))
        {
            // The official Mbed TLS GitHub source archive does not include its
            // framework git submodule. This library-only build has GEN_FILES,
            // tests and programs disabled, so an empty framework project is
            // sufficient; CMake also suppresses the optional Python config probe.
            var framework = Path.Combine(staging, "framework");
            Directory.CreateDirectory(framework);
            File.WriteAllText(
                Path.Combine(framework, "CMakeLists.txt"),
                "# intentionally empty for MKW VoiceChat library-only build" +
                Environment.NewLine);
        }

        if (spec.Key.Equals("usrsctp", StringComparison.OrdinalIgnoreCase))
        {
            // usrsctp 0.9.5.0 declares CMake 3.0 compatibility, which CMake 4
            // rejects. Raise only the compatibility floor; no project behavior
            // changes are required.
            var cmake = Path.Combine(staging, "CMakeLists.txt");
            var cmakeText = File.ReadAllText(cmake);
            cmakeText = cmakeText.Replace(
                "cmake_minimum_required(VERSION 3.0)",
                "cmake_minimum_required(VERSION 3.5)",
                StringComparison.Ordinal);
            File.WriteAllText(cmake, cmakeText);

            // Match vcpkg's MinGW fix: LLVM-MinGW has stdint.h and must not
            // fall back to MSVC-specific __intN typedefs.
            var header = Path.Combine(staging, "usrsctplib", "usrsctp.h");
            var headerText = File.ReadAllText(header);
            headerText = headerText.Replace(
                "#if defined(_MSC_VER) && _MSC_VER >= 1600\n#include <stdint.h>\n#elif defined(SCTP_STDINT_INCLUDE)",
                "#if defined(_MSC_VER) && _MSC_VER >= 1600\n#include <stdint.h>\n#elif defined(__MINGW32__)\n#include <stdint.h>\n#elif defined(SCTP_STDINT_INCLUDE)",
                StringComparison.Ordinal);
            File.WriteAllText(header, headerText);

            // usrsctp deliberately omits its fallback lowercase min/max macros
            // for MinGW because classic MinGW normally inherits them from
            // Windows headers. WiiCompiled compiles with -DNOMINMAX, so that
            // assumption is false and multiple SCTP C files fail under C99.
            // Provide usrsctp's own portable macros whenever they are missing.
            var environmentHeader = Path.Combine(
                staging,
                "usrsctplib",
                "user_environment.h");
            var environmentText = File.ReadAllText(environmentHeader);
            environmentText = environmentText.Replace(
                "#if !defined(_MSC_VER) && !defined(__MINGW32__)\n#define min(a,b) (((a)>(b))?(b):(a))\n#define max(a,b) (((a)>(b))?(a):(b))\n#endif",
                "#ifndef min\n#define min(a,b) (((a)>(b))?(b):(a))\n#endif\n#ifndef max\n#define max(a,b) (((a)>(b))?(a):(b))\n#endif",
                StringComparison.Ordinal);
            File.WriteAllText(environmentHeader, environmentText);
        }

        if (spec.Key.Equals("libdatachannel", StringComparison.OrdinalIgnoreCase))
        {
            // We embed libdatachannel as a build-only subproject and never run
            // its install target. Its upstream install(EXPORT ...) validation
            // rejects our separately-built Mbed TLS target because that target
            // is intentionally not part of LibDataChannelTargets. Remove only
            // the package-install/export block; library targets and all runtime
            // behavior remain unchanged.
            var cmake = Path.Combine(staging, "CMakeLists.txt");
            var cmakeText = File.ReadAllText(cmake);
            const string marker = "# MKWVC_EMBEDDED_NO_INSTALL_EXPORT";
            if (!cmakeText.Contains(marker, StringComparison.Ordinal))
            {
                const string startNeedle =
                    "install(TARGETS datachannel EXPORT LibDataChannelTargets";
                const string endNeedle = "# Tests";

                var start = cmakeText.IndexOf(
                    startNeedle,
                    StringComparison.Ordinal);
                var end = start < 0
                    ? -1
                    : cmakeText.IndexOf(
                        endNeedle,
                        start,
                        StringComparison.Ordinal);

                if (start < 0 || end < 0)
                {
                    throw new InvalidDataException(
                        "libdatachannel install/export block could not be located for embedded-build normalization.");
                }

                cmakeText =
                    cmakeText[..start] +
                    marker + Environment.NewLine +
                    "# Installation/package-export rules are intentionally disabled." +
                    Environment.NewLine +
                    cmakeText[end..];
                File.WriteAllText(cmake, cmakeText);
            }
        }
    }

    private static void ReportExtractProgress(
        FileStream archive,
        long total,
        DependencySpec spec)
    {
        if (total <= 0)
            return;

        ConsoleUi.ProgressMeasured(
            Math.Min(archive.Position, total),
            total,
            $"Extracting {spec.Key} {spec.Version}");
    }

    private static string SafeUnder(string root, string relative)
    {
        var normalized = relative.Replace(
            '/',
            Path.DirectorySeparatorChar);
        var rootFull = Path.GetFullPath(root)
            .TrimEnd(Path.DirectorySeparatorChar) +
            Path.DirectorySeparatorChar;
        var full = Path.GetFullPath(Path.Combine(root, normalized));

        if (!full.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(
                $"Dependency archive entry escapes extraction root: {relative}");

        return full;
    }

    private static bool HashMatches(string path, string expected)
    {
        try
        {
            using var stream = File.OpenRead(path);
            var actual = Convert.ToHexString(
                SHA512.HashData(stream)).ToLowerInvariant();
            return string.Equals(
                actual,
                expected,
                StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
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
