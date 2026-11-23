// The UI thread's watch (TEST builds: TestHooks.StartUiWatch runs it; WPF-free, so the Mac tests
// run it against a clock of their own). On the owner's PC (2026-11-20, RB-KIT's Prepare after an
// Exit and a restart) the window turned all white and the Prepare tab's playlists never came, with
// nothing in the log after "state:". Whether the UI thread was blocked (and in what), or WPF
// stopped presenting with the UI thread free, the log can't tell; this watch says it:
//  - Host marks each core call the UI thread makes (Enter/Leave: an event with its JSON, the
//    drain of the main queue with its renders, the facts, the start), and keeps the last ones;
//  - a background thread asks the UI thread for a beat (Ask, then Beat from a Send-priority
//    operation); one not answered for StallMs is a stall: Check reports, once per stall, the core
//    call the UI thread is inside (none: WPF's own work or a wait outside the core) and the calls
//    before it; the next beat ends the stall ("answered again after").
// The report's caller (TestHooks) adds the UI thread's managed stack and a minidump.
using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace RbStemsPlus.Shell
{
    internal sealed class UiWatch
    {
        public const int StallMs = 5000;
        private const int Depth = 4, Keep = 16;

        private readonly object gate = new object();
        private readonly Func<long> nowMs;
        private readonly string[] inside = new string[Depth];
        private readonly long[] since = new long[Depth];
        private int depth;
        private readonly string[] lastOp = new string[Keep];
        private readonly long[] lastAt = new long[Keep], lastMs = new long[Keep];
        private int lastNext, lastCount;
        private bool asked, reported;
        private long askedAt, answeredAfter = -1;

        public UiWatch(Func<long> nowMs = null)
        {
            this.nowMs = nowMs ?? (() => SwapGuard.TimestampMs(Stopwatch.GetTimestamp()));
        }

        /// UI thread: a core call starts (nested calls kept up to Depth deep).
        public void Enter(string op)
        {
            long now = nowMs();
            lock (gate)
            {
                if (depth < Depth) { inside[depth] = op; since[depth] = now; }
                depth++;
            }
        }

        /// UI thread: the call Enter started returned.
        public void Leave()
        {
            long now = nowMs();
            lock (gate)
            {
                if (depth == 0) return;
                depth--;
                if (depth >= Depth) return;
                lastOp[lastNext] = inside[depth];
                lastAt[lastNext] = since[depth];
                lastMs[lastNext] = now - since[depth];
                lastNext = (lastNext + 1) % Keep;
                if (lastCount < Keep) lastCount++;
                inside[depth] = null;
            }
        }

        /// The watch's thread: true when a beat must be posted to the UI thread now (none is out).
        public bool Ask()
        {
            long now = nowMs();
            lock (gate)
            {
                if (asked) return false;
                asked = true;
                askedAt = now;
                return true;
            }
        }

        /// UI thread: the beat posted after Ask ran.
        public void Beat()
        {
            long now = nowMs();
            lock (gate)
            {
                if (!asked) return;
                asked = false;
                if (reported)
                {
                    reported = false;
                    answeredAfter = now - askedAt;
                }
            }
        }

        /// The watch's thread: the line to log now, or null. stalled: true for a new stall's report
        /// (the caller adds the stack and the dump then).
        public string Check(out bool stalled)
        {
            stalled = false;
            long now = nowMs();
            lock (gate)
            {
                if (answeredAfter >= 0)
                {
                    string l = "ui-watch: the UI thread answered again after " + Sec(answeredAfter);
                    answeredAfter = -1;
                    return l;
                }
                if (!asked || reported || now - askedAt < StallMs) return null;
                reported = true;
                stalled = true;
                var sb = new StringBuilder("ui-watch: the UI thread hasn't answered for " + Sec(now - askedAt) + "; ");
                if (depth == 0) sb.Append("inside no core call");
                else
                {
                    sb.Append("inside ");
                    for (int i = 0; i < Math.Min(depth, Depth); i++)
                        sb.Append(i > 0 ? " > " : "").Append(inside[i]).Append(" (").Append(Sec(now - since[i])).Append(')');
                    if (depth > Depth) sb.Append(" > ...");
                }
                sb.Append("; the calls before it, newest first:");
                for (int k = 0; k < lastCount; k++)
                {
                    int i = (lastNext - 1 - k + Keep) % Keep;
                    sb.Append(' ').Append(lastOp[i]).Append(' ').Append(lastMs[i].ToString(CultureInfo.InvariantCulture))
                      .Append(" ms at -").Append(Sec(now - lastAt[i])).Append(k + 1 < lastCount ? ";" : "");
                }
                if (lastCount == 0) sb.Append(" none");
                return sb.ToString();
            }
        }

        private static string Sec(long ms) => (ms / 1000.0).ToString("0.0", CultureInfo.InvariantCulture) + " s";

        /// An event as the watch names it: its JSON, cut after 80 characters.
        public static string EventOp(string json)
        {
            if (json == null) return "event";
            return "event " + (json.Length <= 80 ? json : json.Substring(0, 80) + "...");
        }
    }
}
