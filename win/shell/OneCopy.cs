// One copy of the app at a time (app.md §4.3, §5.9, I88; main.swift's lock): the core DLL's
// core_win_one_* (core_win.h, win/c/lifecycle.h), without WPF: the Mac tests run it against a fake.
//  - Take, before the first render: HELD runs on; HANDED (a first copy was given the foreground
//    rights and signalled) exits quietly; BUSY (a copy holds the mutex and shows no window for
//    WaitMs: one that is quitting, or starting) is "Already open"; FAILED is "can't open".
//  - Window, once the main window is shown: the findable message-only window; a second start's
//    signal runs on the UI thread, which pumps it.
//  - Quitting, as soon as a quit is decided: the window goes, so a later start waits for our mutex
//    rather than signal a copy that is going. Close, after the app's run: the mutex.
// The thread rule (core_win.h): a mutex belongs to the thread that took it, so every call here is
// on the UI thread, which lives as long as the app; the handle is an IntPtr, never a SafeHandle
// (no close from a finalizer's thread).
using System;

namespace RbStemsPlus.Shell
{
    /// The core DLL's one-copy entries (CoreWin's in the app, a fake in the tests). UI thread only.
    internal interface IOneCopyNative
    {
        /// core_win_one_take(NULL: the app's own mutex, ms): CORE_WIN_ONE_*; one is set on HELD only.
        int Take(uint ms, out IntPtr one);
        /// core_win_one_window: 0, or an errno. signal runs on this thread when a second start signals.
        int Window(IntPtr one, Action signal);
        void WindowClose(IntPtr one);
        void Close(IntPtr one);
    }

    internal enum OneCopyStart { Run, ExitQuietly, AlreadyOpen, CantOpen }

    internal sealed class OneCopy
    {
        public const int Held = 0, Handed = 1, Busy = 2, Failed = 3;   // core_win.h's CORE_WIN_ONE_*
        public const uint WaitMs = 10000;   // a copy that is quitting, waited for (main.swift's wait: 10)

        private readonly IOneCopyNative native;
        private readonly Action<string> log;
        private IntPtr one;
        private bool window;

        public OneCopy(IOneCopyNative native, Action<string> log)
        {
            this.native = native;
            this.log = log;
        }

        /// The one-copy decision at the start. Any answer but Run ends the start.
        public OneCopyStart Take()
        {
            int r;
            IntPtr h;
            try { r = native.Take(WaitMs, out h); }
            catch (Exception e)
            {
                log("shell: the one-copy check failed (" + e.GetType().Name + ")");
                return OneCopyStart.CantOpen;
            }
            switch (r)
            {
                case Held:
                    one = h;
                    return OneCopyStart.Run;
                case Handed:
                    log("shell: a copy is already open: it comes forward");
                    return OneCopyStart.ExitQuietly;
                case Busy:
                    log("shell: a copy is already open and shows no window after " + WaitMs / 1000 + " s");
                    return OneCopyStart.AlreadyOpen;
                default:
                    log("shell: the one-copy mutex can't be used (" + r + ")");
                    return OneCopyStart.CantOpen;
            }
        }

        /// The findable window, once the main window is shown (held only). Without it a second start
        /// waits for our mutex, then says "Already open": the app itself runs on.
        public void Window(Action signal)
        {
            if (one == IntPtr.Zero || window) return;
            int e;
            try { e = native.Window(one, signal); }
            catch (Exception x)
            {
                log("shell: the one-copy window can't be made (" + x.GetType().Name + ")");
                return;
            }
            if (e != 0) log("shell: the one-copy window can't be made (" + e + "): a second start says \"Already open\"");
            else window = true;
        }

        /// A quit is decided: the window goes (once); the mutex stays until Close.
        public void Quitting()
        {
            if (!window) return;
            window = false;
            native.WindowClose(one);
        }

        /// After the app's run, on the UI thread: the window if still there, then the mutex.
        public void Close()
        {
            if (one == IntPtr.Zero) return;
            IntPtr h = one;
            one = IntPtr.Zero;
            window = false;
            native.Close(h);
        }
    }
}
