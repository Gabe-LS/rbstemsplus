// The C# host table (core.h "Host callbacks", app.md §4.1), without WPF: the window's side is
// IShellUi (Shell.cs in the app, a fake in the Mac tests). The shell fills the entries it owns:
// render, front_facts, modal_open, facts, post_main, dialog, pick_folder, close_reply, log. The
// start config's "native" list hands post_work, slow_fact, step, admin and the helper to the core
// DLL's own Windows code (core_native.c), so those stay NULL here.
//
//  - Delegates live in fields of the one Host, which lives for the process (the GC must never
//    collect a delegate the core holds). Each entry is a try/catch wrapper returning its safe value.
//  - post_main: one locked queue, at most one drain pending on the UI thread. The drain runs jobs
//    for at most 6 ms, then core_commit (the turn's one render), and posts itself again (Input
//    priority in WPF: below Render, so frames still draw under a flood) while jobs are left.
//    While the core lives no job is dropped; once the dispatcher has stopped every job is dropped,
//    synchronously (core_job_drop).
//  - render: the snapshot is held until the next render has been applied, then released. A render
//    that fails is released at once, and the next one reloads everything.
//  - dialog and pick_folder run on a flow thread and block it until the window's answer.
//  - The main-only entries (render, front_facts, modal_open, facts, close_reply) check that they
//    run on the thread core_new ran on (app.md §4.1): called from another one they log, touch no
//    window state and return their safe value; a TEST build then stops at once (FailFast), as a
//    debug build asserts.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    internal interface IShellUi
    {
        /// Run drain on the UI thread, later, as one operation (WPF: BeginInvoke at Input).
        void PostDrain(Action drain);
        /// main: apply one render. delta is IntPtr.Zero for a full reload.
        void Render(IntPtr snap, IntPtr delta);
        /// main, live: the CORE_FRONT_* bits now.
        uint FrontFacts();
        /// main, live: a modal dialog of ours is up.
        bool ModalOpen();
        /// flow thread: show the form, block until it is answered. null: not shown (shutting down).
        DialogAnswer ShowDialog(DialogForm form);
        /// flow thread: the folder picker. 0 chosen (path set), 1 cancelled, negative not shown.
        int PickFolder(string start, out string path);
        /// main, from the core's own job: the answer to a LATER. Post the action, never do it here.
        void CloseReply(int what, bool proceed);
        /// any thread: one line for the log file and the log pane.
        void Log(string line);
    }

    internal sealed unsafe class Host
    {
        private readonly IShellUi ui;

        // kept alive with the Host: the core holds their function pointers (core.h Lifetime)
        private readonly RenderFn renderFn;
        private readonly FrontFactsFn frontFactsFn;
        private readonly ModalOpenFn modalOpenFn;
        private readonly FactsFn factsFn;
        private readonly PostMainFn postMainFn;
        private readonly PostWorkFn postWorkFn;
        private readonly DialogFn dialogFn;
        private readonly PickFolderFn pickFolderFn;
        private readonly CloseReplyFn closeReplyFn;
        private readonly LogFn logFn;

        /// The core (main thread only), once Start made it.
        public IntPtr C { get; private set; }
        /// The snapshot the window shows (the last render's), held until the next is applied.
        public IntPtr Held { get; private set; }
        /// The last render was applied (true once ui.Render returned; false after one threw, until
        /// a reload succeeds). The update commits only on a good frame (WU1).
        public bool RenderOk { get; private set; }
        /// The dispatcher has stopped: every job is dropped, every dialog answers "not shown".
        public volatile bool Stopped;
        /// TEST hosts only (the Mac tests): the shell runs post_work itself (a flow thread, the
        /// quit flow's own thread, the thread pool) instead of the DLL, which has no Windows code there.
        public readonly bool ShellWork;

        /// The main queue's run and drop: core_run and core_job_drop (a test's own counters in the
        /// Mac tests' race of PostMain against Stop).
        internal Action<IntPtr> RunJob = Core.core_run, DropJob = Core.core_job_drop;
        /// post_main's entry, for that test.
        internal void Post(IntPtr job) => PostMain(job);

        private readonly object qlock = new object();
        private readonly Queue<IntPtr> mainQueue = new Queue<IntPtr>();
        private bool drainPending;
        private bool reloadNext = true;   // the first render is a full reload; so is the one after a failed one
        private static readonly long DrainTicks = Stopwatch.Frequency * 6 / 1000;

        /// The thread core_new ran on (the core's main), 0 before Start.
        private int mainThread;
        /// A main-only entry called from another thread: logged, then (TEST builds) FailFast. The
        /// Mac tests replace it to see the call.
        internal Action<string> OffMain;

        /// True when this entry runs on the core's main thread; otherwise reports it once per call.
        private bool OnMain(string what)
        {
            if (mainThread == 0 || Thread.CurrentThread.ManagedThreadId == mainThread) return true;
            OffMain(what);
            return false;
        }

        public Host(IShellUi ui, bool shellWork = false)
        {
            this.ui = ui;
            ShellWork = shellWork;
            OffMain = what =>
            {
                ui.Log("shell: " + what + " called off the main thread (thread " + Thread.CurrentThread.ManagedThreadId + ")");
#if SHELL_TEST
                Environment.FailFast("shell: " + what + " called off the main thread");
#endif
            };
            renderFn = (ctx, snap, delta) =>
            {
                if (!OnMain("render")) { Core.core_snap_release(snap); reloadNext = true; RenderOk = false; return; }   // as a failed one: not applied, the next reloads
                try { Render(snap, delta); } catch (Exception e) { RenderFailed(snap, e); }
            };
            frontFactsFn = ctx => { if (!OnMain("front_facts")) return 0; try { return ui.FrontFacts(); } catch (Exception e) { Fail("front_facts", e); return 0; } };
            modalOpenFn = ctx => { if (!OnMain("modal_open")) return 1; try { return ui.ModalOpen() ? 1 : 0; } catch (Exception e) { Fail("modal_open", e); return 1; } };
            factsFn = (ctx, sheet) => { if (!OnMain("facts")) return; try { Facts(sheet); TestFacts(sheet); } catch (Exception e) { Fail("facts", e); } };
            postMainFn = (ctx, job) => { try { PostMain(job); } catch (Exception e) { Fail("post_main", e); Environment.FailFast("post_main: " + e.Message); } };
            postWorkFn = (ctx, kind, job) => { try { PostWork(kind, job); } catch (Exception e) { Fail("post_work", e); Environment.FailFast("post_work: " + e.Message); } };
            dialogFn = (ctx, d, r) => { try { return Dialog(d, r); } catch (Exception e) { Fail("dialog", e); return 1; } };
            pickFolderFn = (ctx, start, len, r) => { try { return PickFolder(start, len, r); } catch (Exception e) { Fail("pick_folder", e); return -1; } };
            closeReplyFn = (ctx, what, proceed) => { if (!OnMain("close_reply")) return; try { ui.CloseReply(what, proceed != 0); } catch (Exception e) { Fail("close_reply", e); } };
            logFn = (ctx, line, len) => { try { ui.Log(Core.Utf8(line, len)); } catch (Exception) { } };
        }

        /// The start config with "prepare_tabs":true when the window shows the Status and Prepare
        /// tabs (Shell.PrepareShown: the Prepare release, a TEST build's prepare scenarios); without
        /// it the core hides View > Status, View > Prepare and the Prepare menu (I10, I86).
        internal static string WithPrepareTabs(string config, bool shown)
        {
            if (!shown) return config;
            int end = config.LastIndexOf('}');
            return end < 0 ? config : config.Substring(0, end) + ",\"prepare_tabs\":true" + config.Substring(end);
        }

        /// The start config's "native" list: the entries the core DLL runs itself.
        public string NativeList => ShellWork
            ? "[\"slow_fact\",\"step\",\"admin\",\"helper\"]"
            : "[\"post_work\",\"slow_fact\",\"step\",\"admin\",\"helper\"]";

        private CoreHost Table()
        {
            IntPtr Fp(Delegate d) => Marshal.GetFunctionPointerForDelegate(d);
            return new CoreHost
            {
                size = (uint)Marshal.SizeOf(typeof(CoreHost)),
                reserved = 0,
                ctx = IntPtr.Zero,
                render = Fp(renderFn), front_facts = Fp(frontFactsFn), modal_open = Fp(modalOpenFn), facts = Fp(factsFn),
                post_main = Fp(postMainFn), post_work = ShellWork ? Fp(postWorkFn) : IntPtr.Zero,
                dialog = Fp(dialogFn), pick_folder = Fp(pickFolderFn),
                slow_fact = IntPtr.Zero, helper_spawn = IntPtr.Zero, helper_signal = IntPtr.Zero, admin = IntPtr.Zero, step = IntPtr.Zero,
                close_reply = Fp(closeReplyFn), log = Fp(logFn),
            };
        }

        /// core.h Life cycle, up to the first render: core_new (this thread becomes the core's
        /// main), {"ev":"start"}, core_commit. The window is shown after it, then Shown().
        /// False: the core refused the host or the config (the reason is in the log).
        public bool Start(string config)
        {
            if (!Create(config)) return false;
            Begin();
            return true;
        }

        /// core_new (this thread becomes the core's main), then the cheap facts' warm-up started on a
        /// worker (WarmFacts): the shell builds its window meanwhile (HR4). False: the core refused
        /// the host or the config (the reason is in the log).
        public bool Create(string config)
        {
            var table = Table();
            mainThread = Thread.CurrentThread.ManagedThreadId;   // core_new makes this thread the core's main
            C = Core.core_new(ref table, CoreApi.CORE_ROLE_APP, config, CoreApi.Nts);
            if (C == IntPtr.Zero) { ui.Log("core: core_new refused the host or the config"); return false; }
            StartWarm();
            return true;
        }

        /// The warm-up awaited (WarmWaitMs from Create at most), then {"ev":"start"} and core_commit.
        public void Begin()
        {
            WaitWarm();
            Send("{\"ev\":\"start\"}");
            Core.core_commit(C);
        }

        /// The window is on screen (ContentRendered): welcome, the update result, resume, ...
        public void Shown() => Send("{\"ev\":\"shown\"}");

        /// One event to the core (main, outside callbacks). A refusal is logged.
        public int Send(string json)
        {
            if (C == IntPtr.Zero) return CoreApi.CORE_E_STATE;
#if SHELL_TEST
            ShellProbe.Sent(json);   // the trigger's timing point (TEST builds only)
#endif
            int r = Core.core_event(C, json, CoreApi.Nts);
            if (r != CoreApi.CORE_OK) ui.Log("core: event refused (" + r + "): " + json);
            if (json == Activation.Activated) WarmOnActivation();   // after its refresh: that sheet decides
            return r;
        }

        public void Fail(string where, Exception e) => ui.Log("shell: exception in " + where + ": " + e.GetType().Name + ": " + e.Message);

        // ------------------------------------------------------------------ the main queue

        // Stopped is read and set under qlock, so a post racing Stop is either queued before it (and
        // dropped by Stop's DropAll) or dropped here, synchronously: never left neither run nor dropped
        private void PostMain(IntPtr job)
        {
            bool post = false, drop = false;
            lock (qlock)
            {
                if (Stopped) drop = true;
                else
                {
                    mainQueue.Enqueue(job);
                    if (!drainPending) { drainPending = true; post = true; }
                }
            }
            if (drop) { DropJob(job); return; }
            if (post) ui.PostDrain(Drain);
        }

        /// The UI thread's drain: jobs for at most 6 ms, then the one render, then again if needed.
        public void Drain()
        {
            if (Stopped) { DropAll(); return; }
            long limit = Stopwatch.GetTimestamp() + DrainTicks;
            while (true)
            {
                IntPtr job;
                lock (qlock)
                {
                    if (mainQueue.Count == 0) break;
                    job = mainQueue.Dequeue();
                }
                RunJob(job);
                if (Stopwatch.GetTimestamp() >= limit) break;
            }
            if (C != IntPtr.Zero) Core.core_commit(C);
            bool again;
            lock (qlock)
            {
                again = mainQueue.Count > 0;
                drainPending = again;
            }
            if (again) ui.PostDrain(Drain);
        }

        /// The dispatcher has stopped (shutdown): the queued jobs are dropped, and every later one.
        public void Stop()
        {
            lock (qlock) Stopped = true;
            DropAll();
        }

        private void DropAll()
        {
            while (true)
            {
                IntPtr job;
                lock (qlock)
                {
                    if (mainQueue.Count == 0) { drainPending = false; return; }
                    job = mainQueue.Dequeue();
                }
                DropJob(job);
            }
        }

        // ------------------------------------------------------------------ the TEST hosts' workers

        private System.Collections.Concurrent.BlockingCollection<IntPtr> flowQueue;

        // as PostMain: Stopped read under qlock, the job handed on (or dropped) inside it
        private void PostWork(int kind, IntPtr job)
        {
            bool drop;
            lock (qlock)
            {
                drop = Stopped;
                if (!drop)
                {
                    switch (kind)
                    {
                        case CoreApi.CORE_WORK_FLOW:
                            if (flowQueue == null)
                            {
                                var q = new System.Collections.Concurrent.BlockingCollection<IntPtr>();
                                flowQueue = q;
                                new Thread(() => { foreach (var j in q.GetConsumingEnumerable()) Core.core_run(j); }) { IsBackground = true, Name = "flow" }.Start();
                            }
                            flowQueue.Add(job);
                            break;
                        case CoreApi.CORE_WORK_QUIT:
                            new Thread(() => Core.core_run(job)) { IsBackground = true, Name = "quit flow" }.Start();
                            break;
                        default:
                            ThreadPool.UnsafeQueueUserWorkItem(_ => Core.core_run(job), null);
                            break;
                    }
                }
            }
            if (drop) Core.core_job_drop(job);
        }

        // ------------------------------------------------------------------ render

        private void Render(IntPtr snap, IntPtr delta)
        {
            ui.Render(snap, reloadNext ? IntPtr.Zero : delta);
            reloadNext = false;
            RenderOk = true;
            IntPtr previous = Held;
            Held = snap;
            if (previous != IntPtr.Zero) Core.core_snap_release(previous);
        }

        private void RenderFailed(IntPtr snap, Exception e)
        {
            Core.core_snap_release(snap);
            reloadNext = true;
            RenderOk = false;
            Fail("render", e);
        }

        // ------------------------------------------------------------------ facts

        // The cheap facts (core.h facts, facts.def): the core DLL's reads (core_win_cheap_facts,
        // win/c/host_facts.c), the one call the app sets; the Mac tests' host leaves the sheet empty.
        // Its answer: 1 the sheet carries FACT_LIB_UNCHECKED, 0 it doesn't, -1 nothing read (kept).
        internal Func<IntPtr, IntPtr, int> CheapFacts { get; set; }
        internal bool LibUnchecked { get; private set; }   // main: the last sheet's FACT_LIB_UNCHECKED
        private void Facts(IntPtr sheet)
        {
            var cheap = CheapFacts;
            if (cheap == null) return;
            int r = cheap(C, sheet);
            if (r >= 0) LibUnchecked = r == 1;
        }

        // The cheap facts' files (rekordbox.exe's and the ORTs' sha256 and AlphaTheta pin) read on a
        // worker first (core_win_warm_facts), started by Create right after core_new while the
        // window is built, so "start"'s refresh on the UI thread finds them memoised (app.md §2
        // rule 1); Begin waits for it, WarmWaitMs from Create at most. A warm still running then
        // carries on (WarmTask): what it hasn't memoised is read on the UI thread, and the DLL logs
        // it. The core is never freed while WarmTask runs (the shell never calls core_free).
        internal Action<IntPtr> WarmFacts { get; set; }
        internal int WarmWaitMs { get; set; } = 10000;
        internal Task WarmTask { get; private set; }
        private Task<bool> startWarm;          // main: the start's warm (StartWarm)
        private bool refreshAfterStartWarm;    // main: an unchecked activation found it running
        private readonly Stopwatch warmClock = new Stopwatch();
        private void StartWarm()
        {
            var warm = WarmFacts;
            if (warm == null) return;
            IntPtr c = C;
            warmClock.Restart();
            WarmTask = startWarm = Task.Run(() =>
            {
                try { warm(c); return true; }
                catch (Exception e) { ui.Log("shell: the facts' warm-up failed: " + e.GetType().Name + ": " + e.Message); return false; }
            });
        }
        private void WaitWarm()
        {
            if (WarmTask == null) return;
            int left = (int)Math.Max(0, WarmWaitMs - warmClock.ElapsedMilliseconds);
            if (!WarmTask.Wait(left)) ui.Log("shell: the facts' warm-up is still running after " + (WarmWaitMs / 1000) + " s: the first refresh reads on the UI thread");
        }

        // HG3 (app.md §2 rule 1): the last sheet said Pioneer's ORT unchecked (a pin with no answer,
        // which the UI thread remembers by stat and never asks again): on "activated" the warm runs
        // again on a worker, at most one at a time (the start's included: one still running is
        // left to end, and its end then refreshes once), then {"ev":"facts_warm"}, one refresh on
        // main that finds the files memoised. A sheet that says checked starts nothing. Pins stay
        // off the UI thread (HF2).
        public const string FactsWarm = "{\"ev\":\"facts_warm\"}";
        private void WarmOnActivation()
        {
            var warm = WarmFacts;
            if (!LibUnchecked || warm == null || Stopped || C == IntPtr.Zero) return;
            if (WarmTask != null && !WarmTask.IsCompleted)
            {
                // the start's warm still running posts nothing of its own: refresh once when it ends
                if (WarmTask == startWarm && !refreshAfterStartWarm)
                {
                    refreshAfterStartWarm = true;
                    startWarm.ContinueWith(t =>
                    {
                        if (!t.Result) return;
                        try { ui.PostDrain(Warmed); } catch (Exception e) { Fail("the warm's refresh", e); }
                    }, TaskScheduler.Default);
                }
                return;
            }
            IntPtr c = C;
            WarmTask = Task.Run(() =>
            {
                try { warm(c); }
                catch (Exception e) { ui.Log("shell: the facts' warm-up on activation failed: " + e.GetType().Name + ": " + e.Message); return; }
                try { ui.PostDrain(Warmed); } catch (Exception e) { Fail("the warm's refresh", e); }
            });
        }
        // main: the warm on activation ended (the queue's own UI-thread post, any order with its drain)
        private void Warmed()
        {
            if (Stopped || C == IntPtr.Zero) return;
            Send(FactsWarm);
        }

        // TEST builds: probe.conf's stand-in facts over the real ones (Probe.cs WriteFacts); a
        // release has no path to it (build.sh's gate refuses the probe in a release exe)
        private static void TestFacts(IntPtr sheet)
        {
#if SHELL_TEST
            ShellProbe.WriteFacts(sheet);
#endif
        }

        // ------------------------------------------------------------------ dialogs (flow thread)

        private int Dialog(IntPtr d, IntPtr r)
        {
            if (Stopped) return 1;
            var form = DialogForm.Read(d);
            var answer = ui.ShowDialog(form);
            if (answer == null) return 1;
            answer.Fill(r);
            return 0;
        }

        private int PickFolder(IntPtr start, UIntPtr len, IntPtr r)
        {
            if (Stopped) return -1;
            string s = len == CoreApi.Nts ? Core.Utf8z(start) : Core.Utf8(start, len);
            int rc = ui.PickFolder(s, out string path);
            if (rc == 0) Core.core_reply_field(r, 0, path ?? "", CoreApi.Nts);
            return rc;
        }
    }
}
