import AppKit
// full-bleed square for Icon Composer that keeps the original proportions: the whole rounded
// square of the artwork (its opaque body, 1144 x 1131 px at 55,61) scaled to 1024, on the
// artwork's dark background; macOS's own icon shape trims the outside
let src = NSImage(contentsOfFile: CommandLine.arguments[1])!
let bx = 55.0, byTop = 61.0, bw = 1144.0, bh = 1131.0
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 1024, pixelsHigh: 1024, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSGraphicsContext.current?.imageInterpolation = .high
NSColor(srgbRed: 0.07, green: 0.09, blue: 0.12, alpha: 1).setFill()
NSRect(x: 0, y: 0, width: 1024, height: 1024).fill()
src.draw(in: NSRect(x: 0, y: 0, width: 1024, height: 1024),
         from: NSRect(x: bx, y: 1254 - (byTop + bh), width: bw, height: bh), operation: .sourceOver, fraction: 1)
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
