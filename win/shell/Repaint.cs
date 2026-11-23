// The window's picture after a session or display change (app.md §4.6). WPF stops presenting while
// the session is away (locked, switched out, disconnected, asleep) and is meant to draw again when
// it is back; nothing in the shell took part. On the owner's PC (2026-11-19) the window came back
// all black once, after about 6 hours and many moves of the session between the console and
// disconnected, minimized through most of them, its tabs offscreen to UI Automation (a stale
// layout). It didn't come back in 40 quick moves after, so the cause in WPF's HwndTarget is
// unproven; what the shell does is safe in every case: after each change, a round of nudges (0,
// 0.5, 2 and 6 s: WPF's own retry window is 10 s), each a layout pass and a full invalidate of the
// window, as a resize would bring. A session return can also reset the window's render mode
// briefly when its layout is current but WPF has stopped presenting.
// RepaintPlan decides when, WPF-free (the Mac tests run it); MainWindow.xaml.cs nudges. A change
// while the window is minimized or the session away waits for the restore or the session's return;
// away, minimizing and closing end a running round, so no stale round survives.
using System;

namespace RbStemsPlus.Shell
{
    internal enum RepaintCause { None, Away, Back, Display, Minimized, Restored, Closed }

    internal enum RenderResetReason { None, PresentFailed, FirstFrameTimeout, SessionChange }

    internal static class RenderResetDecision
    {
        public static RenderResetReason Choose(bool canReset, bool alreadyTried, bool presentFailed, bool firstFrameTimedOut)
        {
            if (!canReset || alreadyTried) return RenderResetReason.None;
            if (presentFailed) return RenderResetReason.PresentFailed;
            return firstFrameTimedOut ? RenderResetReason.FirstFrameTimeout : RenderResetReason.None;
        }
    }

    internal sealed class RepaintPlan
    {
        // winuser.h, wtsapi32.h, pbt.h
        public const int WM_DISPLAYCHANGE = 0x007E, WM_POWERBROADCAST = 0x0218, WM_WTSSESSION_CHANGE = 0x02B1,
            WM_DPICHANGED = 0x02E0, WM_DWMCOMPOSITIONCHANGED = 0x031E;
        public const int WTS_CONSOLE_CONNECT = 1, WTS_CONSOLE_DISCONNECT = 2, WTS_REMOTE_CONNECT = 3,
            WTS_REMOTE_DISCONNECT = 4, WTS_SESSION_LOCK = 7, WTS_SESSION_UNLOCK = 8;
        public const int PBT_APMSUSPEND = 0x4, PBT_APMRESUMESUSPEND = 0x7, PBT_APMRESUMEAUTOMATIC = 0x12;

        /// the nudges, in ms from the change
        public static readonly int[] NudgeAtMs = { 0, 500, 2000, 6000 };

        private long start = -1;      // the running round's start; -1: none
        private int next;             // the next nudge
        private bool pending;         // a round at the next restore or return of the session
        private bool away;            // locked, switched out, disconnected or asleep
        private bool closed;          // the window is closed: nothing ever again

        public bool Pending => pending;
        public bool Away => away;
        public bool Running => start >= 0;

        public static bool IsSessionReturn(int msg, RepaintCause cause) =>
            msg == WM_WTSSESSION_CHANGE && cause == RepaintCause.Back;

        /// What a window message means for the picture; None for every other message.
        public static RepaintCause Classify(int msg, long wParam)
        {
            switch (msg)
            {
                case WM_WTSSESSION_CHANGE:
                    switch ((int)wParam)
                    {
                        case WTS_CONSOLE_CONNECT: case WTS_REMOTE_CONNECT: case WTS_SESSION_UNLOCK: return RepaintCause.Back;
                        case WTS_CONSOLE_DISCONNECT: case WTS_REMOTE_DISCONNECT: case WTS_SESSION_LOCK: return RepaintCause.Away;
                    }
                    return RepaintCause.None;
                case WM_POWERBROADCAST:
                    switch ((int)wParam)
                    {
                        case PBT_APMRESUMEAUTOMATIC: case PBT_APMRESUMESUSPEND: return RepaintCause.Back;
                        case PBT_APMSUSPEND: return RepaintCause.Away;
                    }
                    return RepaintCause.None;
                case WM_DISPLAYCHANGE: case WM_DPICHANGED: case WM_DWMCOMPOSITIONCHANGED:
                    return RepaintCause.Display;
            }
            return RepaintCause.None;
        }

        /// A change, with the window's state now.
        public void Note(RepaintCause c, bool minimized, long nowMs)
        {
            if (closed) return;
            switch (c)
            {
                case RepaintCause.None:
                    return;
                case RepaintCause.Closed:
                    closed = true;
                    pending = false;
                    start = -1;
                    return;
                case RepaintCause.Away:
                    away = true;
                    Hold();
                    return;
                case RepaintCause.Minimized:
                    Hold();
                    return;
                case RepaintCause.Back:
                    away = false;
                    if (minimized) { pending = true; start = -1; return; }
                    Begin(nowMs);
                    return;
                case RepaintCause.Restored:
                    if (!pending || minimized || away) return;
                    Begin(nowMs);
                    return;
                default:
                    if (minimized || away) { pending = true; start = -1; return; }
                    Begin(nowMs);
                    return;
            }
        }

        // a running round ends; its remaining nudges wait for the window or the session
        private void Hold()
        {
            if (start >= 0) pending = true;
            start = -1;
        }

        private void Begin(long nowMs)
        {
            pending = false;
            start = nowMs;
            next = 0;
        }

        /// ms until the next nudge, 0 when one is due; -1: nothing scheduled
        public long DueIn(long nowMs) => start < 0 ? -1 : Math.Max(0, start + NudgeAtMs[next] - nowMs);

        /// Whether a nudge is due now (and on to the next); the round ends after the last.
        public bool Take(long nowMs)
        {
            if (start < 0 || nowMs < start + NudgeAtMs[next]) return false;
            if (++next == NudgeAtMs.Length) start = -1;
            return true;
        }
    }
}
