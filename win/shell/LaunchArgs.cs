// The app's --uninstall (release.md §4, app.md §5.9): the Apps & features entry's UninstallString
// is "<stub>" --uninstall (lifecycle.h's life_uninstall_string), and the stub passes its arguments
// to the app it starts. Such a start opens straight into the complete uninstall: the click
// "uninstall_from_settings" (core/flows_win.c), sent once after the window's first "shown". The
// core holds it until the start-up flow is done (Welcome, an update's result, ...) and then asks
// "Uninstall RB Stems Plus?" over the app's window; the menu's "uninstall_all" would be dropped
// once the start-up flow had asked something, and is off until the first refresh (review UN2).
// With another copy open it refuses cleanly: that copy comes forward (or is busy) and this start
// says "Already open" and ends, never a second uninstall. Without WPF: the Mac tests run it.
using System;

namespace RbStemsPlus.Shell
{
    internal static class LaunchArgs
    {
        public const string Uninstall = "--uninstall";
        public const string UninstallClick = "uninstall_from_settings";   // core/flows_win.c's win_flows_event

        /// whether this start is the Apps & features uninstall: --uninstall as its one argument
        /// (anything else, --swap among it, is another start)
        public static bool IsUninstall(string[] args) => args != null && args.Length == 1 && args[0] == Uninstall;

        /// the one-copy answer under --uninstall: a copy that came forward (ExitQuietly) is said, as
        /// a busy one is, so the user knows the uninstall is in that window; the others unchanged
        public static OneCopyStart ForUninstall(OneCopyStart s) => s == OneCopyStart.ExitQuietly ? OneCopyStart.AlreadyOpen : s;
    }

    /// The shell's quit callback's kind (update_shell.h's core_win_set_quit): ctx 1
    /// (CORE_WIN_QUIT_UNINSTALL) is the complete uninstall's SELF_DELETE, whose REMOVE_OWN_FOLDERS
    /// already deleted %LOCALAPPDATA%\rbstemsplus: from then on no log file line and no window
    /// place are written, so nothing makes the folder again (review UN5); ctx 0 (an update's
    /// UPDATE_SWAP) keeps both. Any thread; read on main by Closing.
    internal sealed class QuitKind
    {
        public static readonly IntPtr Uninstall = (IntPtr)1;   // update_shell.h's CORE_WIN_QUIT_UNINSTALL
        private readonly LogBuffer log;
        private volatile bool uninstalled;

        public QuitKind(LogBuffer log) { this.log = log; }

        /// the quit callback's ctx, before the quit is posted
        public void Quit(IntPtr ctx)
        {
            if (ctx != Uninstall) return;
            uninstalled = true;
            log?.StopFile();
        }

        /// Closing: whether the window's size and position are kept
        public bool SavePlace => !uninstalled;
    }

    /// --uninstall's request: the click "uninstall_from_settings", once, at the first "shown"; later
    /// frames send nothing
    internal sealed class UninstallOnce
    {
        private readonly Func<string, int> send;
        private readonly Action<string> log;
        private bool pending;

        public UninstallOnce(bool uninstall, Func<string, int> send, Action<string> log)
        {
            pending = uninstall;
            this.send = send;
            this.log = log;
        }

        /// at "shown": true when the request went now
        public bool Shown()
        {
            if (!pending) return false;
            pending = false;
            log("shell: started by --uninstall (Settings > Apps): the complete uninstall");
            send(Json.Click(LaunchArgs.UninstallClick));
            return true;
        }
    }
}
