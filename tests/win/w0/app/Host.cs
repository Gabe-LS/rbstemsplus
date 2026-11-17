// The C# host table (app.md §4.1): delegates held in static fields for the process lifetime, each
// one a try/catch wrapper returning its entry's safe value; one locked main queue with at most one
// pending dispatcher operation that drains for at most 6 ms, then core_commit (the one render),
// re-posted at Input priority; a serial flow thread; the quit flow beside it; slow work on the
// thread pool; a fake helper reader thread.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;
using RbStemsPlus.Core;

namespace RbStemsPlus.W0
{
    internal static unsafe class Host
    {
        // kept alive: the GC must never collect a delegate the core holds (core.h Lifetime)
        private static RenderFn renderFn;
        private static FrontFactsFn frontFactsFn;
        private static ModalOpenFn modalOpenFn;
        private static FactsFn factsFn;
        private static PostMainFn postMainFn;
        private static PostWorkFn postWorkFn;
        private static DialogFn dialogFn;
        private static PickFolderFn pickFolderFn;
        private static SlowFactFn slowFactFn;
        private static HelperSpawnFn helperSpawnFn;
        private static HelperSignalFn helperSignalFn;
        private static AdminFn adminFn;
        private static StepFn stepFn;
        private static CloseReplyFn closeReplyFn;
        private static LogFn logFn;

        public static IntPtr C;                 // the core
        public static Dispatcher Ui;
        public static MainWindow Window;
        public static uint UiThreadId;
        public static IntPtr Held;              // the snapshot the window shows
        public static int Modal;                // our modal count (modal_open)
        public static volatile bool Stopped = false;    // the dispatcher has stopped: drop jobs

        // counters the spike checks
        public static int Renders, ChangedRows, Reloads, Drains, MainJobs, WorkJobs, OffMainPosts;
        public static double LongestDrainMs, LongestRenderMs;
        public static readonly List<string> Log = new List<string>();
        public static readonly List<string> Steps = new List<string>();
        public static readonly List<int> CloseReplies = new List<int>();   // what * 10 + proceed
        public static volatile bool AutoAnswer;  // dialogs answered on the flow thread, no UI (the stress loop)
        public static volatile int DialogsShown;
        public static int HelperLinesPerRun = 5000;

        private static readonly object qlock = new object();
        private static readonly Queue<IntPtr> mainQueue = new Queue<IntPtr>();
        private static bool drainPending;
        private static readonly BlockingCollection<IntPtr> flowQueue = new BlockingCollection<IntPtr>();

        private static readonly double TicksToMs = 1000.0 / Stopwatch.Frequency;

        public static void Fail(string where, Exception e)
        {
            lock (Log) Log.Add("HOST EXCEPTION in " + where + ": " + e.GetType().Name + ": " + e.Message);
        }

        public static CoreHost MakeTable()
        {
            renderFn = (ctx, snap, delta) => { try { Render(snap, delta); } catch (Exception e) { Fail("render", e); Core.core_snap_release(snap); } };
            frontFactsFn = ctx => { try { return FrontFacts(); } catch (Exception e) { Fail("front_facts", e); return 0; } };
            modalOpenFn = ctx => { try { return Modal > 0 ? 1 : 0; } catch (Exception e) { Fail("modal_open", e); return 1; } };
            factsFn = (ctx, sheet) => { try { Core.core_fact_flag(sheet, (int)Fact.FACT_STEMS_PLUS_PRESENT, 1); } catch (Exception e) { Fail("facts", e); } };
            postMainFn = (ctx, job) => { try { PostMain(job); } catch (Exception e) { Fail("post_main", e); Environment.FailFast("post_main: " + e.Message); } };
            postWorkFn = (ctx, kind, job) => { try { PostWork(kind, job); } catch (Exception e) { Fail("post_work", e); Environment.FailFast("post_work: " + e.Message); } };
            dialogFn = (ctx, d, r) => { try { return Dialog(d, r); } catch (Exception e) { Fail("dialog", e); return 1; } };
            pickFolderFn = (ctx, start, len, r) => { try { return 1; } catch (Exception e) { Fail("pick_folder", e); return -1; } };
            slowFactFn = (ctx, kind, key, len, sheet) => { try { return 1; } catch (Exception e) { Fail("slow_fact", e); return 1; } };
            helperSpawnFn = (ctx, run, argv, argc) => { try { return HelperSpawn(run, argv, argc); } catch (Exception e) { Fail("helper_spawn", e); return 1; } };
            helperSignalFn = (ctx, run, sig) => { try { } catch (Exception e) { Fail("helper_signal", e); } };
            adminFn = (ctx, kind, v, b, bs, os, force) => { try { return -1; } catch (Exception e) { Fail("admin", e); return -1; } };
            stepFn = (ctx, kind, args, nargs, sheet) => { try { return Step(kind, args, nargs); } catch (Exception e) { Fail("step", e); return 1; } };
            closeReplyFn = (ctx, what, proceed) => { try { lock (CloseReplies) CloseReplies.Add(what * 10 + proceed); } catch (Exception e) { Fail("close_reply", e); } };
            logFn = (ctx, line, len) => { try { var s = Core.Utf8(line, len); lock (Log) Log.Add(s); } catch (Exception e) { Fail("log", e); } };

            return new CoreHost
            {
                size = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(CoreHost)),
                reserved = 0,
                ctx = IntPtr.Zero,
                render = Fp(renderFn), front_facts = Fp(frontFactsFn), modal_open = Fp(modalOpenFn), facts = Fp(factsFn),
                post_main = Fp(postMainFn), post_work = Fp(postWorkFn), dialog = Fp(dialogFn), pick_folder = Fp(pickFolderFn),
                slow_fact = Fp(slowFactFn), helper_spawn = Fp(helperSpawnFn), helper_signal = Fp(helperSignalFn),
                admin = Fp(adminFn), step = Fp(stepFn), close_reply = Fp(closeReplyFn), log = Fp(logFn),
            };
        }

        private static IntPtr Fp(Delegate d) => System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(d);

        public static void StartFlowThread()
        {
            var t = new Thread(() =>
            {
                foreach (var job in flowQueue.GetConsumingEnumerable()) Core.core_run(job);
            }) { IsBackground = true, Name = "flow" };
            t.Start();
        }

        // ------------------------------------------------------------------ main queue and drain

        private static void PostMain(IntPtr job)
        {
            if (Win32.GetCurrentThreadId() != UiThreadId) Interlocked.Increment(ref OffMainPosts);
            if (Stopped) { Core.core_job_drop(job); return; }
            bool post = false;
            lock (qlock)
            {
                mainQueue.Enqueue(job);
                if (!drainPending) { drainPending = true; post = true; }
            }
            if (post) Ui.BeginInvoke(DispatcherPriority.Input, (Action)Drain);
        }

        private static void Drain()
        {
            long t0 = Stopwatch.GetTimestamp();
            long limit = t0 + (long)(6.0 / TicksToMs);
            Drains++;
            while (true)
            {
                IntPtr job;
                lock (qlock)
                {
                    if (mainQueue.Count == 0) break;
                    job = mainQueue.Dequeue();
                }
                Core.core_run(job);
                MainJobs++;
                if (Stopwatch.GetTimestamp() >= limit) break;
            }
            if (C != IntPtr.Zero) Core.core_commit(C);
            bool again;
            lock (qlock)
            {
                again = mainQueue.Count > 0;
                drainPending = again;
            }
            if (again) Ui.BeginInvoke(DispatcherPriority.Input, (Action)Drain);
            double ms = (Stopwatch.GetTimestamp() - t0) * TicksToMs;
            if (ms > LongestDrainMs) LongestDrainMs = ms;
        }

        private static void PostWork(int kind, IntPtr job)
        {
            Interlocked.Increment(ref WorkJobs);
            if (Stopped) { Core.core_job_drop(job); return; }
            switch (kind)
            {
                case CoreApi.CORE_WORK_FLOW: flowQueue.Add(job); break;
                case CoreApi.CORE_WORK_QUIT: new Thread(() => Core.core_run(job)) { IsBackground = true, Name = "quit flow" }.Start(); break;
                default: ThreadPool.UnsafeQueueUserWorkItem(_ => Core.core_run(job), null); break;
            }
        }

        // ------------------------------------------------------------------ render

        private static void Render(IntPtr snap, IntPtr delta)
        {
            long t0 = Stopwatch.GetTimestamp();
            if (Win32.GetCurrentThreadId() != UiThreadId) lock (Log) Log.Add("HOST: render off the UI thread");
            Renders++;
            Window.Apply(snap, delta);
            if (delta != IntPtr.Zero)
            {
                int t = (int)Table.T_PREP_TRACKS;
                if (Core.core_delta_reload(delta, t) != 0) Reloads++;
                else { Core.core_delta_changed(delta, t, out int n); ChangedRows += n; }
            }
            if (Held != IntPtr.Zero) Core.core_snap_release(Held);
            Held = snap;
            double ms = (Stopwatch.GetTimestamp() - t0) * TicksToMs;
            if (ms > LongestRenderMs) LongestRenderMs = ms;
        }

        private static uint FrontFacts()
        {
            uint bits = 0;
            var w = Window;
            if (w == null) return 0;
            foreach (System.Windows.Window x in Application.Current.Windows) if (x.IsActive) bits |= CoreApi.CORE_FRONT_ACTIVE;
            if (w.IsLoaded) bits |= CoreApi.CORE_FRONT_WINDOW_LOADED;
            if (w.WindowState == WindowState.Minimized) bits |= CoreApi.CORE_FRONT_MINIATURIZED;
            bits |= (uint)CoreApi.CORE_TAB_PREPARE << CoreApi.CORE_FRONT_TAB_SHIFT;
            return bits;
        }

        // ------------------------------------------------------------------ dialogs (flow thread)

        public static string DlgText(IntPtr d, int what, int i)
        {
            IntPtr p = Core.core_dlg_text(d, what, i, 0, out UIntPtr len);
            return Core.Utf8(p, len);
        }

        public static long LastDialogShownTicks, LastDialogAskedTicks;

        private static int Dialog(IntPtr d, IntPtr r)
        {
            string title = DlgText(d, 0 /* CORE_DLG_TITLE */, 0);
            int n = Core.core_dlg_int(d, 7 /* CORE_DLG_N_BUTTONS */, 0, 0);
            int def = Core.core_dlg_int(d, 11 /* CORE_DLG_DEFAULT */, 0, 0);
            int cancel = Core.core_dlg_int(d, 12 /* CORE_DLG_CANCEL */, 0, 0);
            var buttons = new string[n];
            for (int i = 0; i < n; i++) buttons[i] = DlgText(d, 2 /* CORE_DLG_BUTTON */, i);
            if (AutoAnswer) { Core.core_reply_button(r, def); return 0; }
            // the app-modal presenter: ShowDialog-like, from its own dispatcher operation, owned by the window
            var done = new ManualResetEventSlim(false);
            int chosen = cancel;
            LastDialogAskedTicks = Stopwatch.GetTimestamp();
            Ui.BeginInvoke(DispatcherPriority.Send, (Action)(() =>
            {
                Modal++;
                try
                {
                    DialogsShown++;
                    var dismiss = Dismisser.Start(UiThreadId, Dismisser.Kind.TaskDialog);
                    IntPtr owner = new WindowInteropHelper(Window).Handle;
                    int hr = Win32.TaskDialog(owner, IntPtr.Zero, title, title, string.Join(" / ", buttons),
                                              Win32.TDCBF_OK_BUTTON | Win32.TDCBF_CANCEL_BUTTON, IntPtr.Zero, out int pressed);
                    LastDialogShownTicks = dismiss.Wait();
                    chosen = hr == 0 && pressed == Win32.IDOK ? def : cancel;
                    Core.core_reply_button(r, chosen);
                }
                finally { Modal--; done.Set(); }
            }));
            done.Wait();
            return 0;
        }

        // ------------------------------------------------------------------ steps and the fake helper

        private static int Step(int kind, IntPtr args, int nargs)
        {
            var sb = new StringBuilder("step " + kind);
            for (int i = 0; i < nargs; i++)
            {
                IntPtr p = ((IntPtr*)args)[i];
                int len = 0;
                while (((byte*)p)[len] != 0) len++;
                sb.Append(' ').Append(Encoding.UTF8.GetString((byte*)p, len));
            }
            lock (Steps) Steps.Add(sb.ToString());
            return 0;
        }

        public static readonly List<string> HelperArgv = new List<string>();
        public static volatile int HelperRunsEnded;

        private static int HelperSpawn(ulong run, IntPtr argv, int argc)
        {
            var a = new List<string>();
            for (int i = 0; i < argc; i++) a.Add(Core.Utf8(((IntPtr*)argv)[i], Strlen(((IntPtr*)argv)[i])));
            lock (HelperArgv) HelperArgv.Add(string.Join(" ", a));
            IntPtr c = C;
            int lines = HelperLinesPerRun;
            new Thread(() =>
            {
                // the helper's reader thread: each line once, then exactly one core_helper_ended
                byte* buf = stackalloc byte[64];
                for (int i = 0; i < lines; i++)
                {
                    int n = Ascii(buf, "{\"line\":" + i + "}");
                    Core.core_helper_line(c, run, buf, (UIntPtr)n);
                }
                Core.core_helper_ended(c, run, 0, null, UIntPtr.Zero);
                HelperRunsEnded++;
            }) { IsBackground = true, Name = "helper reader" }.Start();
            return 0;
        }

        private static UIntPtr Strlen(IntPtr p)
        {
            int n = 0;
            while (((byte*)p)[n] != 0) n++;
            return (UIntPtr)n;
        }

        private static int Ascii(byte* dst, string s)
        {
            for (int i = 0; i < s.Length; i++) dst[i] = (byte)s[i];
            return s.Length;
        }
    }

    // Finds the dialog our UI thread shows (#32770), times its appearance and dismisses it, so the
    // spike runs unattended in the VM. A real user is never involved.
    internal sealed unsafe class Dismisser
    {
        public enum Kind { TaskDialog, FileDialog }
        private readonly ManualResetEventSlim done = new ManualResetEventSlim(false);
        private long shownTicks;

        public static Dismisser Start(uint uiThread, Kind kind, int holdMs = 150)
        {
            var d = new Dismisser();
            new Thread(() => d.Run(uiThread, kind, holdMs)) { IsBackground = true, Name = "dismisser" }.Start();
            return d;
        }

        public long Wait() { done.Wait(15000); return shownTicks; }

        private void Run(uint uiThread, Kind kind, int holdMs)
        {
            var sw = Stopwatch.StartNew();
            IntPtr found = IntPtr.Zero;
            while (found == IntPtr.Zero && sw.ElapsedMilliseconds < 10000)
            {
                Win32.EnumThreadWindows(uiThread, (h, l) =>
                {
                    char* cls = stackalloc char[64];
                    int n = Win32.GetClassNameW(h, cls, 64);
                    if (n > 0 && new string(cls, 0, n) == "#32770" && Win32.IsWindowVisible(h) != 0) { found = h; return 0; }
                    return 1;
                }, IntPtr.Zero);
                if (found == IntPtr.Zero) Thread.Sleep(1);
            }
            shownTicks = found == IntPtr.Zero ? 0 : Stopwatch.GetTimestamp();
            if (found != IntPtr.Zero)
            {
                Thread.Sleep(holdMs);
                if (kind == Kind.TaskDialog) Win32.PostMessageW(found, Win32.TDM_CLICK_BUTTON, (IntPtr)Win32.IDOK, IntPtr.Zero);
                else Win32.PostMessageW(found, Win32.WM_COMMAND, (IntPtr)Win32.IDCANCEL, IntPtr.Zero);
            }
            done.Set();
        }
    }
}
