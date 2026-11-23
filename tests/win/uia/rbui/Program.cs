// rbui: the VM suites' UI Automation driver (the by-hand plan's W1; what it reaches: tests/win/uia/
// build.sh's header). FlaUI 5.0.0 over UIA3, net481, one verb per run, every line it prints
// starting with the UTC time (HH:mm:ss.fff), so a step can order them against the app's log.
//
//   rbui tree [TARGET] [--depth N]                    the UIA tree (all top-level windows, or TARGET's)
//   rbui wait-window TARGET [--timeout S]             a top-level window of TARGET is there (with
//                                                     --title, also a dialog one of them owns)
//   rbui invoke TARGET ELEMENT [--type T] [--timeout S]
//                                                     the first enabled ELEMENT pressed (Invoke,
//                                                     Toggle, SelectionItem, ExpandCollapse, the
//                                                     default action, else a click)
//   rbui menu TARGET ITEM... [--timeout S]            a menu path: each ITEM but the last expanded,
//                                                     the last invoked
//   rbui read TARGET [--timeout S]                    every element's control type, name and id
//   rbui events --pid N --seconds S --ready FILE      record focus and live-region events
//   rbui notice TITLE (--pid N | --process NAME) [--timeout S]                   record the shell banner and tray icon lifetime
//   rbui settings-uninstall NAME [--timeout S]        Installed apps > More options > Uninstall twice
//   rbui taskbar pin|unpin|click|count NAME [--timeout S]
//   rbui zorder PID [--above-settings MESSAGE]        visible windows, or check the one kept dialog
//   rbui on-close PID then taskbar click NAME --hwnd N --confirm BUTTON --delay-ms N [--timeout S]
//                                                     arm WindowClosed before confirming; click once
//   rbui screenshot FILE                              the screen of the desktop rbui runs on (PNG)
//   rbui consent yes|no|read [--expect TEXT]... [--details] [--delay S] [--after S] [--timeout S]
//                [--then EXE [--then-args ARGS] [--then-when REGEX]]
//                                                     UAC's consent prompt (consent.exe): read, and
//                                                     answered only when every --expect TEXT is in
//                                                     it (else No, exit 4); --details opens "Show
//                                                     more details" first (the program's location);
//                                                     --delay waits before the answer; --after
//                                                     watches S more seconds for another prompt
//                                                     (refused, exit 5); --then starts EXE ARGS
//                                                     right after a yes (not waited for: the
//                                                     suites' sign-out while the worker runs),
//                                                     once a process whose image path matches
//                                                     REGEX runs (polled every 50 ms, 30 s at most;
//                                                     started anyway then, and said)
//   rbui blockscreen read|cancel|anyway [--expect TEXT]... [--timeout S] [--shot FILE]
//                                                     Windows' block screen while an app holds a
//                                                     sign-out or restart (LogonUI): read, Cancel,
//                                                     or "... anyway" (exit 4 when an --expect TEXT
//                                                     is missing; the button is still pressed)
// TARGET: --pid N, --process NAME (image name, no .exe) and/or --title TEXT (in the window's
// name). ELEMENT: a name, or #AutomationId. T: a FlaUI ControlType (Button, MenuItem, ...).
// --timeout defaults to 60 s (consent and blockscreen: 300 and 180).
//
// consent and blockscreen reach the secure desktop (winsta0\winlogon) only when rbui runs there:
// as SYSTEM in the console session, which sysdesk.exe does (prlctl exec, a SYSTEM task). Anywhere
// else they find nothing (exit 3).
//
// Exit: 0 done; 1 an error (said); 2 usage; 3 nothing found within the timeout; 4 an --expect text
// missing; 5 another consent prompt during --after.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using FlaUI.Core.AutomationElements;
using FlaUI.Core.Definitions;
using FlaUI.UIA3;

namespace RbStemsPlus.Rbui
{
    internal static partial class Program
    {
        private const int Done = 0, Error = 1, Usage = 2, NotFound = 3, Unexpected = 4, Another = 5;

        [STAThread]
        private static int Main(string[] args)
        {
            // UTF-8 out, flushed per line, whether stdout is a console, a pipe or sysdesk's file
            var w = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true };
            Console.SetOut(w);
            Console.SetError(w);
            try { return Run(args); }
            catch (UsageException e) { Say("usage: " + e.Message); return Usage; }
            catch (Exception e) { Say("error: " + e.GetType().Name + ": " + e.Message); return Error; }
        }

        private sealed class UsageException : Exception { public UsageException(string m) : base(m) { } }

        internal static void Say(string line) =>
            Console.Out.WriteLine(DateTime.UtcNow.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + " " + line);

        // ---- options ---------------------------------------------------------------------------

        private sealed class Opts
        {
            public readonly List<string> Args = new List<string>();
            private readonly Dictionary<string, List<string>> named = new Dictionary<string, List<string>>();
            private static readonly HashSet<string> Flags = new HashSet<string> { "--details" };

            public Opts(string[] argv, int from)
            {
                for (int i = from; i < argv.Length; i++)
                {
                    string a = argv[i];
                    if (Flags.Contains(a)) { Add(a, "1"); continue; }
                    if (a.StartsWith("--", StringComparison.Ordinal))
                    {
                        if (i + 1 >= argv.Length) throw new UsageException(a + " needs a value");
                        Add(a, argv[++i]);
                        continue;
                    }
                    Args.Add(a);
                }
            }
            private void Add(string k, string v)
            {
                if (!named.TryGetValue(k, out var l)) named[k] = l = new List<string>();
                l.Add(v);
            }
            public string Get(string k) => named.TryGetValue(k, out var l) ? l[l.Count - 1] : null;
            public List<string> All(string k) => named.TryGetValue(k, out var l) ? l : new List<string>();
            public bool Has(string k) => named.ContainsKey(k);
            public int Int(string k, int dflt)
            {
                string v = Get(k);
                if (v == null) return dflt;
                if (!int.TryParse(v, NumberStyles.None, CultureInfo.InvariantCulture, out int n)) throw new UsageException(k + " takes a number");
                return n;
            }
            public void Only(params string[] keys)
            {
                foreach (var k in named.Keys)
                    if (Array.IndexOf(keys, k) < 0) throw new UsageException("no option " + k + " here");
            }
        }

        private sealed class Target
        {
            public int Pid;
            public string Process, Title;
            public bool Any => Pid != 0 || Process != null || Title != null;
            public static Target From(Opts o)
            {
                var t = new Target { Pid = o.Int("--pid", 0), Process = o.Get("--process"), Title = o.Get("--title") };
                if (t.Process != null && t.Process.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) t.Process = t.Process.Substring(0, t.Process.Length - 4);
                return t;
            }
            public override string ToString() =>
                string.Join(" ", new[] { Pid != 0 ? "pid " + Pid : null, Process != null ? "process " + Process : null, Title != null ? "title \"" + Title + "\"" : null }.Where(s => s != null));
        }

        // ---- verbs -----------------------------------------------------------------------------

        private static int Run(string[] argv)
        {
            if (argv.Length == 0) throw new UsageException("rbui tree|wait-window|invoke|menu|read|events|notice|settings-uninstall|taskbar|zorder|on-close|screenshot|consent|blockscreen ...");
            string verb = argv[0];
            var o = new Opts(argv, 1);
            // UIA event registration and removal are on the same MTA thread.
            if (verb == "on-close" || verb == "events")
                return Task.Run(() =>
                {
                    using (var ua = new UIA3Automation())
                        return verb == "events" ? Events(ua, ua.GetDesktop(), o) : OnClose(ua, ua.GetDesktop(), o);
                }).GetAwaiter().GetResult();
            using (var ua = new UIA3Automation())
            {
                var desk = ua.GetDesktop();
                switch (verb)
                {
                    case "tree": return Tree(desk, o);
                    case "wait-window": return WaitWindow(desk, o);
                    case "invoke": return InvokeVerb(desk, o);
                    case "menu": return Menu(desk, o);
                    case "read": return Read(desk, o);
                    case "notice": return Notice(desk, o);
                    case "settings-uninstall": return SettingsUninstall(desk, o);
                    case "taskbar": return Taskbar(desk, o);
                    case "zorder": return ZOrder(desk, o);
                    case "screenshot": return Screenshot(o);
                    case "prepare-input": return PrepareInput(desk, o);
                    case "consent": return Consent.Run(desk, o);
                    case "blockscreen": return BlockScreen.Run(desk, o);
                    default: throw new UsageException("no verb " + verb);
                }
            }
        }

        private static int Tree(AutomationElement desk, Opts o)
        {
            o.Only("--pid", "--process", "--title", "--depth");
            var t = Target.From(o);
            int depth = o.Int("--depth", 8);
            var wins = t.Any ? Windows(desk, t) : Ui.Children(desk).ToList();
            Say("tree: " + wins.Count + " top-level window(s)" + (t.Any ? " of " + t : ""));
            foreach (var w in wins) Dump(w, 0, depth);
            return Done;
        }

        private static void Dump(AutomationElement e, int level, int depth)
        {
            Console.Out.WriteLine(new string(' ', 2 * level) + Ui.Describe(e));
            if (level >= depth) return;
            foreach (var c in Ui.Children(e)) Dump(c, level + 1, depth);
        }

        private static int WaitWindow(AutomationElement desk, Opts o)
        {
            o.Only("--pid", "--process", "--title", "--timeout");
            var t = Target.From(o);
            if (!t.Any) throw new UsageException("wait-window needs --pid, --process or --title");
            var w = Ui.Poll(o.Int("--timeout", 60), () => Windows(desk, t).FirstOrDefault());
            if (w == null) { Say("wait-window: no window of " + t); return NotFound; }
            Say("wait-window: " + Ui.Describe(w));
            return Done;
        }

        private static int InvokeVerb(AutomationElement desk, Opts o)
        {
            o.Only("--pid", "--process", "--title", "--timeout", "--type");
            var t = Target.From(o);
            if (!t.Any || o.Args.Count != 1) throw new UsageException("invoke TARGET ELEMENT [--type T] [--timeout S]");
            var type = Ui.Type(o.Get("--type"));
            string spec = o.Args[0];
            var e = Ui.Poll(o.Int("--timeout", 60), () =>
                Windows(desk, t).SelectMany(Ui.Descendants).FirstOrDefault(x => Ui.Matches(x, spec, type) && Ui.Enabled(x)));
            if (e == null) { Say("invoke: no enabled \"" + spec + "\" in a window of " + t); return NotFound; }
            Say("invoke: " + Ui.Describe(e) + ": " + Ui.Press(e));
            return Done;
        }

        private static int Menu(AutomationElement desk, Opts o)
        {
            o.Only("--pid", "--process", "--title", "--timeout");
            var t = Target.From(o);
            if (!t.Any || o.Args.Count == 0) throw new UsageException("menu TARGET ITEM... [--timeout S]");
            var sw = Stopwatch.StartNew();
            int secs = o.Int("--timeout", 60);
            for (int i = 0; i < o.Args.Count; i++)
            {
                string item = o.Args[i];
                // a submenu's items live in a popup: a top-level window of the same process
                var e = Ui.Poll(Math.Max(1, secs - (int)sw.Elapsed.TotalSeconds), () =>
                    Windows(desk, t).SelectMany(Ui.Descendants).FirstOrDefault(x => Ui.Matches(x, item, ControlType.MenuItem) && Ui.Enabled(x)));
                if (e == null) { Say("menu: no enabled item \"" + item + "\" in a window of " + t); return NotFound; }
                string how = i < o.Args.Count - 1 && e.Patterns.ExpandCollapse.IsSupported ? Ui.Expand(e) : Ui.Press(e);
                Say("menu: " + Ui.Describe(e) + ": " + how);
            }
            return Done;
        }

        private static int Read(AutomationElement desk, Opts o)
        {
            o.Only("--pid", "--process", "--title", "--timeout");
            var t = Target.From(o);
            if (!t.Any) throw new UsageException("read needs --pid, --process or --title");
            var wins = Ui.Poll(o.Int("--timeout", 60), () => { var l = Windows(desk, t); return l.Count > 0 ? l : null; });
            if (wins == null) { Say("read: no window of " + t); return NotFound; }
            foreach (var w in wins)
            {
                Say("read: window " + Ui.Describe(w));
                foreach (var e in Ui.Descendants(w)) Console.Out.WriteLine("  " + Ui.Describe(e));
            }
            return Done;
        }

        private static int Screenshot(Opts o)
        {
            o.Only();
            if (o.Args.Count != 1) throw new UsageException("screenshot FILE");
            using (var img = FlaUI.Core.Capturing.Capture.Screen())
            {
                img.ToFile(o.Args[0]);
                Say("screenshot: " + o.Args[0] + " (" + img.Bitmap.Width + "x" + img.Bitmap.Height + ")");
            }
            return Done;
        }

        // what a secure-desktop verb that found nothing saw (for the spike): every top-level
        // window of this desktop, and the whole tree of the target's
        private static void Diagnose(AutomationElement desk, Target t)
        {
            var names = new Dictionary<int, string>();
            foreach (var w in Ui.Children(desk)) Console.Out.WriteLine("  top-level: " + Ui.Describe(w) + " (" + Ui.ProcessName(Ui.Pid(w), names) + ")");
            foreach (var w in Windows(desk, t)) Dump(w, 1, 12);
        }

        // the top-level windows (the desktop's children) of a target; with --title, also a window
        // one of them owns (UI Automation lists an owned dialog, such as the welcome or a WPF
        // MessageBox with an owner, as a Window child of its owner, not of the desktop)
        private static List<AutomationElement> Windows(AutomationElement desk, Target t)
        {
            var names = new Dictionary<int, string>();
            var res = new List<AutomationElement>();
            foreach (var w in Ui.Children(desk))
            {
                int pid = Ui.Pid(w);
                if (t.Pid != 0 && pid != t.Pid) continue;
                if (t.Process != null && !string.Equals(Ui.ProcessName(pid, names), t.Process, StringComparison.OrdinalIgnoreCase)) continue;
                if (t.Title == null || Titled(w, t.Title)) { res.Add(w); continue; }
                foreach (var c in Ui.Children(w))
                    if (Ui.Type(c) == ControlType.Window && Titled(c, t.Title)) res.Add(c);
            }
            return res;
        }

        private static bool Titled(AutomationElement w, string title) =>
            Ui.Name(w).IndexOf(title, StringComparison.OrdinalIgnoreCase) >= 0;

        // ---- the secure desktop ----------------------------------------------------------------

        // UAC's consent prompt: consent.exe's window on the secure desktop (Windows 11's XAML dialog:
        // the Yes and No buttons by AutomationId OkButton/CancelButton, else by their English names)
        private static class Consent
        {
            public static int Run(AutomationElement desk, Opts o)
            {
                o.Only("--expect", "--details", "--delay", "--after", "--timeout", "--then", "--then-args", "--then-when");
                if (o.Args.Count != 1 || (o.Args[0] != "yes" && o.Args[0] != "no" && o.Args[0] != "read")) throw new UsageException("consent yes|no|read ...");
                string what = o.Args[0];
                var t = new Target { Process = "consent" };
                Say("consent: waiting for the prompt (" + o.Int("--timeout", 300) + " s), to answer " + what);
                var w = Ui.Poll(o.Int("--timeout", 300), () => Windows(desk, t).FirstOrDefault(x => Ui.Descendants(x).Any(IsAnswer)));
                if (w == null) { Say("consent: no prompt"); Diagnose(desk, t); return NotFound; }
                int pid = Ui.Pid(w);
                Say("consent: prompt seen (consent.exe pid " + pid + ")");
                if (o.Has("--details"))
                {
                    var more = Ui.Descendants(w).FirstOrDefault(x => Ui.Name(x).IndexOf("more details", StringComparison.OrdinalIgnoreCase) >= 0);
                    if (more != null) { Say("consent: details: " + Ui.Press(more)); Thread.Sleep(1000); }
                    else Say("consent: no \"Show more details\"");
                }
                string text = Ui.Text(w);
                Say("consent: text: " + text);
                var missing = o.All("--expect").Where(x => text.IndexOf(x, StringComparison.OrdinalIgnoreCase) < 0).ToList();
                foreach (var m in missing) Say("consent: not in it: \"" + m + "\"");
                if (what == "read") return missing.Count > 0 ? Unexpected : Done;
                int delay = o.Int("--delay", 0);
                if (delay > 0) { Say("consent: answering in " + delay + " s"); Thread.Sleep(delay * 1000); }
                // a prompt that isn't the one expected is refused, never accepted
                string answer = missing.Count > 0 ? "no" : what;
                if (!Answer(w, answer)) { Say("consent: no " + answer + " button"); return Error; }
                if (answer == "yes" && o.Has("--then")) return Then(o);
                Ui.Poll(10, () => Ui.Alive(pid) ? null : "gone");
                if (missing.Count > 0) return Unexpected;
                int after = o.Int("--after", 0);
                if (after > 0)
                {
                    Say("consent: watching " + after + " s for another prompt");
                    var again = Ui.Poll(after, () => Windows(desk, t).FirstOrDefault(x => Ui.Pid(x) != pid && Ui.Descendants(x).Any(IsAnswer)));
                    if (again != null)
                    {
                        Say("consent: another prompt: " + Ui.Text(again));
                        Answer(again, "no");
                        return Another;
                    }
                    Say("consent: no other prompt");
                }
                return Done;
            }

            // --then: EXE ARGS started once --then-when's process runs (or after 30 s without it)
            private static int Then(Opts o)
            {
                string when = o.Get("--then-when");
                if (when != null)
                {
                    var re = new Regex(when, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
                    string seen = Ui.Poll(30, () => Ui.RunningImage(re), 50);
                    Say(seen != null ? "consent: then: running: " + seen : "consent: then: nothing matching " + when + " ran within 30 s: starting it anyway");
                }
                var psi = new ProcessStartInfo(o.Get("--then"), o.Get("--then-args") ?? "") { UseShellExecute = false, CreateNoWindow = true };
                using (var p = Process.Start(psi)) Say("consent: then: started " + psi.FileName + " " + psi.Arguments + " (pid " + p.Id + ")");
                return Done;
            }

            private static bool IsAnswer(AutomationElement e) =>
                Ui.Type(e) == ControlType.Button && (Ui.Id(e) == "OkButton" || Ui.Id(e) == "CancelButton" || Ui.Name(e) == "Yes" || Ui.Name(e) == "No");

            private static bool Answer(AutomationElement w, string answer)
            {
                string id = answer == "yes" ? "OkButton" : "CancelButton", name = answer == "yes" ? "Yes" : "No";
                var all = Ui.Descendants(w).Where(x => Ui.Type(x) == ControlType.Button).ToList();
                var b = all.FirstOrDefault(x => Ui.Id(x) == id) ?? all.FirstOrDefault(x => Ui.Name(x) == name);
                if (b == null) return false;
                Say("consent: pressing " + Ui.Describe(b));
                Say("consent: answered " + answer + ": " + Ui.Press(b));
                return true;
            }
        }

        // the block screen: LogonUI's "N apps need to close" while an app holds a sign-out or a
        // restart (its Cancel, and "Sign out anyway" or "Restart anyway"). Windows 11's is LogonUI's
        // "Blocked shutdown" window (class BlockedShutdownUX XAML Host), known by that window or its
        // "... anyway" button, its XAML buttons given a moment to come. A screenshot of it goes to
        // --shot FILE when given
        private static class BlockScreen
        {
            public static int Run(AutomationElement desk, Opts o)
            {
                o.Only("--expect", "--timeout", "--shot");
                if (o.Args.Count != 1 || (o.Args[0] != "read" && o.Args[0] != "cancel" && o.Args[0] != "anyway")) throw new UsageException("blockscreen read|cancel|anyway ...");
                string what = o.Args[0];
                var t = new Target { Process = "LogonUI" };
                Say("blockscreen: waiting for it (" + o.Int("--timeout", 180) + " s), to press " + what);
                var w = Ui.Poll(o.Int("--timeout", 180), () => Windows(desk, t).FirstOrDefault(x => IsBlocked(x) || Ui.Descendants(x).Any(IsAnyway)));
                if (w == null) { Say("blockscreen: none"); Diagnose(desk, t); return NotFound; }
                Say("session-trace " + DateTime.UtcNow.Ticks + " block");
                Say("blockscreen: seen (LogonUI pid " + Ui.Pid(w) + ", " + Ui.Describe(w) + ")");
                // its buttons, when UIA has them: a moment for the XAML content to come
                var buttons = Ui.Poll(3, () => { var b = Ui.Descendants(w).Where(x => Ui.Type(x) == ControlType.Button).ToList(); return b.Count > 0 ? b : null; }) ?? new List<AutomationElement>();
                if (o.Get("--shot") != null) Shot(o.Get("--shot"));
                List<string> missing;
                if (buttons.Count > 0)
                {
                    string text = Ui.Text(w);
                    Say("blockscreen: text: " + text);
                    missing = o.All("--expect").Where(x => text.IndexOf(x, StringComparison.OrdinalIgnoreCase) < 0).ToList();
                    foreach (var m in missing) Say("blockscreen: not in it: \"" + m + "\"");
                }
                else
                {
                    missing = o.All("--expect").ToList();
                    Say("blockscreen: no buttons in its UIA tree: its text unread");
                    foreach (var m in missing) Say("blockscreen: not checked: \"" + m + "\"");
                }
                if (what == "read") return missing.Count > 0 ? Unexpected : Done;
                var b = what == "cancel" ? buttons.FirstOrDefault(x => Ui.Name(x) == "Cancel") : buttons.FirstOrDefault(IsAnyway);
                if (b == null) { Say("blockscreen: no " + what + " button"); return Error; }
                Say("blockscreen: pressing " + Ui.Describe(b));
                Say("session-trace " + DateTime.UtcNow.Ticks + " answer");
                Say("blockscreen: pressed: " + Ui.Press(b));
                return missing.Count > 0 ? Unexpected : Done;
            }

            private static void Shot(string file)
            {
                try
                {
                    using (var img = FlaUI.Core.Capturing.Capture.Screen()) img.ToFile(file);
                    Say("blockscreen: screenshot " + file);
                }
                catch (Exception e) { Say("blockscreen: no screenshot: " + e.Message); }
            }

            private static bool IsBlocked(AutomationElement e) =>
                Ui.Class(e).IndexOf("BlockedShutdown", StringComparison.OrdinalIgnoreCase) >= 0;

            private static bool IsAnyway(AutomationElement e) =>
                Ui.Type(e) == ControlType.Button && Ui.Name(e).IndexOf("anyway", StringComparison.OrdinalIgnoreCase) >= 0;
        }
    }

    // UIA reads that never throw: an element can go away between two reads
    internal static class Ui
    {
        public static T Safe<T>(Func<T> f, T dflt)
        {
            try { return f(); } catch (Exception) { return dflt; }
        }

        public static string Name(AutomationElement e) => Safe(() => e.Properties.Name.ValueOrDefault, null) ?? "";
        public static string Id(AutomationElement e) => Safe(() => e.Properties.AutomationId.ValueOrDefault, null) ?? "";
        public static string Class(AutomationElement e) => Safe(() => e.Properties.ClassName.ValueOrDefault, null) ?? "";
        public static ControlType Type(AutomationElement e) => Safe(() => e.Properties.ControlType.ValueOrDefault, ControlType.Unknown);
        public static int Pid(AutomationElement e) => Safe(() => e.Properties.ProcessId.ValueOrDefault, 0);
        public static bool Enabled(AutomationElement e) => Safe(() => e.Properties.IsEnabled.ValueOrDefault, false);
        public static AutomationElement[] Children(AutomationElement e) => Safe(() => e.FindAllChildren(), new AutomationElement[0]);
        public static AutomationElement[] Descendants(AutomationElement e) => Safe(() => e.FindAllDescendants(), new AutomationElement[0]);

        public static string ProcessName(int pid, Dictionary<int, string> cache)
        {
            if (cache.TryGetValue(pid, out var n)) return n;
            n = Safe(() => { using (var p = Process.GetProcessById(pid)) return p.ProcessName; }, "");
            cache[pid] = n;
            return n;
        }
        public static bool Alive(int pid) => Safe(() => { using (var p = Process.GetProcessById(pid)) return !p.HasExited; }, false);

        public static ControlType? Type(string name)
        {
            if (name == null) return null;
            if (!Enum.TryParse(name, true, out ControlType t)) throw new ArgumentException("no control type " + name);
            return t;
        }

        // ELEMENT: a name, or #AutomationId
        public static bool Matches(AutomationElement e, string spec, ControlType? type)
        {
            if (type.HasValue && Type(e) != type.Value) return false;
            return spec.StartsWith("#", StringComparison.Ordinal) ? Id(e) == spec.Substring(1) : Name(e) == spec;
        }

        public static string Describe(AutomationElement e)
        {
            var sb = new StringBuilder();
            sb.Append(Type(e)).Append(" \"").Append(Name(e)).Append('"');
            string id = Id(e), cls = Class(e);
            if (id.Length > 0) sb.Append(" #").Append(id);
            if (cls.Length > 0) sb.Append(" class=").Append(cls);
            if (!Enabled(e)) sb.Append(" disabled");
            sb.Append(" pid=").Append(Pid(e));
            string help = Safe(() => e.HelpText, null);
            if (!string.IsNullOrEmpty(help)) sb.Append(" help=\"").Append(help).Append('"');
            return sb.ToString();
        }

        // every name in a window, in tree order, once each: what the prompt or screen says
        public static string Text(AutomationElement w)
        {
            var seen = new HashSet<string>();
            var parts = new List<string>();
            foreach (var e in new[] { w }.Concat(Descendants(w)))
            {
                string n = Name(e).Trim();
                if (n.Length > 0 && seen.Add(n)) parts.Add(n);
            }
            return string.Join(" | ", parts);
        }

        public static string Press(AutomationElement e)
        {
            if (e.Patterns.Invoke.IsSupported) { e.Patterns.Invoke.Pattern.Invoke(); return "invoked"; }
            if (e.Patterns.Toggle.IsSupported) { e.Patterns.Toggle.Pattern.Toggle(); return "toggled"; }
            if (e.Patterns.SelectionItem.IsSupported) { e.Patterns.SelectionItem.Pattern.Select(); return "selected"; }
            if (e.Patterns.ExpandCollapse.IsSupported) return Expand(e);
            if (e.Patterns.LegacyIAccessible.IsSupported) { e.Patterns.LegacyIAccessible.Pattern.DoDefaultAction(); return "default action"; }
            e.Click();
            return "clicked";
        }

        public static string Expand(AutomationElement e)
        {
            e.Patterns.ExpandCollapse.Pattern.Expand();
            return "expanded";
        }

        // "PID PATH" of the first process whose full image path matches re (any process this one
        // may query: all of them as SYSTEM), or null
        public static string RunningImage(Regex re)
        {
            foreach (var p in Process.GetProcesses())
            {
                using (p)
                {
                    IntPtr h = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, p.Id);
                    if (h == IntPtr.Zero) continue;
                    try
                    {
                        var sb = new StringBuilder(1024);
                        uint n = (uint)sb.Capacity;
                        if (QueryFullProcessImageNameW(h, 0, sb, ref n) && re.IsMatch(sb.ToString())) return p.Id + " " + sb;
                    }
                    finally { CloseHandle(h); }
                }
            }
            return null;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool QueryFullProcessImageNameW(IntPtr h, uint flags, StringBuilder buf, ref uint size);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr h);

        // f's first non-null value, tried every ms milliseconds (250) for at most secs (once at least)
        public static T Poll<T>(int secs, Func<T> f, int ms = 250) where T : class
        {
            var sw = Stopwatch.StartNew();
            while (true)
            {
                T v = Safe(f, null);
                if (v != null) return v;
                if (sw.Elapsed.TotalSeconds >= secs) return null;
                Thread.Sleep(ms);
            }
        }
    }
}
