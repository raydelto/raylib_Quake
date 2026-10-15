// Lists windows owned by a PID as JSON (id, layer, onscreen, bounds). Needs no TCC permission.
import CoreGraphics
import Foundation

guard CommandLine.arguments.count > 1, let pid = Int32(CommandLine.arguments[1]) else {
	FileHandle.standardError.write("usage: macos_windows <pid>\n".data(using: .utf8)!)
	exit(2)
}
let info = (CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]]) ?? []
let mine = info.filter { ($0[kCGWindowOwnerPID as String] as? Int32) == pid }.map { w -> [String: Any] in
	["id": w[kCGWindowNumber as String] ?? 0, "layer": w[kCGWindowLayer as String] ?? 0,
	 "onscreen": (w[kCGWindowIsOnscreen as String] as? Bool) ?? false,
	 "bounds": w[kCGWindowBounds as String] ?? [:]]
}
let data = try JSONSerialization.data(withJSONObject: mine, options: [.sortedKeys])
print(String(data: data, encoding: .utf8)!)
