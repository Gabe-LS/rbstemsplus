// The window's view model for the S_APP section (app.md §4.2): the window title, the Manage tab's
// lights, buttons, status line, notice and spinner, the tab the core asks for and the log's
// one-shot. Filled from a snapshot in one pass; PropertyChanged is raised only for the values that
// changed. It holds what the snapshot says and nothing else: no decision, no text of its own.
// No WPF here (the Mac tests use it).
using System;
using System.Collections.Generic;
using System.ComponentModel;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    public sealed class ManageModel : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;

        // pure.h's light looks
        public const long LookOn = 0, LookLimited = 1, LookOff = 2;

        private readonly Dictionary<string, object> values = new Dictionary<string, object>();

        /// The probe's (TEST builds; null otherwise): the spinner's state on every S_APP pass, changed
        /// or not, where the Mac shell calls start/stopAnimation on every render (CoreShell.swift's
        /// renderApp), so perf mode's c.spinner events count the same on both (P4).
        public Action<bool> SpinnerPass;

        private T Get<T>(string name, T fallback = default(T)) => values.TryGetValue(name, out var v) ? (T)v : fallback;

        private void Set<T>(string name, T value)
        {
            if (values.TryGetValue(name, out var old) && Equals(old, value)) return;
            values[name] = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        public string WindowTitle => Get(nameof(WindowTitle), "");
        public string StatusLine => Get(nameof(StatusLine), "");
        public string Notice => Get(nameof(Notice), "");
        public bool Spinner => Get<bool>(nameof(Spinner));
        public long PlusLook => Get(nameof(PlusLook), LookOff);
        public string PlusTip => Get(nameof(PlusTip), "");
        public string PlusAx => Get(nameof(PlusAx), "");
        public long CacheLook => Get(nameof(CacheLook), LookOff);
        public string CacheTip => Get(nameof(CacheTip), "");
        public string CacheAx => Get(nameof(CacheAx), "");
        public bool InstallPlusEnabled => Get<bool>(nameof(InstallPlusEnabled));
        public bool ReinstallPlusEnabled => Get<bool>(nameof(ReinstallPlusEnabled));
        public bool UninstallPlusEnabled => Get<bool>(nameof(UninstallPlusEnabled));
        public bool InstallCacheEnabled => Get<bool>(nameof(InstallCacheEnabled));
        public bool ReinstallCacheEnabled => Get<bool>(nameof(ReinstallCacheEnabled));
        public bool UninstallCacheEnabled => Get<bool>(nameof(UninstallCacheEnabled));
        public bool SettingsEnabled => Get<bool>(nameof(SettingsEnabled));
        public bool UninstallAllEnabled => Get<bool>(nameof(UninstallAllEnabled));
        public bool UpdateVisible => Get<bool>(nameof(UpdateVisible));
        public long Tab => Get(nameof(Tab), (long)CoreApi.CORE_TAB_MANAGE);
        public long LogShowSeq => Get<long>(nameof(LogShowSeq));
        /// a light is drawn once the view model gives its tip (as the Mac shell's renderApp)
        public bool PlusLightShown => PlusTip.Length > 0;
        public bool CacheLightShown => CacheTip.Length > 0;

        /// One render's S_APP pass (main, inside render: only accessors are called). reload (a full
        /// reload: the first render, or the one after a render that failed part-way) forgets every
        /// value first, so every property is raised again and the views are redrawn whole.
        public void Apply(IntPtr s, bool reload)
        {
            if (reload) values.Clear();
            Set(nameof(WindowTitle), Snap.Str(s, Field.F_WINDOW_TITLE));
            Set(nameof(StatusLine), Snap.Str(s, Field.F_STATUS_LINE));
            Set(nameof(Notice), Snap.Str(s, Field.F_NOTICE));
            Set(nameof(Spinner), Snap.Flag(s, Field.F_SPINNER));
            SpinnerPass?.Invoke(Spinner);   // after the status line, before the lights: the Mac's order
            Set(nameof(PlusLook), Snap.I64(s, Field.F_LIGHT_PLUS_LOOK));
            Set(nameof(PlusAx), Snap.Str(s, Field.F_LIGHT_PLUS_AX));
            Set(nameof(CacheLook), Snap.I64(s, Field.F_LIGHT_CACHE_LOOK));
            Set(nameof(CacheAx), Snap.Str(s, Field.F_LIGHT_CACHE_AX));
            bool plusShown = PlusLightShown, cacheShown = CacheLightShown;
            Set(nameof(PlusTip), Snap.Str(s, Field.F_LIGHT_PLUS_TIP));
            Set(nameof(CacheTip), Snap.Str(s, Field.F_LIGHT_CACHE_TIP));
            if (reload || plusShown != PlusLightShown) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(PlusLightShown)));
            if (reload || cacheShown != CacheLightShown) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(CacheLightShown)));
            Set(nameof(InstallPlusEnabled), Snap.Flag(s, Field.F_INSTALL_PLUS_ENABLED));
            Set(nameof(ReinstallPlusEnabled), Snap.Flag(s, Field.F_REINSTALL_PLUS_ENABLED));
            Set(nameof(UninstallPlusEnabled), Snap.Flag(s, Field.F_UNINSTALL_PLUS_ENABLED));
            Set(nameof(InstallCacheEnabled), Snap.Flag(s, Field.F_INSTALL_CACHE_ENABLED));
            Set(nameof(ReinstallCacheEnabled), Snap.Flag(s, Field.F_REINSTALL_CACHE_ENABLED));
            Set(nameof(UninstallCacheEnabled), Snap.Flag(s, Field.F_UNINSTALL_CACHE_ENABLED));
            Set(nameof(SettingsEnabled), Snap.Flag(s, Field.F_SETTINGS_ENABLED));
            Set(nameof(UninstallAllEnabled), Snap.Flag(s, Field.F_UNINSTALL_ALL_ENABLED));
            Set(nameof(UpdateVisible), Snap.Flag(s, Field.F_UPDATE_VISIBLE));
            Set(nameof(Tab), Snap.I64(s, Field.F_TAB));
            Set(nameof(LogShowSeq), Snap.I64(s, Field.F_LOG_SHOW_SEQ));
        }
    }
}
