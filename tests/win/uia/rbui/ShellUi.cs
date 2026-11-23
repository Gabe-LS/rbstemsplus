// The shell-ui suite drives only ordinary windows on the signed-in user's desktop.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using FlaUI.Core.AutomationElements;
using FlaUI.Core.Definitions;
using FlaUI.Core.Input;
using FlaUI.Core.WindowsAPI;
using FlaUI.UIA3;

namespace RbStemsPlus.Rbui
{
    internal static partial class Program
    {
        private static AutomationElement SettingsWindow(AutomationElement desk)
        {
            var names = new Dictionary<int, string>();
            return Windows(desk, new Target { Process = "SystemSettings", Title = "Settings" }).FirstOrDefault()
                ?? Windows(desk, new Target { Process = "ApplicationFrameHost", Title = "Settings" })
                .FirstOrDefault(w => Ui.Name(w) == "Settings" && Ui.Descendants(w)
                    .Any(e => Ui.ProcessName(Ui.Pid(e), names) == "SystemSettings"));
        }

        private static AutomationElement AppRowOptions(AutomationElement settings, string name)
        {
            var walker = settings.Automation.TreeWalkerFactory.GetControlViewWalker();
            foreach (var app in Ui.Descendants(settings).Where(e => Ui.Name(e) == name))
            {
                // Stay in the result's smallest containing row. Settings attaches the button
                // to that row, and app names can also occur outside the Installed apps list.
                for (var row = app; row != null && !row.Equals(settings); row = walker.GetParent(row))
                {
                    var contents = Ui.Descendants(row);
                    var options = contents.Where(e => Ui.Type(e) == ControlType.Button && Ui.Enabled(e)
                        && (Ui.Name(e) == "More options" || Ui.Name(e) == "More options for " + name)).ToArray();
                    if (options.Length > 1) break;
                    if (options.Length == 1) return options[0];
                }
            }
            return null;
        }

        private static int SettingsUninstall(AutomationElement desk, Opts o)
        {
            o.Only("--timeout");
            if (o.Args.Count != 1) throw new UsageException("settings-uninstall NAME [--timeout S]");
            string name = o.Args[0];
            int secs = o.Int("--timeout", 60);
            var clock = Stopwatch.StartNew();
            int Left() => Math.Max(0, secs - (int)clock.Elapsed.TotalSeconds);
            T Find<T>(Func<T> f) where T : class => Ui.Poll(Left(), f);
            RequireUserDesktop();
            Process.Start(new ProcessStartInfo("ms-settings:appsfeatures") { UseShellExecute = true })?.Dispose();
            var search = Find(() =>
            {
                var w = SettingsWindow(desk);
                // Windows 11 26H2's Installed apps names its filter box "Search", by this id
                return w == null ? null : Ui.Descendants(w).FirstOrDefault(e => Ui.Type(e) == ControlType.Edit
                    && (Ui.Name(e) == "Search apps" || Ui.Id(e) == "AppsSearchBox"
                        || Ui.Id(e) == "SystemSettings_StorageSense_AppSizesListFilter_DisplayStringValue"));
            });
            if (search == null) return ShellMissing(desk, "Settings search box", SettingsWindow(desk));
            RequireUserDesktop();
            search.Patterns.Value.Pattern.SetValue(name);
            var options = Find(() =>
            {
                var w = SettingsWindow(desk);
                return w == null ? null : AppRowOptions(w, name);
            });
            if (options == null) return ShellMissing(desk, "More options for " + name, SettingsWindow(desk));
            ShellPress(options);
            var settings = SettingsWindow(desk);
            if (settings == null) return ShellMissing(desk, "Settings window", null);
            // A flyout can be a separate top-level window of the control's provider.
            var provider = new Target { Pid = Ui.Pid(options) };
            AutomationElement Uninstall(ControlType type) => Windows(desk, provider)
                .Concat(new[] { SettingsWindow(desk) }).Where(w => w != null)
                .SelectMany(Ui.Descendants).FirstOrDefault(e => Ui.Matches(e, "Uninstall", type) && Ui.Enabled(e));
            var item = Find(() => Uninstall(ControlType.MenuItem));
            if (item == null) return ShellMissing(desk, "Settings Uninstall menu item", settings);
            ShellPress(item);
            var button = Find(() => Uninstall(ControlType.Button));
            if (button == null) return ShellMissing(desk, "Settings Uninstall confirmation button", settings);
            ShellPress(button);
            return Done;
        }

        private static int ShellMissing(AutomationElement desk, string what, AutomationElement window)
        {
            Say("shell-ui: no " + what);
            if (window != null) Dump(window, 0, 16);
            else Diagnose(desk, new Target());
            return NotFound;
        }

        private static void ShellPress(AutomationElement element)
        {
            RequireUserDesktop();
            string what = Ui.Describe(element);
            Say("shell-ui: " + what + ": " + Ui.Press(element));
        }

        private static IntPtr TaskbarHandle() => FindWindowW("Shell_TrayWnd", null);

        private static AutomationElement TaskbarWindow(AutomationElement desk)
        {
            var handle = TaskbarHandle();
            return handle == IntPtr.Zero ? null : desk.Automation.FromHandle(handle);
        }

        private static AutomationElement[] TaskbarButtons(AutomationElement bar, string name) =>
            bar == null ? Array.Empty<AutomationElement>() : Ui.Descendants(bar).Where(e =>
                (Ui.Type(e) == ControlType.Button || Ui.Type(e) == ControlType.ListItem) &&
                TaskbarName(Ui.Name(e), name)).ToArray();

        // Windows adds the running-window count to the button's accessible name on some builds,
        // and 26H2 adds " pinned" to a pinned button's ("NAME pinned", "NAME - 1 running window pinned").
        private static bool TaskbarName(string actual, string name) =>
            actual == name || Regex.IsMatch(actual, "^" + Regex.Escape(name) + @"( pinned)?( [-–] [0-9]+ (running )?windows?)?( pinned)?$",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

        private static int Taskbar(AutomationElement desk, Opts o)
        {
            o.Only("--timeout");
            if (o.Args.Count != 2) throw new UsageException("taskbar pin|unpin|click|count NAME");
            string action = o.Args[0], name = o.Args[1];
            if (action != "pin" && action != "unpin" && action != "click" && action != "count")
                throw new UsageException("taskbar pin|unpin|click|count NAME");
            RequireUserDesktop();
            int secs = o.Int("--timeout", 60);
            if (action == "count")
            {
                var bar = Ui.Poll(secs, () => TaskbarWindow(desk));
                if (bar == null) return ShellMissing(desk, "Explorer taskbar", null);
                int count = TaskbarButtons(bar, name).Length;
                Say("taskbar: count=" + count + " name=" + name);
                if (count != 1)
                    foreach (var b in Ui.Descendants(bar).Where(e => Ui.Type(e) == ControlType.Button || Ui.Type(e) == ControlType.ListItem))
                        Say("taskbar: button \"" + Ui.Name(b) + "\"");
                return Done;
            }
            var button = Ui.Poll(secs, () => TaskbarButtons(TaskbarWindow(desk), name).SingleOrDefault(), 25);
            if (button == null) return ShellMissing(desk, "unique taskbar button for " + name, TaskbarWindow(desk));
            if (action == "click")
            {
                // a real click: on 26H2 a pinned button's default action launches nothing
                RequireUserDesktop();
                Mouse.Click(TaskbarPoint(button));
                Say("shell-ui: " + Ui.Describe(button) + ": clicked");
                return Done;
            }
            Mouse.RightClick(TaskbarPoint(button));
            string label = action == "pin" ? "Pin to taskbar" : "Unpin from taskbar";
            // Windows 11 26H2 shows the taskbar's jump list in ShellExperienceHost ("Jump List
            // for NAME"), earlier builds in explorer
            var menu = Ui.Poll(secs, () => Windows(desk, new Target { Process = "explorer" })
                .Concat(Windows(desk, new Target { Process = "ShellExperienceHost" }))
                .SelectMany(Ui.Descendants)
                .FirstOrDefault(e => Ui.Name(e) == label && Ui.Enabled(e) &&
                    (Ui.Type(e) == ControlType.MenuItem || Ui.Type(e) == ControlType.Button ||
                     Ui.Type(e) == ControlType.ListItem)));
            if (menu == null)
            {
                Diagnose(desk, new Target { Process = "explorer" });
                Say("taskbar: no " + label);
                return NotFound;
            }
            ShellPress(menu);
            return Done;
        }

        private static int ZOrder(AutomationElement desk, Opts o)
        {
            o.Only("--above-settings");
            if (o.Args.Count != 1 || !int.TryParse(o.Args[0], out int pid) || pid <= 0)
                throw new UsageException("zorder PID [--above-settings MESSAGE]");
            var visible = VisibleWindows();
            var foreground = GetForegroundWindow();
            var settings = SettingsWindow(desk);
            int settingsIndex = settings == null ? -1 : visible.IndexOf(Handle(settings));
            var owned = visible.Where(h => WindowPid(h) == pid).ToArray();
            foreach (var h in owned)
                Say($"zorder: hwnd={h} index={visible.IndexOf(h)} class={WindowClass(h)} topmost={Topmost(h)} foreground={h == foreground}");
            Say("zorder: settings index=" + settingsIndex);
            string expected = o.Get("--above-settings");
            if (expected == null) return owned.Length > 0 ? Done : NotFound;
            if (owned.Length != 1 || WindowClass(owned[0]) != "#32770")
            {
                Say("zorder: expected exactly one visible MessageBox window from the after-exit process");
                return Unexpected;
            }
            var target = owned[0];
            var dialog = Windows(desk, new Target { Pid = pid }).SingleOrDefault(w => Handle(w) == target);
            string[] texts = dialog == null ? Array.Empty<string>() : Ui.Descendants(dialog)
                .Where(e => Ui.Type(e) == ControlType.Text).Select(Ui.Name).Where(s => !string.IsNullOrWhiteSpace(s)).ToArray();
            Say("zorder: dialog text: " + string.Join(" | ", texts));
            bool ok = settingsIndex > visible.IndexOf(target) && Topmost(target) && foreground == target &&
                texts.Length == 1 && NormalizeMessage(texts[0]) == NormalizeMessage(expected);
            if (!ok) Say("zorder: expected the exact kept message, topmost and foreground, above Settings");
            return ok ? Done : Unexpected;
        }

        // UIA may strip the ampersand mnemonic from a Win32 static control.
        private static string NormalizeMessage(string s) => Regex.Replace(s.Replace("&", ""), @"\s+", " ").Trim();

        private static int OnClose(UIA3Automation ua, AutomationElement desk, Opts o)
        {
            o.Only("--hwnd", "--confirm", "--delay-ms", "--timeout");
            if (o.Args.Count != 5 || !int.TryParse(o.Args[0], out int pid) || pid <= 0 ||
                o.Args[1] != "then" || o.Args[2] != "taskbar" || o.Args[3] != "click" ||
                !long.TryParse(o.Get("--hwnd"), out long hwnd) || o.Get("--confirm") == null)
                throw new UsageException("on-close PID then taskbar click NAME --hwnd N --confirm BUTTON --delay-ms N");
            int delay = o.Int("--delay-ms", 0), secs = o.Int("--timeout", 60);
            if (delay > 2000) throw new UsageException("--delay-ms must be between 0 and 2000");
            var handle = new IntPtr(hwnd);
            if (WindowPid(handle) != pid || !IsWindowVisible(handle))
                throw new InvalidOperationException("the specified main window is no longer visible");
            string name = o.Args[4];
            var app = ua.FromHandle(handle);
            var confirm = Ui.Poll(Math.Min(secs, 15), () => Windows(desk, new Target { Pid = pid })
                .SelectMany(Ui.Descendants).FirstOrDefault(e => Ui.Matches(e, o.Get("--confirm"), ControlType.Button) && Ui.Enabled(e)));
            if (confirm == null) return ShellMissing(desk, "update confirmation", app);
            if (TaskbarButtons(TaskbarWindow(desk), name).Length != 1)
                return ShellMissing(desk, "unique taskbar button before the swap", TaskbarWindow(desk));

            long closedAt = 0;
            var handler = app.RegisterAutomationEvent(ua.EventLibrary.Window.WindowClosedEvent, TreeScope.Element,
                (element, eventId) => Interlocked.CompareExchange(ref closedAt, Stopwatch.GetTimestamp(), 0));
            try
            {
                Say("on-close: armed for the old main window " + handle);
                ShellPress(confirm);
                var closed = Ui.Poll(secs, () => Interlocked.Read(ref closedAt) != 0 ? "closed" : null, 5);
                if (closed == null)
                {
                    Say("on-close: no WindowClosed event for " + handle);
                    return NotFound;
                }
                long since = Interlocked.Read(ref closedAt);
                double ElapsedMs() => (Stopwatch.GetTimestamp() - since) * (1000.0 / Stopwatch.Frequency);
                int remaining = (int)Math.Ceiling(delay - ElapsedMs());
                if (remaining > 0) Thread.Sleep(remaining);

                // Resolve the button again: Explorer can replace the running group with the
                // pinned launcher as the window closes. Never click an unrelated group.
                var button = Ui.Poll(2, () => TaskbarButtons(TaskbarWindow(desk), name).SingleOrDefault(), 15);
                if (button == null) return ShellMissing(desk, "unique pin after WindowClosed", TaskbarWindow(desk));
                // Shift-click asks Explorer to launch from the pin even if the new window
                // has already joined the group (an ordinary click there just activates it).
                var point = TaskbarPoint(button);
                double actual = ElapsedMs();
                Say($"on-close: click at +{actual:F0} ms (requested +{delay} ms)");
                if (actual > delay + 250)
                {
                    Say("on-close: missed the requested click by more than 250 ms");
                    return Unexpected;
                }
                using (Keyboard.Pressing(VirtualKeyShort.SHIFT)) Mouse.Click(point);
                // Only visible main windows count. --swap intentionally renders one off-screen
                // window, which has a tool-window style and no taskbar presence.
                var observe = Stopwatch.StartNew();
                while (observe.Elapsed.TotalSeconds < 15)
                {
                    RequireUserDesktop();
                    if (VisibleWindows().Count(h => IsAppMainWindow(h, name)) > 1)
                    {
                        Say("on-close: two visible app main windows");
                        return Unexpected;
                    }
                    Thread.Sleep(25);
                }
                return Done;
            }
            finally { handler.Dispose(); }
        }

        private static Point TaskbarPoint(AutomationElement button)
        {
            RequireUserDesktop();
            var point = button.GetClickablePoint();
            var bar = TaskbarHandle();
            if (bar == IntPtr.Zero || GetAncestor(WindowFromPoint(point), 2) != bar) // GA_ROOT
                throw new InvalidOperationException("the taskbar button is covered or moved");
            return point;
        }

        private static bool IsAppMainWindow(IntPtr h, string name)
        {
            if (GetWindow(h, 4) != IntPtr.Zero || !WindowClass(h).StartsWith("HwndWrapper[", StringComparison.Ordinal) ||
                (GetWindowLongW(h, -20) & 0x80) != 0) return false; // GW_OWNER, WS_EX_TOOLWINDOW
            var title = new StringBuilder(512);
            GetWindowTextW(h, title, title.Capacity);
            return title.ToString().StartsWith(name, StringComparison.Ordinal) &&
                Ui.ProcessName(WindowPid(h), new Dictionary<int, string>()) == name;
        }

        private static void RequireUserDesktop()
        {
            IntPtr desktop = OpenInputDesktop(0, false, 1); // DESKTOP_READOBJECTS
            try
            {
                var name = new StringBuilder(64);
                if (desktop == IntPtr.Zero ||
                    !GetUserObjectInformationW(desktop, 2, name, name.Capacity * sizeof(char), out _) ||
                    !string.Equals(name.ToString(), "Default", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("the ordinary input desktop is unavailable; check Windows for an unanswered security prompt");
            }
            finally { if (desktop != IntPtr.Zero) CloseDesktop(desktop); }
        }

        private static IntPtr Handle(AutomationElement e) => e.Properties.NativeWindowHandle.ValueOrDefault;
        private static int WindowPid(IntPtr h) { GetWindowThreadProcessId(h, out int pid); return pid; }
        private static string WindowClass(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, s.Capacity); return s.ToString(); }
        private static bool Topmost(IntPtr h) => (GetWindowLongW(h, -20) & 8) != 0; // WS_EX_TOPMOST

        private static List<IntPtr> VisibleWindows()
        {
            var windows = new List<IntPtr>();
            EnumWindows((h, _) => { if (IsWindowVisible(h)) windows.Add(h); return true; }, IntPtr.Zero);
            return windows;
        }

        private delegate bool EnumWindow(IntPtr h, IntPtr unused);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindow callback, IntPtr unused);
        [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowW(string className, string windowName);
        [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr h, uint flags);
        [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
        [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr h, uint command);
        [DllImport("user32.dll")] private static extern int GetWindowLongW(IntPtr h, int index);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(IntPtr h, StringBuilder text, int capacity);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowTextW(IntPtr h, StringBuilder text, int capacity);
        [DllImport("user32.dll")] private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetUserObjectInformationW(IntPtr h, int index, StringBuilder name, int bytes, out int needed);
        [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
    }
}
