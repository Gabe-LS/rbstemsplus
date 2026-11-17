// The main window's views: the core's texts on the controls, the lights drawn from the model's
// looks, the status and notice lines' live regions, the log pane, the start size clamped to the
// work area or the last size and position restored, and the aggregator's two window sources
// (WM_ACTIVATEAPP, leaving Minimized). It holds view handles only; every click goes to the shell
// as the core's event id.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    public partial class MainWindow : Window
    {
        internal ManageModel Model { get; }
        /// a click: the {"ev":"click"} id (flows.c click_ids)
        internal event Action<string> Clicked;
        /// WM_ACTIVATEAPP: our app activated (true) or another app (false); never our own windows
        internal event Action<bool> AppActivation;
        /// the window left Minimized
        internal event Action Restored;
        /// set before Show: the last size and position, fitted to a connected monitor (Fitted)
        internal WindowPlace Place;
        private WindowState lastState;

        internal MainWindow(ManageModel model)
        {
            Model = model;
            InitializeComponent();
            DataContext = model;
            Title = Texts.Get(CoreText.T_APP_NAME);
            ManageTab.Header = DialogWindow.NoAccessKey(Texts.Get(CoreText.T_TAB_MANAGE));
            PlusLabel.Text = Texts.Get(CoreText.T_FEATURE_PLUS);
            CacheLabel.Text = Texts.Get(CoreText.T_FEATURE_CACHE);
            Wire(InstallPlus, CoreText.T_BTN_INSTALL_PLUS, "install_plus");
            Wire(ReinstallPlus, CoreText.T_BTN_REINSTALL_PLUS, "reinstall_plus");
            Wire(UninstallPlus, CoreText.T_BTN_UNINSTALL_PLUS, "uninstall_plus");
            Wire(InstallCache, CoreText.T_BTN_INSTALL_CACHE, "install_cache");
            Wire(ReinstallCache, CoreText.T_BTN_REINSTALL_CACHE, "reinstall_cache");
            Wire(SettingsButton, CoreText.T_MENU_SETTINGS, "settings");
            Wire(UninstallCache, CoreText.T_BTN_UNINSTALL_CACHE, "uninstall_cache");
            AutomationProperties.SetLiveSetting(StatusLine, AutomationLiveSetting.Polite);
            AutomationProperties.SetLiveSetting(NoticeLine, AutomationLiveSetting.Polite);
            AutomationProperties.SetName(LogView, Texts.Get(CoreText.T_MENU_SHOW_LOG));
            AutomationProperties.SetName(LeftColumn, Texts.Get(CoreText.T_TAB_MANAGE));
            model.PropertyChanged += ModelChanged;
            SystemParameters.StaticPropertyChanged += (s, e) => { if (e.PropertyName == nameof(SystemParameters.HighContrast)) DrawLights(); };
            ClampToWorkArea();
            DrawLights();
        }

        private void Wire(Button b, CoreText title, string id)
        {
            string text = Texts.Get(title);
            b.Content = DialogWindow.NoAccessKey(text);
            DialogWindow.SetRawName(b, text);
            b.Click += (s, e) => Clicked?.Invoke(id);
        }

        // the start size (1040×680 DIP), clamped to the work area (app.md §4.6)
        private void ClampToWorkArea()
        {
            Rect work = SystemParameters.WorkArea;
            Width = Math.Max(MinWidth, Math.Min(Width, work.Width));
            Height = Math.Max(MinHeight, Math.Min(Height, work.Height));
        }

        /// The kept place as it may be used now, or null for the start size (app.md §4.6): the
        /// monitor its title strip is on, and WindowPlace.Fit into that monitor's work area with
        /// the window's minimum at that monitor's DPI. GetWindowPlacement's rectangle is in
        /// workspace coordinates (the primary monitor's work area at 0,0): moved to the screen's
        /// for the monitor calls, and back for SetWindowPlacement.
        internal WindowPlace Fitted(WindowPlace p)
        {
            if (!PrimaryOffset(out int dx, out int dy)) return null;
            RECT saved = p.Rect;
            saved.left += dx; saved.right += dx; saved.top += dy; saved.bottom += dy;
            RECT strip = WindowPlace.Strip(saved);
            IntPtr mon = Win32.MonitorFromRect(ref strip, Win32.MONITOR_DEFAULTTONULL);
            var mi = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };
            if (mon == IntPtr.Zero || Win32.GetMonitorInfoW(mon, ref mi) == 0) return null;
            uint dpi = 96;
            try { if (Win32.GetDpiForMonitor(mon, Win32.MDT_EFFECTIVE_DPI, out uint x, out uint _) == 0 && x > 0) dpi = x; }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException) { }
            RECT? fit = WindowPlace.Fit(saved, mi.rcWork, (int)Math.Ceiling(MinWidth * dpi / 96.0), (int)Math.Ceiling(MinHeight * dpi / 96.0));
            if (fit == null) return null;
            RECT r = fit.Value;
            return new WindowPlace { Left = r.left - dx, Top = r.top - dy, Right = r.right - dx, Bottom = r.bottom - dy, Maximized = p.Maximized };
        }

        // workspace to screen: the primary monitor's work area's offset from its corner (a taskbar
        // at the top or left)
        private static bool PrimaryOffset(out int dx, out int dy)
        {
            dx = dy = 0;
            IntPtr primary = Win32.MonitorFromPoint(new POINT { x = 0, y = 0 }, Win32.MONITOR_DEFAULTTOPRIMARY);
            var mi = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };
            if (primary == IntPtr.Zero || Win32.GetMonitorInfoW(primary, ref mi) == 0) return false;
            dx = mi.rcWork.left - mi.rcMonitor.left;
            dy = mi.rcWork.top - mi.rcMonitor.top;
            return true;
        }

        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);
            IntPtr hwnd = new WindowInteropHelper(this).Handle;
            HwndSource.FromHwnd(hwnd)?.AddHook(Hook);
            if (Place == null) return;
            // the window is created, not shown: SetWindowPlacement puts it at the kept rectangle
            // (WPF's MinWidth/MinHeight still apply) and shows it there, maximized if it was
            var wp = new WINDOWPLACEMENT { length = Marshal.SizeOf(typeof(WINDOWPLACEMENT)) };
            if (Win32.GetWindowPlacement(hwnd, ref wp) == 0) return;
            wp.flags = 0;
            wp.showCmd = Place.Maximized ? Win32.SW_SHOWMAXIMIZED : Win32.SW_SHOWNORMAL;
            wp.normalPosition = Place.Rect;
            Win32.SetWindowPlacement(hwnd, ref wp);
        }

        /// The size and position to keep: the restored rectangle, and whether it is maximized
        /// (or minimized from maximized). null: no window.
        internal WindowPlace CurrentPlace()
        {
            IntPtr hwnd = new WindowInteropHelper(this).Handle;
            var wp = new WINDOWPLACEMENT { length = Marshal.SizeOf(typeof(WINDOWPLACEMENT)) };
            if (hwnd == IntPtr.Zero || Win32.GetWindowPlacement(hwnd, ref wp) == 0) return null;
            const int WPF_RESTORETOMAXIMIZED = 2;
            RECT r = wp.normalPosition;
            return new WindowPlace
            {
                Left = r.left, Top = r.top, Right = r.right, Bottom = r.bottom,
                Maximized = wp.showCmd == Win32.SW_SHOWMAXIMIZED || (wp.showCmd == Win32.SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED) != 0),
            };
        }

        // WM_ACTIVATEAPP (app.md §4.3): only when activation moves between our thread and another
        // app's; the aggregator only takes note (it sends from its own operation)
        private IntPtr Hook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg == Win32.WM_ACTIVATEAPP) AppActivation?.Invoke(wParam != IntPtr.Zero);
            return IntPtr.Zero;
        }

        protected override void OnStateChanged(EventArgs e)
        {
            base.OnStateChanged(e);
            if (lastState == WindowState.Minimized && WindowState != WindowState.Minimized) Restored?.Invoke();
            lastState = WindowState;
        }

        private void ModelChanged(object sender, PropertyChangedEventArgs e)
        {
            switch (e.PropertyName)
            {
                case nameof(ManageModel.WindowTitle):
                    Title = Model.WindowTitle.Length > 0 ? Model.WindowTitle : Texts.Get(CoreText.T_APP_NAME);
                    break;
                case nameof(ManageModel.StatusLine):
                    // the status line is a polite live region: Narrator reads each new text (§4.7)
                    UIElementAutomationPeer.FromElement(StatusLine)?.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
                    break;
                case nameof(ManageModel.Notice):
                    UIElementAutomationPeer.FromElement(NoticeLine)?.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
                    break;
                case nameof(ManageModel.PlusLook): case nameof(ManageModel.PlusAx): case nameof(ManageModel.PlusTip):
                case nameof(ManageModel.CacheLook): case nameof(ManageModel.CacheAx): case nameof(ManageModel.CacheTip):
                    DrawLights();
                    break;
            }
        }

        // The lights (Controller.swift lightDrawing): a check circle, an exclamation triangle, a
        // cross circle; the shapes differ as well as the colours. In high contrast every look
        // takes the system's text colour, so only the shape tells them apart.
        private void DrawLights()
        {
            Draw(PlusLight, PlusLabel, Model.PlusLook, Model.PlusAx, Model.PlusTip);
            Draw(CacheLight, CacheLabel, Model.CacheLook, Model.CacheAx, Model.CacheTip);
        }

        internal static void Draw(TextBlock light, TextBlock label, long look, string ax, string tip)
        {
            light.Text = look == ManageModel.LookOn ? "" : look == ManageModel.LookLimited ? "" : "";
            // high contrast: a reference to the system's brush, so a theme change while it is on redraws it
            if (SystemParameters.HighContrast) light.SetResourceReference(TextBlock.ForegroundProperty, SystemColors.WindowTextBrushKey);
            else light.Foreground = look == ManageModel.LookOn ? new SolidColorBrush(Color.FromRgb(0x10, 0x7C, 0x10))
                : look == ManageModel.LookLimited ? new SolidColorBrush(Color.FromRgb(0x9D, 0x5D, 0x00))
                : new SolidColorBrush(Color.FromRgb(0xC4, 0x2B, 0x1C));
            AutomationProperties.SetName(light, ax);
            AutomationProperties.SetHelpText(light, tip);
            light.ToolTip = tip.Length > 0 ? tip : null;
            AutomationProperties.SetHelpText(label, tip);
        }

        /// The log pane: new lines at the end, scrolled to them (Controller.log).
        internal void AppendLog(string text)
        {
            if (text.Length == 0) return;
            LogView.AppendText(text);
            LogView.ScrollToEnd();
        }

        /// The log's one-shot (F_LOG_SHOW_SEQ): the Manage tab, the log scrolled to its end.
        internal void ShowLogEnd()
        {
            // inside render: the switch is the core's, so no {"ev":"tab"} goes back (PT2)
#if PREPARE_TABS || SHELL_TEST
            selectingTab = true;
            try { ManageTab.IsSelected = true; }
            finally { selectingTab = false; }
#else
            ManageTab.IsSelected = true;   // the Manage tab only: no SelectionChanged handler (I86)
#endif
            LogView.ScrollToEnd();
        }

        // ------------------------------------------------------------------ the tabs

#if PREPARE_TABS || SHELL_TEST
        /// the user chose a tab (core.h's CORE_TAB_*): input only, never the render's selection
        internal event Action<int> TabChosen;
        private bool selectingTab;
        private TabItem statusTab, prepareTab;

        /// The Status and Prepare tabs (W8b), before Manage as on the Mac (Status, Prepare, Manage).
        /// Named as the Mac's tab items (the probe's tab.sel).
        internal void AddPrepareTabs(UIElement status, UIElement prepare)
        {
            statusTab = new TabItem { Name = "StatusTab", Header = DialogWindow.NoAccessKey(Texts.Get(CoreText.T_TAB_STATUS)), Content = status };
            prepareTab = new TabItem { Name = "PrepareTab", Header = DialogWindow.NoAccessKey(Texts.Get(CoreText.T_TAB_PREPARE)), Content = prepare };
            selectingTab = true;
            try
            {
                Tabs.Items.Insert(0, statusTab);
                Tabs.Items.Insert(1, prepareTab);
                ManageTab.IsSelected = true;
            }
            finally { selectingTab = false; }
            Tabs.SelectionChanged += (s, e) =>
            {
                if (e.OriginalSource != Tabs || selectingTab) return;   // a list's selection inside a tab bubbles here too
                TabChosen?.Invoke(ShownTab);
            };
        }

        internal bool HasTab(long tab) => tab == CoreApi.CORE_TAB_MANAGE
            || ((tab == CoreApi.CORE_TAB_PREPARE || tab == CoreApi.CORE_TAB_STATUS) && prepareTab != null);

        /// the tab shown now (core.h's CORE_TAB_*)
        internal int ShownTab => prepareTab == null ? CoreApi.CORE_TAB_MANAGE
            : statusTab.IsSelected ? CoreApi.CORE_TAB_STATUS : prepareTab.IsSelected ? CoreApi.CORE_TAB_PREPARE : CoreApi.CORE_TAB_MANAGE;

        /// The tab the core asks for (inside the render: no TabChosen back).
        internal void SelectTab(long tab)
        {
            if (!HasTab(tab) || ShownTab == tab) return;
            selectingTab = true;
            try
            {
                if (tab == CoreApi.CORE_TAB_STATUS) statusTab.IsSelected = true;
                else if (tab == CoreApi.CORE_TAB_PREPARE) prepareTab.IsSelected = true;
                else ManageTab.IsSelected = true;
            }
            finally { selectingTab = false; }
        }
#else
        // the first release: the Manage tab only (I86)
        internal bool HasTab(long tab) => tab == CoreApi.CORE_TAB_MANAGE;
        internal int ShownTab => CoreApi.CORE_TAB_MANAGE;
        internal void SelectTab(long tab) { }
#endif
    }
}
