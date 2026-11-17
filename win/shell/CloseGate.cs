// The flow-based close and the logoff (app.md §5.9, W6b; core.h "Close and quit"), without WPF:
// the Mac tests run it against a fake.
//  - Window.Closing is cancelled and the core decides (core_close_asked): NOW, or a later
//    close_reply(proceed 1), lets the window go once CriticalWork is idle, waited for off the UI
//    thread for at most 60 s (Shell.swift's whenIdle). REFUSE, or a reply of 0, stays.
//  - The flow's UPDATE_SWAP (and SELF_DELETE) quits at once, asking nothing, after the same wait.
//  - Logoff, in WPF's order: Application.SessionEnding (WM_QUERYENDSESSION on WPF's own window)
//    vetoes only while a step that must finish runs, naming it (ShutdownBlockReasonCreate):
//    Windows then shows the reason, and WM_ENDSESSION(TRUE) on our window runs the bounded wait,
//    (FALSE) clears the reason. After a veto the app quits by itself once no step runs (by the
//    veto's 30 s deadline at the latest: a step that begins meanwhile is waited for, for the time
//    left; Exit's wait then takes only what is left too), so the logoff Windows holds goes on, as macOS's terminateLater and its reply: Windows
//    continues once the app blocking it has exited. A cancelled logoff can't be resumed, so that
//    quit is dropped when WM_ENDSESSION(FALSE) or another query came first (a session count).
//    With nothing running WPF's query-time shutdown stands (Closing, then Exit, whose EndWait is a
//    bounded catch for a step that started after the query); a logoff cancelled later leaves the
//    app closed, as on macOS. The session's end asks the core nothing: the worker finishes on its
//    own.
//  - CriticalWork's admission (core_win_critical_close) closes when the quit is decided: Proceed,
//    WM_ENDSESSION(TRUE) (EndWait), WPF's session-end Closing and the veto's quit. A query that
//    can still be cancelled leaves it open. A step already running is still waited for (MC1, MC2).
using System;
using System.Threading;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    /// What the close needs from the window, the core and the DLL's CriticalWork.
    internal interface ICloseSide
    {
        /// core_close_asked (main).
        int Ask(int what);
        /// CriticalWork: a step that must finish runs now (any thread).
        bool CriticalRunning();
        /// CriticalWork's whenIdle: true once idle, false after about ms (any thread but a step's).
        bool CriticalWait(int ms);
        /// CriticalWork's admission (core_win_critical_close, main): the quit is decided, so no new
        /// step starts (a new elevation is "stopped" without a prompt, a model copy isn't begun);
        /// one running is still waited for. One-way.
        void CriticalClose();
        /// Run on a worker thread.
        void RunOff(Action a);
        /// Run on the UI thread, later (its own dispatcher operation).
        void Post(Action a);
        /// Close the main window (main): Closing runs again and lets it go.
        void Close();
        /// The logoff's reason on the main window (ShutdownBlockReasonCreate), or none (Destroy).
        void BlockReason(bool on);
        /// A quit is decided (main): the one-copy window goes, so a later start waits for our
        /// mutex rather than signal a copy that is going (OneCopy.Quitting).
        void Quitting();
        void Log(string line);
        /// A monotonic clock in ms (any thread): the veto's deadline.
        long NowMs();
    }

    internal sealed class CloseGate
    {
        public const int QuitWaitMs = 60000, LogoffWaitMs = 30000;
        private enum State { Open, Waiting, Approved }

        private readonly ICloseSide side;
        private State state;
        private bool endWaited;
        private int session;   // one per query and per cancel (main): a veto's quit is for its own session only
        private bool vetoed;   // this session's query was vetoed (main)
        private long deadline; // then: its 30 s end, NowMs's (written on main, read off it too)

        public CloseGate(ICloseSide side) { this.side = side; }

        /// The session is ending (WM_QUERYENDSESSION seen, not cancelled since).
        public bool SessionEnding { get; private set; }
        /// The close is decided and waits for CriticalWork, or is done.
        public bool Closing => state != State.Open;

        /// Window.Closing (main). True: let the window close now; false: cancel it.
        public bool OnClosing()
        {
            if (state == State.Approved) return true;
            if (SessionEnding)
            {
                side.CriticalClose();   // no new step from here (WC1)
                side.Quitting();   // WPF's shutdown at the session's end
                return true;
            }
            if (state == State.Waiting) return false;   // already decided: the close comes after the wait
            if (side.Ask(CoreApi.CORE_CLOSE_WINDOW) == CoreApi.CORE_CLOSE_NOW) Proceed();
            return false;   // LATER: close_reply answers; REFUSE: stay
        }

        /// close_reply's action, posted to main (never inside the callback).
        public void OnReply(int what, bool proceed)
        {
            if (proceed && (what == CoreApi.CORE_CLOSE_WINDOW || what == CoreApi.CORE_CLOSE_QUIT)) Proceed();
        }

        /// The flow's quit (UPDATE_SWAP, SELF_DELETE), posted to main: asking nothing.
        public void QuitNow() => Proceed();

        private void Proceed()
        {
            if (state != State.Open) return;
            state = State.Waiting;
            side.CriticalClose();   // decided, never undone: no new step from here, the wait sees all (WC1, MC2)
            side.Quitting();
            side.RunOff(() =>
            {
                if (!side.CriticalWait(QuitWaitMs)) side.Log("quit: a step that must finish still runs after 60 s: quitting anyway");
                side.Post(() =>
                {
                    state = State.Approved;
                    side.Close();
                });
            });
        }

        /// Application.SessionEnding (main): the session may end. True: veto (a step must finish;
        /// its reason is set), so WPF answers FALSE and stays; false: WPF shuts the app down.
        public bool OnQueryEndSession()
        {
            SessionEnding = true;
            endWaited = false;
            vetoed = false;
            session++;
            bool veto = side.CriticalRunning();
            if (!veto) return false;
            side.BlockReason(true);
            // the logoff waits for us: quit once no step runs, by the veto's deadline at the latest,
            // unless it was cancelled
            vetoed = true;
            Interlocked.Exchange(ref deadline, side.NowMs() + LogoffWaitMs);
            WaitThenQuit(session);
            return true;
        }

        // off the UI thread: wait for the time left; then on it: quit, unless the session changed or a
        // step began meanwhile (then wait again, for the time left). endWaited stays: Exit's EndWait
        // still waits, for the time left, for a step that begins after the close (WC2).
        private void WaitThenQuit(int g)
        {
            side.RunOff(() =>
            {
                bool idle = side.CriticalWait((int)Math.Max(0, Interlocked.Read(ref deadline) - side.NowMs()));
                side.Post(() =>
                {
                    if (g != session || !SessionEnding) return;   // cancelled, or another query since
                    if (side.CriticalRunning() && side.NowMs() < Interlocked.Read(ref deadline))
                    {
                        WaitThenQuit(g);
                        return;
                    }
                    if (!idle) side.Log("logoff: a step that must finish still runs after " + LogoffWaitMs / 1000 + " s: quitting anyway");
                    side.BlockReason(false);
                    side.CriticalClose();   // no new step from here (WC1)
                    state = State.Approved;
                    side.Quitting();
                    side.Close();
                });
            });
        }

        /// WM_ENDSESSION (main): ending, or the logoff was cancelled.
        public void OnEndSession(bool ending)
        {
            if (!ending)
            {
                SessionEnding = false;
                session++;
                side.BlockReason(false);
                return;
            }
            EndWait();
        }

        /// The session's bounded wait, once (WM_ENDSESSION or the app's exit, whichever comes first).
        public void EndWait()
        {
            if (!SessionEnding || endWaited) return;
            endWaited = true;
            side.CriticalClose();   // WM_ENDSESSION(TRUE) is final: no new step from here (MC1)
            // after a veto, only what is left of its 30 s; else the whole bound
            long ms = vetoed ? Math.Max(0, Interlocked.Read(ref deadline) - side.NowMs()) : LogoffWaitMs;
            if (side.CriticalRunning() && ms > 0 && !side.CriticalWait((int)ms))
                side.Log("logoff: a step that must finish still runs after " + LogoffWaitMs / 1000 + " s: the session ends anyway");
            side.BlockReason(false);
        }
    }
}
