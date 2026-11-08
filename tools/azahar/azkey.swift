// Sends one key press (down, hold, up) to Azahar's main window only, by Latin letter.
// Usage: azkey <letter>[:seconds]   e.g. azkey a   azkey z:2.5
import AppKit
let codes: [String: UInt16] = ["a":0,"s":1,"d":2,"f":3,"h":4,"g":5,"z":6,"x":7,"c":8,"v":9,"b":11,"q":12,"w":13,"e":14,"r":15,"y":16,"t":17,"n":45,"m":46]
let arg = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "a"
let parts = arg.split(separator: ":")
let key = String(parts[0]); let hold = parts.count > 1 ? Double(parts[1]) ?? 0.2 : 0.2
guard let code = codes[key] else { print("unknown key"); exit(2) }
let apps = NSRunningApplication.runningApplications(withBundleIdentifier: "org.azahar-emu.azahar") + NSWorkspace.shared.runningApplications.filter { ($0.executableURL?.path ?? "").hasSuffix("MacOS/azahar") }
guard let app = apps.first else { print("azahar not running"); exit(1) }
let pid = app.processIdentifier
var win = 0, best = 0
if let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] {
  for w in list where (w[kCGWindowOwnerPID as String] as? Int32) == pid && (w[kCGWindowLayer as String] as? Int) == 0 {
    let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]
    let area = Int((b["Width"] ?? 0) * (b["Height"] ?? 0))
    if area > best { best = area; win = w[kCGWindowNumber as String] as? Int ?? 0 }
  }
}
func send(_ type: NSEvent.EventType) {
  let e = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
    windowNumber: win, context: nil, characters: key, charactersIgnoringModifiers: key, isARepeat: false, keyCode: code)
  e?.cgEvent?.postToPid(pid)
}
send(.keyDown); usleep(useconds_t(hold * 1_000_000)); send(.keyUp)
print("sent \(key) to pid \(pid) window \(win)")
