// TEST builds only (win/shell/build.sh --test-app: ShellTest=1, SHELL_TEST): triggers for the VM
// smoke (vm/smoke.ps1) that the release app has no path to yet. RBSP_SHELL_TEST, comma-separated:
//   about-over-welcome  About asked 1.5 s after "shown", while the core's welcome is up: it must
//                       wait in the presenter's queue, never stack over the welcome
//   overlap             after About was asked (when it is) and once the presenter has been idle for
//                       0.5 s (the welcome and About closed), two dialogs asked from two threads, 0.2 s apart, the
//                       first with a '_' in a button: shown one at a time, in call order; each
//                       answer is logged ("shell test: dialog <n> answered <i>")
//   fail-first-render   the first render throws (a view failing): under --swap the update recovers
//   hang-offscreen      --swap's off-screen start blocks the UI thread: the swap guard must fire
// And always: the updater's and the flows' download base, the TEST build's own server
// (SetUpdateBase: build.sh --test-base's URL, compiled in as assembly metadata; release.md T12).
// Their texts are the test's own, not the core's: never in a release.
#if SHELL_TEST
using System;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Threading;

namespace RbStemsPlus.Shell
{
    internal static class TestHooks
    {
        private static volatile bool aboutAsked;
        private static int failRenders = Has("fail-first-render") ? 1 : 0;

        public static bool Has(string what) => (Environment.GetEnvironmentVariable("RBSP_SHELL_TEST") ?? "").Contains(what);

        // the TEST core DLL's hook (core/build.sh dll --test-hooks; the release DLL has no core_test_ export)
        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int core_test_update_base([MarshalAs(UnmanagedType.LPUTF8Str)] string url);

        /// The download base this TEST build was built for (its assembly's RbspTestUpdateBase
        /// metadata), given to the core before anything downloads; none: GitHub's, as a release.
        public static void SetUpdateBase(Action<string> log)
        {
            string url = typeof(TestHooks).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
                .FirstOrDefault(a => a.Key == "RbspTestUpdateBase")?.Value ?? "";
            if (url.Length == 0) return;
            try
            {
                int r = core_test_update_base(url);
                log(r == 0 ? "shell test: downloads from " + url : "shell test: the core refused the download base " + url);
            }
            catch (EntryPointNotFoundException) { log("shell test: this core DLL has no test hooks: downloads from GitHub"); }
        }

        /// fail-first-render: throws on the first render only
        public static void MaybeFailRender()
        {
            if (Interlocked.Exchange(ref failRenders, 0) == 1) throw new InvalidOperationException("shell test: the first render fails");
        }

        /// hang-offscreen: the UI thread blocks for good
        public static void MaybeHangOffScreen()
        {
            if (Has("hang-offscreen")) Thread.Sleep(Timeout.Infinite);
        }

        public static void Start(Shell shell, Dispatcher ui)
        {
            string what = Environment.GetEnvironmentVariable("RBSP_SHELL_TEST") ?? "";
            shell.Log("shell test: " + (what.Length > 0 ? what : "no trigger"));
            if (what.Contains("about-over-welcome"))
                After(ui, 1500, () => { shell.Log("shell test: About asked"); shell.ShowAbout(); aboutAsked = true; });
            else aboutAsked = true;
            if (what.Contains("overlap"))
                new Thread(() =>
                {
                    Thread.Sleep(3000);                                    // the welcome is asked by then
                    while (!aboutAsked) Thread.Sleep(50);
                    for (int idle = 0; idle < 10; idle = shell.PresenterIdle ? idle + 1 : 0) Thread.Sleep(50);
                    var t1 = new Thread(() => Ask(shell, 1, "First_One")) { IsBackground = true };
                    t1.Start();
                    Thread.Sleep(200);
                    new Thread(() => Ask(shell, 2, "Second")) { IsBackground = true }.Start();
                }) { IsBackground = true }.Start();
        }

        private static void Ask(Shell shell, int n, string button)
        {
            var f = new DialogForm { Title = "Test dialog " + n, Message = "shell test: overlapping dialogs", Default = 0, Cancel = 1 };
            f.Buttons.Add(button); f.Buttons.Add("Cancel");
            f.Enabled.Add(true); f.Enabled.Add(true);
            f.Left.Add(false); f.Left.Add(false);
            shell.Log("shell test: dialog " + n + " asked");
            var a = shell.ShowDialog(f);
            shell.Log("shell test: dialog " + n + " answered " + (a == null ? "not shown" : a.Button.ToString()));
        }

        private static void After(Dispatcher ui, int ms, Action a)
        {
            var t = new DispatcherTimer(DispatcherPriority.Normal, ui) { Interval = TimeSpan.FromMilliseconds(ms) };
            t.Tick += (s, e) => { t.Stop(); a(); };
            t.Start();
        }
    }
}
#endif
