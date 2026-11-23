// The WPF side of the host (IShellUi): the dispatcher queue's drain at Input priority, render into
// the window, the live front facts and modal state, dialogs, the folder picker and About shown one
// at a time in call order by the presenter (Presenter.cs) while a flow thread waits, close and quit
// (CloseGate: the flow-based close, the flow's own quit, the logoff), the log, the menu bar with
// its shortcuts, the activation aggregator's sources, the window's size and position kept, the
// window's messages (the session's end, WM_DEVICECHANGE's volumes), and the update's commit after
// the first frame.
// The core runs on this dispatcher's thread (the thread core_new ran on).
using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Win32;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;
using CoreMenu = RbStemsPlus.Core.Menu;

namespace RbStemsPlus.Shell
{
    internal sealed class Shell : IShellUi, ICloseSide
    {
        public readonly Host Host;
        public readonly ManageModel Model = new ManageModel();
        /// the Status and Prepare tabs' models and views
        public readonly PrepareModel Prepare;
        public readonly StatusModel Status;
        private PrepareView prepareView;
        private StatusView statusView;
        /// the Prepare and Status tabs' views, where shown (the probe's fields)
        internal PrepareView PrepareTab => prepareView;
        internal StatusView StatusTab => statusView;
        public MainWindow Window { get; private set; }
        private readonly Dispatcher ui;
        private readonly LogSink log;
        private readonly UpdateNative update;
        private readonly CloseGate gate;
        private readonly CriticalWork critical;
        private readonly WinQuitFn quitFn;   // kept alive with the Shell: the DLL holds it (core_win_set_quit)
        private readonly QuitKind quitKind;  // the uninstall's quit writes no window place and no log file (UN5)
        private IntPtr hwnd;
        private readonly string placePath;   // %LOCALAPPDATA%\rbstemsplus\window (app.md §4.6)
        private Activation activation;
        private EventWaitHandle rekordboxChanged, rekordboxRepair;
        private RegisteredWaitHandle rekordboxWait, repairWait;
        private DispatcherTimer inventoryPoll, inventoryDebounce;
        private FileSystemWatcher inventoryFolders;
        private string inventoryPath;
        private ulong inventoryGeneration;
        private int inventoryFallbackTicks;
        private bool inventoryRetry, inventoryAwaitingRefresh, inventoryRepairPending;

        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern ulong core_win_rekordbox_generation();
        private readonly Presenter presenter;
        private bool rendered;
        private readonly CommitGate commit = new CommitGate();
        private bool tabAsked;             // the core was told once which tab the window shows
        private readonly Dictionary<MenuItem, int> menuIds = new Dictionary<MenuItem, int>();

        private readonly SwapGuard guard;   // the update's start (--swap) to its first good frame; null otherwise
        private readonly OneCopy oneCopy;   // the one-copy window, taken down as a quit is decided
        /// --uninstall's request (LaunchArgs.cs), sent at the first "shown"; null: a normal start
        public UninstallOnce Uninstall { get; set; }

        public Shell(Dispatcher ui, LogSink log, string placePath, UpdateNative update, SwapGuard guard, OneCopy oneCopy)
        {
            this.guard = guard;
            this.oneCopy = oneCopy;
            this.ui = ui;
            this.placePath = placePath;
            this.log = log;
            this.update = update;
            Host = new Host(this);
            Host.CheapFacts = CoreWin.core_win_cheap_facts;
            Host.WarmFacts = CoreWin.core_win_warm_facts;
            Prepare = new PrepareModel(json => Host.Send(json));   // from the tab's input handlers, on main, outside a render
            Status = new StatusModel(json => Host.Send(json));
            gate = new CloseGate(this);
            critical = new CriticalWork(Log);
            quitKind = new QuitKind(log.Buffer);
            quitFn = ctx =>
            {
                quitKind.Quit(ctx);   // before the quit is posted: no line after this reaches the file
                try { ui.BeginInvoke(DispatcherPriority.Normal, (Action)gate.QuitNow); } catch (Exception) { }
            };
#if SHELL_TEST
            // the probe leaves a window's nested frame out of the operation that shows it
            presenter = new Presenter(a => ui.BeginInvoke(DispatcherPriority.Normal, a), e => Host.Fail("a dialog", e), ShellProbe.ModalBegin, ShellProbe.ModalEnd);
#else
            presenter = new Presenter(a => ui.BeginInvoke(DispatcherPriority.Normal, a), e => Host.Fail("a dialog", e));
#endif
        }

        /// core.h Life cycle: core_new, start, the first render (Start); then the window shown at its
        /// last size and position, fitted to a connected monitor's work area (Show), then "shown"
        /// once its first frame is drawn (ContentRendered); the aggregator's events only after
        /// "shown", as on the Mac, where the first activation comes after launching. Program checks
        /// Host.RenderOk between Start and Show under --swap (WU1).
        public bool Start(string config)
        {
            // the flow's UPDATE_SWAP quits the app through this (asking nothing), before core_new
            try { CoreWin.core_win_set_quit(quitFn, IntPtr.Zero); }
            catch (EntryPointNotFoundException) { Log("shell: this core DLL takes no quit: an update can't quit the app"); }
            // core_new and the facts' warm-up first, so the warm runs while the window is built
            // (HR4); a refusal returns before any window exists. CoreConfig uses PrepareShown for
            // the core's menu rows ("prepare_tabs") and the window uses it for the tabs (PM1).
            if (!Host.Create(config)) return false;
            Window = new MainWindow(Model, Log);
            if (CoreConfig.PrepareShown)
            {
                prepareView = new PrepareView(Prepare);
                statusView = new StatusView(Status, Model);
                Window.AddPrepareTabs(statusView, prepareView);
                Window.TabChosen += tab => Host.Send(Json.Tab(tab));
            }
#if SHELL_TEST
            ShellProbe.Start(this, ui);       // before the first render; nothing without the harness's probe.conf
#endif
            // the log pane's flush, for the shell's lines and the updater's; one now for the lines
            // written before the window existed
            log.SetFlush(PostFlush);
            PostFlush();
            activation = new Activation(Host.SendActivation, flush => ui.BeginInvoke(DispatcherPriority.Background, flush));
            Window.AppActivation += activation.App;
#if SHELL_TEST
            Window.AppActivation += ShellProbe.AppActivation;
#endif
            Window.Restored += activation.Restored;
#if SHELL_TEST
            Window.RepaintTrace += line => TestHooks.SessionTrace(line, Log);
#endif
            Window.Clicked += id => Host.Send(Json.Click(id));
            Window.R1Dismissed += versions => Host.Send(Json.R1Dismiss(versions));
            Window.Closing += Closing;
            Window.Closed += (s, e) =>
            {
                StopWatchingRekordbox();
                Host.Stop();
                presenter.Stop();
                Application.Current.Shutdown();
            };
            Window.SourceInitialized += (s, e) =>
            {
                hwnd = new WindowInteropHelper(Window).Handle;
                HwndSource.FromHwnd(hwnd)?.AddHook(WndProc);
                WatchRekordbox();
            };
            // a frame is on screen: the core's "shown" (ungated: its events bring a reload after a
            // failed render), then the update's `started` and pointers (or the leftovers) on a
            // worker thread, once the frame shows an applied render
            Window.ContentRendered += (s, e) =>
            {
                Host.Shown();
                Uninstall?.Shown();
                commit.Framed();
                TryCommit();
                activation.Ready();
#if SHELL_TEST
                TestHooks.Start(this, ui);
#endif
            };
            Host.Begin();   // the warm awaited (10 s from Create at most), then "start" and the first render
            BuildMenus();
#if SHELL_TEST
            ShellProbe.Mark("shell.core");    // the core shell's launches told apart, as the Mac's
#endif
            return true;
        }

        public void Show()
        {
            WindowPlace place = WindowPlace.Load(placePath);
            if (place != null) place = Window.Fitted(place);
            if (place != null)
            {
                Window.Place = place;   // applied once the window is created (OnSourceInitialized)
                Window.WindowStartupLocation = WindowStartupLocation.Manual;
            }
            Window.Show();
        }

        private RegisteredWaitHandle WatchSignal(string name, Action action, out EventWaitHandle signal)
        {
            signal = new EventWaitHandle(false, EventResetMode.AutoReset, name);
            return ThreadPool.RegisterWaitForSingleObject(signal, (state, timedOut) => PostInventory(action),
                                                          null, Timeout.Infinite, false);
        }

        private void PostInventory(Action action)
        {
            try
            {
                ui.BeginInvoke(DispatcherPriority.Background, (Action)(() =>
                {
                    if (inventoryPoll != null) action();
                }));
            }
            catch (InvalidOperationException) { } // the dispatcher is shutting down
        }

        private void WatchRekordbox()
        {
            inventoryDebounce = new DispatcherTimer(DispatcherPriority.Background, ui);
            inventoryDebounce.Tick += (s, e) =>
            {
                inventoryDebounce.Stop();
                inventoryAwaitingRefresh = !inventoryRetry;
                Host.RefreshRekordbox(showManage: inventoryRepairPending);
                inventoryRepairPending = false;
            };
            Host.RekordboxRefreshed += InventoryRefreshed;
            inventoryPoll = new DispatcherTimer(DispatcherPriority.Background, ui) { Interval = TimeSpan.FromSeconds(2) };
            inventoryPoll.Tick += (s, e) => PollInventory();
            inventoryPoll.Start();
            try
            {
                rekordboxWait = WatchSignal(@"Local\io.github.rbstemsplus.rekordbox-changed", InventoryChanged, out rekordboxChanged);
                repairWait = WatchSignal(@"Local\io.github.rbstemsplus.rekordbox-repair",
                    () =>
                    {
                        inventoryRepairPending = true;
                        InventoryChanged();
                    }, out rekordboxRepair);
            }
            catch (Exception e) { Host.Fail("watcher refresh signals", e); }
            try { inventoryPath = RekordboxFolder(); }
            catch (Exception e) { Host.Fail("rekordbox folder location", e); }
            PollInventory();
        }

        private static string RekordboxFolder()
        {
            string programs = null;
#if SHELL_TEST
            programs = Environment.GetEnvironmentVariable("RBSP_TEST_PROGRAMS");
#endif
            if (string.IsNullOrEmpty(programs))
            {
                using (var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
                using (var current = machine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion"))
                {
                    const string value = "ProgramFilesDir";
                    if (current?.GetValueKind(value) == RegistryValueKind.String)
                        programs = current.GetValue(value, null, RegistryValueOptions.DoNotExpandEnvironmentNames) as string;
                }
            }
            return string.IsNullOrEmpty(programs) ? null : Path.Combine(programs, "rekordbox");
        }

        private void PollInventory()
        {
            ulong generation = core_win_rekordbox_generation();
            if (generation != inventoryGeneration || (generation == 0 && ++inventoryFallbackTicks >= 30))
            {
                inventoryGeneration = generation;
                inventoryFallbackTicks = 0;
                InventoryChanged();
            }
            bool exists = Directory.Exists(inventoryPath);
            if (exists == (inventoryFolders != null)) return;
            if (exists) WatchInventoryFolder();
            else
            {
                inventoryFolders.Dispose();
                inventoryFolders = null;
                InventoryChanged();
            }
        }

        private void WatchInventoryFolder()
        {
            FileSystemWatcher watcher = null;
            try
            {
                watcher = new FileSystemWatcher(inventoryPath)
                {
                    IncludeSubdirectories = true,
                    NotifyFilter = NotifyFilters.DirectoryName | NotifyFilters.FileName | NotifyFilters.LastWrite
                };
                watcher.Created += OnInventoryFolder;
                watcher.Changed += OnInventoryFolder;
                watcher.Deleted += OnInventoryFolder;
                watcher.Renamed += (s, e) =>
                {
                    if (InventoryName(e.OldName, e.ChangeType) || InventoryName(e.Name, e.ChangeType))
                        PostInventory(InventoryChanged);
                };
                watcher.Error += (s, e) => PostInventory(() =>
                {
                    if (inventoryFolders != watcher) return;
                    inventoryFolders.Dispose();
                    inventoryFolders = null;
                    InventoryChanged();
                });
                watcher.EnableRaisingEvents = true;
                inventoryFolders = watcher;
                InventoryChanged();
            }
            catch (Exception e)
            {
                watcher?.Dispose();
                Host.Fail("rekordbox folder notifications", e);
            }
        }

        private void OnInventoryFolder(object sender, FileSystemEventArgs e)
        {
            if (InventoryName(e.Name, e.ChangeType)) PostInventory(InventoryChanged);
        }

        private static bool InventoryName(string name, WatcherChangeTypes change)
        {
            if (string.IsNullOrEmpty(name)) return false;
            string[] parts = name.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            const string prefix = "rekordbox ";
            if (!parts[0].StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return false;
            string[] version = parts[0].Substring(prefix.Length).Split('.');
            if (version.Length != 3) return false;
            foreach (string component in version)
            {
                if (component.Length == 0) return false;
                foreach (char c in component)
                    if (c < '0' || c > '9') return false;
            }
            if (parts.Length == 1) return change != WatcherChangeTypes.Changed;
            return parts.Length == 2 &&
                   (parts[1].Equals("rekordbox.exe", StringComparison.OrdinalIgnoreCase) ||
                    parts[1].Equals("onnxruntime.dll", StringComparison.OrdinalIgnoreCase));
        }

        private void InventoryChanged()
        {
            inventoryRetry = inventoryAwaitingRefresh = false;
            DebounceInventory(TimeSpan.FromMilliseconds(1500));
        }

        private void DebounceInventory(TimeSpan delay)
        {
            inventoryDebounce.Stop();
            inventoryDebounce.Interval = delay;
            inventoryDebounce.Start();
        }

        private void InventoryRefreshed()
        {
            if (!inventoryAwaitingRefresh) return;
            inventoryAwaitingRefresh = false;
            if (!Host.LibUnchecked) return;
            inventoryRetry = true;
            DebounceInventory(TimeSpan.FromSeconds(3));
        }

        private void StopWatchingRekordbox()
        {
            inventoryPoll?.Stop();
            inventoryPoll = null;
            inventoryDebounce?.Stop();
            Host.RekordboxRefreshed -= InventoryRefreshed;
            inventoryFolders?.Dispose();
            rekordboxWait?.Unregister(null);
            repairWait?.Unregister(null);
            rekordboxChanged?.Dispose();
            rekordboxRepair?.Dispose();
        }

        // the update's commit after a good frame, once (CommitGate); under --swap it also ends the guard
        private void TryCommit()
        {
            if (commit.TryCommit(Host.RenderOk, guard == null ? (Func<bool>)null : guard.Finish)) gate.RunHandoff(update.AfterRender);
        }

        /// A second start of the app signalled this copy (the one-copy window's signal, §5.9): the
        /// window comes forward (the second copy passed us the foreground rights), then
        /// "activated" and "check_request" (main.swift:169-172). Program posts it from the signal.
        public void SecondStart()
        {
            if (Window == null) return;
#if SHELL_TEST
            ShellProbe.Reopen();
#endif
            if (Window.WindowState == WindowState.Minimized) Window.WindowState = WindowState.Normal;
            Window.Activate();
            activation.SecondStart();
        }

        // ------------------------------------------------------------------ IShellUi

        public void PostDrain(Action drain) => ui.BeginInvoke(DispatcherPriority.Input, drain);

        public void Render(IntPtr snap, IntPtr delta)
        {
#if SHELL_TEST
            ShellProbe.Mark("refresh.begin");
            TestHooks.MaybeFailRender();
#endif
            RenderApp(snap, delta);
#if SHELL_TEST
            ShellProbe.Mark("refresh.end");
#endif
            // a good render after the first frame (the first one failed): commit after its frame
            if (commit.Rendered(true)) ui.BeginInvoke(DispatcherPriority.ContextIdle, (Action)TryCommit);
        }

        private void RenderApp(IntPtr snap, IntPtr delta)
        {
            uint moved = Snap.Moved(delta);
            if (prepareView != null && Snap.Has(moved, Section.S_PREPARE)) Prepare.Apply(new SnapPrepareSource(snap, delta));
            if (statusView != null && Snap.Has(moved, Section.S_STATUS)) Status.Apply(new SnapPrepareSource(snap, delta));
            if (!Snap.Has(moved, Section.S_APP)) return;
            long seq = Model.LogShowSeq;
            Model.Apply(snap, delta == IntPtr.Zero);
            if (rendered && Model.LogShowSeq != seq) Window.ShowLogEnd();
            rendered = true;
            // If the core asks for a tab the window lacks, report the actual tab once from a
            // separate operation outside render.
            if (Window.HasTab(Model.Tab))
            {
                tabAsked = false;
                Window.SelectTab(Model.Tab);
            }
            else if (!tabAsked)
            {
                tabAsked = true;
                int shown = Window.ShownTab;
                ui.BeginInvoke(DispatcherPriority.Input, (Action)(() => Host.Send(Json.Tab(shown))));
            }
        }

        public uint FrontFacts()
        {
            var w = Window;
            if (w == null) return 0;
            uint bits = 0;
            foreach (Window x in Application.Current.Windows) if (x.IsActive) bits |= CoreApi.CORE_FRONT_ACTIVE;
            if (w.IsLoaded) bits |= CoreApi.CORE_FRONT_WINDOW_LOADED;
            if (w.WindowState == WindowState.Minimized) bits |= CoreApi.CORE_FRONT_MINIATURIZED;
            bits |= (uint)w.ShownTab << CoreApi.CORE_FRONT_TAB_SHIFT;
            return bits;
        }

        public bool ModalOpen() => presenter.ModalOpen;
#if SHELL_TEST
        internal bool PresenterIdle => presenter.Idle;
        /// support (the parent of Logs and of the window's place file)
        internal string SupportDir => System.IO.Path.GetDirectoryName(placePath);
#endif

        // the flow thread waits while the presenter shows the window, in turn, from its own
        // dispatcher operation (never inside a core callback on main). null: not shown.
        public DialogAnswer ShowDialog(DialogForm form)
        {
            DialogAnswer answer = null;
            return presenter.RunAndWait(() => answer = new DialogWindow(form, Window, Log).Run()) ? answer : null;
        }

        public int PickFolder(string start, out string path)
        {
            string chosen = null;
            int rc = -1;
            if (!presenter.RunAndWait(() => rc = FolderPicker.Show(Window, start, out chosen))) rc = -1;
            path = chosen;
            return rc;
        }

        public void CloseReply(int what, bool proceed) =>
            ui.BeginInvoke(DispatcherPriority.Normal, (Action)(() => gate.OnReply(what, proceed)));

        public void Log(string line)
        {
            // at Input, as the drain: a flood of lines can't starve the pane (each flush is one batch)
            log.Add(line);   // the sink posts the flush when this line starts a batch (WU5)
        }

        private void PostFlush() => ui.BeginInvoke(DispatcherPriority.Input, (Action)Flush);

        private void Flush()
        {
            var w = Window;
            if (w != null) w.AppendLog(log.Buffer.Take());
        }

        // ------------------------------------------------------------------ close

        // Window.Closing: the core decides (CloseGate: core_close_asked; NOW or close_reply(1) closes
        // after the bounded CriticalWork wait; REFUSE stays). Closing the window quits the app. The
        // window's size and position are kept once the close goes through, except after the
        // complete uninstall's quit (QuitKind: its folder is gone and stays gone).
        private void Closing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (Host.C != IntPtr.Zero && !gate.OnClosing())
            {
                e.Cancel = true;
                return;
            }
            if (!quitKind.SavePlace) return;
            WindowPlace place = Window.CurrentPlace();
            if (place != null && !place.Save(placePath)) Log("shell: the window's size and position weren't saved");
        }

        /// Application.SessionEnding (WM_QUERYENDSESSION on WPF's own window): true vetoes it while
        /// a step must finish (the reason shown); otherwise WPF shuts the app down after it.
        public bool QueryEndSession()
        {
            bool veto = gate.OnQueryEndSession();
#if SHELL_TEST
            TestHooks.SessionTrace(veto ? "query-veto" : "query-allow", Log);
#endif
            Log(veto ? "logoff: a step must finish: the session waits for it" : "logoff: nothing to finish: quitting");
            return veto;
        }

        /// Application.Exit: after a query WPF let through, a bounded catch for a step that started since.
        public void Exiting() => gate.EndWait();

        // ICloseSide
        int ICloseSide.Ask(int what) => Host.C == IntPtr.Zero ? CoreApi.CORE_CLOSE_NOW : Core.core_close_asked(Host.C, what);
        bool ICloseSide.CriticalRunning() => critical.Running();
        bool ICloseSide.CriticalWait(int ms) => critical.WaitIdle(ms);
        void ICloseSide.CriticalClose() => critical.Close();
        void ICloseSide.RunOff(Action a) => new Thread(() => a()) { IsBackground = true, Name = "shell worker" }.Start();
        void ICloseSide.Post(Action a) => ui.BeginInvoke(DispatcherPriority.Normal, a);
        void ICloseSide.Close() => Window?.Close();
        void ICloseSide.Quitting() => oneCopy?.Quitting();
        long ICloseSide.NowMs() => System.Diagnostics.Stopwatch.GetTimestamp() / (System.Diagnostics.Stopwatch.Frequency / 1000);

        // the logoff's reason: what the window says the app is doing (the core's status line)
        void ICloseSide.BlockReason(bool on)
        {
            if (hwnd == IntPtr.Zero) return;
            if (!on) { Win32.ShutdownBlockReasonDestroy(hwnd); return; }
            string why = Model.StatusLine.Length > 0 ? Model.StatusLine : Texts.Get(CoreText.T_APP_NAME);
            if (Win32.ShutdownBlockReasonCreate(hwnd, why) == 0) Log("shell: the logoff's reason couldn't be set (" + Marshal.GetLastWin32Error() + ")");
        }

        // ------------------------------------------------------------------ the window's messages

        private IntPtr WndProc(IntPtr h, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            switch (msg)
            {
                case Win32.WM_ENDSESSION:
#if SHELL_TEST
                    TestHooks.SessionTrace(wParam != IntPtr.Zero ? "end-true" : "end-false", Log);
#endif
                    gate.OnEndSession(wParam != IntPtr.Zero);
                    break;
                case Win32.WM_DEVICECHANGE:
                    DeviceChange(wParam, lParam);
                    break;
            }
            return IntPtr.Zero;
        }

        // A volume's arrival or removal (app.md §5.8): the core's "mount"/"unmount" with its drive
        // root, sent from their own operation (never inside a message the core may be under).
        private void DeviceChange(IntPtr wParam, IntPtr lParam)
        {
            int ev = (int)(long)wParam;
            if ((ev != Win32.DBT_DEVICEARRIVAL && ev != Win32.DBT_DEVICEREMOVECOMPLETE) || lParam == IntPtr.Zero) return;
            int devtype = Marshal.ReadInt32(lParam, 4);
            if (devtype != Win32.DBT_DEVTYP_VOLUME) return;
            uint unitmask = (uint)Marshal.ReadInt32(lParam, 12);
            foreach (string json in update.DeviceEvents((uint)ev, (uint)devtype, unitmask))
                ui.BeginInvoke(DispatcherPriority.Input, (Action)(() => Host.Send(json)));
        }

        // ------------------------------------------------------------------ the menu bar

        // The menu bar from menus.def's Windows rows (core_menu_count/_at), titles from core_text as
        // the definition writes them (a "_" before a letter is its access key, app.md §4.5), states
        // and titles pulled live when a menu opens (core_menu_state, allowed on main). Each item's
        // shortcut is a KeyBinding on the window whose CanExecute asks the core too, so a shortcut
        // does what the item would: nothing while it is disabled or hidden, nothing under a dialog
        // (ShowDialog disables the window). The core's Prepare and Status menu rows follow
        // "prepare_tabs" in CoreConfig. A start cross-check logs any disagreement with the window.
        // The native rows are the shell's (MenuNatives.Of):
        // Exit closes the window as its close box does, Edit's items are WPF's own commands on the
        // focused text, About (at the end of Help) is ours; their keys (Alt+F4, Ctrl+X/C/V/A) are
        // Windows' and WPF's, so they are shown, not bound.

        private void BuildMenus()
        {
            var bar = Window.MenuBar;
            bar.Items.Clear();
            var items = new Dictionary<int, MenuItem>();
            int count = Core.core_menu_count(CoreApi.CORE_OS_WIN);
            for (int i = 0; i < count; i++)
            {
                if (Core.core_menu_at(CoreApi.CORE_OS_WIN, i, out CoreMenuItem m) != CoreApi.CORE_OK) continue;
                ItemsControl parent = m.menu < 0 ? (ItemsControl)bar : (items.TryGetValue(m.menu, out var p) ? p : null);
                if (parent == null) continue;
                if (m.kind == CoreApi.CORE_MENU_SEPARATOR) { parent.Items.Add(new Separator()); continue; }
                var mi = new MenuItem { Header = m.title >= 0 ? Texts.Get((CoreText)m.title) : "" };
                string ev = Core.Utf8z(m.evt);
                if (m.kind == CoreApi.CORE_MENU_NATIVE)
                {
                    if (!Native(mi, m.id)) continue;
                    mi.InputGestureText = MenuKey.Parse(Core.Utf8z(m.key), m.mods)?.Text ?? "";
                }
                else if (ev.Length > 0)
                {
                    mi.Click += (s, e) => { if (e.OriginalSource == s) Host.Send(Json.Menu(ev)); };
                    Bind(mi, m.id, ev, Core.Utf8z(m.key), m.mods);
                }
                mi.SubmenuOpened += (s, e) => { if (e.OriginalSource == s) RefreshStates((MenuItem)s); };
                items[m.id] = mi;
                menuIds[mi] = m.id;
                parent.Items.Add(mi);
            }
            RefreshStates(bar);   // the top-level menus: the rules' hidden ones stay hidden
            // the core's Prepare menu and the window's tabs come from the same PrepareShown (PM1)
            bool coreShows = (Core.core_menu_state(Host.C, (int)CoreMenu.M_PREPARE, out IntPtr _, out UIntPtr _) & CoreApi.CORE_MENU_HIDDEN) == 0;
            if (coreShows != CoreConfig.PrepareShown)
            {
                Log("shell: the core's Prepare menu disagrees with the window's tabs");
#if SHELL_TEST
                ShellProbe.Mark("shell.prepare_mismatch");
#endif
            }
        }

        // A native row's action; false: one this shell doesn't draw (logged, left out)
        private bool Native(MenuItem mi, int id)
        {
            switch (MenuNatives.Of(id))
            {
                case MenuNative.Exit: mi.Click += (s, e) => Window.Close(); return true;
                case MenuNative.Cut: mi.Command = ApplicationCommands.Cut; return true;
                case MenuNative.Copy: mi.Command = ApplicationCommands.Copy; return true;
                case MenuNative.Paste: mi.Command = ApplicationCommands.Paste; return true;
                case MenuNative.SelectAll: mi.Command = ApplicationCommands.SelectAll; return true;
                case MenuNative.About: mi.Click += (s, e) => ShowAbout(); return true;
            }
            Log("shell: menus.def's native row " + (CoreMenu)id + " isn't one the shell draws");
            return false;
        }

        // An item's shortcut: the gesture shown beside its title, and a KeyBinding sending the
        // item's event when the core says the item is on.
        private void Bind(MenuItem mi, int id, string ev, string key, uint mods)
        {
            MenuKey k = MenuKey.Parse(key, mods);
            if (k == null)
            {
                if (key.Length > 0) Log("shell: menus.def's key \"" + key + "\" for " + ev + " isn't one the shell knows: no shortcut");
                return;
            }
            KeyGesture gesture;
            try
            {
                var wk = (Key)Enum.Parse(typeof(Key), k.Key);
                var mk = (k.Ctrl ? ModifierKeys.Control : 0) | (k.Shift ? ModifierKeys.Shift : 0) | (k.Alt ? ModifierKeys.Alt : 0);
                gesture = new KeyGesture(wk, mk, k.Text);
            }
            catch (Exception e) when (e is ArgumentException || e is NotSupportedException)
            {
                Log("shell: the shortcut " + k.Text + " for " + ev + " can't be bound: " + e.Message);
                return;
            }
            var cmd = new RoutedCommand(ev, typeof(MainWindow));
            Window.CommandBindings.Add(new CommandBinding(cmd,
                (s, e) => { e.Handled = true; Host.Send(Json.Menu(ev)); },
                (s, e) => { e.CanExecute = MenuOn(id); e.Handled = true; }));
            Window.InputBindings.Add(new KeyBinding(cmd, gesture));
            mi.InputGestureText = k.Text;
        }

        // the item's state now, as its menu would show it: enabled and not hidden
        private bool MenuOn(int id)
        {
            if (Host.C == IntPtr.Zero || Host.Stopped) return false;
            uint st = Core.core_menu_state(Host.C, id, out IntPtr _, out UIntPtr _);
            return (st & CoreApi.CORE_MENU_ENABLED) != 0 && (st & CoreApi.CORE_MENU_HIDDEN) == 0;
        }

        private void RefreshStates(ItemsControl parent)
        {
            if (Host.C == IntPtr.Zero) return;
            foreach (var o in parent.Items)
            {
                if (!(o is MenuItem mi) || !menuIds.TryGetValue(mi, out int id)) continue;
                uint st = Core.core_menu_state(Host.C, id, out IntPtr title, out UIntPtr len);
                mi.IsEnabled = (st & CoreApi.CORE_MENU_ENABLED) != 0;
                mi.Visibility = (st & CoreApi.CORE_MENU_HIDDEN) != 0 ? Visibility.Collapsed : Visibility.Visible;
                mi.IsChecked = (st & CoreApi.CORE_MENU_CHECKED) != 0;
                if (title != IntPtr.Zero) mi.Header = Core.Utf8(title, len);
            }
        }

#if SHELL_TEST
        /// The probe's trace mode: every menu's items as the core validates them now.
        internal void RefreshMenusForProbe()
        {
            void All(ItemsControl p)
            {
                RefreshStates(p);
                foreach (var o in p.Items) if (o is MenuItem mi && mi.Items.Count > 0) All(mi);
            }
            if (Window?.MenuBar != null) All(Window.MenuBar);
        }

        /// The probe's ids: the menu bar's item for a core menu id, or null.
        internal MenuItem MenuItemFor(int id)
        {
            foreach (var kv in menuIds) if (kv.Value == id) return kv.Key;
            return null;
        }
#endif

        // through the presenter too: queued behind a dialog that is up, never stacked over it
        internal void ShowAbout() => presenter.Run(() => new AboutWindow(Window, update.ShownVersion(), Log).ShowDialog());
    }
}
