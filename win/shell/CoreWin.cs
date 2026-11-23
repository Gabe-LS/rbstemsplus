// The core DLL's Windows-only entries (win/c/update_shell.h; app.md §5.4, §5.8, §5.9) and the
// shell's side of them: the update's start and --swap (IStartNative), after the first render, the
// flow's quit, the device events, CriticalWork, one copy (OneCopy.cs). core.h's ABI rules: cdecl, strings in as LPUTF8Str,
// int answers. An entry the DLL doesn't have yet throws EntryPointNotFoundException at its first
// call; each wrapper turns that into its "not callable" answer.
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;
using System.Text;
using System.Threading;

namespace RbStemsPlus.Shell
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void WinLogFn(IntPtr ctx, IntPtr line, UIntPtr len);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void WinQuitFn(IntPtr ctx);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void WinSignalFn(IntPtr ctx);

    [SuppressUnmanagedCodeSecurity]
    internal static class CoreWin
    {
        private const string Dll = "rbsp-core.dll";
        private const CallingConvention C = CallingConvention.Cdecl;
        private const UnmanagedType U8 = UnmanagedType.LPUTF8Str;

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_swap_wait([MarshalAs(U8)] string support, [MarshalAs(U8)] string app, [MarshalAs(U8)] string own, uint ms, WinLogFn log, IntPtr ctx);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_swap_failed([MarshalAs(U8)] string support, [MarshalAs(U8)] string app, [MarshalAs(U8)] string own, WinLogFn log, IntPtr ctx);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_update_at_start([MarshalAs(U8)] string support, [MarshalAs(U8)] string app, [MarshalAs(U8)] string own, WinLogFn log, IntPtr ctx);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_update_after_render([MarshalAs(U8)] string support, [MarshalAs(U8)] string app, [MarshalAs(U8)] string own, WinLogFn log, IntPtr ctx);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_update_shown([MarshalAs(U8)] string support, [MarshalAs(U8)] string own, [Out] byte[] version, UIntPtr cap);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_os_check();
        /// lifecycle.h's life_token_check: LIFE_TOKEN_OK 0, ELEVATED 1, UAC_OFF 2, SERVICE 3, UNKNOWN 4
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_token_check();
        /// host->facts: the cheap rows (win/c/host_facts.c), main, inside facts only; 1 the sheet
        /// carries FACT_LIB_UNCHECKED, 0 it doesn't, -1 nothing read
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_cheap_facts(IntPtr core, IntPtr sheet);
        /// the same reads on a worker, keeping only the file memo (rekordbox's hashes and pins): after core_new, before "start",
        /// and on activation after an unchecked sheet (Host.WarmOnActivation)
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_win_warm_facts(IntPtr core);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_set_aumid();
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_win_set_quit(WinQuitFn quit, IntPtr ctx);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_device_events(uint ev, uint devtype, uint unitmask, [MarshalAs(U8)] string iface, byte[] buf, UIntPtr n);
        // CriticalWork in the core DLL (core_win.h: shell.c's process-wide critical_work)
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_critical_running();
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_critical_wait(uint ms);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_win_critical_close();
        // one copy (core_win.h; UI thread only; the handle an IntPtr, never a SafeHandle)
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_one_take([MarshalAs(U8)] string name, uint waitMs, out IntPtr one);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_win_one_window(IntPtr one, WinSignalFn onSignal, IntPtr ctx);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_win_one_window_close(IntPtr one);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_win_one_close(IntPtr one);
    }

    /// OneCopy's entries in the core DLL: the app's own mutex (name NULL). The signal's delegate
    /// is kept alive here, with the window, for as long as the app runs.
    internal sealed class OneCopyNative : IOneCopyNative
    {
        private WinSignalFn signalFn;

        public int Take(uint ms, out IntPtr one) => CoreWin.core_win_one_take(null, ms, out one);

        public int Window(IntPtr one, Action signal)
        {
            signalFn = ctx => { try { signal(); } catch (Exception) { } };
            return CoreWin.core_win_one_window(one, signalFn, IntPtr.Zero);
        }

        public void WindowClose(IntPtr one) => CoreWin.core_win_one_window_close(one);
        public void Close(IntPtr one) => CoreWin.core_win_one_close(one);
    }

    /// The update's entries for this app's paths, its log, and the stub fallback.
    internal sealed class UpdateNative : IStartNative
    {
        private readonly string support, app, own;
        private readonly LogSink log;
        private readonly WinLogFn logFn;   // kept alive with this object: the DLL calls it during each call

        /// support: %LOCALAPPDATA%\rbstemsplus; app: the running exe's folder (the version folder); own: the app's version.
        public UpdateNative(string support, string app, string own, LogSink log)
        {
            this.support = support;
            this.app = app;
            this.own = own;
            this.log = log;
            logFn = (ctx, line, len) => { try { Log(Core.Utf8(line, len)); } catch (Exception) { } };
        }

        public void Log(string line) => log.Add(line);   // posts the pane's flush when it starts a batch

        private int Call(string what, Func<int> f, int unavailable)
        {
            try { return f(); }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException || e is BadImageFormatException)
            {
                Log("shell: " + what + " can't be called (" + e.GetType().Name + ")");
                return unavailable;
            }
        }

        public int SwapWait(uint ms) => Call("core_win_swap_wait", () => CoreWin.core_win_swap_wait(support, app, own, ms, logFn, IntPtr.Zero), -1);
        public int SwapFailed() => Call("core_win_swap_failed", () => CoreWin.core_win_swap_failed(support, app, own, logFn, IntPtr.Zero), -1);
        public int OsCheck() => Call("core_win_os_check", CoreWin.core_win_os_check, -1);
        public int TokenCheck() => Call("core_win_token_check", CoreWin.core_win_token_check, -1);
        public int AtStart() => Call("core_win_update_at_start", () => CoreWin.core_win_update_at_start(support, app, own, logFn, IntPtr.Zero), StartSequence.UpdateFailNone);

        public string ShownVersion()
        {
            var version = new byte[64];   // room for update.h's PAYLOAD_VERSION_MAX
            int n = Call("core_win_update_shown", () => CoreWin.core_win_update_shown(support, own, version, (UIntPtr)version.Length), -1);
            return n > 0 && n < version.Length ? Encoding.UTF8.GetString(version, 0, n) : own;
        }

        /// Without the core (WU9): the old app named by update\pending (by pid, checked by its start
        /// time within a second, as update_win.c's same_start) waited for, at most ms.
        public void WaitOldApp(uint ms)
        {
            PendingRecord p;
            try { p = PendingRecord.Parse(File.ReadAllText(Path.Combine(support, "update", "pending"), new UTF8Encoding(false))); }
            catch (Exception) { return; }
            if (p == null || p.To != own) return;
            IntPtr h = Win32.OpenProcess(Win32.SYNCHRONIZE | Win32.PROCESS_QUERY_LIMITED_INFORMATION, 0, p.Pid);
            if (h == IntPtr.Zero) return;   // gone
            try
            {
                if (Win32.GetProcessTimes(h, out long created, out _, out _, out _) == 0) return;
                long s = DateTime.FromFileTimeUtc(created).Subtract(new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).Ticks / TimeSpan.TicksPerSecond;
                if (Math.Abs(s - p.Started) > 1) return;   // another process got the pid
                if (Win32.WaitForSingleObject(h, ms) != Win32.WAIT_OBJECT_0) Log("--swap: the app being replaced still runs after " + ms / 1000 + " s");
            }
            finally { Win32.CloseHandle(h); }
        }

        /// The guard's recovery must not need the loader once the UI thread may hang: bound now.
        public void Prelink()
        {
            try { Marshal.Prelink(typeof(CoreWin).GetMethod(nameof(CoreWin.core_win_swap_failed))); }
            catch (Exception e) { Log("shell: core_win_swap_failed can't be bound (" + e.GetType().Name + ")"); }
        }

        /// SetCurrentProcessExplicitAppUserModelID before the first window (aumid.h's one spelling).
        public void SetAumid()
        {
            if (Call("core_win_set_aumid", CoreWin.core_win_set_aumid, -1) != 0) Log("shell: the AppUserModelID couldn't be set");
        }

        /// After the first full snapshot is on screen, once: `started` and the pointers (or the
        /// leftovers). Called by the close gate's handoff worker.
        public void AfterRender()
        {
            int r = Call("core_win_update_after_render", () => CoreWin.core_win_update_after_render(support, app, own, logFn, IntPtr.Zero), -1);
            if (r > 0) Log("update: the commit didn't finish (" + r + "): the next start finishes it");
        }

        /// The stub next to the version folders (%LOCALAPPDATA%\Programs\RB Stems Plus\RB Stems Plus.exe),
        /// started with no arguments when the core can't do it (--swap without a usable core).
        public bool StartStub()
        {
            try
            {
                string stub = Path.Combine(Path.GetDirectoryName(app.TrimEnd('\\')), "RB Stems Plus.exe");
                if (!File.Exists(stub)) return false;
                using (Process.Start(new ProcessStartInfo(stub) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(stub) })) { }
                return true;
            }
            catch (Exception e)
            {
                Log("shell: the stub couldn't be started (" + e.Message + ")");
                return false;
            }
        }

        /// The core's "mount"/"unmount" events for one WM_DEVICECHANGE (vol_device_events), each one JSON object.
        public string[] DeviceEvents(uint ev, uint devtype, uint unitmask)
        {
            var buf = new byte[1024];
            int n = Call("core_win_device_events", () => CoreWin.core_win_device_events(ev, devtype, unitmask, null, buf, (UIntPtr)buf.Length), -1);
            if (n <= 0) return new string[0];
            int len = Array.IndexOf(buf, (byte)0);
            return Encoding.UTF8.GetString(buf, 0, len < 0 ? buf.Length : len).Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
        }
    }

    /// CriticalWork through the core DLL. A DLL without its entries (an older one): nothing counts
    /// as running (logged once), so the close and the logoff wait for nothing.
    internal sealed class CriticalWork
    {
        private readonly Action<string> log;
        private int missing;

        public CriticalWork(Action<string> log) { this.log = log; }

        public bool Running() => Call(() => CoreWin.core_win_critical_running() != 0, false);
        public bool WaitIdle(int ms) => Call(() => CoreWin.core_win_critical_wait((uint)Math.Max(0, ms)) != 0, true);
        public void Close() => Call(() => { CoreWin.core_win_critical_close(); return true; }, true);

        private bool Call(Func<bool> f, bool absent)
        {
            if (Volatile.Read(ref missing) != 0) return absent;
            try { return f(); }
            catch (EntryPointNotFoundException)
            {
                if (Interlocked.Exchange(ref missing, 1) == 0) log("shell: this core DLL has no CriticalWork entry: quitting waits for nothing");
                return absent;
            }
        }
    }
}
