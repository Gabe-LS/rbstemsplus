// The shell's start (app.md §3, §4.1): our own Main, so the DLL search order and the core's
// preload come before WPF. SetDefaultDllDirectories(SYSTEM32 | USER_DIRS) is the first call; then
// x64\ or arm64\rbsp-core.dll (by the process's architecture) is loaded by full path, and the one
// DllImport("rbsp-core.dll") set binds to it (W0). The core is refused when its API version, its
// ids (core_ids_hash against the generated CoreIds.cs), its self-test or the frozen struct
// layouts don't match this build. Then the update's share of the start (Startup.cs: --swap's
// checks with the off-screen WPF start, update_at_start), the WPF application, the window and
// core.h's life cycle.
using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Threading;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    internal static class Program
    {
        private const int OffScreenSeconds = 30;   // the off-screen window's first frame, at most
        private static volatile bool coreReady;

        [STAThread]
        private static int Main(string[] args)
        {
            AppDomain.CurrentDomain.UnhandledException += (s, e) => LastChance("shell: unhandled exception: " + e.ExceptionObject);
            bool swap = Array.IndexOf(args, "--swap") >= 0;
            bool uninstall = LaunchArgs.IsUninstall(args);   // the Apps & features entry, through the stub
            string why;
            // a DLL without one of our entry points (or not a DLL for this process) is refused, not a crash
            try { why = LoadCore(); }
            catch (Exception e) when (e is EntryPointNotFoundException || e is DllNotFoundException || e is BadImageFormatException)
            {
                why = "rbsp-core.dll isn't this app's (" + e.GetType().Name + ": " + e.Message + ")";
            }
            coreReady = why == null;
            string version = Version();
            string support = SupportDir();
            string appDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
            var log = new LogSink(new LogBuffer(support, coreReady: coreReady));
            var update = new UpdateNative(support, appDir, version, log);
#if SHELL_TEST
            // the TEST build's own download server, before anything can download (release.md T12)
            string updateBaseLine = null;
            if (why == null) TestHooks.SetUpdateBase(line => updateBaseLine = line);
#endif
            if (why == null) update.SetAumid();   // before the first window, the off-screen one included

            App app = null;
            // the guard's kill bound before anything can hang (WV2): no loader work at kill time
            IntPtr self = Win32.GetCurrentProcess();
            Marshal.Prelink(typeof(Win32).GetMethod(nameof(Win32.TerminateProcess)));
            var guard = new SwapGuard(update, () => Win32.TerminateProcess(self, 1));
            // `why` reaches the sequence: under --swap a refused core restarts the old app (no dialog)
            // ...and the token first (app.md §5.9, I35): an elevated start, UAC off or a service account
            // is refused before anything is written, the mutex taken or a --swap goes on (failed swap)
            StartOutcome start = StartSequence.Run(swap, why, update, () => OffScreen(ref app), guard, out int exit, out bool swapping, out CoreText? refusal);
            if (start == StartOutcome.Refuse)
            {
                if (why != null) Refuse(why);
                else RefuseStart(refusal ?? CoreText.T_CANT_OPEN);
                return 1;
            }
#if SHELL_TEST
            if (updateBaseLine != null) update.Log(updateBaseLine);
#endif
            if (start == StartOutcome.Exit) return exit;
            // the update's real start, up to its first good frame, is guarded too (Shell finishes it)
            if (swapping) guard.Arm(SwapGuard.StageMs);
            // one copy at a time (OneCopy.cs, core_win_one_take): after update_at_start, before the
            // first render. Under --swap the old app was waited for: anything but HELD stops the update.
            var oneCopy = new OneCopy(new OneCopyNative(), update.Log);
            OneCopyStart copy = oneCopy.Take();
            if (uninstall) copy = LaunchArgs.ForUninstall(copy);   // never a second uninstall: the open copy is said
            if (copy != OneCopyStart.Run)
            {
                if (swapping) return Recover(update, guard, "another copy of the app is open");
                if (copy == OneCopyStart.ExitQuietly) return 0;
                if (copy == OneCopyStart.AlreadyOpen)
                {
                    Refuse(Texts.Get(CoreText.T_ALREADY_OPEN_MSG), Texts.Get(CoreText.T_ALREADY_OPEN));
                    return 0;
                }
                Refuse(Texts.Get(CoreText.T_CANT_OPEN));
                return 1;
            }

            Shell shell = null;
            try
            {
                if (app == null)
                {
                    app = new App();
                    app.InitializeComponent();
                }
                shell = new Shell(app.Dispatcher, log, Path.Combine(support, "window"), update, swapping ? guard : null, oneCopy);
                if (uninstall) shell.Uninstall = new UninstallOnce(true, shell.Host.Send, update.Log);
                app.DispatcherUnhandledException += (s, e) => { shell.Host.Fail("the dispatcher", e.Exception); e.Handled = true; };
                app.SessionEnding += (s, e) => e.Cancel = shell.QueryEndSession();
                app.Exit += (s, e) => shell.Exiting();
                if (!shell.Start(Config(version, support, shell.Host.NativeList)))
                {
                    oneCopy.Close();   // before the old app is started again or the refusal waits on its OK (WC6)
                    if (swapping)
                    {
                        int r = Recover(update, guard, "the core refused the start");
                        StopCore(shell);
                        return r;
                    }
                    StopCore(shell);
                    Refuse(Texts.Get(CoreText.T_CANT_OPEN));
                    return 2;
                }
                if (swapping && !shell.Host.RenderOk)
                {
                    // the new app can't draw its first snapshot: no window, the old app back (WU1)
                    oneCopy.Close();
                    int r = Recover(update, guard, "the first render failed");
                    StopCore(shell);
                    return r;
                }
                shell.Show();
                // findable once the window is there: a second start's signal (on this thread) brings it forward
                Shell s = shell;
                oneCopy.Window(() => app.Dispatcher.BeginInvoke(DispatcherPriority.Normal, (Action)s.SecondStart));
            }
            catch (Exception e)
            {
                LastChance("shell: start failed: " + e);
                try { oneCopy.Close(); } catch (Exception) { }
                if (swapping)
                {
                    int r = Recover(update, guard, "the start failed (" + e.GetType().Name + ")");
                    StopCore(shell);
                    return r;
                }
                StopCore(shell);
                try { Refuse(Texts.Get(CoreText.T_CANT_OPEN)); } catch (Exception) { }
                return 2;
            }
            int rc = app.Run();
            oneCopy.Close();   // the UI thread that took the mutex
            guard.Settle();
            return rc;
        }

        // --swap's off-screen start (app.md §5.4 step 3): the WPF Application and the main window,
        // shown outside every screen, not activated and not in the taskbar, until their first frame
        // is drawn; then the window closes and the Application stays for the real start. core_selftest
        // is pure C: this is what catches a WPF or XAML start failure. null: rendered; else why not.
        private static string OffScreen(ref App app)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();   // the 30 s include WPF's construction (WV3)
            try
            {
                app = new App();
                app.InitializeComponent();
                var w = new MainWindow(new ManageModel())
                {
                    WindowStartupLocation = WindowStartupLocation.Manual,
                    ShowActivated = false,
                    ShowInTaskbar = false,
                };
                w.Left = SystemParameters.VirtualScreenLeft - w.Width - 1000;
                w.Top = SystemParameters.VirtualScreenTop - w.Height - 1000;
                bool rendered = false;
                var frame = new DispatcherFrame();
                w.ContentRendered += (s, e) => { rendered = true; frame.Continue = false; };
                var timer = new DispatcherTimer(TimeSpan.FromMilliseconds(Math.Max(1, OffScreenSeconds * 1000 - sw.ElapsedMilliseconds)), DispatcherPriority.Normal,
                                                (s, e) => frame.Continue = false, app.Dispatcher);
                w.Show();
#if SHELL_TEST
                TestHooks.BeforeOffScreenFrame();
#endif
                if (!rendered) Dispatcher.PushFrame(frame);
                timer.Stop();
                w.Close();
                return rendered ? null : "the off-screen window drew no frame in " + OffScreenSeconds + " s";
            }
            catch (Exception e)
            {
                return "the off-screen WPF start failed (" + e.GetType().Name + ": " + e.Message + ")";
            }
        }

        // the update's start failed after its checks (WU3): no dialog, "failed swap", the old app
        // started again; nothing when the guard has recovered already
        private static int Recover(UpdateNative update, SwapGuard guard, string why)
        {
            update.Log("--swap: " + why + ": the update stops here");
            if (guard.Finish()) StartSequence.Recover(update);
            else guard.Settle();
            return 1;
        }

        // a core already made is stopped before the refusal's TaskDialog, whose message loop would
        // otherwise run its drain and its dialogs with no window shown (review N1)
        private static void StopCore(Shell shell)
        {
            try { shell?.Host.Stop(); } catch (Exception) { }
        }

        private static string SupportDir() =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "rbstemsplus");

        // last-chance diagnostics: one line in app.log, including before the core is ready
        private static void LastChance(string line)
        {
            try
            {
                LogBuffer.Append(SupportDir(), DateTime.Now, line, coreReady);
            }
            catch (Exception) { }
        }

        /// null: the core is loaded and is this build's; else why not (for the refusal).
        private static string LoadCore()
        {
            if (Win32.SetDefaultDllDirectories(Win32.LOAD_LIBRARY_SEARCH_SYSTEM32 | Win32.LOAD_LIBRARY_SEARCH_USER_DIRS) == 0)
                return "SetDefaultDllDirectories failed (" + Marshal.GetLastWin32Error() + ")";
            Architecture a = RuntimeInformation.ProcessArchitecture;
            string sub = a == Architecture.Arm64 ? "arm64" : a == Architecture.X64 ? "x64" : null;
            if (sub == null) return "a 64-bit Windows is needed (this process is " + a + ")";
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, sub, "rbsp-core.dll");
            if (Win32.LoadLibraryExW(path, IntPtr.Zero, 0) == IntPtr.Zero)
                return path + " can't be loaded (" + Marshal.GetLastWin32Error() + ")";
            uint v = Core.core_api_version();
            if (v != CoreApi.Version) return "rbsp-core.dll speaks API " + v + ", this app " + CoreApi.Version;
            ulong h = Core.core_ids_hash();
            if (h != CoreApi.IdsHash) return "rbsp-core.dll's ids (0x" + h.ToString("x16") + ") aren't this app's (0x" + CoreApi.IdsHash.ToString("x16") + ")";
            int st = Core.core_selftest();
            if (st != 0) return "rbsp-core.dll's self-test failed (" + st + ")";
            if (Marshal.SizeOf(typeof(CoreHost)) != Core.HostSize || Marshal.SizeOf(typeof(CoreMenuItem)) != Core.MenuItemSize
                || Marshal.SizeOf(typeof(CoreSlot)) != Core.SlotSize || Marshal.SizeOf(typeof(CoreMaskCtx)) != Core.MaskCtxSize)
                return "the core's struct layouts aren't this build's";
            return null;
        }

        private static string Version()
        {
            var a = Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>();
            return a != null && a.InformationalVersion.Length > 0 ? a.InformationalVersion : "0.0.0";
        }

        /// core_new's config (CoreConfig): this account's folders.
        private static string Config(string version, string support, string native) =>
            CoreConfig.Build(version, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), support,
                             AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\'), Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                             Path.GetTempPath().TrimEnd('\\'), native);

        // the start's one line under "can't open", as in the Mac's root refusal
        private static void RefuseStart(CoreText t)
        {
            if (t == CoreText.T_CANT_OPEN) Refuse(Texts.Get(CoreText.T_CANT_OPEN));
            else Refuse(Texts.Get(t), Texts.Get(CoreText.T_CANT_OPEN));
        }

        // Before the core's UI (app.md §4.4): one plain TaskDialog. With the core loaded its text
        // is the core's; without it (no DLL, a wrong one) the reason is all there is. instruction:
        // the dialog's main line ("Already open"), the app's name by default.
        private static void Refuse(string why, string instruction = null)
        {
            string title = "RB Stems Plus";
            try { Win32.TaskDialog(IntPtr.Zero, IntPtr.Zero, title, instruction ?? title, why, Win32.TDCBF_OK_BUTTON, Win32.TD_ERROR_ICON, out _); }
            catch (Exception) { }
        }
    }
}
