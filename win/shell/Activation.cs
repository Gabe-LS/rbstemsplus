// The activation aggregator (app.md §4.3, events.def's header): the one source of "activated".
// The shell never listens to Window.Activated: on WPF a closing modal window re-raises it on the
// main window, and a quiet re-check started then would drop the choice the dialog just made. The
// sources are:
//  - the app's activation changing (WM_ACTIVATEAPP: Windows sends it only when activation moves
//    between our thread and another app's, so our own dialogs, the folder picker and About, all
//    owned by the main window, never count);
//  - the main window leaving Minimized (Windows activates the window it restores);
//  - a second start of the app (the one-copy signal): "activated", then "check_request"
//    (main.swift:169-172).
// Several sources in one dispatcher turn send one event: the first schedules one flush (WPF: at
// Background priority, after the turn's input), which sends what the turn adds up to. Nothing is
// sent before Ready (the shell calls it right after "shown"): the start's own activation then
// comes after "shown", as applicationDidBecomeActive comes after launching on the Mac. A UAC
// elevation needs nothing here: its activations come as any other, and the core defers them
// (front_facts carries CORE_FRONT_ELEVATING while the core's own admin call is in flight).
// No WPF here (the Mac tests use it).
using System;

namespace RbStemsPlus.Shell
{
    internal sealed class Activation
    {
        public const string Activated = "{\"ev\":\"activated\"}";
        public const string Deactivated = "{\"ev\":\"deactivated\"}";
        public const string CheckRequest = "{\"ev\":\"check_request\"}";

        private readonly Func<string, int> send;
        private readonly Action<Action> post;
        private bool ready, scheduled;
        private bool appChanged, appActive;   // the turn's last WM_ACTIVATEAPP
        private bool restored, secondStart;

        /// send: one event to the core (main). post: run the flush later, once, on main.
        public Activation(Func<string, int> send, Action<Action> post)
        {
            this.send = send;
            this.post = post;
        }

        /// WM_ACTIVATEAPP: active is its wParam (our app activated, or another app's).
        public void App(bool active)
        {
            appChanged = true;
            appActive = active;
            Schedule();
        }

        /// The main window left Minimized.
        public void Restored()
        {
            restored = true;
            Schedule();
        }

        /// A second start of the app signalled this copy, which came forward.
        public void SecondStart()
        {
            secondStart = true;
            Schedule();
        }

        /// "shown" was sent: what came before it is flushed now, and every source from now on.
        public void Ready()
        {
            if (ready) return;
            ready = true;
            if (appChanged || restored || secondStart) Schedule();
        }

        private void Schedule()
        {
            if (!ready || scheduled) return;
            scheduled = true;
            post(Flush);
        }

        /// The turn's one event: "activated" when the app ends the turn activated, its window
        /// restored or a second start came ("check_request" after it for the second start);
        /// "deactivated" when the app ends the turn deactivated and nothing else came.
        public void Flush()
        {
            bool activate = secondStart || restored || (appChanged && appActive);
            bool deactivate = !activate && appChanged && !appActive;
            bool second = secondStart;
            scheduled = appChanged = restored = secondStart = false;
            if (activate) send(Activated);
            else if (deactivate) send(Deactivated);
            if (second) send(CheckRequest);
        }
    }
}
