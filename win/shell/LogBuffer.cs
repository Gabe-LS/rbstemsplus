// The app's log (host->log and the shell's own lines, one path: today's Controller.log): each line
// goes to the log file with the date ("yyyy-MM-dd HH:mm:ss  line", as appendLog writes it) and to
// the log pane with the time ("HH:mm:ss  line"). Any thread. The pane's lines wait here until the
// UI takes them in one go (one dispatcher operation per batch, so a flood of lines costs one
// update per turn, not one per line).
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace RbStemsPlus.Shell
{
    internal sealed class LogBuffer
    {
        private readonly object gate = new object();
        private readonly string support;
        private readonly Func<DateTime> now;
        private readonly bool coreReady;
        private List<string> pending = new List<string>();
        private bool flushPosted;
        private bool fileOff;   // StopFile: the pane only from then on

        /// support: the parent of Logs ("" or null: the pane only). now: the clock (a test's own).
        public LogBuffer(string support, Func<DateTime> now = null, bool coreReady = true)
        {
            this.support = string.IsNullOrEmpty(support) ? null : support;
            this.now = now ?? (() => DateTime.Now);
            this.coreReady = coreReady;
        }

        /// One line. True when the caller must post the pane's flush (none is pending yet).
        public bool Add(string line)
        {
            DateTime t = now();
            line = line ?? "";
            lock (gate)
            {
                if (support != null && !fileOff) Append(support, t, line, coreReady);
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
        public static void Append(string support, DateTime time, string line, bool coreReady = true)
        {
            try
            {
                var b = Encoding.UTF8.GetBytes(time.ToString("yyyy-MM-dd HH:mm:ss") + "  " + line + "\n");
                if (coreReady)
                    Core.core_win_log_append(Path.GetDirectoryName(support), Path.GetFileName(support), b, (UIntPtr)b.Length);
                else
                    AppendWithoutCore(support, b);
            }
            catch (Exception) { }
        }

        private static void AppendWithoutCore(string support, byte[] bytes)
        {
            if (!EnsureLogDirectory(support)) return;
            string logs = Path.Combine(support, "Logs");
            if (!EnsureLogDirectory(logs)) return;

            string path = Path.Combine(logs, "app.log");
            try
            {
                if ((File.GetAttributes(path) & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0) return;
            }
            catch (FileNotFoundException) { }   // Append can create app.log

            using (var f = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
            {
                // On Windows, count the links on the same handle that will write the line.
                if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows) &&
                    (GetFileInformationByHandle(f.SafeFileHandle, out FileInformation info) == 0 || info.NumberOfLinks != 1)) return;
                f.Write(bytes, 0, bytes.Length);
            }
        }

        private static bool EnsureLogDirectory(string path)
        {
            FileAttributes attrs;
            try { attrs = File.GetAttributes(path); }
            catch (Exception e) when (e is FileNotFoundException || e is DirectoryNotFoundException)
            {
                Directory.CreateDirectory(path);
                attrs = File.GetAttributes(path);
            }
            return (attrs & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == FileAttributes.Directory;
        }

        // BY_HANDLE_FILE_INFORMATION from fileapi.h.
        [StructLayout(LayoutKind.Sequential)]
        private struct FileInformation
        {
            public uint FileAttributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime, LastAccessTime, LastWriteTime;
            public uint VolumeSerialNumber, FileSizeHigh, FileSizeLow, NumberOfLinks, FileIndexHigh, FileIndexLow;
        }

        [DllImport("kernel32.dll", ExactSpelling = true)]
        private static extern int GetFileInformationByHandle(SafeFileHandle file, out FileInformation info);
    }
}
