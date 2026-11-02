// Renders docs/assets/tsugumi-icon.svg into macOS/Resources/AppIcon.icns.
//
// Run `swift macOS/Resources/tools/make_icon.swift` after changing the SVG;
// build-app.sh bundles the committed AppIcon.icns.
import AppKit

let repo = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()
let svg = repo.appendingPathComponent("docs/assets/tsugumi-icon.svg")
let icns = repo.appendingPathComponent("macOS/Resources/AppIcon.icns")

guard let image = NSImage(contentsOf: svg) else {
    fatalError("cannot load \(svg.path)")
}

let iconset = FileManager.default.temporaryDirectory
    .appendingPathComponent("AppIcon-\(ProcessInfo.processInfo.processIdentifier).iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: iconset) }

for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let px = points * scale
        guard let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)
        else { fatalError("cannot allocate \(px)px bitmap") }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        NSGraphicsContext.current?.imageInterpolation = .high
        image.draw(in: NSRect(x: 0, y: 0, width: px, height: px))
        NSGraphicsContext.restoreGraphicsState()
        let name = scale == 1 ? "icon_\(points)x\(points).png" : "icon_\(points)x\(points)@2x.png"
        try rep.representation(using: .png, properties: [:])!
            .write(to: iconset.appendingPathComponent(name))
    }
}

let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", icns.path]
try iconutil.run()
iconutil.waitUntilExit()
guard iconutil.terminationStatus == 0 else { fatalError("iconutil failed") }
print(icns.path)
