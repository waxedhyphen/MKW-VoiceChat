namespace MKWVoiceChat.Patcher;

internal static class ConsoleUi
{
    private static readonly object Gate = new();
    private static int _lastPercent = -1;
    private static string _lastMessage = "";
    private static bool _progressVisible;
    private static int _progressRenderWidth;

    public static void Banner()
    {
        WriteLine("");
        WriteLine("Wiicompiled (Voicechat) Installer");
        WriteLine("======================================");
    }

    public static void PrintInstallPlan(PatcherLayout layout)
    {
        WriteLine("");
        WriteLine("Install location:");
        WriteLine($"  {PatchStateStore.Root(layout)}");
        WriteLine("");
        WriteLine("Important:");
        WriteLine("  This installs and compiles a separate Wiicompiled (Voicechat) build.");
        WriteLine("  Wheel Wizard's normal Retro Rewind button does NOT launch the voice-chat build.");
        WriteLine("  Start the installed build with the \"Wiicompiled (Voicechat)\" desktop shortcut.");
        WriteLine("");
    }

    public static void BeginPhase(string message)
    {
        InstallerLog.Write("PHASE", message);
        lock (Gate)
        {
            _lastPercent = -1;
            _lastMessage = "";
            RenderProgressLocked(0, message);
        }
    }

    public static void Progress(int percent, string message)
    {
        percent = Math.Clamp(percent, 0, 100);

        lock (Gate)
        {
            if (percent < _lastPercent)
                return;

            if (percent == _lastPercent &&
                string.Equals(message, _lastMessage, StringComparison.Ordinal))
                return;

            _lastPercent = percent;
            _lastMessage = message;

            InstallerLog.Write("PROGRESS", $"{percent}% {message}");
            RenderProgressLocked(percent, message);
        }
    }

    public static void ProgressMeasured(
        long completed,
        long total,
        string message)
    {
        if (total <= 0)
            return;

        completed = Math.Clamp(completed, 0, total);
        var percent = (int)Math.Round(100.0 * completed / total);

        lock (Gate)
        {
            RenderProgressLocked(
                percent,
                $"{message} ({completed}/{total})");
        }
    }

    private static void RenderProgressLocked(int percent, string message)
    {
        const int width = 30;
        var filled = (int)Math.Round(width * (percent / 100.0));
        filled = Math.Clamp(filled, 0, width);
        var bar = new string('#', filled) + new string('-', width - filled);
        var rendered = $"[{bar}] {percent,3}%  {message}";

        // CI/log redirection cannot rewrite an existing line, so keep
        // normal line-oriented output there. In an interactive console,
        // update one physical progress line in place.
        if (Console.IsOutputRedirected)
        {
            Console.WriteLine(rendered);
            return;
        }

        _progressRenderWidth = Math.Max(_progressRenderWidth, rendered.Length);
        Console.Write('\r');
        Console.Write(rendered.PadRight(_progressRenderWidth));
        _progressVisible = true;
    }

    public static void WriteLine(string text)
    {
        InstallerLog.Write("INFO", text);
        lock (Gate)
        {
            FinishProgressLineLocked();
            Console.WriteLine(text);
        }
    }

    public static void WriteError(string text)
    {
        InstallerLog.Write("ERROR", text);
        lock (Gate)
        {
            FinishProgressLineLocked();
            Console.Error.WriteLine(text);
        }
    }

    public static string ChooseInstallerAction()
    {
        if (Console.IsInputRedirected)
            return "run";

        WriteLine("");
        WriteLine("Wiicompiled (Voicechat) Installer");
        WriteLine("================================");
        WriteLine("");
        WriteLine("[1] Install / Update");
        WriteLine("[2] Repair / Repatch");
        WriteLine("[3] Uninstall");
        WriteLine("");
        WriteLine("Press 1, 2 or 3. Press Esc to exit.");

        while (true)
        {
            var key = Console.ReadKey(intercept: true);
            switch (key.Key)
            {
                case ConsoleKey.D1:
                case ConsoleKey.NumPad1:
                    WriteLine("Selected: Install / Update");
                    return "run";
                case ConsoleKey.D2:
                case ConsoleKey.NumPad2:
                    WriteLine("Selected: Repair / Repatch");
                    return "repair";
                case ConsoleKey.D3:
                case ConsoleKey.NumPad3:
                    WriteLine("Selected: Uninstall");
                    return "unpatch";
                case ConsoleKey.Escape:
                    return "exit";
            }
        }
    }

    public static bool AskYesNo(string question)
    {
        if (Console.IsInputRedirected)
        {
            WriteLine(question + " [Y/N] N");
            return false;
        }

        lock (Gate)
            FinishProgressLineLocked();

        while (true)
        {
            lock (Gate)
                Console.Write(question + " [Y/N] ");

            var key = Console.ReadKey(intercept: true);
            lock (Gate)
                Console.WriteLine(char.ToUpperInvariant(key.KeyChar));

            if (key.Key == ConsoleKey.Y)
                return true;
            if (key.Key == ConsoleKey.N)
                return false;
        }
    }

    public static void ResetProgress()
    {
        lock (Gate)
        {
            FinishProgressLineLocked();
            _lastPercent = -1;
            _lastMessage = "";
            _progressRenderWidth = 0;
        }
    }

    private static void FinishProgressLineLocked()
    {
        if (!_progressVisible)
            return;

        Console.WriteLine();
        _progressVisible = false;
        _progressRenderWidth = 0;
    }
}
