//Renders the large app icon sizes from the game logo.
//Usage: swift source/macos/make_icon.swift data/images/sr_logo.png out.png
import AppKit

let args = CommandLine.arguments
guard args.count == 3, let logo = NSImage(contentsOfFile: args[1]) else {
	FileHandle.standardError.write("usage: make_icon.swift logo.png out.png\n".data(using: .utf8)!)
	exit(1)
}

let size = 1024
guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
		bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
		colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { exit(1) }
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

//macOS icon grid: an 824 pt rounded square centred on the 1024 canvas
let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
let path = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
NSGradient(starting: NSColor(calibratedRed: 0.10, green: 0.13, blue: 0.22, alpha: 1),
		ending: NSColor(calibratedRed: 0.02, green: 0.03, blue: 0.07, alpha: 1))!.draw(in: path, angle: -90)

//Faint glow behind the logo
NSGraphicsContext.current!.saveGraphicsState()
path.addClip()
NSGradient(starting: NSColor(calibratedRed: 0.35, green: 0.45, blue: 0.75, alpha: 0.35),
		ending: NSColor(calibratedRed: 0.0, green: 0.0, blue: 0.0, alpha: 0))!
	.draw(fromCenter: NSPoint(x: 512, y: 512), radius: 0, toCenter: NSPoint(x: 512, y: 512), radius: 420, options: [])
NSGraphicsContext.current!.restoreGraphicsState()

let width: CGFloat = 740
let height = width * logo.size.height / logo.size.width
logo.draw(in: NSRect(x: (CGFloat(size) - width) / 2, y: (CGFloat(size) - height) / 2, width: width, height: height),
		from: .zero, operation: .sourceOver, fraction: 1)

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
