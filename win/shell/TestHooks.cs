// TEST builds only (win/shell/build.sh --test-app: ShellTest=1, SHELL_TEST): triggers for the VM
// smoke (vm/smoke.ps1) that the release app has no path to yet. RBSP_SHELL_TEST, comma-separated:
//   about-over-welcome  About asked 1.5 s after "shown", while the core's welcome is up: it must
//                       wait in the presenter's queue, never stack over the welcome
//   overlap             after About was asked (when it is) and once the presenter has been idle for
//                       0.5 s (the welcome and About closed), two dialogs asked from two threads, 0.2 s apart, the
//                       first with a '_' in a button: shown one at a time, in call order; each
//                       answer is logged ("shell test: dialog <n> answered <i>")
//   fail-first-render   the first render throws (a view failing): under --swap the update recovers
//   broken-offscreen-xaml  the off-screen start tries to load invalid XAML
//   hang-offscreen      --swap's off-screen start blocks the UI thread: the swap guard must fire
//   session-churn       2 s after "shown", the messages a session's moves bring, sent to the window
//                       (WPF's HwndTarget handles them too): RBSP_TEST_CHURN cycles (default 5) of
//                       a console disconnect, a connect and a display change, then one more with
//                       the window minimized, restored after it (Repaint.cs: a round of nudges
//                       after each; the minimized one's after the restore)
// Each repaint nudge goes to the session trace (RBSP_TEST_SESSION_TRACE) as repaint-nudge, and
// churn-sent once the churn is sent.
// And always: the UI thread's watch (UiWatch.cs, StartUiWatch: a stall's line, the UI thread's
// stack, a minidump in Logs once per process); the updater's and the flows' download base, the TEST build's own server
// (SetUpdateBase: build.sh --test-base's URL, compiled in as assembly metadata; release.md T12).
// Their texts are the test's own, not the core's: never in a release.
#if SHELL_TEST
using System;
using System.Diagnostics;
using System.IO;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Threading;

namespace RbStemsPlus.Shell
{
    internal static class TestHooks
    {
        private static volatile bool aboutAsked;
        private static int failRenders = Has("fail-first-render") ? 1 : 0;

        public static bool Has(string what) => (Environment.GetEnvironmentVariable("RBSP_SHELL_TEST") ?? "").Contains(what);

        // W13b moves only the DatePicker's local day, leaving the guest clock and core alone.
        public static DateTime PickerToday()
        {
            string path = Environment.GetEnvironmentVariable("RBSP_TEST_PICKER_DAY");
            return string.IsNullOrEmpty(path) ? DateTime.Today :
                DateTime.ParseExact(File.ReadAllText(path).Trim(), "yyyy-MM-dd", CultureInfo.InvariantCulture);
        }

        // the TEST core DLL's hook (core/build.sh dll --test-hooks; the release DLL has no core_test_ export)
        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int core_test_update_base([MarshalAs(UnmanagedType.LPUTF8Str)] string url);

        /// The download base this TEST build was built for (its assembly's RbspTestUpdateBase
        /// metadata), given to the core before anything downloads; none: GitHub's, as a release.
        public static void SetUpdateBase(Action<string> log)
        {
            string url = typeof(TestHooks).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
                .FirstOrDefault(a => a.Key == "RbspTestUpdateBase")?.Value ?? "";
            if (url.Length == 0) return;
            try
            {
                int r = core_test_update_base(url);
                log(r == 0 ? "shell test: downloads from " + url : "shell test: the core refused the download base " + url);
            }
            catch (EntryPointNotFoundException) { log("shell test: this core DLL has no test hooks: downloads from GitHub"); }
        }

        // The VM keeps this outside the install so it survives Uninstall All.
        public static void SessionTrace(string message, Action<string> log)
        {
            string path = Environment.GetEnvironmentVariable("RBSP_TEST_SESSION_TRACE");
            if (string.IsNullOrEmpty(path)) return;
            try { File.AppendAllText(path, DateTime.UtcNow.Ticks + " " + message + "\n"); }
            catch (IOException e) { log("shell test: session trace failed: " + e.Message); }
            catch (UnauthorizedAccessException e) { log("shell test: session trace failed: " + e.Message); }
        }

        /// fail-first-render: throws on the first render only
        public static void MaybeFailRender()
        {
            if (Interlocked.Exchange(ref failRenders, 0) == 1) throw new InvalidOperationException("shell test: the first render fails");
        }

        /// Faults in the off-screen WPF start, before the dispatcher waits for the first frame.
        public static void BeforeOffScreenFrame()
        {
            if (Has("broken-offscreen-xaml"))
                XamlReader.Parse("<Grid xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" NoSuchProperty=\"true\" />");
            if (Has("hang-offscreen")) Thread.Sleep(Timeout.Infinite);
        }

        public static void Start(Shell shell, Dispatcher ui)
        {
            string what = Environment.GetEnvironmentVariable("RBSP_SHELL_TEST") ?? "";
            shell.Log("shell test: " + (what.Length > 0 ? what : "no trigger"));
            StartUiWatch(shell, ui);
            if (what.Contains("about-over-welcome"))
                After(ui, 1500, () => { shell.Log("shell test: About asked"); shell.ShowAbout(); aboutAsked = true; });
            else aboutAsked = true;
            if (what.Contains("session-churn"))
                new Thread(() => Churn(shell, ui)) { IsBackground = true, Name = "shell test churn" }.Start();
            if (what.Contains("overlap"))
                new Thread(() =>
                {
                    Thread.Sleep(3000);                                    // the welcome is asked by then
                    while (!aboutAsked) Thread.Sleep(50);
                    for (int idle = 0; idle < 10; idle = shell.PresenterIdle ? idle + 1 : 0) Thread.Sleep(50);
                    var t1 = new Thread(() => Ask(shell, 1, "First_One")) { IsBackground = true };
                    t1.Start();
                    Thread.Sleep(200);
                    new Thread(() => Ask(shell, 2, "Second")) { IsBackground = true }.Start();
                }) { IsBackground = true }.Start();
        }

        // ------------------------------------------------------------------ the UI thread's watch

        private static volatile bool watching;

        /// Every TEST build, once, from the first frame on (UiWatch.cs): a beat asked of the UI
        /// thread every 0.5 s; a stall (UiWatch.StallMs without one) is logged with the core call
        /// it is inside and the UI thread's managed stack; the first stall of the process first
        /// leaves a minidump (MiniDumpWriteDump, dbghelp.dll: an OS component) beside the log,
        /// Logs\ui-hang-<pid>-<time>.dmp, with the report in a .txt beside it (written before the
        /// log line, which takes the log's lock). The watch's thread is a background thread: it
        /// never holds the app's quit.
        private static void StartUiWatch(Shell shell, Dispatcher ui)
        {
            if (watching) return;
            watching = true;
            var w = new UiWatch();
            shell.Host.Watch = w;
            Thread uiThread = Thread.CurrentThread;
            string logs = Path.Combine(shell.SupportDir, "Logs");
            Action beat = w.Beat;
            new Thread(() =>
            {
                bool dumped = false;
                while (true)
                {
                    try
                    {
                        if (w.Ask()) ui.BeginInvoke(DispatcherPriority.Send, beat);
                        string line = w.Check(out bool stalled);
                        if (line != null)
                        {
                            if (stalled)
                            {
                                // the dump first: the stack's read suspends the UI thread
                                string dump = dumped ? null : Dump(logs, line);
                                dumped = true;
                                line += "; the UI thread's stack: " + StackOf(uiThread) + (dump != null ? "; " + dump : "");
                            }
                            shell.Log("shell test: " + line);
                        }
                    }
                    catch (Exception e) { try { shell.Log("shell test: ui-watch failed: " + e.GetType().Name + ": " + e.Message); } catch (Exception) { } return; }
                    Thread.Sleep(500);
                }
            }) { IsBackground = true, Name = "shell test ui-watch" }.Start();
        }

        // the managed frames of a thread that doesn't answer: suspended for the read (a thread
        // inside a native call is suspended at once: the frames end at the P/Invoke)
        private static string StackOf(Thread t)
        {
#pragma warning disable 618   // Suspend and StackTrace(Thread): the only way to read another thread's stack in .NET Framework
            try
            {
                t.Suspend();
                try { return new StackTrace(t, false).ToString().Replace("\r", "").Replace("\n", " |"); }
                finally { t.Resume(); }
            }
            catch (Exception e) { return "not read (" + e.GetType().Name + ": " + e.Message + ")"; }
#pragma warning restore 618
        }

        [DllImport("dbghelp.dll", SetLastError = true, ExactSpelling = true)]
        private static extern bool MiniDumpWriteDump(IntPtr process, uint pid, Microsoft.Win32.SafeHandles.SafeFileHandle file, uint type,
                                                     IntPtr exception, IntPtr user, IntPtr callback);

        // MiniDumpWithDataSegs | WithHandleData | WithUnloadedModules | WithPrivateReadWriteMemory
        // (the GC heap: the managed stacks read with SOS) | WithFullMemoryInfo | WithThreadInfo |
        // IgnoreInaccessibleMemory
        private const uint DumpType = 0x1 | 0x4 | 0x20 | 0x200 | 0x800 | 0x1000 | 0x20000;

        private static string Dump(string logs, string report)
        {
            try
            {
                Directory.CreateDirectory(logs);
                var p = Process.GetCurrentProcess();
                string path = Path.Combine(logs, "ui-hang-" + p.Id + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + ".dmp");
                File.WriteAllText(path + ".txt", report + "\n");
                bool ok;
                int err;
                using (var f = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None))
                {
                    ok = MiniDumpWriteDump(p.Handle, (uint)p.Id, f.SafeFileHandle, DumpType, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
                    err = Marshal.GetLastWin32Error();
                }
                return ok ? "dump " + path : "no dump (" + err.ToString("X", CultureInfo.InvariantCulture) + ")";
            }
            catch (Exception e) { return "no dump (" + e.GetType().Name + ": " + e.Message + ")"; }
        }

        [DllImport("user32.dll", ExactSpelling = true)] private static extern IntPtr SendMessageW(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", ExactSpelling = true)] private static extern int GetSystemMetrics(int index);

        // session-churn: from its own thread, each message sent (the window handles it on its thread)
        private static void Churn(Shell shell, Dispatcher ui)
        {
            Thread.Sleep(2000);
            IntPtr h = IntPtr.Zero;
            ui.Invoke((Action)(() => h = new WindowInteropHelper(shell.Window).Handle));
            if (h == IntPtr.Zero) { shell.Log("shell test: session churn: no window"); return; }
            int n = int.TryParse(Environment.GetEnvironmentVariable("RBSP_TEST_CHURN"), out int c) && c > 0 ? c : 5;
            var sid = (IntPtr)Process.GetCurrentProcess().SessionId;
            var screen = (IntPtr)((GetSystemMetrics(1) << 16) | (GetSystemMetrics(0) & 0xFFFF));   // MAKELPARAM(cx, cy)
            void Cycle()
            {
                SendMessageW(h, RepaintPlan.WM_WTSSESSION_CHANGE, (IntPtr)RepaintPlan.WTS_CONSOLE_DISCONNECT, sid);
                Thread.Sleep(300);
                SendMessageW(h, RepaintPlan.WM_WTSSESSION_CHANGE, (IntPtr)RepaintPlan.WTS_CONSOLE_CONNECT, sid);
                Thread.Sleep(300);
                SendMessageW(h, RepaintPlan.WM_DISPLAYCHANGE, (IntPtr)32, screen);
                Thread.Sleep(400);
            }
            for (int i = 0; i < n; i++) Cycle();
            ui.Invoke((Action)(() => shell.Window.WindowState = WindowState.Minimized));
            Thread.Sleep(500);
            Cycle();
            ui.Invoke((Action)(() => { shell.Window.WindowState = WindowState.Normal; shell.Window.Activate(); }));
            SessionTrace("churn-sent", shell.Log);
            shell.Log("shell test: session churn sent (" + (n + 1) + " cycles, the last one minimized)");
        }

        private static void Ask(Shell shell, int n, string button)
        {
            var f = new DialogForm { Title = "Test dialog " + n, Message = "shell test: overlapping dialogs", Default = 0, Cancel = 1 };
            f.Buttons.Add(button); f.Buttons.Add("Cancel");
            f.Enabled.Add(true); f.Enabled.Add(true);
            f.Left.Add(false); f.Left.Add(false);
            shell.Log("shell test: dialog " + n + " asked");
            var a = shell.ShowDialog(f);
            shell.Log("shell test: dialog " + n + " answered " + (a == null ? "not shown" : a.Button.ToString()));
        }

        private static void After(Dispatcher ui, int ms, Action a)
        {
            var t = new DispatcherTimer(DispatcherPriority.Normal, ui) { Interval = TimeSpan.FromMilliseconds(ms) };
            t.Tick += (s, e) => { t.Stop(); a(); };
            t.Start();
        }
    }
}
#endif
