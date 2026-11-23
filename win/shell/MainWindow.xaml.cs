// The main window's views: the core's texts on the controls, the lights drawn from the model's
// looks, the status and notice lines' live regions, the log pane, the start size clamped to the
// work area or the last size and position restored, and the aggregator's two window sources
// (WM_ACTIVATEAPP, leaving Minimized), and the picture redrawn after a session or display change
// (Repaint.cs). It holds view handles only; every click goes to the shell as the core's event id.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    public partial class MainWindow : Window
    {
        internal ManageModel Model { get; }
        /// a click: the {"ev":"click"} id (flows.c click_ids)
        internal event Action<string> Clicked;
        /// R1's Don't Ask Again: the versions its button showed when clicked ({"ev":"r1_dismiss"})
        internal event Action<string> R1Dismissed;
        /// WM_ACTIVATEAPP: our app activated (true) or another app (false); never our own windows
        internal event Action<bool> AppActivation;
        /// the window left Minimized
        internal event Action Restored;
        /// set before Show: the last size and position, fitted to a connected monitor (Fitted)
        internal WindowPlace Place;
        private WindowState lastState;
        private DispatcherTimer reportStatusTimer;

        internal MainWindow(ManageModel model, Action<string> log = null)
        {
            Model = model;
            InitializeComponent();
            renderRecovery = new RenderRecovery(this, log, () => sessionRenderResetPending || repaint.Away);
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
            Wire(RestoreButton, CoreText.T_WIN_BTN_RESTORE, "restore_pioneer");
            SetTitle(R1DismissButton, model.R1DismissTitle);
            R1DismissButton.Click += (s, e) => R1Dismissed?.Invoke(Model.R1DismissVersions);
            AutomationProperties.SetLiveSetting(StatusLine, AutomationLiveSetting.Polite);
            AutomationProperties.SetLiveSetting(NoticeLine, AutomationLiveSetting.Polite);
            AutomationProperties.SetName(LogView, Texts.Plain(CoreText.T_MENU_SHOW_LOG));
            AutomationProperties.SetName(LeftColumn, Texts.Get(CoreText.T_TAB_MANAGE));
            model.PropertyChanged += ModelChanged;
            StatusLine.Text = model.StatusLine; // no binding: UpdateStatusLine sets it from here on
            SystemParameters.StaticPropertyChanged += (s, e) => { if (e.PropertyName == nameof(SystemParameters.HighContrast)) DrawLights(); };
            ClampToWorkArea();
            DrawLights();
        }

        // a button's words, as the Mac's (Settings… is a menu title: its access key's "_" dropped)
        private void Wire(Button b, CoreText title, string id)
        {
            SetTitle(b, title);
            b.Click += (s, e) => Clicked?.Invoke(id);
        }

        private static void SetTitle(Button b, CoreText title)
        {
            string text = Texts.Plain(title);
            b.Content = DialogWindow.NoAccessKey(text);
            DialogWindow.SetRawName(b, text);
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
            // never handled here: WPF's HwndTarget sees each of them too (it registered this window
            // for its session's notifications)
            RepaintCause cause = RepaintPlan.Classify(msg, (long)wParam);
            if (cause != RepaintCause.None)
            {
                if (RepaintPlan.IsSessionReturn(msg, cause)) sessionRenderResetPending = true;
                RepaintNote(cause);
            }
            return IntPtr.Zero;
        }

        protected override void OnStateChanged(EventArgs e)
        {
            base.OnStateChanged(e);
            if (lastState == WindowState.Minimized && WindowState != WindowState.Minimized)
            {
                Restored?.Invoke();
                RepaintNote(RepaintCause.Restored);
            }
            else if (WindowState == WindowState.Minimized && lastState != WindowState.Minimized) RepaintNote(RepaintCause.Minimized);
            lastState = WindowState;
        }

        // ------------------------------------------------------------------ the picture (Repaint.cs)

        /// TEST builds: each nudge, for the session trace
        internal event Action<string> RepaintTrace;
        private readonly RepaintPlan repaint = new RepaintPlan();
        private readonly RenderRecovery renderRecovery;
        private DispatcherTimer repaintTimer;
        private bool sessionRenderResetPending;

        private static long NowMs() => Stopwatch.GetTimestamp() / (Stopwatch.Frequency / 1000);

        /// A session, power or display change, or the window's state (also the TEST hooks').
        internal void RepaintNote(RepaintCause cause)
        {
            repaint.Note(cause, WindowState == WindowState.Minimized, NowMs());
            RepaintArm();
        }

        private void RepaintArm()
        {
            long due = repaint.DueIn(NowMs());
            if (due < 0) { repaintTimer?.Stop(); return; }
            if (repaintTimer == null)
            {
                repaintTimer = new DispatcherTimer(DispatcherPriority.Background, Dispatcher);
                repaintTimer.Tick += (s, e) => RepaintTick();
            }
            repaintTimer.Stop();
            repaintTimer.Interval = TimeSpan.FromMilliseconds(Math.Max(1, due));
            repaintTimer.Start();
        }

        private void RepaintTick()
        {
            repaintTimer.Stop();
            while (repaint.Take(NowMs()))
            {
                if (sessionRenderResetPending) ResetSessionRenderTarget();
                Nudge();
            }
            RepaintArm();
        }

        private void ResetSessionRenderTarget()
        {
            if (!IsVisible || WindowState == WindowState.Minimized) return;
            sessionRenderResetPending = false;
            renderRecovery.Reset(RenderResetReason.SessionChange);
        }

        // WPF asked to lay out and draw the whole window again, as a resize would: the layout now
        // (a window restored with its minimized layout has its controls offscreen to UI
        // Automation) and the client area invalidated (WPF's WM_PAINT invalidates all it draws)
        private void Nudge()
        {
            IntPtr h = new WindowInteropHelper(this).Handle;
            if (h == IntPtr.Zero || !IsVisible || WindowState == WindowState.Minimized) return;
            RepaintTrace?.Invoke("repaint-nudge");
            if (Content is UIElement root)
            {
                root.InvalidateMeasure();
                root.InvalidateVisual();
            }
            UpdateLayout();
            Win32.InvalidateRect(h, IntPtr.Zero, 0);
        }

        protected override void OnClosed(EventArgs e)
        {
            // HwndSource removes the window hook and HwndTarget unregisters the WTS listener.
            sessionRenderResetPending = false;
            reportStatusTimer?.Stop();
            RepaintNote(RepaintCause.Closed);
            base.OnClosed(e);
        }

        private void UpdateStatusLine()
        {
            string text = Model.StatusLine;
            if (reportStatusTimer?.IsEnabled == true || StatusLine.Text == text) return;

            // Set the text before notifying UIA. Clients read it from the event source.
            StatusLine.Text = text;
            if (!AutomationPeer.ListenerExists(AutomationEvents.LiveRegionChanged)) return;
            if (text == Texts.Get(CoreText.T_CREATING_REPORT))
            {
                // A fast report can restore the idle text before a UIA client reads the source.
                // Keep the report text briefly. The timer then publishes the latest status.
                if (reportStatusTimer == null)
                {
                    reportStatusTimer = new DispatcherTimer(DispatcherPriority.Background, Dispatcher)
                    {
                        Interval = TimeSpan.FromMilliseconds(500)
                    };
                    reportStatusTimer.Tick += (s, e) =>
                    {
                        reportStatusTimer.Stop();
                        UpdateStatusLine();
                    };
                }
                reportStatusTimer.Start();
            }
            UIElementAutomationPeer.CreatePeerForElement(StatusLine)?.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
        }

        private void ModelChanged(object sender, PropertyChangedEventArgs e)
        {
            switch (e.PropertyName)
            {
                case nameof(ManageModel.WindowTitle):
                    Title = Model.WindowTitle.Length > 0 ? Model.WindowTitle : Texts.Get(CoreText.T_APP_NAME);
                    break;
                case nameof(ManageModel.StatusLine):
                    UpdateStatusLine();
                    break;
                case nameof(ManageModel.R1DismissVersions):
                    SetTitle(R1DismissButton, Model.R1DismissTitle);
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
            if (label != null) AutomationProperties.SetHelpText(label, tip);
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
            selectingTab = true;
            try { ManageTab.IsSelected = true; }
            finally { selectingTab = false; }
            LogView.ScrollToEnd();
        }

        // ------------------------------------------------------------------ the tabs

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
    }

    // Keep the version's text and automation ID visible to clients that walk Control View.
    public sealed class RbVersionRowText : TextBlock
    {
        protected override AutomationPeer OnCreateAutomationPeer() => new RowPeer(this);

        private sealed class RowPeer : TextBlockAutomationPeer
        {
            public RowPeer(RbVersionRowText owner) : base(owner) { }

            protected override bool IsControlElementCore() => true;
        }
    }

    // The version's text carries its automation name. The glyph is just the visual cue.
    public sealed class RbVersionGlyph : TextBlock
    {
        public static readonly DependencyProperty LookProperty = DependencyProperty.Register(
            nameof(Look), typeof(long), typeof(RbVersionGlyph),
            new PropertyMetadata(ManageModel.LookOff, (element, _) => ((RbVersionGlyph)element).Paint()));

        public long Look { get => (long)GetValue(LookProperty); set => SetValue(LookProperty, value); }

        public RbVersionGlyph()
        {
            Loaded += (sender, args) =>
            {
                SystemParameters.StaticPropertyChanged += SystemParameterChanged;
                Paint();
            };
            Unloaded += (sender, args) => SystemParameters.StaticPropertyChanged -= SystemParameterChanged;
        }

        protected override AutomationPeer OnCreateAutomationPeer() => null;

        private void SystemParameterChanged(object sender, PropertyChangedEventArgs args)
        {
            if (args.PropertyName == nameof(SystemParameters.HighContrast)) Paint();
        }

        private void Paint() => MainWindow.Draw(this, null, Look, "", "");
    }
}
