// The app-modal presenter (core.h Dialogs: one at a time, in call order; app.md §4.4). Every modal
// window of ours goes through it: the core's dialogs and the folder picker from flow threads
// (which wait for their answer), About from the UI thread (which never waits). One FIFO queue
// under plock; at most one Pump is posted or running, so a dialog's nested dispatcher frame can't
// open a second one: the next request is shown only after the current one closes. No WPF here
// (the Mac tests use it): `post` is the dispatcher's BeginInvoke at Normal priority; modalBegin and
// modalEnd (null but in TEST builds: the probe's) bracket each window's Show.
using System;
using System.Collections.Generic;
using System.Threading;

namespace RbStemsPlus.Shell
{
    internal sealed class Presenter
    {
        private sealed class Request
        {
            public Action Show;
            public bool Shown;
            public readonly ManualResetEventSlim Done = new ManualResetEventSlim(false);
        }

        private readonly Action<Action> post;
        private readonly Action<Exception> failed;
        private readonly Action modalBegin, modalEnd;
        private readonly object plock = new object();
        private readonly Queue<Request> queue = new Queue<Request>();
        private bool presenting;   // a Pump is posted or running (under plock)
        private bool stopped;      // under plock
        private int requested;     // callers waiting for their request (any thread)
        private int modal;         // windows up now (UI thread)

        public Presenter(Action<Action> post, Action<Exception> failed, Action modalBegin = null, Action modalEnd = null)
        {
            this.post = post;
            this.failed = failed;
            this.modalBegin = modalBegin;
            this.modalEnd = modalEnd;
        }

        /// UI thread (modal_open's answer): a window of ours is up, or one is asked for and waits.
        public bool ModalOpen => modal > 0 || Volatile.Read(ref requested) > 0;

        /// Nothing shown, posted or queued (any thread; the TEST triggers' wait).
        public bool Idle { get { lock (plock) return !presenting && queue.Count == 0; } }

        /// Any thread but the UI thread: show, in turn, and wait until it has closed. False: not
        /// shown (shutting down, or it threw).
        public bool RunAndWait(Action show)
        {
            Interlocked.Increment(ref requested);
            try
            {
                var r = Enqueue(show);
                r.Done.Wait();
                return r.Shown;
            }
            finally { Interlocked.Decrement(ref requested); }
        }

        /// The UI thread's own (About): queued behind whatever is up, never waited for.
        public void Run(Action show) => Enqueue(show);

        /// Shutting down: every queued request completes as not shown, and so does every later one.
        public void Stop()
        {
            Request[] left;
            lock (plock)
            {
                stopped = true;
                left = queue.ToArray();
                queue.Clear();
            }
            foreach (var r in left) r.Done.Set();
        }

        private Request Enqueue(Action show)
        {
            var r = new Request { Show = show };
            bool postNow = false;
            lock (plock)
            {
                if (stopped) { r.Done.Set(); return r; }
                queue.Enqueue(r);
                if (!presenting) { presenting = true; postNow = true; }
            }
            if (postNow) post(Pump);
            return r;
        }

        // UI thread: one request, then the next one in its own operation
        private void Pump()
        {
            Request r;
            lock (plock)
            {
                if (queue.Count == 0) { presenting = false; return; }
                r = queue.Dequeue();
            }
            try
            {
                bool show;
                lock (plock) show = !stopped;
                if (show)
                {
                    modal++;
                    modalBegin?.Invoke();
                    try { r.Show(); r.Shown = true; }
                    catch (Exception e) { try { failed(e); } catch (Exception) { } }
                    finally
                    {
                        modal--;
                        modalEnd?.Invoke();
                    }
                }
            }
            finally
            {
                r.Done.Set();
                bool again;
                lock (plock)
                {
                    again = queue.Count > 0;
                    if (!again) presenting = false;
                }
                if (again) post(Pump);
            }
        }
    }
}
