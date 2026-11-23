// Physical PT8 input on the TEST app's Prepare outline and DatePicker.
using System;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using FlaUI.Core.AutomationElements;
using FlaUI.Core.Definitions;
using FlaUI.Core.Input;
using FlaUI.Core.WindowsAPI;

namespace RbStemsPlus.Rbui
{
    internal static partial class Program
    {
        private static int PrepareInput(AutomationElement desk, Opts o)
        {
            RequireUserDesktop();
            o.Only("--pid", "--timeout");
            int pid = o.Int("--pid", 0);
            if (pid == 0 || o.Args.Count != 2)
                throw new UsageException("prepare-input click|double-click|expander|up|enter|calendar|tab|escape ELEMENT --pid N");
            string action = o.Args[0], name = o.Args[1];
            var control = action == "calendar" ? ControlType.Button :
                action == "tab" || action == "escape" ? ControlType.Tab : ControlType.TreeItem;
            var target = new Target { Pid = pid, Process = "RB Stems Plus" };
            var match = Ui.Poll(o.Int("--timeout", 10), () =>
                (from w in Windows(desk, target)
                 from e in Ui.Descendants(w)
                 where Ui.Matches(e, name, control)
                    && Ui.Enabled(e) && Ui.Pid(e) == pid && !e.Properties.IsOffscreen.ValueOrDefault
                 select new { Window = w, Element = e }).FirstOrDefault());
            if (match == null) return NotFound;

            IntPtr hwnd = Handle(match.Window);
            if (hwnd == IntPtr.Zero || hwnd != GetForegroundWindow())
                throw new InvalidOperationException("Prepare input needs the foreground app window");
            if (action == "tab" || action == "escape")
            {
                using (Keyboard.Pressing(action == "tab" ? VirtualKeyShort.TAB : VirtualKeyShort.ESCAPE)) { }
            }
            else if (action == "up" || action == "enter")
            {
                if (!match.Element.Properties.HasKeyboardFocus.ValueOrDefault)
                    throw new InvalidOperationException("the requested Prepare row does not have keyboard focus");
                using (Keyboard.Pressing(action == "up" ? VirtualKeyShort.UP : VirtualKeyShort.RETURN)) { }
            }
            else
            {
                var rect = match.Element.Properties.BoundingRectangle.Value;
                double dpi = GetDpiForWindow(hwnd) / 96.0;
                // A TreeItem's bounds also contain its children. Use its own first line.
                Point pt = action == "calendar" ? match.Element.GetClickablePoint() :
                    new Point(rect.Left + (int)((action == "expander" ? 9 : 31) * dpi),
                              rect.Top + Math.Min((int)(10 * dpi), rect.Height / 2));
                if (!rect.Contains(pt) || GetAncestor(WindowFromPoint(pt), 2) != hwnd)
                    throw new InvalidOperationException("the Prepare input target is obscured");
                if (action == "double-click") Mouse.DoubleClick(pt);
                else if (action == "click" || action == "expander" || action == "calendar") Mouse.Click(pt);
                else throw new UsageException("unknown Prepare input action " + action);
            }
            Say("prepare-input: " + action + " " + Ui.Describe(match.Element));
            return Done;
        }

        [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    }
}
