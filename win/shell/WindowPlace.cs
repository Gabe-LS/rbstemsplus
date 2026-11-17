// The main window's last size and position (app.md §4.6): kept per user in
// %LOCALAPPDATA%\rbstemsplus\window, a convenience the app never depends on. One line of ASCII,
// "v1 <left> <top> <right> <bottom> <maximized 0|1>": the restored rectangle in physical pixels
// as GetWindowPlacement gives it (rcNormalPosition), and whether the window was maximized. A file
// that is missing, unreadable or not this form is ignored, and the start size is used. Fit decides
// whether a kept rectangle may be used on the monitors connected now, and fits it into that
// monitor's work area (MainWindow asks Windows for the monitor, its work area and its DPI). No WPF
// here (the Mac tests use it).
using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace RbStemsPlus.Shell
{
    /// Win32's RECT (physical pixels); also the type of WindowPlace.Fit's pure arithmetic.
    [StructLayout(LayoutKind.Sequential)]
    internal struct RECT
    {
        public int left, top, right, bottom;
        public int Width => right - left;
        public int Height => bottom - top;
        public override string ToString() => left + "," + top + "," + right + "," + bottom;
    }

    internal sealed class WindowPlace
    {
        public int Left, Top, Right, Bottom;
        public bool Maximized;

        // larger than any real screen: a corrupt file never makes a huge window
        private const int Max = 100000;

        public int Width => Right - Left;
        public int Height => Bottom - Top;

        /// The strip a kept rectangle is judged by: its title bar's, the top 32 pixels.
        public const int StripHeight = 32;
        /// At least this much of the strip (or all of it, for a narrower window) must be in the
        /// work area, so the window can be grabbed and moved there.
        public const int StripMinInside = 120;

        public RECT Rect => new RECT { left = Left, top = Top, right = Right, bottom = Bottom };

        public static RECT Strip(RECT r) => new RECT { left = r.left, top = r.top, right = r.right, bottom = Math.Min(r.bottom, r.top + StripHeight) };

        /// A kept rectangle (screen coordinates) fitted into the work area of the monitor its title
        /// strip is on (work, screen coordinates), or null when it can't be used there: the strip's
        /// top isn't inside the work area, or less than min(width, 120) pixels of the strip are. It
        /// is shrunk to the work area, never below minW x minH (the window's minimum at that
        /// monitor's DPI), then moved inside it. A maximized window's normal rectangle is fitted the
        /// same way, so un-maximizing it on a smaller screen stays on screen.
        public static RECT? Fit(RECT saved, RECT work, int minW, int minH)
        {
            if (saved.Width <= 0 || saved.Height <= 0 || work.Width <= 0 || work.Height <= 0) return null;
            if (saved.top < work.top || saved.top >= work.bottom) return null;
            int inside = Math.Min(saved.right, work.right) - Math.Max(saved.left, work.left);
            if (inside < Math.Min(saved.Width, StripMinInside)) return null;
            int w = Math.Max(Math.Min(saved.Width, work.Width), minW);
            int h = Math.Max(Math.Min(saved.Height, work.Height), minH);
            int left = saved.left, top = saved.top;
            if (left + w > work.right) left = work.right - w;
            if (left < work.left) left = work.left;
            if (top + h > work.bottom) top = work.bottom - h;
            if (top < work.top) top = work.top;
            return new RECT { left = left, top = top, right = left + w, bottom = top + h };
        }

        public static WindowPlace Parse(string text)
        {
            if (text == null) return null;
            string[] p = text.Trim().Split(' ');
            if (p.Length != 6 || p[0] != "v1") return null;
            var n = new int[5];
            for (int i = 0; i < 5; i++)
                if (!int.TryParse(p[i + 1], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out n[i])) return null;
            var w = new WindowPlace { Left = n[0], Top = n[1], Right = n[2], Bottom = n[3] };
            if (n[4] != 0 && n[4] != 1) return null;
            w.Maximized = n[4] == 1;
            if (w.Width <= 0 || w.Height <= 0 || w.Width > Max || w.Height > Max
                || Math.Abs((long)w.Left) > Max || Math.Abs((long)w.Top) > Max) return null;
            return w;
        }

        public string Format() => string.Format(CultureInfo.InvariantCulture, "v1 {0} {1} {2} {3} {4}\n",
            Left, Top, Right, Bottom, Maximized ? 1 : 0);

        /// null: no usable file.
        public static WindowPlace Load(string path)
        {
            try { return File.Exists(path) ? Parse(File.ReadAllText(path, Encoding.ASCII)) : null; }
            catch (Exception) { return null; }
        }

        /// Written next to itself, then moved over the old one. false: not written (never an error
        /// the user sees: the window opens at its start size next time).
        public bool Save(string path)
        {
            string tmp = path + ".tmp";
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllText(tmp, Format(), Encoding.ASCII);
                if (File.Exists(path)) File.Replace(tmp, path, null);
                else File.Move(tmp, path);
                return true;
            }
            catch (Exception)
            {
                try { File.Delete(tmp); } catch (Exception) { }
                return false;
            }
        }
    }
}
