// The characterisation harness's probe on Windows (app.md §6, §7, W10; mac-core.md §2.2-2.3), its
// WPF-free part: the configuration, the records, the frame deltas and the driver's commands and
// waits, in app/Probe/Probe.swift's formats so tests/harness and tests/perf read both shells'
// records alike. TEST BUILDS ONLY (SHELL_TEST: win/shell/build.sh --test-app; the Mac tests build
// it with PROBE_TESTS): build.sh's release gate refuses a release exe that carries it.
//  - probe.conf (%LOCALAPPDATA%\rbstemsplus-probe\probe.conf, key=value lines): mode=trace | perf |
//    perf-min, out=<folder>. Without it, or with mode=off, the probe does nothing.
//  - turns (ProbeTurns): a turn ends when the dispatcher goes inactive (Hooks.DispatcherInactive,
//    after the render operation's layout and render: the Mac's beforeWaiting); its busy time is the
//    dispatcher operations' exclusive time since the last turn, without a nested frame's idle spans
//    (a modal window's: its build and first layout in its first turn) nor the probe's own sample,
//    records and waits.
//  - trace mode: <out>\app-<pid>.jsonl, JSON lines: start, frame (the fields that changed since the
//    last frame: sampled on CompositionTarget.Rendering, written when its turn ends), ev (events,
//    timing points, dialogs, commands).
//  - perf mode: (time, kind, name, number) in memory, <out>\perf-<pid>.tsv on `flush` and at exit:
//    turn (one per turn, n: its busy ns), field (the named short list only, as "<field id>:<the
//    Mac's setter>"), tp (timing points), mark, cmd, ev, sent (each event the shell sends the core,
//    named by its JSON: Windows' addition, as trace mode's ev "sent"). No values, no frame samples, no Rendering
//    handler (it would render every vsync), no menu validation (I35); app-<pid>.jsonl has the start
//    record only.
//  - commands: "<id> <verb> <arg>" lines, acked as "<id> <result>" lines in <out>\acks.
#if SHELL_TEST || PROBE_TESTS
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace RbStemsPlus.Shell
{
    internal enum ProbeMode { Off, Trace, Perf, PerfMin }

    internal static class ProbeConf
    {
        /// key=value lines (Probe.swift's start): trimmed, the first '=' splits.
        public static Dictionary<string, string> Parse(string text)
        {
            var d = new Dictionary<string, string>();
            foreach (string line in (text ?? "").Split('\n'))
            {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                d[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
            }
            return d;
        }

        public static ProbeMode Mode(Dictionary<string, string> conf)
        {
            if (!conf.TryGetValue("out", out string o) || o.Length == 0) return ProbeMode.Off;
            switch (conf.TryGetValue("mode", out string m) ? m : "")
            {
                case "trace": return ProbeMode.Trace;
                case "perf": return ProbeMode.Perf;
                case "perf-min": return ProbeMode.PerfMin;
                default: return ProbeMode.Off;
            }
        }

        public static string Name(ProbeMode m) => m == ProbeMode.Trace ? "trace" : m == ProbeMode.Perf ? "perf" : m == ProbeMode.PerfMin ? "perf-min" : "off";
    }

    /// One cheap fact probe.conf sets (ProbeFacts).
    internal sealed class ProbeFact
    {
        public RbStemsPlus.Core.Fact Id;
        public char Kind;   // 'f' flag, 'i' i64, 's' str
        public long N;
        public string S;
    }

    /// TEST builds' stand-in cheap facts: probe.conf's "fact.NAME=KIND:VALUE" lines (NAME the
    /// facts.def name without FACT_; KIND flag (0 or 1), i64 or str), written over the shell's real
    /// facts on every read of the sheet (Host.Facts' TEST hook), so a VM run can show the Prepare
    /// tab's playlists from the harness's fake helper without rekordbox or an install (W13b). A
    /// line that names no fact, whose kind or value doesn't parse, or whose kind isn't facts.def's
    /// (gen_ids' FactKinds) is refused (in bad, by name and reason, never its value), never guessed.
    /// KIND esc is a str whose value carries \n, \t and \\ escapes (any other '\' refused), for a
    /// fact of several lines on probe.conf's one line: FACT_KEPT_SETS' "name\tuntil\tfiles" sets,
    /// the Status tab's rows (vm/w13b-fixture.ps1). str keeps every '\' as it is (Windows paths).
    internal static class ProbeFacts
    {
        /// esc's value: \n, \t, \\ decoded; false on any other escape or a lone '\' at the end
        public static bool Unescape(string v, out string s)
        {
            var b = new System.Text.StringBuilder(v.Length);
            s = null;
            for (int i = 0; i < v.Length; i++)
            {
                if (v[i] != '\\') { b.Append(v[i]); continue; }
                if (++i >= v.Length) return false;
                switch (v[i])
                {
                    case 'n': b.Append('\n'); break;
                    case 't': b.Append('\t'); break;
                    case '\\': b.Append('\\'); break;
                    default: return false;
                }
            }
            s = b.ToString();
            return true;
        }

        public static List<ProbeFact> Parse(Dictionary<string, string> conf, List<string> bad)
        {
            var r = new List<ProbeFact>();
            foreach (var kv in conf.OrderBy(k => k.Key, StringComparer.Ordinal))
            {
                if (!kv.Key.StartsWith("fact.", StringComparison.Ordinal)) continue;
                string name = kv.Key.Substring(5), v = kv.Value;
                int colon = v.IndexOf(':');
                string kind = colon < 0 ? "" : v.Substring(0, colon), val = colon < 0 ? "" : v.Substring(colon + 1);
                if (name.Length == 0 || name.Any(ch => !(ch >= 'A' && ch <= 'Z' || ch >= '0' && ch <= '9' || ch == '_')) || !(name[0] >= 'A' && name[0] <= 'Z')
                    || !Enum.TryParse("FACT_" + name, false, out RbStemsPlus.Core.Fact id) || !Enum.IsDefined(typeof(RbStemsPlus.Core.Fact), id))
                {
                    bad.Add(kv.Key + ": no such fact");
                    continue;
                }
                var f = new ProbeFact { Id = id };
                if (kind == "flag" && (val == "0" || val == "1")) { f.Kind = 'f'; f.N = val == "1" ? 1 : 0; }
                else if (kind == "i64" && long.TryParse(val, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long n)) { f.Kind = 'i'; f.N = n; }
                else if (kind == "str") { f.Kind = 's'; f.S = val; }
                else if (kind == "esc" && Unescape(val, out string u)) { f.Kind = 's'; f.S = u; }
                else { bad.Add(kv.Key + ": not flag:0|1, i64:N, str:TEXT or esc:TEXT (\\n \\t \\\\ only)"); continue; }
                // the core drops a write of the wrong kind without a word: refused here instead
                char want = RbStemsPlus.Core.FactKinds.Of(id);
                if (f.Kind != want) { bad.Add(kv.Key + ": facts.def says " + KindName(want) + ", not " + kind); continue; }
                r.Add(f);
            }
            return r;
        }

        private static string KindName(char k) => k == 'f' ? "flag" : k == 'i' ? "i64" : k == 's' ? "str" : "no kind";

        /// Each fact through the sheet's writers (core_fact_flag, _i64, _str), in the list's order.
        public static void Write(List<ProbeFact> facts, Action<int, int> flag, Action<int, long> i64, Action<int, string> str)
        {
            foreach (var f in facts)
            {
                if (f.Kind == 'f') flag((int)f.Id, (int)f.N);
                else if (f.Kind == 'i') i64((int)f.Id, f.N);
                else str((int)f.Id, f.S);
            }
        }
    }

    /// The records, the event counts and the driver's waits. One thread (the UI thread) calls it.
    internal sealed class ProbeRecorder
    {
        /// Probe.swift's perfFields: the field events perf mode keeps (the Windows shell has the
        /// Manage tab's, and the Prepare and Status tabs' where they are shown).
        public static readonly HashSet<string> PerfFields = new HashSet<string>
        {
            "c.plusIcon", "c.cacheIcon", "c.status", "c.spinner", "prepare.tracksTable", "prepare.info",
            "prepare.prepareBtn", "prepare.bar", "status.table", "prepare.outline",
        };
        /// perf-min's events (Probe.swift's event()): what the metrics need and nothing else
        private static readonly HashSet<string> PerfMinEvents = new HashSet<string> { "didBecomeActive", "reopen", "windowDrawn", "dialog", "dialogEnd", "tabShown" };
        private static readonly string[] Kinds = { "turn", "field", "tp", "mark", "cmd", "rowfetch", "ev", "sent" };
        public const byte KTurn = 0, KField = 1, KTp = 2, KMark = 3, KCmd = 4, KRowFetch = 5, KEv = 6, KSent = 7;

        private struct PerfEvent { public long T; public byte Kind; public int Name; public long N; }

        public readonly ProbeMode Mode;
        private readonly Func<long> now;
        private readonly Action<string> emit;
        private readonly List<PerfEvent> perf = new List<PerfEvent>(1 << 16);
        private readonly List<string> perfNames = new List<string>();
        private readonly Dictionary<string, int> perfIndex = new Dictionary<string, int>();
        private readonly Dictionary<string, int> counts = new Dictionary<string, int>();
        private Dictionary<string, string> last = new Dictionary<string, string>();
        private Dictionary<string, string> pending;   // trace mode: this turn's sample
        private readonly Dictionary<string, string> lastLook = new Dictionary<string, string>();
        private int dialogs, turn;
        public long BusyNs { get; set; }    // the probe's own time on the UI thread (the header's probe_busy_ns)
        public long WaitNs { get; set; }    // the driver's waits checked on it (wait_ns)
        public long LastChange;

        public ProbeRecorder(ProbeMode mode, Func<long> now, Action<string> emit)
        {
            Mode = mode;
            this.now = now;
            this.emit = emit;
        }

        public bool Trace => Mode == ProbeMode.Trace;
        public bool PerfLike => Mode == ProbeMode.Perf || Mode == ProbeMode.PerfMin;
        public int Count(string e) => counts.TryGetValue(e, out int n) ? n : 0;
        /// The last frame's value of a key (trace mode's waits).
        public string Last(string key) => last.TryGetValue(key, out string v) ? v : "";

        private void Counted(string e) => counts[e] = Count(e) + 1;

        private int Index(string name)
        {
            if (perfIndex.TryGetValue(name, out int i)) return i;
            i = perfNames.Count;
            perfNames.Add(name);
            perfIndex[name] = i;
            return i;
        }

        private void Add(byte kind, string name, long n = 0, long? t = null) =>
            perf.Add(new PerfEvent { T = t ?? now(), Kind = kind, Name = Index(name), N = n });

        /// The exe's build (ProbeArch.Exe: anycpu or x64), after "arch" in the start record and the
        /// tsv header.
        public string Exe = "";

        public void Start(int pid, long launchNs, string arch) =>
            emit("{\"k\":\"start\",\"t\":0,\"mode\":\"" + ProbeConf.Name(Mode) + "\",\"pid\":" + pid + ",\"launch_ns\":" + launchNs + ",\"arch\":\"" + arch + "\"" +
                 (Exe.Length > 0 ? ",\"exe\":\"" + Exe + "\"" : "") + ",\"shell\":\"wpf\"}");

        /// A timing point (Probe.swift's mark), t: when it happened (a point from another thread).
        public void Mark(string name, long? t = null)
        {
            Counted(name);
            if (Trace) emit("{\"k\":\"ev\",\"t\":" + (t ?? now()) + ",\"e\":\"tp\",\"v\":" + Json.Quote(name) + "}");
            else if (PerfLike) Add(KTp, name, 0, t);
        }

        public void Event(string e, string v = "")
        {
            Counted(e);
            if (Trace) emit("{\"k\":\"ev\",\"t\":" + now() + ",\"e\":" + Json.Quote(e) + ",\"v\":" + Json.Quote(v) + "}");
            else if (Mode == ProbeMode.Perf || (Mode == ProbeMode.PerfMin && PerfMinEvents.Contains(e))) Add(KEv, e);
        }

        public void Dialog(bool begin, string describe)
        {
            if (begin) dialogs++;
            string e = begin ? "dialog" : "dialogEnd";
            Counted(e);
            if (Trace) emit("{\"k\":\"ev\",\"t\":" + now() + ",\"e\":\"" + e + "\",\"n\":" + dialogs + ",\"v\":" + Json.Quote(describe) + ",\"origin\":\"event\"}");
            else if (PerfLike) Add(KEv, e, dialogs);
        }

        /// An event the shell sent to the core (Host.Send, the one place every event goes through:
        /// tab, select, folder, until, a button's click, activation, ...), the Windows probe's
        /// addition for the VM's echo-guard checks (an echo is an event sent the user didn't make).
        /// Trace: ev "sent" with the JSON as its value; perf: a "sent" row named by the JSON (made
        /// one line); perf-min: counted only. Counted as "sent" and "sent:<ev>" for the count verb.
        public void Sent(string json, long? t = null)
        {
            Counted("sent");
            Counted("sent:" + SentName(json));
            if (Trace) emit("{\"k\":\"ev\",\"t\":" + (t ?? now()) + ",\"e\":\"sent\",\"v\":" + Json.Quote(json) + "}");
            else if (Mode == ProbeMode.Perf) Add(KSent, OneLine(json), 0, t);
        }

        /// The event's name: its leading "ev" member as the shell writes it ({"ev":"<name>", ...}), else "?".
        public static string SentName(string json)
        {
            const string p = "{\"ev\":\"";
            if (json == null || !json.StartsWith(p, StringComparison.Ordinal)) return "?";
            int end = json.IndexOf('"', p.Length);
            return end <= p.Length ? "?" : json.Substring(p.Length, end - p.Length);
        }

        private static string OneLine(string s) => (s ?? "").Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');

        public void Command(string verb, string arg)
        {
            Counted("cmd");
            if (Trace) emit("{\"k\":\"ev\",\"t\":" + now() + ",\"e\":\"cmd\",\"v\":" + Json.Quote(verb + " " + arg) + "}");
            else if (PerfLike) Add(KCmd, verb + " " + arg);
        }

        /// A field of the short list set (perf mode): "<id>:<setter>". A light's look set to the
        /// one it has is no change on screen and isn't recorded (Probe.swift's setImage rule).
        public void Field(string id, string setter, string look = null, long n = 0)
        {
            if (!PerfLike) return;
            LastChange = now();
            if (look != null)
            {
                if (lastLook.TryGetValue(id, out string was) && was == look) return;
                lastLook[id] = look;
            }
            if (PerfFields.Contains(id)) Add(KField, id + ":" + setter, n);
        }

        /// Trace mode's sample (on a Rendering tick): what every field shows now, written as a frame
        /// when its turn ends (a later sample in the same turn replaces it).
        public void Sample(Dictionary<string, string> s)
        {
            if (Trace && s != null) pending = s;
        }

        /// A turn's end (ProbeTurns.Inactive): perf mode's turn record, or trace mode's frame (the
        /// keys that changed since the last frame, a key gone as null) when the turn was sampled.
        public void Turn(long busyNs)
        {
            turn++;
            long t = now();
            if (PerfLike) { Add(KTurn, "turn", busyNs, t); return; }
            if (!Trace || pending == null) return;
            var s = pending;
            pending = null;
            var keys = new SortedSet<string>(StringComparer.Ordinal);
            foreach (var kv in s) if (!last.TryGetValue(kv.Key, out string v) || v != kv.Value) keys.Add(kv.Key);
            foreach (var k in last.Keys) if (!s.ContainsKey(k)) keys.Add(k);
            if (keys.Count == 0) return;
            last = s;
            LastChange = t;
            var b = new StringBuilder();
            foreach (string k in keys)
            {
                if (b.Length > 0) b.Append(',');
                b.Append(Json.Quote(k)).Append(':').Append(s.TryGetValue(k, out string v) ? Json.Quote(v) : "null");
            }
            emit("{\"k\":\"frame\",\"t\":" + t + ",\"n\":" + turn + ",\"busy\":" + busyNs + ",\"d\":{" + b + "}}");
        }

        /// perf-<pid>.tsv's text (Probe.swift's flushPerf): null when there is nothing to write.
        public string PerfTsv(int pid, long launchNs, string arch)
        {
            if (!PerfLike || (perf.Count == 0 && BusyNs == 0)) return null;
            var s = new StringBuilder(perf.Count * 32 + 128);
            s.Append("# mode=").Append(ProbeConf.Name(Mode)).Append(" pid=").Append(pid).Append(" launch_ns=").Append(launchNs)
             .Append(" probe_busy_ns=").Append(BusyNs).Append(" wait_ns=").Append(WaitNs).Append(" arch=").Append(arch);
            if (Exe.Length > 0) s.Append(" exe=").Append(Exe);
            s.Append(" shell=wpf\n");
            foreach (var e in perf)
                s.Append(e.T).Append('\t').Append(Kinds[e.Kind]).Append('\t').Append(perfNames[e.Name]).Append('\t').Append(e.N).Append('\n');
            return s.ToString();
        }
    }

    /// The UI thread's turns (app.md §7, Probe.swift's afterWaiting..beforeWaiting): Probe.cs calls
    /// it from the dispatcher's hooks, on the UI thread only. A turn ends at Hooks.DispatcherInactive
    /// (raised outside any operation once the queue is empty: after the render operation's layout
    /// and render, so a frame's render lands in its own turn). Its busy time is the operations'
    /// exclusive time: a nested operation (a modal window's frame runs inside its caller's) counts
    /// on its own, and the caller's time leaves out the nested one's. A nested frame's turn ends
    /// the caller's time so far (a modal window's build and first layout land in its frame's first
    /// turn, as on the Mac); from it to the frame's next operation or turn, or to ModalEnd, is idle,
    /// never the caller's. A modal bracket with no turn inside (a picker's own loop): the whole
    /// bracket left out. OperationAborted is never used: it reports a pending operation, never one
    /// started. The probe's own time (trace mode's sample, the turn's record, the waits) is never
    /// the app's: it goes to the header's probe_busy_ns and wait_ns. Records made inside an
    /// operation (marks, events, sent, fields) are that operation's time: sub-µs, per event.
    internal sealed class ProbeTurns
    {
        // IdleFrom: a nested frame's idle span open since then (-1: none), not the operation's
        private sealed class Op { public long Start, Nested, IdleFrom = -1; }
        private sealed class Modal { public Op Op; public long T0, Saved; public bool Turned; }
        private readonly Func<long> now;
        private readonly ProbeRecorder rec;
        private readonly ProbeWaits waits;
        private readonly Stack<Op> ops = new Stack<Op>();
        private readonly Stack<Modal> modals = new Stack<Modal>();
        private long busy;   // the operations' exclusive time since the last turn

        public ProbeTurns(Func<long> now, ProbeRecorder rec, ProbeWaits waits)
        {
            this.now = now;
            this.rec = rec;
            this.waits = waits;
        }

        public int Depth => ops.Count;

        // the operation's idle span (since a nested frame's turn) closes: none of it was its own
        private static void Wake(Op op, long t)
        {
            if (op.IdleFrom < 0) return;
            op.Nested += t - op.IdleFrom;
            op.IdleFrom = -1;
        }

        /// Hooks.OperationStarted: work again under the operation up now (a nested frame's next
        /// operation ends its idle span)
        public void Started()
        {
            long t = now();
            if (ops.Count > 0) Wake(ops.Peek(), t);
            ops.Push(new Op { Start = t });
        }

        /// Hooks.OperationCompleted
        public void Completed()
        {
            if (ops.Count == 0) return;
            long t = now();
            var op = ops.Pop();
            Wake(op, t);
            long whole = t - op.Start;
            busy += Math.Max(0, whole - op.Nested);
            if (ops.Count > 0) ops.Peek().Nested += whole;
        }

        /// The presenter's window opens (Presenter.Pump, inside the operation that shows it). Its
        /// build and first layout are the operation's, counted in the window frame's first turn
        /// (Inactive); its idle spans aren't.
        public void ModalBegin()
        {
            var op = ops.Count > 0 ? ops.Peek() : null;
            modals.Push(new Modal { Op = op, T0 = now(), Saved = op?.Nested ?? 0 });
        }

        /// The presenter's window closed. A frame that turned: an idle span still open closes here (a
        /// close by input outside any operation, its teardown and the reply's read-back left out with
        /// it, the input gap); after a dispatcher operation that closed the window (the driver's
        /// answer), Started already ended the span, so teardown and the read-back are the operation's
        /// own, in the turn after the frame.
        /// One whose own loop raised no turn (a folder picker): the whole bracket left out, as
        /// nested time, so its idle is never busy.
        public void ModalEnd()
        {
            if (modals.Count == 0) return;
            var m = modals.Pop();
            if (m.Op == null) return;
            long t = now();
            if (m.Turned) Wake(m.Op, t);
            else m.Op.Nested = m.Saved + (t - m.T0);
        }

        // the probe's own time inside an operation (the render operation's sample): not that
        // operation's; inside an idle span it is already left out
        private void Exclude(long ns)
        {
            if (ops.Count > 0 && ops.Peek().IdleFrom < 0) ops.Peek().Nested += ns;
        }

        /// Trace mode's sample on a Rendering tick (inside the render operation, before its layout
        /// and render).
        public void Sample(Func<Dictionary<string, string>> snapshot)
        {
            long start = now();
            rec.Sample(snapshot());
            long d = now() - start;
            rec.BusyNs += d;
            Exclude(d);
        }

        /// Hooks.DispatcherInactive: the turn's end, its record (perf mode's turn, trace mode's
        /// frame), then the driver's waits (the harness's cost, counted apart, as on the Mac). With
        /// an operation up (a nested frame: a presenter window's, a picker's) the operation's own
        /// time so far is this turn's, as the Mac's first nested beforeWaiting counts from the
        /// outer afterWaiting; the record, the waits and the idle after them are not its own.
        public void Inactive()
        {
            long start = now();
            Op op = ops.Count > 0 ? ops.Peek() : null;
            if (op != null)
            {
                Wake(op, start);
                busy += Math.Max(0, start - op.Start - op.Nested);
                op.Nested = start - op.Start;
                foreach (var m in modals) if (m.Op == op) m.Turned = true;
            }
            long b = busy;
            busy = 0;
            rec.Turn(b);
            long mid = now();
            if (rec.Mode != ProbeMode.PerfMin) rec.BusyNs += mid - start;
            waits.Check();
            long end = now();
            rec.WaitNs += end - mid;
            if (op != null) op.IdleFrom = start;
        }
    }

    /// One driver command: "<id> <verb> <arg…>" (tests/harness/lib.sh).
    internal sealed class ProbeCommand
    {
        public string Id, Verb, Arg;

        public static ProbeCommand Parse(string line)
        {
            var p = (line ?? "").Trim().Split(new[] { ' ' }, 3, StringSplitOptions.RemoveEmptyEntries);
            if (p.Length < 2) return null;
            return new ProbeCommand { Id = p[0], Verb = p[1], Arg = p.Length > 2 ? p[2] : "" };
        }
    }

    /// The driver's waits (Probe.swift's addWait and checkWaits): checked at every turn's end, acked
    /// once. The shell runs a 20 ms timer while any is pending (Pending), as the Mac does: its turns
    /// check them while nothing else runs (minimised, the session locked).
    internal sealed class ProbeWaits
    {
        private sealed class W { public string Id; public long Deadline; public Func<bool> Test; }
        private readonly List<W> waits = new List<W>();
        private readonly Func<long> now;
        private readonly Action<string, string> ack;

        public ProbeWaits(Func<long> now, Action<string, string> ack)
        {
            this.now = now;
            this.ack = ack;
        }

        public int Pending => waits.Count;

        public void Add(string id, long ms, Func<bool> test)
        {
            if (test()) { ack(id, "ok"); return; }
            waits.Add(new W { Id = id, Deadline = now() + ms * 1000000, Test = test });
        }

        public void Check()
        {
            if (waits.Count == 0) return;
            long t = now();
            waits.RemoveAll(w =>
            {
                if (w.Test()) { ack(w.Id, "ok"); return true; }
                if (t > w.Deadline) { ack(w.Id, "timeout"); return true; }
                return false;
            });
        }

        /// "wait <ms> <key>=<value>" or "<key>!=<value>": the test over value(key), or null (usage).
        public static Func<bool> Condition(string arg, Func<string, string> value, out long ms)
        {
            ms = 0;
            var a = arg.Split(new[] { ' ' }, 2);
            if (a.Length != 2 || !long.TryParse(a[0], NumberStyles.None, CultureInfo.InvariantCulture, out ms)) return null;
            string cond = a[1];
            bool neg = cond.Contains("!=");
            string sep = neg ? "!=" : "=";
            int at = cond.IndexOf(sep, StringComparison.Ordinal);
            if (at <= 0) return null;
            string key = cond.Substring(0, at), want = cond.Substring(at + sep.Length);
            return () => neg ? value(key) != want : value(key) == want;
        }

        /// "<a> <b> [<c>]": whole numbers, or null.
        public static long[] Numbers(string arg, int n)
        {
            var a = arg.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            if (a.Length != n) return null;
            var r = new long[n];
            for (int i = 0; i < n; i++) if (!long.TryParse(a[i], NumberStyles.None, CultureInfo.InvariantCulture, out r[i])) return null;
            return r;
        }
    }

    /// The field ids (tests/harness/fields.tsv's, so the same scenarios read both shells): the
    /// main window's named elements by the Mac controller's property names, and the menu items by
    /// their title path.
    internal static class ProbeIds
    {
        /// MainWindow.xaml's x:Name → the Mac field id. NoticeLine has no Mac twin ("c.notice", a
        /// per-OS field); LogView is "c.text", excluded from the oracle (clock times) as on the Mac.
        public static readonly IReadOnlyList<KeyValuePair<string, string>> Window = new[]
        {
            Pair("StatusLine", "c.status"), Pair("Spinner", "c.spinner"),
            Pair("PlusLight", "c.plusIcon"), Pair("CacheLight", "c.cacheIcon"),
            Pair("PlusLabel", "c.plusLabel"), Pair("CacheLabel", "c.cacheLabel"),
            Pair("InstallPlus", "c.installPlusBtn"), Pair("ReinstallPlus", "c.reinstallPlusBtn"), Pair("UninstallPlus", "c.uninstallPlusBtn"),
            Pair("InstallCache", "c.installCacheBtn"), Pair("ReinstallCache", "c.reinstallCacheBtn"), Pair("UninstallCache", "c.uninstallCacheBtn"),
            Pair("SettingsButton", "c.settingsBtn"), Pair("NoticeLine", "c.notice"), Pair("LogView", "c.text"),
        };

        private static KeyValuePair<string, string> Pair(string a, string b) => new KeyValuePair<string, string>(a, b);

        /// The update offer's menu item (M_APP_UPDATE) is a field, as the Mac's controller.updateItem
        /// (CoreShell.swift), not under its title path; its .hid is the snapshot's F_UPDATE_VISIBLE.
        public const string UpdateItem = "c.updateItem";
        /// The app menu's segment (M_APP, titled with the app's name): "App", as Probe.swift's.
        public const string AppMenu = "App";

        /// Every field id the Windows shell registers besides its menus' title paths.
        public static IEnumerable<string> Mapped => Window.Select(kv => kv.Value).Concat(new[] { UpdateItem });

        public static bool Oracle(string id) => id != "c.text";

        /// tab.sel: the shown tab's shared id (core.h core_tab, the Mac's tab index: Status 0,
        /// Prepare 1, Manage 2) by its TabItem's x:Name, not its place among the tabs Windows has.
        public static int Tab(string name) => name == "StatusTab" ? 0 : name == "PrepareTab" ? 1 : name == "ManageTab" ? 2 : -1;

        /// A key's field: the key without its last ".prop" (a menu id has dots of its own:
        /// "menu.Manage.Stems Cache.Install.en" is menu.Manage.Stems Cache.Install's), or null.
        public static string FieldOf(string key)
        {
            int dot = key.LastIndexOf('.');
            return dot > 0 ? key.Substring(0, dot) : null;
        }

        /// A menu item's id: its parent's, then its title without the access key's '_' ("__" is a
        /// '_'), '.' as '_' (Probe.swift's registerMenu); a second item of the same id gets "#2"...
        public static string Menu(string parent, string title, ICollection<string> taken)
        {
            var t = new StringBuilder();
            for (int i = 0; i < title.Length; i++)
            {
                if (title[i] == '_') { if (i + 1 < title.Length && title[i + 1] == '_') { t.Append('_'); i++; } continue; }
                t.Append(title[i] == '.' ? '_' : title[i]);
            }
            string id = parent + "." + t, first = id;
            for (int n = 2; taken.Contains(id); n++) id = first + "#" + n;
            return id;
        }

        /// A colour as the Mac's normalised sRGB ("#rrggbb").
        public static string Rgb(byte r, byte g, byte b) => "#" + r.ToString("x2") + g.ToString("x2") + b.ToString("x2");
    }

    /// The start record's "arch": arm64, x64 or x64-prism (an x64 process on ARM64 Windows). The
    /// host's machine comes from IsWow64Process2's nativeMachine: under Prism, .NET Framework's
    /// RuntimeInformation.OSArchitecture answers the emulated X64 (seen in the W10 VM round), so it
    /// is only the fallback when IsWow64Process2 can't be asked (native 0).
    internal static class ProbeArch
    {
        public const ushort MachineArm64 = 0xAA64;

        /// process: RuntimeInformation.ProcessArchitecture (right under emulation too). native:
        /// IsWow64Process2's nativeMachine, or 0. os: RuntimeInformation.OSArchitecture (Prism's X64
        /// under emulation: the fallback only). An x86 or other process says so (stats.py refuses it).
        public static string Label(Architecture process, ushort native, Architecture os)
        {
            if (process == Architecture.Arm64) return "arm64";
            if (process == Architecture.X86) return "x86";
            if (process != Architecture.X64) return process.ToString().ToLowerInvariant();
            bool armHost = native != 0 ? native == MachineArm64 : os == Architecture.Arm64;
            return armHost ? "x64-prism" : "x64";
        }

        /// The exe's build from its PE kind (Module.GetPEKind): "anycpu" (IL only, i386, neither
        /// 32-bit flag), "x64" (PE32+ for AMD64: build.sh --x64's), else "other". An AnyCPU exe
        /// recorded as x64-prism ran emulated because its parent was x64 (W10's VM round): the
        /// runner starts it from a native parent, and stats.py refuses such a record.
        public static string Exe(PortableExecutableKinds k, ImageFileMachine m)
        {
            bool il = (k & PortableExecutableKinds.ILOnly) != 0, plus = (k & PortableExecutableKinds.PE32Plus) != 0;
            bool bits32 = (k & (PortableExecutableKinds.Required32Bit | PortableExecutableKinds.Preferred32Bit)) != 0;
            if (il && m == ImageFileMachine.I386 && !plus && !bits32) return "anycpu";
            if (plus && m == ImageFileMachine.AMD64) return "x64";
            return "other";
        }
    }
}
#endif
