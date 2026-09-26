using System.Diagnostics;

namespace MKWVoiceChat.Patcher;

internal sealed record ProcessResult(int ExitCode, string Stdout, string Stderr)
{
    public string Combined => string.Join(
        Environment.NewLine,
        new[] { Stdout, Stderr }.Where(value => !string.IsNullOrWhiteSpace(value)));
}

internal static class ProcessUtil
{
    public static async Task<ProcessResult> RunAsync(
        string fileName,
        IEnumerable<string> arguments,
        string? workingDirectory = null,
        bool echoOutput = false,
        Action<string>? stdoutLine = null,
        Action<string>? stderrLine = null,
        IReadOnlyDictionary<string, string>? environment = null,
        CancellationToken cancellationToken = default)
    {
        var start = new ProcessStartInfo
        {
            FileName = fileName,
            WorkingDirectory = workingDirectory ?? string.Empty,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };

        var argumentList = arguments.ToList();
        foreach (var argument in argumentList)
            start.ArgumentList.Add(argument);

        InstallerLog.ProcessStart(fileName, argumentList, workingDirectory);

        if (environment is not null)
        {
            foreach (var pair in environment)
                start.Environment[pair.Key] = pair.Value;
        }

        using var process = Process.Start(start)
            ?? throw new InvalidOperationException($"Failed to start {fileName}.");

        var stdout = new List<string>();
        var stderr = new List<string>();

        var readStdout = ReadLinesAsync(
            process.StandardOutput,
            stdout,
            echoOutput ? Console.Out : null,
            stdoutLine,
            "CHILD-STDOUT");
        var readStderr = ReadLinesAsync(
            process.StandardError,
            stderr,
            echoOutput ? Console.Error : null,
            stderrLine,
            "CHILD-STDERR");

        await process.WaitForExitAsync(cancellationToken);
        await Task.WhenAll(readStdout, readStderr);

        InstallerLog.ProcessExit(fileName, process.ExitCode);
        return new(
            process.ExitCode,
            string.Join(Environment.NewLine, stdout),
            string.Join(Environment.NewLine, stderr));
    }

    private static async Task ReadLinesAsync(
        StreamReader reader,
        List<string> destination,
        TextWriter? echo,
        Action<string>? observer,
        string logScope)
    {
        string? line;
        while ((line = await reader.ReadLineAsync()) is not null)
        {
            destination.Add(line);
            InstallerLog.Write(logScope, line);
            observer?.Invoke(line);
            if (echo is not null)
                await echo.WriteLineAsync(line);
        }
    }
}
