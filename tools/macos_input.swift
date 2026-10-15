// Synthetic input for the macOS harness. Needs the Accessibility permission for the
// terminal/agent that runs it (System Settings > Privacy & Security > Accessibility).
//   macos_input check
//   macos_input activate <pid>
//   macos_input key <pid> <virtual-keycode> <hold-ms> [cmd|alt|ctrl|shift]   (key held >= 100 ms)
//   macos_input mouse <pid> <dx> <dy> <steps>           (relative motion, per step)
//   macos_input click <pid> <hold-ms>                   (left button at the pointer)
import ApplicationServices
import AppKit
import Foundation

func fail(_ message: String) -> Never {
	FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
	exit(2)
}
let args = CommandLine.arguments
guard args.count > 1 else { fail("usage: macos_input check|activate|key|mouse|click ...") }
if args[1] == "check" {
	print(AXIsProcessTrusted() ? "accessibility: granted" : "accessibility: NOT granted")
	exit(AXIsProcessTrusted() ? 0 : 1)
}
guard AXIsProcessTrusted() else { fail("Accessibility permission not granted") }
guard args.count > 2, let pid = Int32(args[2]) else { fail("missing pid") }
let src = CGEventSource(stateID: .hidSystemState)
switch args[1] {
case "activate":
	guard let app = NSRunningApplication(processIdentifier: pid) else { fail("no such process") }
	app.activate(options: [.activateIgnoringOtherApps])
case "key":
	guard args.count > 4, let code = UInt16(args[3]), let hold = UInt32(args[4]) else { fail("key <pid> <code> <ms>") }
	var flags: CGEventFlags = []
	var modcode: UInt16 = 0
	if args.count > 5 {
		switch args[5] {
		case "cmd": flags = .maskCommand; modcode = 55
		case "alt": flags = .maskAlternate; modcode = 58
		case "ctrl": flags = .maskControl; modcode = 59
		case "shift": flags = .maskShift; modcode = 56
		default: fail("unknown modifier")
		}
	}
	// A real modifier press first (flagsChanged), so apps that poll modifier key state see it.
	func modifier(_ on: Bool) {
		guard modcode != 0, let m = CGEvent(keyboardEventSource: src, virtualKey: modcode, keyDown: on) else { return }
		m.type = .flagsChanged
		m.flags = on ? flags : []
		m.postToPid(pid)
		usleep(60000)
	}
	modifier(true)
	let down = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true)
	down?.flags = flags
	down?.postToPid(pid)
	usleep(max(hold, 100) * 1000)
	let up = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)
	up?.flags = flags
	up?.postToPid(pid)
	usleep(60000)
	modifier(false)
case "mouse":
	guard args.count > 5, let dx = Int64(args[3]), let dy = Int64(args[4]), let steps = Int(args[5]) else { fail("mouse <pid> <dx> <dy> <steps>") }
	for _ in 0..<steps {
		if let e = CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: CGEvent(source: nil)!.location, mouseButton: .left) {
			e.setIntegerValueField(.mouseEventDeltaX, value: dx)
			e.setIntegerValueField(.mouseEventDeltaY, value: dy)
			e.postToPid(pid)
		}
		usleep(16000)
	}
case "click":
	guard args.count > 3, let hold = UInt32(args[3]) else { fail("click <pid> <ms>") }
	let at = CGEvent(source: nil)!.location
	CGEvent(mouseEventSource: src, mouseType: .leftMouseDown, mouseCursorPosition: at, mouseButton: .left)?.postToPid(pid)
	usleep(max(hold, 100) * 1000)
	CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: at, mouseButton: .left)?.postToPid(pid)
default:
	fail("unknown command")
}
