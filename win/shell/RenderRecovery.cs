using System;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace RbStemsPlus.Shell
{
    // WPF's render thread posts this per-HWND message when it cannot present (including when
    // occluded). ContentRendered is a startup fallback: it does not confirm a successful present.
    internal sealed class RenderRecovery
    {
        private static readonly int presentFailedMessage = Win32.RegisterWindowMessageW("NeedsRePresentOnWake");

        private readonly Window window;
        private readonly Action<string> log;
        private readonly Func<bool> sessionResetPending;
        private HwndSource source;
        private DispatcherTimer firstFrameTimeout, restore;
        private DispatcherOperation check;
        private bool presentFailed, firstFrameTimedOut, alreadyTried;

        public RenderRecovery(Window window, Action<string> log, Func<bool> sessionResetPending = null)
        {
            this.window = window;
            this.log = log;
            this.sessionResetPending = sessionResetPending;
            window.SourceInitialized += OnSourceInitialized;
            window.Loaded += OnLoaded;
            window.ContentRendered += OnContentRendered;
            window.Activated += OnCheck;
            window.StateChanged += OnCheck;
            window.Deactivated += OnDeactivated;
            window.Closed += OnClosed;
        }

        private void OnSourceInitialized(object sender, EventArgs e)
        {
            window.SourceInitialized -= OnSourceInitialized;
            source = HwndSource.FromHwnd(new WindowInteropHelper(window).Handle);
            if (presentFailedMessage != 0) source?.AddHook(Hook);
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            window.Loaded -= OnLoaded;
            firstFrameTimeout = new DispatcherTimer(TimeSpan.FromMilliseconds(750), DispatcherPriority.Background,
                (s, args) =>
                {
                    firstFrameTimeout.Stop();
                    firstFrameTimeout = null;
                    firstFrameTimedOut = true;
                    Check();
                }, window.Dispatcher);
        }

        private void OnContentRendered(object sender, EventArgs e) => StopFirstFrameCheck();

        private void StopFirstFrameCheck()
        {
            window.Loaded -= OnLoaded;
            window.ContentRendered -= OnContentRendered;
            firstFrameTimeout?.Stop();
            firstFrameTimeout = null;
            firstFrameTimedOut = false;
        }

        private bool CanCheck => window.IsVisible && window.IsActive && window.WindowState != WindowState.Minimized
            && sessionResetPending?.Invoke() != true;

        private IntPtr Hook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg == presentFailedMessage && CanCheck && restore == null)
            {
                presentFailed = true;
                QueueCheck();
            }
            return IntPtr.Zero; // WPF still handles the message.
        }

        private void OnCheck(object sender, EventArgs e) => QueueCheck();

        private void OnDeactivated(object sender, EventArgs e) => presentFailed = false;

        private void QueueCheck()
        {
            if (check != null || (!presentFailed && !firstFrameTimedOut)) return;
            // Let HwndTarget finish its own message handling before switching its render mode.
            check = window.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
            {
                check = null;
                Check();
            }));
        }

        private void Check()
        {
            RenderResetReason reason = RenderResetDecision.Choose(CanCheck, alreadyTried, presentFailed, firstFrameTimedOut);
            presentFailed = false;
            if (reason != RenderResetReason.None && Reset(reason))
            {
                alreadyTried = true;
                StopChecking();
            }
        }

        public bool Reset(RenderResetReason reason)
        {
            if (restore != null || !window.IsVisible || window.WindowState == WindowState.Minimized) return false;
            var target = source?.CompositionTarget;
            if (target == null || target.RenderMode == RenderMode.SoftwareOnly) return false;

            presentFailed = firstFrameTimedOut = false;
            RenderMode original = target.RenderMode;
            target.RenderMode = RenderMode.SoftwareOnly;
            restore = new DispatcherTimer(TimeSpan.FromMilliseconds(250), DispatcherPriority.Background,
                (s, e) =>
                {
                    restore.Stop();
                    restore = null;
                    if (source == null || source.IsDisposed || source.CompositionTarget != target) return;
                    target.RenderMode = original;
                    Invalidate();
                    log?.Invoke(reason switch
                    {
                        RenderResetReason.SessionChange => "shell: session render target reset",
                        RenderResetReason.PresentFailed => "shell: render target reset (present failed)",
                        _ => "shell: render target reset (first frame timeout)",
                    });
                }, window.Dispatcher);
            Invalidate();
            return true;
        }

        private void Invalidate()
        {
            (window.Content as UIElement)?.InvalidateVisual();
            Win32.InvalidateRect(source.Handle, IntPtr.Zero, 0);
        }

        private void StopChecking()
        {
            StopFirstFrameCheck();
            check?.Abort();
            check = null;
            window.Activated -= OnCheck;
            window.StateChanged -= OnCheck;
            window.Deactivated -= OnDeactivated;
            if (presentFailedMessage != 0 && source?.IsDisposed == false) source.RemoveHook(Hook);
        }

        private void OnClosed(object sender, EventArgs e)
        {
            if (!alreadyTried) StopChecking();
            restore?.Stop();
            restore = null;
            source = null;
        }
    }
}
