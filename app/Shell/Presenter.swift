// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.5, §3.7, M7): the
// dialog presenter. A flow (or the admin path, or the quit flow) asks for a dialog from its own
// thread and blocks there; main presents it as one app-modal presentation:
//
//   - scheduled with CFRunLoopPerformBlock on the main run loop in the common modes, so it runs
//     outside any dispatch block (today's onMainSync { Sheet.run } held the main queue under the
//     dialog; this doesn't: the main queue keeps draining in the modal mode, so renders, progress
//     and rows keep updating under every dialog: the one recorded difference G2.1 waives);
//   - followed at once by CFRunLoopWakeUp: CFRunLoopPerformBlock only enqueues, and an idle app
//     would show nothing until its next event;
//   - the body runs today's Sheet.run(on:) (beginSheet, NSApp.runModal(for:), endSheet), so the
//     dialog is an app-modal sheet as in 1.3.0: the main menu is disabled and NSApp.modalWindow
//     is set while it is up. Never beginSheet with a completion handler (document-modal);
//   - one at a time, in call order (core.h "Dialogs": the quit flow, the normal flow and the
//     watcher share one modal count): a ticket per call, served in order;
//   - once the host is shutting down, every pending and later call answers "not shown".
//
// Only the calling thread blocks, on a semaphore the body's end signals. Never called on main
// (a core callback on main never shows a dialog, core.h). Every build (the app is this shell).
import AppKit

final class DialogPresenter {
    static let shared = DialogPresenter()
    #if TEST_BUILD
    init() {}                                            // the app's is `shared`; a test makes its own
    #else
    private init() {}
    #endif
    private let order = NSCondition()
    private var nextTicket: UInt64 = 0
    private var serving: UInt64 = 0
    private var closing = false

    /// Runs `body` on main as one app-modal presentation and returns its result; blocks the calling
    /// thread until then. nil: called on main (refused), or the host is shutting down.
    func present<T>(_ body: @escaping () -> T) -> T? {
        guard !Thread.isMainThread else { return nil }
        order.lock()
        if closing { order.unlock(); return nil }
        let ticket = nextTicket
        nextTicket += 1
        while serving != ticket && !closing { order.wait() }
        let refused = closing
        order.unlock()
        defer {
            order.lock()
            serving += 1
            order.broadcast()
            order.unlock()
        }
        if refused { return nil }
        // its turn: scheduled on main as a request that shutDown can still cancel until main
        // starts it (every state change under `order`, so a cancel and a start can't both win)
        let req = Request()
        order.lock()
        if closing { order.unlock(); return nil }
        scheduled.append(req)
        order.unlock()
        let box = ResultBox<T>()
        let main = CFRunLoopGetMain()
        CFRunLoopPerformBlock(main, CFRunLoopMode.commonModes.rawValue) { [self] in
            order.lock()
            let start = req.state == .pending
            if start { req.state = .running }
            order.unlock()
            guard start else { return }                      // cancelled at shutdown: never shown
            box.value = body()
            req.done.signal()
        }
        CFRunLoopWakeUp(main)
        req.done.wait()
        order.lock()
        scheduled.removeAll { $0 === req }
        let cancelled = req.state == .cancelled
        order.unlock()
        return cancelled ? nil : box.value
    }

    /// The host is shutting down: presentations that haven't started answer nil at once (their
    /// body never runs), and later ones too. One already up runs to its end.
    func shutDown() {
        order.lock()
        closing = true
        for r in scheduled where r.state == .pending {
            r.state = .cancelled
            r.done.signal()
        }
        order.broadcast()
        order.unlock()
    }

    var isShuttingDown: Bool { order.lock(); defer { order.unlock() }; return closing }

    /// The request scheduled now, if any (at most one: they are served in turn), under `order`.
    private var scheduled: [Request] = []
}

/// One scheduled presentation: pending until main starts it, then running; cancelled by shutDown
/// while still pending. `done` is signalled once: by the body's end, or by the cancel.
private final class Request {
    enum State { case pending, running, cancelled }
    var state = State.pending
    let done = DispatchSemaphore(value: 0)
}

/// The body's result, written on main before the semaphore is signalled (which orders it before
/// the waiting thread reads it).
private final class ResultBox<T> { var value: T? }
