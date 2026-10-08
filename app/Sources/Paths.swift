// The shared contract: every path, name and fixed value the app, the watcher, the admin scripts
// and the bridge agree on. Change them here only.
import Foundation

/// Where the release assets are downloaded from (one GitHub Release per version).
/// The latest release of github.com/Gabe-LS/rbstemsplus; the file names are in payload.json (Payload.swift).
/// A test build (scripts/build.sh --test URL) downloads from a local test server instead. That
/// is decided when the app is compiled, never by a setting or the environment: the bridge root
/// installs comes from here. A release build contains no "TEST BUILD" text (build.sh checks).
/// curl gets only HTTPS, also after redirects; a test build may use plain HTTP.
#if TEST_BUILD
let downloadBase = testDownloadBase                 // build/app/TestBase.swift, from build-app.sh
let buildNote = " - TEST BUILD, downloads from " + testDownloadBase   // over 15 bytes: kept as text in the binary
let curlProtocols = "=http,https"
#else
let downloadBase = "https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/"
let buildNote = ""
let curlProtocols = "=https"
#endif
/// The window title's addition: the build note, except in a test build made for the docs'
/// screenshots (RBSTEMSPLUS_SCREENSHOTS=1 scripts/build.sh --test URL), whose log still says it.
#if SCREENSHOT_BUILD
let titleNote = ""
#else
let titleNote = buildNote
#endif

/// The project's home page (Help › RB Stems Plus Help, and the About panel).
let homeURL = "https://github.com/Gabe-LS/rbstemsplus"
/// The troubleshooting page (Help › RB Stems Plus Troubleshooting); its sections below.
let troubleshootingURL = homeURL + "/blob/main/docs/troubleshooting.md"
/// The issues page (Help › Report an Issue).
let issuesPageURL = homeURL + "/issues"
/// Where a problem report is filed: a new GitHub issue, prefilled by the app.
let issuesURL = homeURL + "/issues/new"
/// How to get rekordbox's own stems model back without a saved copy (after "Remove RB Stems Plus anyway").
let noSavedModelHelpURL = troubleshootingURL + "#no-saved-copy-of-rekordboxs-model"
/// How to put the saved copy of rekordbox's model back by hand (after "Remove RB Stems Plus anyway" kept it).
let removeAnywayHelpURL = troubleshootingURL + "#remove-rb-stems-plus-anyway"

/// The app's bundle ID (Info.plist): only a bundle with it is ever deleted or replaced.
let bundleID = "io.github.rbstemsplus.app"

/// The app's version, from its Info.plist (shown in the log only, never in the window).
let appVersion = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
let appBuild = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"

// rekordbox
let rbApp = "/Applications/rekordbox 7/rekordbox.app"
let rbLib = rbApp + "/Contents/Frameworks/libonnxruntime.1.18.0.dylib"
let home = accountHome()                  // the password database's, never HOME (Shell.swift)
let rbSettingsDir = home + "/Library/Application Support/Pioneer/rekordbox6"
let modelDir = rbSettingsDir + "/models/demucs3_model"

/// The Team ID that signs Pioneer's rekordbox and its ONNX Runtime (AlphaTheta).
let pioneerTeam = "6BRHGXQ6VU"

// our model, as installed in rekordbox's model folder: the 1.0.0 export. payload.json names the
// current one; this one is still recognised after a payload update.
let ourModel = "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772"
let ourModelSize = 308_572_524

// user files
let support = home + "/Library/Application Support/rbstemsplus"
let chosenFile = support + "/installed"       // what the user chose to install: "model=1\ncache=1"
let dismissedFile = support + "/dismissed"    // prompts the user chose not to see again
let requestFile = support + "/request"        // "reinstall", written by the watcher for the app
let busyFile = support + "/busy"              // pid, start time and action while the app changes things (State.swift)
let configFile = support + "/config.ini"      // enabled, max_gb, max_days (read by the bridge too)
let originalsDir = support + "/originals"     // Pioneer's models: <sha256>.onnx and .engine (Models.swift)
let payloadRoot = support + "/payload"        // payload.json (the last good one) and <payload version>/
let welcomedFile = support + "/welcomed"      // the welcome sheet was shown
let updateDir = support + "/update"           // a new app, unpacked, waiting to replace this one
let watcherLock = support + "/watcher.lock"       // a folder (mkdir), held by the running check
let appLock = support + "/app.lock"           // held by the open app: one copy at a time
let logDir = home + "/Library/Logs/rbstemsplus"
let logPath = logDir + "/app.log"
let watcherLogPath = logDir + "/watcher.log"
let bridgeLogPath = logDir + "/bridge.log"
let cacheDir = home + "/Library/Caches/rbstemsplus"   // the stems Stems Cache saved

// root files (Stems Cache only): Pioneer's real library in ort/, the three originals in pioneer/<version>/
let rootDir = "/Library/Application Support/rbstemsplus"
let rootOrt = rootDir + "/ort/libonnxruntime.1.18.0.dylib"   // the copy the bridge loads
/// The prototype's root folder (the developer's own Mac only): its bridge loads Pioneer's library
/// from here, and this app can't reinstall or uninstall it (docs/RELEASING.md, "The prototype").
let prototypeRootDir = "/Library/Application Support/StemsPlus"

// the watcher: a helper app inside the app, so macOS names it apart (Login Items, the background
// notice, Activity Monitor, crash reports). Its executable is a copy of the app's, signed as the
// helper (scripts/build-app.sh); it runs as the watcher because of the bundle it runs from.
let watcherBundleID = "io.github.rbstemsplus.watcher"
let watcherName = "RB Stems Plus Watcher"
/// The helper inside the app (Apple's place for helper apps: Contents/Helpers).
let watcherHelperPath = "Contents/Helpers/\(watcherName).app"
let agentLabel = watcherBundleID
let agentPlist = home + "/Library/LaunchAgents/\(agentLabel).plist"
let guiDomain = "gui/\(getuid())"                     // launchctl's name for this user's session
/// The code the LaunchAgent was registered with: its executable, then that code's identity
/// (watcherIdentity). Another identity (an update, the install command) means registering again.
let watcherRegisteredFile = support + "/watcher.registered"
/// Left by a watcher start that found another check running: that check looks again.
let watcherAgainFile = support + "/watcher.again"

// an update's staging names, in the app's own folder (/Applications, normally), so both moves are
// renames on one disk. Deleted only with our bundle ID (Safety.swift, and the update script).
let updateStagingName = ".RB Stems Plus.update.app"
let updateOldName = ".RB Stems Plus.old.app"
let updateStagingApp = (Bundle.main.bundlePath as NSString).deletingLastPathComponent + "/" + updateStagingName
let updateOldApp = (Bundle.main.bundlePath as NSString).deletingLastPathComponent + "/" + updateOldName
/// The install command's staging names (scripts/bootstrap.sh), left behind if it was interrupted.
let bootstrapNewApp = "/Applications/.RB Stems Plus.new.app"
let bootstrapOldApp = "/Applications/.RB Stems Plus.bootstrap-old.app"
