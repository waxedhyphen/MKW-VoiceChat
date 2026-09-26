namespace MKWVoiceChat.Patcher;

internal sealed class PatcherOperationLock : IDisposable
{
    private readonly FileStream _stream;

    private PatcherOperationLock(FileStream stream)
    {
        _stream = stream;
    }

    public static PatcherOperationLock Acquire(PatcherLayout layout)
    {
        Directory.CreateDirectory(layout.RecompRoot);
        var path = Path.Combine(layout.RecompRoot, ".mkwvc-patcher.lock");

        try
        {
            var stream = new FileStream(
                path,
                FileMode.OpenOrCreate,
                FileAccess.ReadWrite,
                FileShare.None,
                bufferSize: 1,
                FileOptions.None);

            stream.SetLength(0);
            using (var writer = new StreamWriter(
                       stream,
                       System.Text.Encoding.UTF8,
                       bufferSize: 1024,
                       leaveOpen: true))
            {
                writer.WriteLine(Environment.ProcessId);
                writer.WriteLine(DateTimeOffset.UtcNow.ToString("O"));
                writer.Flush();
            }
            stream.Position = 0;
            return new(stream);
        }
        catch (IOException ex)
        {
            throw new InvalidOperationException(
                "Another Wiicompiled (Voicechat) install/update/launch operation is already running.",
                ex);
        }
    }

    public void Dispose() => _stream.Dispose();
}

internal static class RunningGameGuard
{
    public static void EnsureRetroRewindNotRunning()
    {
        try
        {
            if (System.Diagnostics.Process.GetProcessesByName("RetroRewind").Length != 0)
            {
                throw new InvalidOperationException(
                    "Retro Rewind is currently running. Close the game before installing or rebuilding Wiicompiled (Voicechat).");
            }
        }
        catch (InvalidOperationException)
        {
            throw;
        }
        catch
        {
            // Failure to enumerate processes is not proof that a game is
            // running. The official WiiCompiled setup has its own product lock
            // as a second guard for its installation.
        }
    }
}
