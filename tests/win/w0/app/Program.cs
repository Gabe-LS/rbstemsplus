// The W0 spike's start-up sequence and checks (app.md §3 S0 and §4.1; mac-core §9 G1.5, G1.9,
// G1.10). It runs unattended: every dialog it shows is found and dismissed by Dismisser. Results go
// to the file named by the first argument (default: w0-result.txt beside the exe); the exit code is
// the number of failed checks. A second argument --startup runs the start-up sequence only (for a
// release DLL, which has no test hooks).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.W0
{
    internal static class R
    {
        public static readonly List<string> Lines = new List<string>();
        public static int Failures;
        public static readonly double TicksToMs = 1000.0 / Stopwatch.Frequency;

        public static void Check(bool ok, string what)
        {
            if (!ok) Failures++;
            Lines.Add((ok ? "PASS " : "FAIL ") + what);
        }

        public static void Info(string what) => Lines.Add("INFO " + what);
        public static void Time(string what, double ms) => Lines.Add("TIME " + what + ": " + ms.ToString("0.000") + " ms");
        public static void PerCall(string what, double ns) => Lines.Add("CALL " + what + ": " + ns.ToString("0.0") + " ns/call");
        public static double Ms(long t0, long t1) => (t1 - t0) * TicksToMs;
    }

    internal static class Program
    {
        private static string resultPath;
        private static long tMain, tProcessStartMs;
        private static IntPtr preloaded;
        private static bool startupOnly;

        [STAThread]
        private static int Main(string[] args)
        {
            tMain = Stopwatch.GetTimestamp();
            try
            {
                using (var p = Process.GetCurrentProcess())
                    tProcessStartMs = (long)(DateTime.Now - p.StartTime).TotalMilliseconds;
            }
            catch { tProcessStartMs = -1; }
            resultPath = args.Length > 0 ? args[0] : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "w0-result.txt");
            startupOnly = args.Length > 1 && args[1] == "--startup";
            try
            {
                if (!StartUp()) { Write(); return 100; }
                var app = new App();
                long a0 = Stopwatch.GetTimestamp();
                app.InitializeComponent();   // App.xaml's BAML
                long a1 = Stopwatch.GetTimestamp();
                R.Time("WPF Application + App.xaml InitializeComponent", R.Ms(a0, a1));
                app.DispatcherUnhandledException += (s, e) => { R.Check(false, "unhandled exception: " + e.Exception); e.Handled = true; Finish(app); };
                var win = new MainWindow();   // MainWindow.xaml's BAML
                long a2 = Stopwatch.GetTimestamp();
                R.Time("MainWindow construct (XAML load)", R.Ms(a1, a2));
                R.Check(win.Tracks != null && win.StatusLine != null, "XAML: the x:Name fields are wired (MarkupCompile pass 1 and 2, BAML)");
                Host.Ui = app.Dispatcher;
                Host.Window = win;
                Host.UiThreadId = Win32.GetCurrentThreadId();
                Host.StartFlowThread();

                // core.h Life cycle: core_new, {"ev":"start"}, core_commit (the first render), show, {"ev":"shown"}
                var table = Host.MakeTable();
                long c0 = Stopwatch.GetTimestamp();
                Host.C = Core.core_new(ref table, CoreApi.CORE_ROLE_APP, "{\"os\":\"win\",\"version\":\"0.0.0-w0\"}", CoreApi.Nts);
                long c1 = Stopwatch.GetTimestamp();
                R.Check(Host.C != IntPtr.Zero, "core_new with the C# host table (config in as LPUTF8Str)");
                R.Time("core_new", R.Ms(c0, c1));
                if (Host.C == IntPtr.Zero) { Write(); return 101; }
                int r = Core.core_event(Host.C, "{\"ev\":\"start\"}", CoreApi.Nts);
                long c2 = Stopwatch.GetTimestamp();
                R.Check(r == CoreApi.CORE_OK, "core_event start (" + r + ")");
                Core.core_commit(Host.C);
                long c3 = Stopwatch.GetTimestamp();
                R.Check(Host.Renders == 1 && Host.Held != IntPtr.Zero, "the first render at core_commit, synchronously (" + Host.Renders + ")");
                R.Time("core_event start", R.Ms(c1, c2));
                R.Time("core_commit, the first render (full reload)", R.Ms(c2, c3));
                int menus = win.BuildMenus();
                R.Check(menus > 0 && win.MenuBar.Items.Count > 0, "the menu bar from menus.def: " + menus + " Windows items, " + win.MenuBar.Items.Count + " top-level");
                long c4 = Stopwatch.GetTimestamp();
                win.ContentRendered += (s, e) =>
                {
                    long c5 = Stopwatch.GetTimestamp();
                    R.Time("window.Show to ContentRendered", R.Ms(c4, c5));
                    R.Time("Main entered to first frame", R.Ms(tMain, c5));
                    R.Time("process start to first frame (approx.)", tProcessStartMs + R.Ms(tMain, c5));
                    int sr = Core.core_event(Host.C, "{\"ev\":\"shown\"}", CoreApi.Nts);
                    R.Check(sr == CoreApi.CORE_OK, "core_event shown (" + sr + ")");
                    app.Dispatcher.BeginInvoke(DispatcherPriority.Background, (Action)(async () =>
                    {
                        try { if (!startupOnly) await Spike(win); else R.Info("--startup: the start-up sequence only (a release DLL, no test hooks)"); }
                        catch (Exception ex) { R.Check(false, "spike threw: " + ex); }
                        Finish(app);
                    }));
                };
                win.Show();
                int code = app.Run();
                return code;
            }
            catch (Exception e)
            {
                R.Check(false, "start-up threw: " + e);
                Write();
                return 102;
            }
        }

        // ------------------------------------------------------------------ before WPF

        private static unsafe bool StartUp()
        {
            R.Info("process " + RuntimeInformation.ProcessArchitecture + " on OS " + RuntimeInformation.OSArchitecture +
                   ", " + RuntimeInformation.FrameworkDescription + ", " + Environment.OSVersion.VersionString);
            R.Time("process start to Main (approx., DateTime)", tProcessStartMs);
            long t0 = Stopwatch.GetTimestamp();
            int ok = Win32.SetDefaultDllDirectories(Win32.LOAD_LIBRARY_SEARCH_SYSTEM32 | Win32.LOAD_LIBRARY_SEARCH_USER_DIRS);
            long t1 = Stopwatch.GetTimestamp();
            R.Check(ok != 0, "SetDefaultDllDirectories(SYSTEM32 | USER_DIRS), the first call");
            R.Time("SetDefaultDllDirectories", R.Ms(t0, t1));

            string sub = RuntimeInformation.ProcessArchitecture == Architecture.Arm64 ? "arm64"
                       : RuntimeInformation.ProcessArchitecture == Architecture.X64 ? "x64" : null;
            R.Check(sub != null, "a 64-bit process (" + RuntimeInformation.ProcessArchitecture + ")");
            if (sub == null) return false;
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, sub, "rbsp-core.dll");
            R.Check(!File.Exists(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "rbsp-core.dll")), "no rbsp-core.dll beside the exe (only x64\\ and arm64\\)");
            long t2 = Stopwatch.GetTimestamp();
            preloaded = Win32.LoadLibraryExW(path, IntPtr.Zero, 0);
            int err = Marshal.GetLastWin32Error();
            long t3 = Stopwatch.GetTimestamp();
            R.Check(preloaded != IntPtr.Zero, "LoadLibraryExW by full path: " + sub + "\\rbsp-core.dll (error " + (preloaded == IntPtr.Zero ? err : 0) + ")");
            R.Time("LoadLibraryExW(rbsp-core.dll)", R.Ms(t2, t3));
            if (preloaded == IntPtr.Zero) return false;

            long t4 = Stopwatch.GetTimestamp();
            uint v;
            try { v = Core.core_api_version(); }
            catch (Exception e) { R.Check(false, "the first P/Invoke into the preloaded DLL: " + e.GetType().Name + " " + e.Message); return false; }
            long t5 = Stopwatch.GetTimestamp();
            R.Time("the first P/Invoke (DllImport binding + call)", R.Ms(t4, t5));
            R.Check(v == CoreApi.Version, "core_api_version " + v);
            IntPtr bound = Win32.GetModuleHandleW("rbsp-core.dll");
            R.Check(bound == preloaded, "DllImport(\"rbsp-core.dll\") bound to the preloaded module");
            var mods = Modules().Where(m => Path.GetFileName(m).Equals("rbsp-core.dll", StringComparison.OrdinalIgnoreCase)).ToList();
            R.Check(mods.Count == 1 && mods[0].Equals(path, StringComparison.OrdinalIgnoreCase),
                    "one rbsp-core.dll in the process, the preloaded one: " + string.Join(", ", mods));

            ulong h = Core.core_ids_hash();
            R.Check(h == CoreApi.IdsHash, "core_ids_hash 0x" + h.ToString("x16") + " matches CoreIds.cs");
            long t6 = Stopwatch.GetTimestamp();
            int st = Core.core_selftest();
            long t7 = Stopwatch.GetTimestamp();
            R.Check(st == 0, "core_selftest " + st);
            R.Time("core_selftest", R.Ms(t6, t7));
            R.Check(Marshal.SizeOf(typeof(CoreHost)) == 136, "sizeof core_host 136 (" + Marshal.SizeOf(typeof(CoreHost)) + ")");
            R.Check(Marshal.SizeOf(typeof(CoreMenuItem)) == 40, "sizeof core_menu_item 40 (" + Marshal.SizeOf(typeof(CoreMenuItem)) + ")");
            R.Check(Marshal.SizeOf(typeof(CoreSlot)) == 24, "sizeof core_slot 24 (" + Marshal.SizeOf(typeof(CoreSlot)) + ")");
            R.Check(Marshal.SizeOf(typeof(CoreMaskCtx)) == 40, "sizeof core_mask_ctx 40 (" + Marshal.SizeOf(typeof(CoreMaskCtx)) + ")");
            return true;
        }

        private static unsafe List<string> Modules()
        {
            var list = new List<string>();
            IntPtr proc = Win32.GetCurrentProcess();
            IntPtr* mods = stackalloc IntPtr[1024];
            if (Win32.EnumProcessModulesEx(proc, mods, (uint)(1024 * IntPtr.Size), out uint needed, 3 /* LIST_MODULES_ALL */) == 0) return list;
            char* buf = stackalloc char[1024];
            for (int i = 0; i < Math.Min(1024, (int)(needed / IntPtr.Size)); i++)
            {
                uint n = Win32.GetModuleFileNameW(mods[i], buf, 1024);
                if (n > 0) list.Add(new string(buf, 0, (int)n));
            }
            return list;
        }

        // ------------------------------------------------------------------ after the first frame

        // fast: yield at Background priority (below Input, so the drain runs first) instead of a
        // timer, whose ~15 ms resolution would dominate a round trip
        private static async Task<bool> Until(Func<bool> cond, int ms, bool fast = false)
        {
            var sw = Stopwatch.StartNew();
            while (!cond())
            {
                if (sw.ElapsedMilliseconds > ms) return false;
                if (fast) await Dispatcher.Yield(DispatcherPriority.Background);
                else await Task.Delay(2);
            }
            return true;
        }

        private static int LogCount(string line) { lock (Host.Log) return Host.Log.Count(l => l == line); }

        // UI-thread items and frame gaps, per phase (app.md §6's timed item, roughly: nested frames included)
        private static double longestOpMs, longestFrameGapMs;
        private static long opStart, lastFrame;
        private static bool watching;

        private static void Watch(bool on)
        {
            if (on) { longestOpMs = 0; longestFrameGapMs = 0; lastFrame = 0; }
            watching = on;
        }

        private static async Task Spike(MainWindow win)
        {
            IntPtr C = Host.C;
            var hooks = Dispatcher.CurrentDispatcher.Hooks;
            hooks.OperationStarted += (s, e) => { if (watching) opStart = Stopwatch.GetTimestamp(); };
            hooks.OperationCompleted += (s, e) =>
            {
                if (!watching || opStart == 0) return;
                double ms = R.Ms(opStart, Stopwatch.GetTimestamp());
                if (ms > longestOpMs) longestOpMs = ms;
            };
            CompositionTarget.Rendering += (s, e) =>
            {
                long now = Stopwatch.GetTimestamp();
                if (watching && lastFrame != 0) { double g = R.Ms(lastFrame, now); if (g > longestFrameGapMs) longestFrameGapMs = g; }
                lastFrame = now;
            };

            // ---- G1.10: LPUTF8Str in, pointer + length out
            string s1 = "Stems Cache is on. ✓ — Größe 日本語 \U0001F3B5";
            int r = Core.core_test_set_str(C, (int)Field.F_STATUS_LINE, s1, CoreApi.Nts);
            Core.core_commit(C);
            string back = MainWindow.Str(Host.Held, Field.F_STATUS_LINE);
            R.Check(r == 0 && back == s1, "LPUTF8Str (CORE_NTS) round trip of BMP and non-BMP text: \"" + back + "\"");
            R.Check(win.StatusLine.Text == s1, "the render applied the field to the XAML TextBlock");
            string s2 = "café \U0001F600 len";
            r = Core.core_test_set_str(C, (int)Field.F_STATUS_LINE, s2, (UIntPtr)Encoding.UTF8.GetByteCount(s2));
            Core.core_commit(C);
            R.Check(r == 0 && MainWindow.Str(Host.Held, Field.F_STATUS_LINE) == s2, "LPUTF8Str with an explicit byte length");
            string s3 = "byte* üñî \U0001F3A7";
            r = SetStrBytes(C, Field.F_STATUS_LINE, s3);
            Core.core_commit(C);
            R.Check(r == 0 && MainWindow.Str(Host.Held, Field.F_STATUS_LINE) == s3, "the fallback: byte* + length");
            r = Core.core_test_set_str(C, (int)Field.F_STATUS_LINE, "lone \uD800 surrogate", CoreApi.Nts);
            Core.core_commit(C);
            string lone = MainWindow.Str(Host.Held, Field.F_STATUS_LINE);
            R.Info("LPUTF8Str of a lone surrogate: rc " + r + ", read back as \"" + lone + "\" (U+FFFD expected)");
            r = Core.core_event(C, "{\"ev\":\"activated\"}", CoreApi.Nts);
            R.Check(r == 0, "core_event activated (LPUTF8Str)");
            r = Core.core_event(C, "{\"ev\":\"click\",\"id\":\"ünknown \U0001F3B5\"}", CoreApi.Nts);
            R.Info("core_event with a non-ASCII member: rc " + r);
            int logsBefore; lock (Host.Log) logsBefore = Host.Log.Count;
            r = Core.core_event(C, "{not json", CoreApi.Nts);
            bool logged; lock (Host.Log) logged = Host.Log.Count > logsBefore;
            R.Check(r == CoreApi.CORE_E_PARSE && logged, "a malformed event: CORE_E_PARSE and a line through the log callback");

            // ---- per-call cost (best of 3 runs of N calls, on the UI thread)
            PerCallCosts(C, win);

            // ---- the thread rule: a main-only entry from another thread is refused (GetCurrentThreadId identity)
            int offMain = 99;
            var th = new Thread(() => offMain = Core.core_event(C, "{\"ev\":\"activated\"}", CoreApi.Nts));
            th.Start(); th.Join();
            R.Check(offMain == CoreApi.CORE_E_THREAD, "core_event off main: CORE_E_THREAD (" + offMain + ")");

            // ---- 5,000 rows in one render, then 5,000 row deltas (the API spike's shape, not counted)
            await Task.Delay(50);
            Watch(true);
            int rendersBefore = Host.Renders;
            long f0 = Stopwatch.GetTimestamp();
            r = Core.core_test_table_reset(C, (int)Table.T_PREP_TRACKS, 5000);
            for (int i = 0; i < 5000; i++)
            {
                Core.core_test_cell_str(C, (int)Column.C_TRK_NAME, i, "Track " + i + " – Artist", CoreApi.Nts);
                Core.core_test_cell_str(C, (int)Column.C_TRK_RESULT, i, "To prepare", CoreApi.Nts);
            }
            long f1 = Stopwatch.GetTimestamp();
            bool ok = await Until(() => Host.Renders > rendersBefore && Core.core_rows(Host.Held, (int)Table.T_PREP_TRACKS) == 5000, 5000);
            long f2 = Stopwatch.GetTimestamp();
            R.Check(ok && r == 0, "5,000 rows reach the window in one render through the core's commit job and the drain (" + (Host.Renders - rendersBefore) + " render)");
            R.Time("10,000 core_test_cell_str calls (LPUTF8Str)", R.Ms(f0, f1));
            R.Time("table filled to the render applied", R.Ms(f1, f2));
            await Until(() => false, 300);   // let the rows draw
            R.Time("longest UI-thread item while 5,000 rows were loaded and drawn", longestOpMs);
            R.Time("longest frame gap then", longestFrameGapMs);

            Watch(true);
            int changedBefore = Host.ChangedRows, rendersB = Host.Renders;
            long d0 = Stopwatch.GetTimestamp();
            for (int batch = 0; batch < 100; batch++)
            {
                for (int k = 0; k < 50; k++) Core.core_test_cell_str(C, (int)Column.C_TRK_RESULT, batch * 50 + k, "Ready", CoreApi.Nts);
                await Task.Yield();
                await Until(() => Host.ChangedRows - changedBefore >= (batch + 1) * 50, 2000);
            }
            long d1 = Stopwatch.GetTimestamp();
            R.Check(Host.ChangedRows - changedBefore == 5000, "5,000 row deltas, each row reported once (" + (Host.ChangedRows - changedBefore) + " rows over " + (Host.Renders - rendersB) + " renders)");
            R.Check(Core.core_cell_str(Host.Held, (int)Column.C_TRK_RESULT, 4321, out UIntPtr l4) != IntPtr.Zero &&
                    MainWindow.Cell(Host.Held, Column.C_TRK_RESULT, 4321) == "Ready", "a cell read back after the deltas");
            R.Time("100 batches of 50 deltas, end to end", R.Ms(d0, d1));
            R.Time("longest UI-thread item during the deltas", longestOpMs);
            R.Time("longest frame gap during the deltas", longestFrameGapMs);
            R.Time("longest drain so far (jobs + core_commit)", Host.LongestDrainMs);
            R.Time("longest render callback so far", Host.LongestRenderMs);

            // ---- the helper's lines: reader thread → core_helper_line → typed main jobs → the drain
            Watch(true);
            int offBefore = Host.OffMainPosts;
            long h0 = Stopwatch.GetTimestamp();
            ulong run = Core.core_test_helper_start(C, 0);
            ok = await Until(() => Host.HelperRunsEnded == 1 && Core.core_test_helper_lines(C, 0) == 5000, 10000);
            long h1 = Stopwatch.GetTimestamp();
            R.Check(run != 0 && ok, "a helper run: 5,000 lines from the reader thread applied on main (" + Core.core_test_helper_lines(C, 0) + "), one core_helper_ended");
            lock (Host.HelperArgv) R.Check(Host.HelperArgv.Count == 1 && Host.HelperArgv[0] == "--test", "helper_spawn's argv marshalled: \"" + string.Join("|", Host.HelperArgv) + "\"");
            R.Check(Host.OffMainPosts > offBefore, "post_main from other threads (" + (Host.OffMainPosts - offBefore) + ")");
            R.Time("5,000 helper lines, spawn to the last applied", R.Ms(h0, h1));
            R.Time("longest UI-thread item during the helper lines", longestOpMs);
            R.Time("longest frame gap during the helper lines", longestFrameGapMs);
            Watch(false);

            // ---- a flow with a dialog: post_work(FLOW) → flow thread → dialog (TaskDialog on the UI
            // thread, owned by the window) → revalidate (flow_query: SRWLOCK + CONDITION_VARIABLE wait)
            const string flowOk = "test flow: button 0, revalidate 1";
            int before = LogCount(flowOk);
            r = Core.core_test_flow(C, 0);
            ok = await Until(() => LogCount(flowOk) == before + 1, 15000);
            R.Check(r == 0 && ok && Host.DialogsShown == 1, "a dialog flow: TaskDialog shown from the flow's dialog callback, answered, revalidated (\"" + flowOk + "\")");
            if (Host.LastDialogShownTicks != 0) R.Time("dialog callback to the TaskDialog visible", R.Ms(Host.LastDialogAskedTicks, Host.LastDialogShownTicks));

            // ---- the plat_shim stress: 300 flows answered on the flow thread, each revalidating on
            // main. One flow at a time (the core refuses a second while one runs, until its end job
            // has run on main), so each starts once the previous one has ended.
            Host.AutoAnswer = true;
            before = LogCount(flowOk);
            int refused = 0, started = 0;
            long s0 = Stopwatch.GetTimestamp();
            for (int i = 0; i < 300; i++)
            {
                int rc;
                while ((rc = Core.core_test_flow(C, 0)) != 0)
                {
                    refused++;
                    if (refused > 100000) break;
                    await Dispatcher.Yield(DispatcherPriority.Background);
                }
                if (rc == 0) started++;
                int want = before + i + 1;
                if (!await Until(() => LogCount(flowOk) >= want, 10000, fast: true)) break;
            }
            long s1t = Stopwatch.GetTimestamp();
            Host.AutoAnswer = false;
            ok = LogCount(flowOk) == before + 300;
            R.Check(ok && started == 300, "300 flows one after another, each a flow_query wait on SRWLOCK/CONDITION_VARIABLE answered from main (" +
                    (LogCount(flowOk) - before) + " done; a start while the previous flow was ending was refused " + refused + " times)");
            R.Time("one flow, start to its log line (post_work → flow thread → dialog → revalidate on main → wake → log)", R.Ms(s0, s1t) / 300);

            // ---- a step flow: host->step with a string array
            r = Core.core_test_flow(C, 1);
            ok = await Until(() => LogCount("test flow: step 0") == 1, 5000);
            lock (Host.Steps) R.Check(r == 0 && ok && Host.Steps.Count == 1 && Host.Steps[0] == "step 7 https://example.invalid/",
                                      "a step flow: the args array marshalled (" + string.Join("|", Host.Steps) + ")");

            // ---- IFileOpenDialog and TaskDialog, owned by the window, twice each (first and warm)
            IntPtr owner = new WindowInteropHelper(win).Handle;
            for (int round = 0; round < 2; round++) FolderPicker(owner, round == 0 ? "first" : "second");
            for (int round = 0; round < 2; round++)
            {
                var dm = Dismisser.Start(Host.UiThreadId, Dismisser.Kind.TaskDialog);
                long t0 = Stopwatch.GetTimestamp();
                Host.Modal++;
                int hr = -1, pressed = 0;
                try { hr = Win32.TaskDialog(owner, IntPtr.Zero, "RB Stems Plus W0", "TaskDialog", "a check", Win32.TDCBF_OK_BUTTON | Win32.TDCBF_CANCEL_BUTTON, IntPtr.Zero, out pressed); }
                catch (Exception e) { R.Check(false, "TaskDialog: " + e.GetType().Name + " " + e.Message); }
                finally { Host.Modal--; }
                long shown = dm.Wait();
                R.Check(hr == 0 && pressed == Win32.IDOK, "TaskDialog (comctl32 v6 by the manifest), " + (round == 0 ? "first" : "second") + ": hr " + hr + ", pressed " + pressed);
                if (shown != 0) R.Time("TaskDialog call to visible, " + (round == 0 ? "first" : "second"), R.Ms(t0, shown));
            }

            // ---- the quit flow during a Prepare run: LATER, the dialog, close_reply(QUIT, 1), then NOW
            Core.core_test_set_preparing(C, 1);
            int shownBefore = Host.DialogsShown;
            int q = Core.core_close_asked(C, CoreApi.CORE_CLOSE_QUIT);
            R.Check(q == CoreApi.CORE_CLOSE_LATER, "core_close_asked(QUIT) during a run: LATER (" + q + ")");
            ok = await Until(() => { lock (Host.CloseReplies) return Host.CloseReplies.Contains(11); }, 15000);
            R.Check(ok && Host.DialogsShown == shownBefore + 1, "the quit flow's dialog (CORE_WORK_QUIT thread), Stop and Quit, close_reply(QUIT, 1)");
            q = Core.core_close_asked(C, CoreApi.CORE_CLOSE_QUIT);
            R.Check(q == CoreApi.CORE_CLOSE_NOW, "the re-ask after the approval: NOW (" + q + ")");

            lock (Host.Log) R.Check(!Host.Log.Any(l => l.StartsWith("HOST")), "no exception in any callback, no render off the UI thread");
            R.Info("renders " + Host.Renders + ", drains " + Host.Drains + ", main jobs " + Host.MainJobs + ", worker jobs " + Host.WorkJobs);
        }

        private static unsafe int SetStrBytes(IntPtr C, Field f, string v)
        {
            byte[] b = Encoding.UTF8.GetBytes(v);
            fixed (byte* p = b) return Core.core_test_set_str_b(C, (int)f, p, (UIntPtr)b.Length);
        }

        private static void FolderPicker(IntPtr owner, string which)
        {
            long t0 = Stopwatch.GetTimestamp();
            IFileOpenDialog dlg = null;
            try
            {
                dlg = (IFileOpenDialog)new FileOpenDialogCoClass();
                long t1 = Stopwatch.GetTimestamp();
                dlg.SetOptions(Fos.PICKFOLDERS | Fos.FORCEFILESYSTEM | Fos.NOCHANGEDIR | Fos.PATHMUSTEXIST);
                dlg.SetTitle(Text(CoreText.T_PICK_FOLDER_MESSAGE));
                dlg.SetOkButtonLabel(Text(CoreText.T_PICK_FOLDER_CHOOSE));
                var dm = Dismisser.Start(Host.UiThreadId, Dismisser.Kind.FileDialog, 300);
                long t2 = Stopwatch.GetTimestamp();
                Host.Modal++;
                int hr;
                try { hr = dlg.Show(owner); }
                finally { Host.Modal--; }
                long shown = dm.Wait();
                R.Check(hr == unchecked((int)0x800704C7), "IFileOpenDialog (FOS_PICKFOLDERS), " + which + ": shown and cancelled (hr 0x" + hr.ToString("x8") + ")");
                R.Time("IFileOpenDialog CoCreateInstance, " + which, R.Ms(t0, t1));
                if (shown != 0) R.Time("IFileOpenDialog Show to visible, " + which, R.Ms(t2, shown));
            }
            catch (Exception e) { R.Check(false, "IFileOpenDialog, " + which + ": " + e.GetType().Name + " " + e.Message); }
            finally { if (dlg != null) Marshal.ReleaseComObject(dlg); }
        }

        private static unsafe string Text(CoreText id)
        {
            byte* buf = stackalloc byte[512];
            int r = Core.core_text(CoreApi.CORE_OS_WIN, (int)id, null, 0, buf, (UIntPtr)512, out UIntPtr len);
            return r == 0 ? Encoding.UTF8.GetString(buf, (int)len.ToUInt64()) : "";
        }

        private static unsafe void PerCallCosts(IntPtr C, MainWindow win)
        {
            const int N = 200000;
            IntPtr snap = Host.Held;
            byte[] ev = Encoding.UTF8.GetBytes("{\"ev\":\"activated\"}");
            byte* buf = stackalloc byte[256];
            Measure("core_api_version (SuppressUnmanagedCodeSecurity)", N, () => Core.core_api_version());
            Measure("core_api_version (with the security check)", N, () => CoreChecked.core_api_version());
            Measure("core_i64 F_TAB", N, () => Core.core_i64(snap, (int)Field.F_TAB));
            Measure("core_flag F_SPINNER", N, () => Core.core_flag(snap, (int)Field.F_SPINNER));
            Measure("core_str F_STATUS_LINE (pointer + length)", N, () => Core.core_str(snap, (int)Field.F_STATUS_LINE, out UIntPtr l));
            Measure("core_str F_STATUS_LINE + Encoding.UTF8.GetString", N, () => MainWindow.Str(snap, Field.F_STATUS_LINE));
            Measure("core_snap_rev", N, () => Core.core_snap_rev(snap, 0));
            Measure("core_snap_retain + core_snap_release", N, () => { Core.core_snap_retain(snap); Core.core_snap_release(snap); return 0; });
            Measure("core_text T_OK into a buffer + GetString", N, () =>
            {
                Core.core_text(CoreApi.CORE_OS_WIN, (int)CoreText.T_OK, null, 0, buf, (UIntPtr)256, out UIntPtr l);
                return Encoding.UTF8.GetString(buf, (int)l.ToUInt64());
            });
            Measure("core_menu_state M_PREP_PAUSE", N, () => Core.core_menu_state(C, (int)Menu.M_PREP_PAUSE, out IntPtr t, out UIntPtr l));
            Measure("core_event activated, LPUTF8Str", 20000, () => Core.core_event(C, "{\"ev\":\"activated\"}", CoreApi.Nts));
            Measure<int>("core_event activated, byte* + length", 20000, () => { fixed (byte* p = ev) return Core.core_event_b(C, p, (UIntPtr)ev.Length); });
            Core.core_commit(C);
            Measure("core_event malformed → the log callback (reverse P/Invoke)", 20000, () => Core.core_event(C, "{x", CoreApi.Nts));
            lock (Host.Log) Host.Log.RemoveAll(l => l == "core: a malformed event is ignored" || l == "core: an unknown event is ignored");
            long seq = 0;
            int rendersBefore = Host.Renders;
            Measure("core_test_set_i64 (a new value) + core_commit → a render callback and its apply", 20000, () =>
            {
                Core.core_test_set_i64(C, (int)Field.F_LOG_SHOW_SEQ, ++seq);
                Core.core_commit(C);
                return 0;
            });
            R.Check(Host.Renders - rendersBefore == 1000 + 3 * 20000, "one render per changed commit (" + (Host.Renders - rendersBefore) + ")");
            Measure("core_commit with nothing changed (no render)", N, () => { Core.core_commit(C); return 0; });
        }

        private static void Measure<T>(string what, int n, Func<T> f)
        {
            for (int i = 0; i < 1000; i++) f();
            double best = double.MaxValue;
            for (int rep = 0; rep < 3; rep++)
            {
                long t0 = Stopwatch.GetTimestamp();
                for (int i = 0; i < n; i++) f();
                long t1 = Stopwatch.GetTimestamp();
                best = Math.Min(best, R.Ms(t0, t1) * 1e6 / n);
            }
            R.PerCall(what, best);
        }

        // ------------------------------------------------------------------ end

        private static int finished;

        private static void Finish(Application app)
        {
            if (Interlocked.Exchange(ref finished, 1) != 0) return;
            lock (Host.Log) foreach (var l in Host.Log) R.Lines.Add("LOG  " + l);
            Write();
            app.Shutdown(R.Failures);
        }

        private static void Write()
        {
            var sb = new StringBuilder();
            sb.AppendLine("W0 spike " + RuntimeInformation.ProcessArchitecture + ": " + (R.Failures == 0 ? "PASS" : R.Failures + " FAILED"));
            foreach (var l in R.Lines) sb.AppendLine(l);
            File.WriteAllText(resultPath, sb.ToString(), new UTF8Encoding(false));
        }
    }
}
