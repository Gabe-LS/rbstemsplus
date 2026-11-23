// The update's share of the start (app.md §5.4, W3b; the private Windows plan §2's swap order), without
// WPF: the Mac tests run it against a fake. The old app writes `update\pending`, starts the new
// one with --swap and quits; the new one:
//   1. waits for the old app to exit (core_win_swap_wait, ≤60 s);
//   2. checks what C alone can't: the core loaded and its self-test passed (Program.LoadCore), the
//      process's token (core_win_token_check, app.md §5.9), the minimum Windows and .NET, then the
//      WPF Application and main window started off-screen;
//   3. on a failed check records "failed swap" and starts the old app again through the stub
//      (core_win_swap_failed), with no UI of its own;
//   4. then starts as every start does: update_at_start (its wait included) before the one-copy
//      mutex and the first render. An update still running (UPDATE_BUSY) ends this start with no
//      UI; a failed update's key is kept for the flow's UPDATE_RESULT.
// `started` and the pointers come only after the first full snapshot is on screen
// (core_win_update_after_render, Shell). A --swap that no `pending` names (an update already
// undone) is a normal start. Every other start asks the token first, before update_at_start or
// the one-copy mutex: an over-elevated start, UAC off, a service account or an unknown token is
// refused with one line and nothing written (I35). While a --swap is in its checks and its real start, a SwapGuard
// (30 s per stage, a background thread) recovers and kills a start that hangs.
using System;
using System.Threading;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    /// The core DLL's update entries (update_shell.h) as the start sequence uses them. Each answers
    /// -1 when the DLL can't be called (not loaded, or without the entry).
    internal interface IStartNative
    {
        /// core_win_swap_wait: StartSequence.SwapReady, SwapNone, SwapOldRuns, or -1.
        int SwapWait(uint ms);
        /// core_win_swap_failed: 0, an errno, or -1.
        int SwapFailed();
        /// core_win_os_check: 0 ok, 1 Windows too old, 2 .NET too old, or -1.
        int OsCheck();
        /// core_win_update_at_start: UPDATE_BUSY (-2), UPDATE_FAIL_NONE (-1) or a failure's key.
        int AtStart();
        /// core_win_token_check: StartSequence.TokenOk ... TokenUnknown (lifecycle.h's LIFE_TOKEN_*), or -1.
        int TokenCheck();
        /// Without the core: the stub started (the current app, with no arguments). False: it couldn't be.
        bool StartStub();
        /// Without the core (WU9): waits, at most ms, for the old app `update\pending` names (its pid
        /// and start time) when that record names this app as the new one; returns at once otherwise.
        void WaitOldApp(uint ms);
        /// Binds what the guard's recovery calls (core_win_swap_failed) before anything can hang.
        void Prelink();
        void Log(string line);
    }

    internal enum StartOutcome
    {
        Go,       // start: the window, the core
        Refuse,   // Program's one-line refusal, then exit 1
        Exit,     // exit with no UI (ExitCode)
    }

    internal static class StartSequence
    {
        public const int SwapReady = 0, SwapNone = 1, SwapOldRuns = 2;
        public const int UpdateBusy = -2, UpdateFailNone = -1;
        public const uint SwapWaitMs = 60000;   // update.h's UPDATE_SWAP_WAIT_MS
        /// lifecycle.h's LIFE_TOKEN_* (core_win_token_check)
        public const int TokenOk = 0, TokenElevated = 1, TokenUacOff = 2, TokenService = 3, TokenUnknown = 4;

        public static StartOutcome Run(bool swap, string coreWhy, IStartNative n, Func<string> offScreen, SwapGuard guard,
                                       out int exitCode, out bool swapping) =>
            Run(swap, coreWhy, n, offScreen, guard, out exitCode, out swapping, out _);

        /// swap: --swap was given. coreWhy: Program.LoadCore's refusal (null: the core is this
        /// build's and its self-test passed). offScreen: the WPF Application and main window started
        /// off-screen and closed again; null when they rendered, else why not. It runs only for a
        /// --swap whose other checks passed, under guard (armed for one stage before it). swapping:
        /// true only for a READY --swap whose every check passed: the real start that follows is the
        /// update's, its failures go to Recover, and the guard stays armed (Program re-arms it).
        /// refusal: the dialog's text for a token or minimum-version refusal; null for coreWhy.
        public static StartOutcome Run(bool swap, string coreWhy, IStartNative n, Func<string> offScreen, SwapGuard guard,
                                       out int exitCode, out bool swapping, out CoreText? refusal)
        {
            exitCode = 0;
            swapping = false;
            refusal = null;
            string swapNoneLine = null;
            if (swap)
            {
                int w = n.SwapWait(SwapWaitMs);
                if (w == SwapNone)
                {
                    swapNoneLine = "--swap: no pending update names this app: a normal start";
                }
                else if (w == SwapReady || w == SwapOldRuns)
                {
                    string fail = w == SwapOldRuns ? "the app being replaced still runs" : coreWhy ?? TokenWhy(n.TokenCheck()) ?? MinimumWhy(n.OsCheck());
                    if (fail == null)
                    {
                        // the off-screen start's own timeout (30 s) wins; the grace covers a hung thread (WV3)
                        guard.Arm(SwapGuard.StageMs + SwapGuard.GraceMs);
                        fail = offScreen();
                    }
                    if (fail != null)
                    {
                        n.Log("--swap: " + fail + ": the update stops here");
                        if (guard.Finish()) Recover(n);
                        else guard.Settle();   // the guard fired: its recovery, then the kill
                        exitCode = 1;
                        return StartOutcome.Exit;
                    }
                    swapping = true;
                }
                else
                {
                    // the core can't record anything: `pending` stays, and the old app's next start
                    // reads it as a failed update (a missing result reads as failed)
                    n.Log("--swap: " + (coreWhy ?? "the core's update entries can't be called") + ": the app being replaced is started again");
                    n.WaitOldApp(SwapWaitMs);   // the stub would only hand off to the old app while it quits
                    if (!n.StartStub()) n.Log("--swap: the stub couldn't be started");
                    exitCode = 1;
                    return StartOutcome.Exit;
                }
            }
            if (coreWhy != null) return StartOutcome.Refuse;
            if (!swapping)   // a READY swap checked these already
            {
                refusal = TokenRefusal(n.TokenCheck()) ?? MinimumRefusal(n.OsCheck());
                if (refusal.HasValue) return StartOutcome.Refuse;
            }
            if (swapNoneLine != null) n.Log(swapNoneLine);
            if (n.AtStart() == UpdateBusy)   // update_at_start logged why
            {
                swapping = false;
                if (!guard.Finish()) guard.Settle();
                return StartOutcome.Exit;
            }
            return StartOutcome.Go;
        }

        /// A --swap that can't go on: "failed swap" and the old app started again through the stub
        /// (core_win_swap_failed); the shell starts the stub itself when that can't be called.
        public static void Recover(IStartNative n)
        {
            int r = n.SwapFailed();
            if (r < 0)
            {
                n.Log("--swap: the result couldn't be written: the app being replaced is started again");
                if (!n.StartStub()) n.Log("--swap: the stub couldn't be started");
            }
            else if (r > 0) n.Log("--swap: the result or the restart failed (" + r + ")");
        }

        /// core_win_token_check's answer as a --swap check's reason, null when it passes
        public static string TokenWhy(int token) =>
            token == TokenOk ? null
            : "the process's token is refused (" + (token == TokenElevated ? "Run as administrator" : token == TokenUacOff ? "UAC off"
                                                    : token == TokenService ? "a service account" : token < 0 ? "not askable" : "unknown") + ")";

        /// The start's refusal for a token (app.md §5.9, I35), under "RB Stems Plus can't open":
        /// Windows' T_START_ROOT, T_WIN_START_UAC_OFF, else T_CANT_OPEN alone; null when it may start.
        public static RbStemsPlus.Core.Text? TokenRefusal(int token) =>
            token == TokenOk ? (RbStemsPlus.Core.Text?)null
            : token == TokenElevated ? RbStemsPlus.Core.Text.T_START_ROOT
            : token == TokenUacOff ? RbStemsPlus.Core.Text.T_WIN_START_UAC_OFF
            : RbStemsPlus.Core.Text.T_CANT_OPEN;

        /// core_win_os_check's answer as a reason, null when it passes (or couldn't be asked)
        public static string MinimumWhy(int os) =>
            os == 1 ? "Windows is older than the minimum (11 22H2)" : os == 2 ? ".NET Framework is older than 4.8.1" : null;

        private static CoreText? MinimumRefusal(int os) =>
            os == 1 ? CoreText.T_WIN_START_OLD_WINDOWS : os == 2 ? CoreText.T_WIN_START_OLD_NET : (CoreText?)null;
    }

    /// `update\pending` as update.c writes it (update_pending_format): the old app's version, the
    /// new one's, the old app's pid and its start time (seconds since 1970), one per line, each
    /// ended by "\n"; read here only when the core can't be called (WU9).
    internal sealed class PendingRecord
    {
        public string From, To;
        public uint Pid;
        public long Started;

        /// null when text isn't such a record
        public static PendingRecord Parse(string text)
        {
            if (text == null || text.Length == 0 || text.Length >= 256 || text[text.Length - 1] != '\n') return null;
            string[] l = text.Substring(0, text.Length - 1).Split('\n');
            if (l.Length != 4 || !Version(l[0]) || !Version(l[1]) || !Digits(l[2], 10) || !Digits(l[3], 18)) return null;
            if (!uint.TryParse(l[2], System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out uint pid)) return null;
            long started = long.Parse(l[3], System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture);
            return new PendingRecord { From = l[0], To = l[1], Pid = pid, Started = started };
        }

        private static bool Digits(string s, int max)
        {
            if (s.Length == 0 || s.Length > max) return false;
            foreach (char c in s) if (c < '0' || c > '9') return false;
            return true;
        }

        // x.y.z, each part digits without a leading zero (payload_is_release_version)
        private static bool Version(string s)
        {
            string[] p = s.Split('.');
            if (p.Length != 3) return false;
            foreach (string x in p) if (!Digits(x, 9) || (x.Length > 1 && x[0] == '0')) return false;
            return true;
        }
    }

    /// The app's one log, written by the updater (its own thread or the start's) and the shell
    /// (WU5): whoever's line starts a batch posts the pane's one flush. Before the window exists
    /// there is no flush; the shell posts one once it does, which drains the lines from before.
    internal sealed class LogSink
    {
        public readonly LogBuffer Buffer;
        private Action flush;

        public LogSink(LogBuffer buffer) { Buffer = buffer; }

        /// The pane's flush poster (the shell's, at Input priority); null: none yet.
        public void SetFlush(Action f) => Volatile.Write(ref flush, f);

        public void Add(string line)
        {
            if (Buffer.Add(line)) Volatile.Read(ref flush)?.Invoke();
        }
    }

    /// When the update commits (WU1): after a frame is drawn (ContentRendered) with the last
    /// render applied, once. A first render that failed commits on the first good reload's frame
    /// instead; under --swap, never once the guard has fired. Main thread only.
    internal sealed class CommitGate
    {
        private bool framed, committed;

        public bool Committed => committed;

        /// A frame is on screen (ContentRendered).
        public void Framed() => framed = true;

        /// A render was applied (ok) or failed: whether to try the commit after its frame.
        public bool Rendered(bool ok) => ok && framed && !committed;

        /// True exactly once: framed, the last render applied, and finish (the swap guard's
        /// Finish; null: none) agrees. A guard that fired ends every later try.
        public bool TryCommit(bool renderOk, Func<bool> finish)
        {
            if (committed || !framed || !renderOk) return false;
            committed = true;
            return finish == null || finish();
        }
    }

    /// The --swap's safety net (WU2, WV1+WV2): a background thread that, when a stage (the off-screen
    /// start, then the real start to its first good frame) isn't finished within its time, has the
    /// recovery (StartSequence.Recover) run on its own "swap recovery" thread, waits for it at most
    /// RecoverMs, then kills this process: a hung UI thread can't record anything, and a recovery
    /// that hangs (a held log lock, a stalled call) can't hold the kill. The guard thread itself
    /// never logs and never takes a lock. The stub is started before the kill; the old app's
    /// update_at_start waits (up to 60 s) for this process to leave the new folder, so the overlap
    /// is safe. Finish() and the expiry race through one Interlocked state: exactly one of them
    /// wins; a caller that loses calls Settle(), which ends the process the same way.
    internal sealed class SwapGuard
    {
        public const int StageMs = 30000;
        public const int GraceMs = 5000;     // the off-screen stage's guard over its own 30 s timeout (WV3)
        public const int RecoverMs = 5000;   // the recovery's bound before the kill
        private const int Idle = 0, Armed = 1, Done = 2, Fired = 3;

        private readonly IStartNative n;
        private readonly Action kill;
        private readonly Func<long> nowMs;
        private readonly int pollMs, recoverMs;
        private readonly ManualResetEventSlim recovered = new ManualResetEventSlim(false);
        private int state;
        private long deadline;
        private Thread thread;

        /// kill: ends the process (TerminateProcess in the app). nowMs, pollMs, recoverMs: the clock,
        /// the thread's poll and the recovery's bound (the tests' own).
        public SwapGuard(IStartNative n, Action kill, Func<long> nowMs = null, int pollMs = 50, int recoverMs = RecoverMs)
        {
            this.n = n;
            this.kill = kill;
            this.nowMs = nowMs ?? (() => TimestampMs(System.Diagnostics.Stopwatch.GetTimestamp()));
            this.pollMs = pollMs;
            this.recoverMs = recoverMs;
        }

        internal static long TimestampMs(long timestamp) => (long)((decimal)timestamp * 1000 / System.Diagnostics.Stopwatch.Frequency);

        public bool HasFired => Volatile.Read(ref state) == Fired;

        /// Arms (or re-arms) for ms from now. Nothing once finished or fired.
        public void Arm(int ms)
        {
            Interlocked.Exchange(ref deadline, nowMs() + ms);
            int was = Interlocked.CompareExchange(ref state, Armed, Idle);
            if (was == Idle)
            {
                n.Prelink();
                thread = new Thread(Watch) { IsBackground = true, Name = "swap guard" };
                thread.Start();
            }
        }

        /// The stage is over (the first good frame committed, or a recovery by the caller): false
        /// when the guard has fired (it recovered; the caller does nothing more), else true.
        public bool Finish()
        {
            Interlocked.CompareExchange(ref state, Done, Armed);
            return Volatile.Read(ref state) != Fired;
        }

        /// A caller that lost to the guard (Finish false), or the app's end: once fired, waits for
        /// the recovery (at most RecoverMs) and kills; in the app it never returns then. Unfired:
        /// returns at once. The kill is idempotent, so the guard and a caller may both reach it.
        public void Settle()
        {
            if (Volatile.Read(ref state) != Fired) return;
            recovered.Wait(recoverMs);
            kill();
        }

        private void Watch()
        {
            while (Volatile.Read(ref state) == Armed)
            {
                if (nowMs() >= Interlocked.Read(ref deadline))
                {
                    if (Interlocked.CompareExchange(ref state, Fired, Armed) != Armed) return;
                    new Thread(() =>
                    {
                        try
                        {
                            n.Log("--swap: the start didn't finish in time: the update stops here");
                            StartSequence.Recover(n);
                        }
                        catch (Exception) { }
                        finally { recovered.Set(); }
                    }) { IsBackground = true, Name = "swap recovery" }.Start();
                    recovered.Wait(recoverMs);
                    kill();
                    return;
                }
                Thread.Sleep(pollMs);
            }
        }
    }
}
