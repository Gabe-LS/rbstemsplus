// The app's log (host->log and the shell's own lines, one path: today's Controller.log): each line
// goes to the log file with the date ("yyyy-MM-dd HH:mm:ss  line", as appendLog writes it) and to
// the log pane with the time ("HH:mm:ss  line"). Any thread. The pane's lines wait here until the
// UI takes them in one go (one dispatcher operation per batch, so a flood of lines costs one
// update per turn, not one per line).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RbStemsPlus.Shell
{
    internal sealed class LogBuffer
    {
        private readonly object gate = new object();
        private readonly string path;
        private readonly Func<DateTime> now;
        private List<string> pending = new List<string>();
        private bool flushPosted;
        private bool fileOff;   // StopFile: the pane only from then on

        /// path: the log file ("" or null: the pane only). now: the clock (a test's own).
        public LogBuffer(string path, Func<DateTime> now = null)
        {
            this.path = string.IsNullOrEmpty(path) ? null : path;
            this.now = now ?? (() => DateTime.Now);
        }

        /// One line. True when the caller must post the pane's flush (none is pending yet).
        public bool Add(string line)
        {
            DateTime t = now();
            line = line ?? "";
            lock (gate)
            {
                if (path != null && !fileOff) Append(t.ToString("yyyy-MM-dd HH:mm:ss") + "  " + line + "\n");
                pending.Add(t.ToString("HH:mm:ss") + "  " + line);
                if (flushPosted) return false;
                flushPosted = true;
                return true;
            }
        }

        /// No more file lines from now on, for good (the complete uninstall's quit: its folder is gone,
        /// and a line must not make Logs\app.log again); the pane keeps them. Any thread.
        public void StopFile()
        {
            lock (gate) fileOff = true;
        }

        /// The UI thread's flush: the lines since the last one, as one text ("" none).
        public string Take()
        {
            List<string> lines;
            lock (gate)
            {
                lines = pending;
                pending = new List<string>();
                flushPosted = false;
            }
            if (lines.Count == 0) return "";
            var sb = new StringBuilder();
            foreach (var l in lines) sb.Append(l).Append('\n');
            return sb.ToString();
        }

        // never throws: a log that can't be written loses the file line, the pane keeps it
        private void Append(string text)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                using (var f = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
                {
                    var b = Encoding.UTF8.GetBytes(text);
                    f.Write(b, 0, b.Length);
                }
            }
            catch (Exception) { }
        }
    }
}
