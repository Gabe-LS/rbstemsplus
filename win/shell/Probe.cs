// The probe's WPF part (ProbeCore.cs has the records, the turns, the commands' parsing and the
// waits): the dispatcher's hooks for the UI thread's turns (ProbeTurns), what the window showed on
// every CompositionTarget.Rendering tick (trace mode only: its frame sample, every field's
// properties, the lights' look, tint and tooltip, the menus as the core validates them now), and
// perf mode's named field events, timing points and per-turn timestamps (I35), in
// app/Probe/Probe.swift's names, so tests/harness's scenarios and tests/perf/stats.py read both
// shells. TEST BUILDS ONLY: compiled with SHELL_TEST alone (RbStemsPlus.csproj), and build.sh's
// release gate refuses a release exe naming it. The shell calls only the static entries below,
// each inside #if SHELL_TEST.
//
// The driver's commands come one line each on the pipe \\.\pipe\rbstemsplus-probe (this user's
// only: its DACL has no SYSTEM, Administrators or network logon), and are acked in <out>\acks:
// mark, click <field>, menu <field>, answer <button title>, fill <n> <text>, tab <title>,
// miniaturize, deminiaturize, close, fields, flush, quit, wait <ms> <key>=<value> (or !=),
// quiet <ms> <timeout ms>, event <ms> <name> <count>, count <name>.
// The Prepare tab's verbs (select, collapse, expand <playlist id>, until <days>, scroll <row>) and
// the Status tab's (select-set <row>, -1 none; doubleclick-set <row>) act as the user's input
// would, where the tabs are shown; password answers "err": no password sheet on Windows (UAC is
// the system's: the FlaUI driver's).
#if SHELL_TEST
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Threading;

namespace RbStemsPlus.Shell
{
    internal static class ShellProbe
    {
        public const string PipeName = "rbstemsplus-probe";
        private static ProbeRecorder rec;
        private static ProbeWaits waits;
        private static Shell shell;
        private static Dispatcher ui;
        private static string outDir;
        private static StreamWriter trace;   // the writer thread's only
        // the records' and acks' file writes, off the UI thread (Probe.swift's writeQ): one
        // background thread ("probe writes") in order
        private static BlockingCollection<Action> writes;
        private static Thread writer;
        private static readonly Stopwatch clock = Stopwatch.StartNew();
        private static long launchNs;
        private static string arch;
        private static readonly List<KeyValuePair<string, object>> fields = new List<KeyValuePair<string, object>>();
        private static readonly HashSet<string> taken = new HashSet<string>();
        private static bool menusBuilt, windowDrawn;
        private static Window dialog;   // the modal dialog up now (owned by the main window)
        private static ProbeTurns turns;
        private static DispatcherTimer waitTimer;   // 20 ms, only while a wait is pending (Probe.swift's)

        public static bool On => rec != null;
        private static long Now() => (long)(clock.ElapsedTicks * (1e9 / Stopwatch.Frequency));

        /// Shell.Start, once the main window exists: reads probe.conf; nothing more without one.
        public static void Start(Shell s, Dispatcher d)
        {
            string conf;
            try { conf = File.ReadAllText(ConfPath()); }
            catch (Exception e) when (e is FileNotFoundException || e is DirectoryNotFoundException) { return; }
            catch (Exception e) { s.Log("shell test: probe.conf unreadable (" + e.GetType().Name + "): no probe"); return; }
            var c = ProbeConf.Parse(conf);
            // the harness's fake helper (probe.conf's helper=, in any mode, as the Mac's
            // probeFakeHelper): through a test-hooks core DLL only
            if (c.TryGetValue("helper", out string fake) && fake.Length > 0) FakeHelper(s, fake);
            LoadFacts(c, s);
            ProbeMode mode = ProbeConf.Mode(c);
            if (mode == ProbeMode.Off) return;
            shell = s;
            ui = d;
            outDir = c["out"];
            int pid = Process.GetCurrentProcess().Id;
            try
            {
                Directory.CreateDirectory(outDir);
                // every mode: the harness finds the process by it (the perf modes write the start record only)
                trace = new StreamWriter(new FileStream(Path.Combine(outDir, "app-" + pid + ".jsonl"), FileMode.Create, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
            }
            catch (Exception e)
            {
                shell.Log("shell test: the probe can't write to " + outDir + " (" + e.Message + ")");
                return;
            }
            writes = new BlockingCollection<Action>();
            writer = new Thread(Writes) { IsBackground = true, Name = "probe writes" };
            writer.Start();
            launchNs = LaunchNs();
            arch = Arch();
            rec = new ProbeRecorder(mode, Now, Emit) { Exe = ExeKind() };
            waits = new ProbeWaits(Now, Ack);
            turns = new ProbeTurns(Now, rec, waits);
            rec.Start(pid, launchNs, arch);
            Register();
            // a Rendering handler makes WPF render every vsync: trace mode's sampling only
            if (rec.Trace) CompositionTarget.Rendering += (o, e) => Sample();
            ui.Hooks.OperationStarted += (o, e) => turns.Started();
            ui.Hooks.OperationCompleted += (o, e) => turns.Completed();
            ui.Hooks.DispatcherInactive += (o, e) => TurnEnded();
            waitTimer = new DispatcherTimer(DispatcherPriority.Background, ui) { Interval = TimeSpan.FromMilliseconds(20) };
            waitTimer.Tick += (o, e) => { };   // its turn's end checks the waits
            Window w = s.Window;
            w.ContentRendered += (o, e) => { if (!windowDrawn) { windowDrawn = true; rec.Event("windowDrawn"); } };
            // the Mac's didMiniaturize and didDeminiaturize: into and out of Minimized only (never
            // a maximise or its restore)
            w.StateChanged += (o, e) => { if (w.WindowState == WindowState.Minimized) rec.Event("didMiniaturize"); };
            s.Window.Restored += () => rec.Event("didDeminiaturize");
            if (w.FindName("Tabs") is TabControl tabs)
                tabs.SelectionChanged += (o, e) => { if (e.OriginalSource == tabs) rec.Event("tabShown", Header(tabs.SelectedItem as TabItem)); };
            EventManager.RegisterClassHandler(typeof(Window), FrameworkElement.LoadedEvent, new RoutedEventHandler(WindowLoaded));
            Application.Current.Exit += (o, e) => { rec.Event("willTerminate"); Flush(); Finish(); };
            AppDomain.CurrentDomain.ProcessExit += (o, e) => { Flush(); Finish(); };
            Model(s.Model);
            new Thread(Commands) { IsBackground = true, Name = "probe commands" }.Start();
            shell.Log("shell test: the probe runs in " + ProbeConf.Name(mode) + " mode");
        }

        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int core_test_win_fake_helper([MarshalAs(UnmanagedType.LPUTF8Str)] string path);

        private static void FakeHelper(Shell s, string path)
        {
            try
            {
                int r = core_test_win_fake_helper(path);
                s.Log(r == 0 ? "shell test: the helper is the harness's fake: " + path : "shell test: the fake helper wasn't taken (" + r + ")");
            }
            catch (EntryPointNotFoundException) { s.Log("shell test: this core DLL takes no fake helper (not a test-hooks build)"); }
        }

        // ------------------------------------------------------------------ stand-in cheap facts

        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern void core_fact_flag(IntPtr sheet, int f, int v);
        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern void core_fact_i64(IntPtr sheet, int f, long v);
        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern void core_fact_str(IntPtr sheet, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v, UIntPtr len);

        private static readonly object factsLock = new object();
        private static List<ProbeFact> testFacts;
        private static List<string> badFacts;

        private static string ConfPath() => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "rbstemsplus-probe", "probe.conf");

        // probe.conf's fact. lines, one snapshot: Start's own parse (LoadFacts), or a read here if
        // the sheet is read first. A missing probe.conf is silent; one that can't be read is said.
        private static List<ProbeFact> TestFacts()
        {
            lock (factsLock)
            {
                if (testFacts != null) return testFacts;
                badFacts = new List<string>();
                try { testFacts = ProbeFacts.Parse(ProbeConf.Parse(File.ReadAllText(ConfPath())), badFacts); }
                catch (Exception e) when (e is FileNotFoundException || e is DirectoryNotFoundException) { testFacts = new List<ProbeFact>(); }
                catch (Exception e) { testFacts = new List<ProbeFact>(); badFacts.Add("probe.conf unreadable: " + e.GetType().Name); }
                return testFacts;
            }
        }

        // Start's parsed probe.conf as the snapshot (no second read), unless the sheet took one first
        private static void LoadFacts(Dictionary<string, string> c, Shell s)
        {
            lock (factsLock)
            {
                if (testFacts == null)
                {
                    badFacts = new List<string>();
                    testFacts = ProbeFacts.Parse(c, badFacts);
                }
            }
            LogFacts(s);
        }

        /// Host.Facts' TEST hook (the UI thread, inside factsFn's OnMain guard, after the real facts): probe.conf's stand-in facts
        /// over them, ProbeFacts' rule. Nothing without a probe.conf naming facts.
        public static void WriteFacts(IntPtr sheet)
        {
            var f = TestFacts();
            if (f.Count == 0 || sheet == IntPtr.Zero) return;
            ProbeFacts.Write(f, (id, v) => core_fact_flag(sheet, id, v), (id, v) => core_fact_i64(sheet, id, v),
                             (id, v) => core_fact_str(sheet, id, v, RbStemsPlus.Core.CoreApi.Nts));
        }

        private static void LogFacts(Shell s)
        {
            var f = TestFacts();   // the snapshot LoadFacts or the sheet took
            lock (factsLock)
            {
                if (f.Count > 0) s.Log("shell test: " + f.Count + " stand-in facts from probe.conf: " + string.Join(", ", f.Select(x => x.Id.ToString())));
                foreach (string b in badFacts ?? new List<string>()) s.Log("shell test: probe.conf's " + b + ": ignored");
            }
        }

        // ------------------------------------------------------------------ entries from the shell

        /// A timing point (Probe.swift's probeMark): refresh.begin and refresh.end around a render,
        /// shell.core once the core started. Any thread: another thread's is stamped now and
        /// recorded on the UI thread.
        public static void Mark(string name)
        {
            if (rec == null) return;
            if (ui.CheckAccess()) { rec.Mark(name); return; }
            long t = Now();
            ui.BeginInvoke(DispatcherPriority.Send, (Action)(() => rec.Mark(name, t)));
        }

        /// An event the shell sends to the core: its trigger's timing point (CoreShell.swift's send).
        public static void Sent(string json)
        {
            if (rec == null) return;
            // every event, recorded (the VM's echo-guard checks); on the UI thread, as Mark's
            if (ui.CheckAccess()) rec.Sent(json);
            else { long t = Now(); ui.BeginInvoke(DispatcherPriority.Send, (Action)(() => rec.Sent(json, t))); }
            if (json == "{\"ev\":\"activated\"}") Mark("trigger.activate");
            else if (json.StartsWith("{\"ev\":\"mount\"", StringComparison.Ordinal) || json.StartsWith("{\"ev\":\"unmount\"", StringComparison.Ordinal)
                     || json.StartsWith("{\"ev\":\"rename\"", StringComparison.Ordinal) || json == "{\"ev\":\"bridge_state\"}" || json == "{\"ev\":\"location\"}")
                Mark("trigger.location");
        }

        /// WM_ACTIVATEAPP (our app activated or another app): didBecomeActive / didResignActive.
        public static void AppActivation(bool active) { if (rec != null) rec.Event(active ? "didBecomeActive" : "didResignActive"); }

        /// A second start signalled this copy: the Mac's reopen (a Dock click on an open app).
        public static void Reopen() { if (rec != null) rec.Event("reopen"); }

        // ------------------------------------------------------------------ records

        private static void Emit(string line) => Write(() => trace.WriteLine(line));

        // queued for the writer thread; none after Finish
        private static void Write(Action a)
        {
            try { writes.Add(a); }
            catch (InvalidOperationException) { }
        }

        private static void Writes()
        {
            foreach (var a in writes.GetConsumingEnumerable())
                try { a(); }
                catch (Exception) { }
        }

        // exiting: the queued writes reach disk (quit's ack among them), for at most 2 s (the
        // Mac's atexit { writeQ.sync {} })
        private static void Finish()
        {
            writes.CompleteAdding();
            writer.Join(2000);
        }

        // perf-<pid>.tsv, on the calling thread: a flush command's ack, queued after it, follows the
        // file (Probe.swift's)
        private static void Flush()
        {
            if (rec == null) return;
            string s = rec.PerfTsv(Process.GetCurrentProcess().Id, launchNs, arch);
            if (s == null) return;
            try { File.WriteAllText(Path.Combine(outDir, "perf-" + Process.GetCurrentProcess().Id + ".tsv"), s, new UTF8Encoding(false)); }
            catch (Exception) { }
        }

        /// The presenter's window opens and closes (Presenter.Pump, UI thread): its build and first
        /// layout are the showing operation's, its nested frame's idle isn't (ProbeTurns). The no-op
        /// at Normal priority is the frame's first operation, so its first turn ends the build even
        /// in a loop that runs nothing else first (IFileOpenDialog's).
        public static void ModalBegin()
        {
            if (rec == null) return;
            turns.ModalBegin();
            ui.BeginInvoke(DispatcherPriority.Normal, (Action)(() => { }));
        }
        public static void ModalEnd() { if (rec != null) turns.ModalEnd(); }

        // trace mode's Rendering tick: nothing is on screen before the window's first frame (or a
        // dialog before it)
        private static void Sample()
        {
            if (windowDrawn || dialog != null) turns.Sample(Snapshot);
        }

        // the dispatcher went inactive: the turn's end, then the waits; the timer stops with the last
        private static void TurnEnded()
        {
            turns.Inactive();
            if (waits.Pending == 0 && waitTimer.IsEnabled) waitTimer.Stop();
        }

        private static void Wait(string id, long ms, Func<bool> test)
        {
            waits.Add(id, ms, test);
            if (waits.Pending > 0 && !waitTimer.IsEnabled) waitTimer.Start();
        }

        /// The process's start, in ns relative to the probe's clock: negative, the launch before it
        /// ran. The kernel's creation time (GetProcessTimes, 100 ns) against the precise system time
        /// (GetSystemTimePreciseAsFileTime, not DateTime.Now's 1-16 ms), read in one tight sample with
        /// the probe's clock: the narrowest of five reads, at its middle.
        private static long LaunchNs()
        {
            try
            {
                if (!GetProcessTimes(GetCurrentProcess(), out long created, out _, out _, out _)) return 0;
                long width = long.MaxValue, at = 0, sys = 0;
                for (int i = 0; i < 5; i++)
                {
                    long a = Now();
                    GetSystemTimePreciseAsFileTime(out long f);
                    long b = Now();
                    if (b - a < width) { width = b - a; at = a + (b - a) / 2; sys = f; }
                }
                return at - (sys - created) * 100;
            }
            catch (Exception) { return 0; }
        }

        [DllImport("kernel32.dll")] private static extern void GetSystemTimePreciseAsFileTime(out long fileTime);
        [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);

        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);

        private static string Arch()
        {
            ushort native = 0;
            try { if (!IsWow64Process2(GetCurrentProcess(), out _, out native)) native = 0; }
            catch (EntryPointNotFoundException) { native = 0; }   // before Windows 10 1709
            return ProbeArch.Label(RuntimeInformation.ProcessArchitecture, native, RuntimeInformation.OSArchitecture);
        }

        // this exe's build (AnyCPU or x64-forced), from its own PE header
        private static string ExeKind()
        {
            try
            {
                typeof(ShellProbe).Assembly.ManifestModule.GetPEKind(out System.Reflection.PortableExecutableKinds k, out System.Reflection.ImageFileMachine m);
                return ProbeArch.Exe(k, m);
            }
            catch (Exception) { return ""; }
        }

        // ------------------------------------------------------------------ fields

        private static void Register()
        {
            Window w = shell.Window;
            foreach (var kv in ProbeIds.Window)
                if (w.FindName(kv.Key) is FrameworkElement e) Add(kv.Value, e);
            // the Prepare tab, where shown, by the Mac's PrepareTab property names
            if (shell.PrepareTab is PrepareView pv)
            {
                Add("prepare.outline", pv.Outline);
                Add("prepare.nameLabel", pv.NameLabel);
                Add("prepare.info", pv.Info);
                Add("prepare.until", pv.Until);
                Add("prepare.prepareBtn", pv.PrepareBtn);
                Add("prepare.checkBtn", pv.CheckBtn);
                Add("prepare.pauseBtn", pv.PauseBtn);
                Add("prepare.stopBtn", pv.StopBtn);
                Add("prepare.bar", pv.Bar);
                Add("prepare.tracksTable", pv.Tracks);
                PrepareEvents(pv.Model);
            }
            // the Status tab, where shown, by the Mac's StatusTab property names (its title and
            // note: status.view0, status.view1, as the Mac's unnamed labels)
            if (shell.StatusTab is StatusView sv)
            {
                Add("status.plusTip", sv.PlusTip);
                Add("status.cacheTip", sv.CacheTip);
                Add("status.table", sv.Table);
                Add("status.againBtn", sv.AgainBtn);
                Add("status.stopBtn", sv.StopBtn);
                Add("status.runLine", sv.RunLine);
                Add("status.view0", sv.TitleLabel);
                Add("status.view1", sv.NoteLabel);
                StatusEvents(sv);
            }
            BuildMenus();
        }

        private static void Add(string id, object o)
        {
            taken.Add(id);
            fields.Add(new KeyValuePair<string, object>(id, o));
        }

        // the menu bar's items by their title path (Shell.BuildMenus has made them before the first
        // command or sample); the app menu's segment is "App" and the update offer's item is
        // c.updateItem, as Probe.swift's
        private static void BuildMenus()
        {
            if (menusBuilt || !(shell.Window.FindName("MenuBar") is Menu bar) || bar.Items.Count == 0) return;
            menusBuilt = true;
            RegisterMenu(bar, "menu");
        }

        private static void RegisterMenu(ItemsControl parent, string path)
        {
            MenuItem app = shell.MenuItemFor((int)RbStemsPlus.Core.Menu.M_APP), update = shell.MenuItemFor((int)RbStemsPlus.Core.Menu.M_APP_UPDATE);
            foreach (var o in parent.Items)
            {
                if (!(o is MenuItem mi)) continue;
                if (mi == update) { Add(ProbeIds.UpdateItem, mi); continue; }
                string id = ProbeIds.Menu(path, mi == app ? ProbeIds.AppMenu : mi.Header as string ?? "", taken);
                Add(id, mi);
                if (mi.Items.Count > 0) RegisterMenu(mi, id);
            }
        }

        /// The fields table (`fields`): every id, its type, oracle or excluded.
        private static string FieldTable()
        {
            BuildMenus();
            var b = new StringBuilder();
            foreach (var f in fields) b.Append(f.Key).Append('\t').Append(f.Value.GetType().Name).Append('\t').Append(ProbeIds.Oracle(f.Key) ? "oracle" : "excluded: clock times").Append('\n');
            return b.ToString();
        }

        /// What every field shows now (trace mode's frame sample), one key per property.
        private static Dictionary<string, string> Snapshot()
        {
            BuildMenus();
            var s = new Dictionary<string, string>();
            WindowKeys(s);
            // the menus as the core validates them now (a dialog may disable them): trace mode only
            shell.RefreshMenusForProbe();
            foreach (var f in fields) if (ProbeIds.Oracle(f.Key)) Props(f.Key, f.Value, s);
            return s;
        }

        // the app's and the window's own keys (Probe.swift's app.*, win.*, tab.sel)
        private static void WindowKeys(Dictionary<string, string> s)
        {
            Window w = shell.Window;
            s["app.active"] = Application.Current.Windows.Cast<Window>().Any(x => x.IsActive) ? "1" : "0";
            s["app.modal"] = dialog != null ? Describe(dialog) : "";
            s["win.title"] = w.Title ?? "";
            s["win.vis"] = w.IsVisible ? "1" : "0";
            s["win.mini"] = w.WindowState == WindowState.Minimized ? "1" : "0";
            s["win.key"] = w.IsActive ? "1" : "0";
            s["win.sheet"] = "";
            s["tab.sel"] = ProbeIds.Tab(((w.FindName("Tabs") as TabControl)?.SelectedItem as TabItem)?.Name ?? "").ToString();
        }

        /// One key now (perf mode's waits: no whole snapshot).
        private static string Value(string key)
        {
            // W13b reads the calendar without adding Windows-only fields to shared traces.
            var picker = shell.PrepareTab?.Until;
            if (key == "prepare.until.first")
                return picker?.DisplayDateStart?.ToString("yyyy-MM-dd", System.Globalization.CultureInfo.InvariantCulture) ?? "";
            if (key == "prepare.until.open") return picker?.IsDropDownOpen == true ? "1" : "0";
            if (rec.Trace) return rec.Last(key);
            if (key.StartsWith("app.") || key.StartsWith("win.") || key.StartsWith("tab."))
            {
                var k = new Dictionary<string, string>();
                WindowKeys(k);   // no menu validation in perf mode (I35)
                return k.TryGetValue(key, out string v) ? v : "";
            }
            string id = ProbeIds.FieldOf(key);
            var f = fields.FirstOrDefault(x => x.Key == id);
            if (f.Key == null) return "";
            var s = new Dictionary<string, string>();
            Props(f.Key, f.Value, s);
            return s.TryGetValue(key, out string r) ? r : "";
        }

        private static void Props(string id, object o, Dictionary<string, string> s)
        {
            if (o is MenuItem mi)
            {
                s[id + ".title"] = Plain(mi.Header as string ?? "");
                s[id + ".en"] = mi.IsEnabled ? "1" : "0";
                // the update item's hidden state is the render's (CoreShell.swift's renderApp sets it
                // from F_UPDATE_VISIBLE), not the last time its menu opened
                s[id + ".hid"] = id == ProbeIds.UpdateItem ? (shell.Model.UpdateVisible ? "0" : "1") : mi.Visibility != Visibility.Visible ? "1" : "0";
                return;
            }
            if (!(o is FrameworkElement e)) return;
            s[id + ".hid"] = e.Visibility != Visibility.Visible ? "1" : "0";
            if (e.ToolTip is string tip && tip.Length > 0) s[id + ".tip"] = tip;
            switch (e)
            {
                case TreeView _ when id == "prepare.outline":
                    OutlineProps(id, shell.PrepareTab, s);
                    break;
                case ListView l when id == "prepare.tracksTable":
                    TracksProps(id, l, shell.PrepareTab.Model, s);
                    break;
                case ListView l when id == "status.table":
                {
                    // as the Mac's rows: "name | tracks | until | state"
                    var sets = shell.StatusTab.Model.Sets;
                    s[id + ".rows"] = sets.Count.ToString();
                    s[id + ".sel"] = l.SelectedIndex.ToString();
                    s[id + ".en"] = l.IsEnabled ? "1" : "0";
                    for (int r = 0; r < sets.Count && r < 500; r++) s[id + ".r" + r] = sets[r].Name + " | " + sets[r].Tracks + " | " + sets[r].Until + " | " + sets[r].State;
                    break;
                }
                case DatePicker d:
                    // as the Mac's: whole days from now to the date shown
                    s[id + ".days"] = d.SelectedDate.HasValue ? ((int)Math.Round((d.SelectedDate.Value - DateTime.Today).TotalDays)).ToString() : "";
                    s[id + ".en"] = d.IsEnabled ? "1" : "0";
                    break;
                case ProgressBar p when id == "prepare.bar":
                    s[id + ".val"] = p.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture);
                    break;
                case TextBlock t when id == "prepare.info":
                {
                    // the summary and its note as one string, as the Mac's attributed string
                    var pm = shell.PrepareTab.Model;
                    s[id + ".str"] = pm.InfoSub.Length > 0 ? pm.Info + "\n" + pm.InfoSub : pm.Info;
                    break;
                }
                case Button b:
                    s[id + ".title"] = Plain(b.Content as string ?? "");
                    s[id + ".en"] = b.IsEnabled ? "1" : "0";
                    break;
                case ProgressBar p:
                    // the Mac's spinner is never hidden (stopAnimation hides it): .anim carries the state
                    s[id + ".hid"] = "0";
                    s[id + ".anim"] = p.IsIndeterminate && p.Visibility == Visibility.Visible ? "1" : "0";
                    break;
                case TextBlock t when id == "c.plusIcon" || id == "c.cacheIcon":
                    // the light: its look (the accessibility name the core gives it, as the Mac's
                    // image description), its tint as sRGB, its tooltip
                    s[id + ".img"] = AutomationProperties.GetName(t) ?? "";
                    s[id + ".tint"] = t.Foreground is SolidColorBrush sb ? ProbeIds.Rgb(sb.Color.R, sb.Color.G, sb.Color.B) : "";
                    break;
                case TextBlock t:
                    s[id + ".str"] = t.Text ?? "";
                    break;
                case TextBox x:
                    s[id + ".str"] = x.Text ?? "";
                    break;
            }
        }

        // the outline as Probe.swift's NSOutlineView rows: the rows shown (a closed folder's children
        // aren't), each indented two spaces per level, "v " open, "> " closed, "  " a playlist
        private static void OutlineProps(string id, PrepareView v, Dictionary<string, string> s)
        {
            var shown = new List<(OutlineRow row, int level)>();
            void Walk(IEnumerable<OutlineRow> rows, int level)
            {
                foreach (var r in rows)
                {
                    shown.Add((r, level));
                    if (r.IsFolder && r.IsExpanded) Walk(r.Children, level + 1);
                }
            }
            Walk(v.Model.Roots, 0);
            s[id + ".rows"] = shown.Count.ToString();
            s[id + ".sel"] = shown.FindIndex(x => x.row.Row == v.Model.SelectedRow).ToString();
            s[id + ".en"] = v.Outline.IsEnabled ? "1" : "0";
            for (int i = 0; i < shown.Count && i < 500; i++)
            {
                var (r, level) = shown[i];
                string open = r.IsFolder ? (r.IsExpanded ? "v " : "> ") : "  ";
                s[id + ".r" + i] = new string(' ', 2 * level) + open + r.Title;
            }
        }

        private static void TracksProps(string id, ListView l, PrepareModel m, Dictionary<string, string> s)
        {
            s[id + ".rows"] = m.Tracks.Count.ToString();
            s[id + ".sel"] = l.SelectedIndex.ToString();
            s[id + ".en"] = l.IsEnabled ? "1" : "0";
            for (int r = 0; r < m.Tracks.Count && r < 500; r++) s[id + ".r" + r] = m.Tracks[r].Name + " | " + m.Tracks[r].Result + " | " + m.Tracks[r].Why;
        }

        // the Status tab's setters, as the Mac's (status.table is one of Probe.swift's perfFields;
        // the others count for perf mode's "quiet" only)
        private static void StatusEvents(StatusView v)
        {
            var m = v.Model;
            m.SetsRendered = (whole, n) => rec.Field("status.table", whole ? "reloadData" : "reloadDataForRowIndexes", null, n);
            m.PropertyChanged += (o, e) =>
            {
                switch (e.PropertyName)
                {
                    case nameof(StatusModel.AgainEnabled): rec.Field("status.againBtn", "setEnabled"); break;
                    case nameof(StatusModel.StopEnabled): rec.Field("status.stopBtn", "setEnabled"); break;
                    case nameof(StatusModel.RunLine): rec.Field("status.runLine", "setStringValue"); break;
                }
            };
            shell.Model.PropertyChanged += (o, e) =>
            {
                if (e.PropertyName == nameof(ManageModel.PlusTip)) rec.Field("status.plusTip", "setStringValue");
                else if (e.PropertyName == nameof(ManageModel.CacheTip)) rec.Field("status.cacheTip", "setStringValue");
            };
        }

        // perf mode's Prepare fields, as the Mac's setters (Probe.swift's perfFields)
        private static void PrepareEvents(PrepareModel m)
        {
            m.TracksRendered = (whole, n) => rec.Field("prepare.tracksTable", whole ? "reloadData" : "reloadDataForRowIndexes", null, n);
            m.OutlineRendered = (whole, n) => rec.Field("prepare.outline", "reloadData", null, n);
            m.PropertyChanged += (o, e) =>
            {
                switch (e.PropertyName)
                {
                    case nameof(PrepareModel.Info):
                    case nameof(PrepareModel.InfoSub): rec.Field("prepare.info", "setAttributedStringValue"); break;
                    case nameof(PrepareModel.PrepareEnabled): rec.Field("prepare.prepareBtn", "setEnabled"); break;
                    case nameof(PrepareModel.Progress): rec.Field("prepare.bar", "setDoubleValue"); break;
                }
            };
        }

        // a title as shown: the access key's '_' left out
        private static string Plain(string t) => t.Replace("__", "\u0001").Replace("_", "").Replace("\u0001", "_");

        private static string Header(TabItem t) => t == null ? "" : Plain(t.Header as string ?? "");

        // ------------------------------------------------------------------ perf mode's field events

        private static void Model(ManageModel m)
        {
            // the spinner on every S_APP pass, as the Mac's start/stopAnimation on every render (P4)
            m.SpinnerPass = on => rec.Field("c.spinner", on ? "startAnimation" : "stopAnimation");
            m.PropertyChanged += (o, e) =>
            {
                switch (e.PropertyName)
                {
                    case nameof(ManageModel.PlusLook): rec.Field("c.plusIcon", "setImage", m.PlusLook.ToString()); break;
                    case nameof(ManageModel.CacheLook): rec.Field("c.cacheIcon", "setImage", m.CacheLook.ToString()); break;
                    case nameof(ManageModel.StatusLine): rec.Field("c.status", "setStringValue"); break;
                    case nameof(ManageModel.InstallPlusEnabled):
                    case nameof(ManageModel.ReinstallPlusEnabled):
                    case nameof(ManageModel.UninstallPlusEnabled):
                    case nameof(ManageModel.InstallCacheEnabled):
                    case nameof(ManageModel.ReinstallCacheEnabled):
                    case nameof(ManageModel.UninstallCacheEnabled):
                    case nameof(ManageModel.SettingsEnabled):
                        rec.Field("c.buttons", "setEnabled"); break;   // perf mode's "quiet" only: not in the short list
                }
            };
        }

        // ------------------------------------------------------------------ dialogs

        private static void WindowLoaded(object sender, RoutedEventArgs e)
        {
            if (!(sender is Window w) || w == shell.Window || w.Owner != shell.Window) return;
            dialog = w;
            rec.Dialog(true, Describe(w));
            w.Closed += (o, a) =>
            {
                rec.Dialog(false, Describe(w));
                if (dialog == w) dialog = null;
            };
        }

        /// A dialog as the user sees it (Probe.swift's describeDialog): its texts in order, its
        /// fields, its buttons ("(off)" disabled, "*" the default).
        private static string Describe(Window w)
        {
            var texts = new List<string>();
            var buttons = new List<string>();
            Walk(w, d =>
            {
                switch (d)
                {
                    case PasswordBox p: texts.Add(p.Password.Length == 0 ? "[password]" : "[password: typed]"); break;
                    case TextBox t: texts.Add(t.IsReadOnly ? t.Text : "[" + t.Text + "]"); break;
                    case TextBlock t when !(VisualTreeHelper.GetParent(t) is ContentPresenter cp && cp.TemplatedParent is Button): texts.Add(t.Text); break;
                    case Button b: buttons.Add(Plain(b.Content as string ?? "") + (b.IsEnabled ? "" : " (off)") + (b.IsDefault ? "*" : "")); break;
                }
            });
            return string.Join(" / ", texts) + " {" + string.Join(", ", buttons) + "}";
        }

        private static void Walk(DependencyObject d, Action<DependencyObject> f)
        {
            f(d);
            int n = d is Visual || d is System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetChildrenCount(d) : 0;
            for (int i = 0; i < n; i++) Walk(VisualTreeHelper.GetChild(d, i), f);
        }

        private static List<T> All<T>(DependencyObject root) where T : DependencyObject
        {
            var r = new List<T>();
            Walk(root, d => { if (d is T t) r.Add(t); });
            return r;
        }

        // ------------------------------------------------------------------ commands

        // the pipe's DACL: this user only (no inherited entries: not the default's SYSTEM and
        // Administrators), and no network logon at all (a deny entry for the NETWORK group)
        private static PipeSecurity PipeAcl()
        {
            var ps = new PipeSecurity();
            ps.SetAccessRuleProtection(true, false);
            using (var me = WindowsIdentity.GetCurrent())
                ps.AddAccessRule(new PipeAccessRule(me.User, PipeAccessRights.FullControl, AccessControlType.Allow));
            ps.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.NetworkSid, null), PipeAccessRights.FullControl, AccessControlType.Deny));
            return ps;
        }

        private static void Commands()
        {
            bool logged = false;
            while (true)
            {
                NamedPipeServerStream made;
                try { made = new NamedPipeServerStream(PipeName, PipeDirection.In, 1, PipeTransmissionMode.Byte, PipeOptions.None, 0, 0, PipeAcl()); }
                catch (Exception e)
                {
                    // the name taken by another process, or the DACL refused: said once, then retried
                    if (!logged) { logged = true; shell.Log("shell test: the probe's pipe can't be made (" + e.Message + "); retrying"); }
                    Thread.Sleep(500);
                    continue;
                }
                try
                {
                    using (var p = made)
                    {
                        p.WaitForConnection();
                        using (var r = new StreamReader(p, new UTF8Encoding(false)))
                        {
                            string line;
                            while ((line = r.ReadLine()) != null)
                            {
                                string text = line;
                                // Send priority: under a modal dialog's nested frame too
                                ui.BeginInvoke(DispatcherPriority.Send, (Action)(() => Command(text)));
                            }
                        }
                    }
                }
                catch (Exception) { Thread.Sleep(500); }
            }
        }

        private static void Ack(string id, string result) =>
            Write(() => File.AppendAllText(Path.Combine(outDir, "acks"), id + " " + result + "\n", new UTF8Encoding(false)));

        private static object Field(string id) => fields.FirstOrDefault(f => f.Key == id).Value;

        private static void Command(string text)
        {
            var c = ProbeCommand.Parse(text);
            if (c == null) return;
            BuildMenus();
            rec.Command(c.Verb, c.Arg);
            string id = c.Id, arg = c.Arg;
            Window w = shell.Window;
            switch (c.Verb)
            {
                case "mark": Ack(id, "ok"); break;
                case "click":
                    if (!(Field(arg) is Button b)) { Ack(id, "err no button " + arg); break; }
                    if (!b.IsEnabled || b.Visibility != Visibility.Visible) { Ack(id, "err disabled " + arg); break; }
                    Ack(id, "ok");
                    Invoke(b);
                    break;
                case "menu":
                    if (!(Field(arg) is MenuItem mi)) { Ack(id, "err no menu item " + arg); break; }
                    shell.RefreshMenusForProbe();
                    if (!mi.IsEnabled || mi.Visibility != Visibility.Visible) { Ack(id, "err disabled " + arg); break; }
                    Ack(id, "ok");
                    mi.RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent, mi));
                    break;
                case "answer":
                {
                    Button b2 = dialog == null ? null : All<Button>(dialog).FirstOrDefault(x => Plain(x.Content as string ?? "") == arg);
                    if (b2 == null) { Ack(id, "err no dialog button " + arg); break; }
                    if (!b2.IsEnabled) { Ack(id, "err disabled " + arg); break; }
                    Ack(id, "ok");
                    Invoke(b2);
                    break;
                }
                case "fill":
                {
                    var a = arg.Split(new[] { ' ' }, 2);
                    if (dialog == null || !int.TryParse(a[0], out int n)) { Ack(id, "err no dialog"); break; }
                    var boxes = All<TextBox>(dialog).Where(x => !x.IsReadOnly).ToList();
                    if (n < 0 || n >= boxes.Count) { Ack(id, "err no field " + n); break; }
                    boxes[n].Text = a.Length > 1 ? a[1] : "";
                    Ack(id, "ok");
                    break;
                }
                case "tab":
                {
                    var tabs = w.FindName("Tabs") as TabControl;
                    var t = tabs?.Items.OfType<TabItem>().FirstOrDefault(x => Header(x) == arg);
                    if (t == null) { Ack(id, "err no tab " + arg); break; }
                    Ack(id, "ok");
                    t.IsSelected = true;
                    break;
                }
                case "miniaturize": Ack(id, "ok"); w.WindowState = WindowState.Minimized; break;
                case "deminiaturize": Ack(id, "ok"); w.WindowState = WindowState.Normal; w.Activate(); break;
                case "close": Ack(id, "ok"); w.Close(); break;
                case "fields":
                    try { File.WriteAllText(Path.Combine(outDir, "fields.tsv"), FieldTable(), new UTF8Encoding(false)); Ack(id, "ok"); }
                    catch (Exception e) { Ack(id, "err " + e.Message); }
                    break;
                case "flush": Flush(); Ack(id, "ok"); break;
                case "quit": Ack(id, "ok"); w.Close(); break;
                case "wait":
                {
                    var test = ProbeWaits.Condition(arg, Value, out long ms);
                    if (test == null) { Ack(id, "err usage"); break; }
                    Wait(id, ms, test);
                    break;
                }
                case "quiet":
                {
                    var a = ProbeWaits.Numbers(arg, 2);
                    if (a == null) { Ack(id, "err usage"); break; }
                    long need = a[0] * 1000000;
                    rec.LastChange = Now();
                    Wait(id, a[1], () => Now() - rec.LastChange >= need);
                    break;
                }
                case "event":
                {
                    var a = arg.Split(' ');
                    if (a.Length != 3 || !long.TryParse(a[0], out long ms) || !int.TryParse(a[2], out int n)) { Ack(id, "err usage"); break; }
                    string name = a[1];
                    Wait(id, ms, () => rec.Count(name) >= n);
                    break;
                }
                case "count": Ack(id, "ok " + rec.Count(arg)); break;
                // the Prepare tab (Probe.swift's verbs): each as the user's input would do it
                case "select":
                case "collapse":
                case "expand":
                {
                    // only a row shown (every ancestor open), as a click can reach it
                    var row = shell.PrepareTab?.Model.ShownOutlineById(arg);
                    if (row == null) { Ack(id, "err no row " + arg); break; }
                    Ack(id, "ok");
                    if (c.Verb == "select") row.IsSelected = true;      // as a click on the row
                    else row.IsExpanded = c.Verb == "expand";           // as a click on its expander
                    break;
                }
                case "until":
                {
                    var pv = shell.PrepareTab;
                    if (pv == null || !double.TryParse(arg, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double days)
                        || double.IsNaN(days) || double.IsInfinity(days) || Math.Abs(days) > 3650) { Ack(id, "err"); break; }
                    pv.Until.SelectedDate = DateTime.Today.AddDays(Math.Round(days));   // as the picker's own change
                    Ack(id, "ok");
                    break;
                }
                case "scroll":
                {
                    var pv = shell.PrepareTab;
                    if (pv == null || !int.TryParse(arg, out int r)) { Ack(id, "err"); break; }
                    int n = pv.Model.Tracks.Count;
                    if (n > 0) pv.Tracks.ScrollIntoView(pv.Model.Tracks[Math.Min(Math.Max(r, 0), n - 1)]);
                    Ack(id, "ok");
                    break;
                }
                // the Status tab (Probe.swift's verbs): a row selected as a click on it would
                // (the list's binding takes it to the model), a double click as the row's
                case "select-set":
                case "doubleclick-set":
                {
                    var sv = shell.StatusTab;
                    if (sv == null || !int.TryParse(arg, out int r) || r >= sv.Model.Sets.Count || (r < 0 && c.Verb == "doubleclick-set")) { Ack(id, "err"); break; }
                    sv.Table.SetCurrentValue(System.Windows.Controls.Primitives.Selector.SelectedIndexProperty, r < 0 ? -1 : r);
                    Ack(id, "ok");
                    if (c.Verb == "doubleclick-set") sv.Model.RowDoubleClicked();
                    break;
                }
                default: Ack(id, "err unknown " + c.Verb + " (not on this shell)"); break;
            }
        }

        // a click as UI Automation does it (the button's own Click, its command, its access text)
        private static void Invoke(Button b)
        {
            if (UIElementAutomationPeer.CreatePeerForElement(b)?.GetPattern(PatternInterface.Invoke) is IInvokeProvider p) p.Invoke();
            else b.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent, b));
        }
    }
}
#endif
