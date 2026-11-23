using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using FlaUI.Core;
using FlaUI.Core.AutomationElements;
using FlaUI.Core.Definitions;
using FlaUI.Core.Identifiers;
using FlaUI.UIA3;

namespace RbStemsPlus.Rbui
{
    internal static partial class Program
    {
        private static int Events(UIA3Automation ua, AutomationElement desk, Opts o)
        {
            o.Only("--pid", "--seconds", "--ready");
            int pid = o.Int("--pid", 0), seconds = o.Int("--seconds", 30);
            string ready = o.Get("--ready");
            if (pid <= 0 || seconds < 1 || seconds > 120 || string.IsNullOrEmpty(ready) || o.Args.Count != 0)
                throw new UsageException("events --pid N --seconds 1..120 --ready FILE");

            var app = Windows(desk, new Target { Pid = pid }).FirstOrDefault(w => Ui.Type(w) == ControlType.Window);
            if (app == null) return NotFound;

            // UIA_LiveRegionChangedEventId, which FlaUI 5's event library doesn't list.
            var live = EventId.Register(AutomationType.UIA3, 20024, "LiveRegionChanged");
            var handler = app.RegisterAutomationEvent(live, TreeScope.Subtree,
                (element, _) => Say("LiveRegionChanged " + Ui.Describe(element) + " live=" +
                    Ui.Safe(() => element.Properties.LiveSetting.ValueOrDefault.ToString(), "unavailable")));
            try
            {
                var focus = ua.RegisterFocusChangedEvent(element =>
                {
                    if (Ui.Pid(element) == pid) Say("FocusChanged " + Ui.Describe(element));
                });
                try
                {
                    Say("events: armed for " + pid);
                    File.WriteAllText(ready, "armed");
                    Thread.Sleep(TimeSpan.FromSeconds(seconds));
                }
                finally { ua.UnregisterFocusChangedEvent(focus); }
            }
            finally { handler.Dispose(); }
            return Done;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NoticeIcon
        {
            public uint Size;
            public IntPtr Window;
            public uint Id;
            public Guid Item;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IconRect { public int Left, Top, Right, Bottom; }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string className, string title);

        [DllImport("shell32.dll")]
        private static extern int Shell_NotifyIconGetRect(ref NoticeIcon icon, out IconRect rect);

        private static bool HasNoticeIcon(Target owner)
        {
            var names = new Dictionary<int, string>();
            var messageOnly = new IntPtr(-3); // HWND_MESSAGE
            IntPtr window = IntPtr.Zero;
            while ((window = FindWindowExW(messageOnly, window, "RBSP.Core.Notify", null)) != IntPtr.Zero)
            {
                int pid = WindowPid(window);
                if (pid == 0 || (owner.Pid != 0 && owner.Pid != pid) ||
                    (owner.Process != null && !owner.Process.Equals(Ui.ProcessName(pid, names), StringComparison.OrdinalIgnoreCase)))
                    continue;
                // notify_win.c's NT_ID. Check the exact icon even if Explorer puts it in overflow.
                var icon = new NoticeIcon { Size = (uint)Marshal.SizeOf(typeof(NoticeIcon)), Window = window, Id = 1, Item = Guid.Empty };
                if (Shell_NotifyIconGetRect(ref icon, out _) == 0) return true;
            }
            return false;
        }

        // Windows 11 can render NIF_INFO as a banner in a different process from Explorer.
        // Use UIA for the banner and the actual icon identity for the tray lifetime.
        private static int Notice(AutomationElement desk, Opts o)
        {
            o.Only("--timeout", "--ready", "--pid", "--process");
            var owner = Target.From(o);
            int seconds = o.Int("--timeout", 45);
            if (!owner.Any || o.Args.Count != 1 || seconds < 1 || seconds > 120)
                throw new UsageException("notice TITLE (--pid N | --process NAME) [--timeout 1..120] [--ready FILE]");
            string title = o.Args[0], ready = o.Get("--ready");
            bool seenIcon = false, banner = false, icon = false;
            int appearances = 0;
            var clock = Stopwatch.StartNew();
            TimeSpan? quietSince = null;
            if (ready != null) File.WriteAllText(ready, "armed");
            do
            {
                var names = new Dictionary<int, string>();
                var roots = Ui.Children(desk).Where(w =>
                {
                    string process = Ui.ProcessName(Ui.Pid(w), names);
                    return process.Equals("explorer", StringComparison.OrdinalIgnoreCase) ||
                        process.Equals("ShellExperienceHost", StringComparison.OrdinalIgnoreCase) ||
                        process.Equals("ShellHost", StringComparison.OrdinalIgnoreCase);
                });

                bool nextBanner = roots.Where(root =>
                {
                    string cls = Ui.Class(root);
                    return cls != "Shell_TrayWnd" && cls != "Shell_SecondaryTrayWnd" && cls != "NotifyIconOverflowWindow" &&
                        cls != "TopLevelWindowForOverflowXamlIsland";
                }).SelectMany(root => new[] { root }.Concat(Ui.Descendants(root)))
                    .Any(element => Ui.Name(element) == title && !Ui.Safe(() => element.IsOffscreen, true));
                bool nextIcon = HasNoticeIcon(owner);

                if (nextBanner != banner)
                {
                    Say("notice: banner " + (nextBanner ? "shown " : "gone ") + title);
                    if (nextBanner && ++appearances > 1) return Unexpected;
                }
                if (nextIcon != icon) Say("notice: tray " + (nextIcon ? "shown " : "gone ") + title);
                seenIcon |= nextIcon;
                banner = nextBanner;
                icon = nextIcon;
                if (appearances == 1 && seenIcon && !banner && !icon)
                {
                    if (!quietSince.HasValue) quietSince = clock.Elapsed;
                    if (clock.Elapsed - quietSince.Value >= TimeSpan.FromSeconds(3)) return Done;
                }
                else quietSince = null;
                Thread.Sleep(125);
            } while (clock.Elapsed.TotalSeconds < seconds);

            Say($"notice: {title}, banner appearances={appearances} tray={seenIcon}, still visible banner={banner} tray={icon}");
            return NotFound;
        }
    }
}
