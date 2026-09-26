using System.Text;

namespace MKWVoiceChat.Patcher;

internal static class InstallerLog
{
    private const int MaxLogs = 20;
    private static readonly object Gate = new();
    private static StreamWriter? _writer;
    private static string? _path;

    public static string? CurrentPath => _path;

    public static void Start(string[] args)
    {
        lock (Gate)
        {
            if (_writer is not null) return;

            var root = ResolveLogRoot();
            Directory.CreateDirectory(root);
            Prune(root);

            var stamp = DateTimeOffset.Now.ToString("yyyyMMdd-HHmmss-fff");
            _path = Path.Combine(root, $"installer-{stamp}-p{Environment.ProcessId}.log");
            var stream = new FileStream(
                _path, FileMode.CreateNew, FileAccess.Write, FileShare.Read,
                16 * 1024, FileOptions.WriteThrough);
            _writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };

            AppDomain.CurrentDomain.ProcessExit += (_, _) => Close();
            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            {
                if (e.ExceptionObject is Exception ex) Exception("UNHANDLED", ex);
            };

            WriteLocked("START", $"UTC={DateTimeOffset.UtcNow:O}");
            WriteLocked("START", $"MKWVC={Safe(() => BuildVersion.ProductVersionText)} patch={Safe(() => BuildVersion.PatchRevision)}");
            WriteLocked("START", $"PID={Environment.ProcessId} args={FormatArgs(args)}");
        }
    }

    public static void Command(string command) => Write("COMMAND", command);

    public static void Write(string scope, string message)
    {
        lock (Gate) WriteLocked(scope, message);
    }

    public static void Exception(string scope, Exception ex) => Write(scope, ex.ToString());

    public static void ProcessStart(string fileName, IEnumerable<string> arguments, string? workingDirectory) =>
        Write("PROCESS", $"start file={fileName} cwd={workingDirectory ?? ""} args={FormatArgs(arguments)}");

    public static void ProcessExit(string fileName, int exitCode) =>
        Write("PROCESS", $"exit file={fileName} code={exitCode}");

    private static string ResolveLogRoot()
    {
        try
        {
            var layout = PatcherPaths.Discover();
            return Path.Combine(PatchStateStore.Root(layout), "logs");
        }
        catch
        {
            return Path.Combine(Path.GetTempPath(), "MKWVoiceChat", "logs");
        }
    }

    private static void Prune(string root)
    {
        try
        {
            foreach (var file in new DirectoryInfo(root).EnumerateFiles("*.log")
                         .OrderByDescending(f => f.LastWriteTimeUtc).Skip(MaxLogs - 1))
            {
                try { file.Delete(); } catch { }
            }
        }
        catch { }
    }

    private static string FormatArgs(IEnumerable<string> args) =>
        string.Join(" ", args.Select(arg => arg.Any(char.IsWhiteSpace) ? $"\"{arg}\"" : arg));

    private static string Safe(Func<string> value)
    {
        try { return value(); } catch { return "unknown"; }
    }

    private static void WriteLocked(string scope, string message)
    {
        if (_writer is null) return;
        foreach (var line in message.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n'))
            _writer.WriteLine($"{DateTimeOffset.Now:O} [{scope}] {line}");
        _writer.Flush();
        if (_writer.BaseStream is FileStream file)
            file.Flush(flushToDisk: true);
    }

    private static void Close()
    {
        lock (Gate)
        {
            try
            {
                _writer?.Flush();
                if (_writer?.BaseStream is FileStream file)
                    file.Flush(flushToDisk: true);
                _writer?.Dispose();
            }
            catch { }
            finally { _writer = null; }
        }
    }
}
