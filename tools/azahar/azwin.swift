// Prints Azahar's main window id (largest layer-0 window).
import AppKit
let apps = NSWorkspace.shared.runningApplications.filter { ($0.executableURL?.path ?? "").hasSuffix("MacOS/azahar") }
guard let pid = apps.first?.processIdentifier else { exit(1) }
var win = 0, best = 0
if let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] {
  for w in list where (w[kCGWindowOwnerPID as String] as? Int32) == pid && (w[kCGWindowLayer as String] as? Int) == 0 {
    let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]
    let area = Int((b["Width"] ?? 0) * (b["Height"] ?? 0))
    if area > best { best = area; win = w[kCGWindowNumber as String] as? Int ?? 0 }
  }
}
print(win)
