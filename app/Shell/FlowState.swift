// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.5, §4, M7): the
// state one flow's steps share. core.h ("The flows' steps") marks the steps that use the flow's
// payload.json and bridge bytes: those of the last PAYLOAD "latest"/"saved" and the last
// CACHE_PREPARE on the flow thread. The host keeps them here; "check" and "fetch" never change
// them, and a new flow replaces them at its first PAYLOAD. host->admin's installs use exactly the
// bytes CACHE_PREPARE read and checked (today's Actions.swift reads them once, then
// cacheInstallScript gets them). Every build (M7's switch-over: the app is this shell).
import Foundation

final class ShellFlowState {
    static let shared = ShellFlowState()
    private let lock = NSLock()
    private var _manifest: Manifest?
    private var _bridge: Data?
    private var _bridgeSha = ""
    private var _ortSha = ""

    private var _report: ReportMeasure?

    /// REPORT_FACTS' measurement, which the report's REPORT_ZIP then uses as it is.
    var report: ReportMeasure? {
        get { lock.lock(); defer { lock.unlock() }; return _report }
        set { lock.lock(); _report = newValue; lock.unlock() }
    }

    /// The flow's payload.json (PAYLOAD "latest" / "saved"); nil before the first one.
    var manifest: Manifest? {
        get { lock.lock(); defer { lock.unlock() }; return _manifest }
        set { lock.lock(); _manifest = newValue; _bridge = nil; _bridgeSha = ""; _ortSha = ""; lock.unlock() }
    }

    /// CACHE_PREPARE's result: the payload's bridge read once and checked, and Pioneer's ONNX
    /// Runtime's sha256 ("unreadable" when it couldn't be read).
    func setPrepared(bridge: Data, bridgeSha: String, ortSha: String) {
        lock.lock(); _bridge = bridge; _bridgeSha = bridgeSha; _ortSha = ortSha; lock.unlock()
    }

    /// The bridge bytes CACHE_PREPARE checked, when their sha256 is `sha` (the one the core passes
    /// to host->admin); nil otherwise: the admin run is then refused, never run with other bytes.
    func preparedBridge(sha: String) -> Data? {
        lock.lock(); defer { lock.unlock() }
        guard let b = _bridge, !sha.isEmpty, sha == _bridgeSha else { return nil }
        return b
    }
}
