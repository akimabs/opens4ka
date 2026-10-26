import AppKit
import Foundation

guard CommandLine.arguments.count == 3 else {
    fputs("usage: create-dmg-background.swift <logo> <output>\n", stderr)
    exit(EXIT_FAILURE)
}

let logoURL = URL(fileURLWithPath: CommandLine.arguments[1])
let outputURL = URL(fileURLWithPath: CommandLine.arguments[2])
let width: CGFloat = 760
let height: CGFloat = 480

guard let bitmap = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: Int(width),
    pixelsHigh: Int(height),
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 32
) else {
    fputs("could not create bitmap\n", stderr)
    exit(EXIT_FAILURE)
}

guard let graphicsContext = NSGraphicsContext(bitmapImageRep: bitmap) else {
    fputs("could not create graphics context\n", stderr)
    exit(EXIT_FAILURE)
}

NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = graphicsContext
let context = graphicsContext.cgContext
context.setAllowsAntialiasing(true)

func color(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(calibratedRed: red, green: green, blue: blue, alpha: alpha)
}

func drawText(_ value: String, at point: NSPoint, font: NSFont, color: NSColor, centered: Bool = false) {
    let attributes: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: color
    ]
    let size = value.size(withAttributes: attributes)
    let origin = centered ? NSPoint(x: point.x - size.width / 2, y: point.y) : point
    (value as NSString).draw(at: origin, withAttributes: attributes)
}

let startColor = color(0.025, 0.045, 0.095).cgColor
let endColor = color(0.055, 0.145, 0.275).cgColor
if let gradient = CGGradient(
    colorsSpace: CGColorSpaceCreateDeviceRGB(),
    colors: [startColor, endColor] as CFArray,
    locations: [0, 1]
) {
    context.drawLinearGradient(
        gradient,
        start: CGPoint(x: 0, y: height),
        end: CGPoint(x: width, y: 0),
        options: []
    )
}

// Subtle brand-colored glow and grid keep the window from feeling empty while
// leaving enough contrast for Finder's app icons and labels.
context.saveGState()
context.setFillColor(color(0.05, 0.42, 0.95, 0.16).cgColor)
context.fillEllipse(in: CGRect(x: -110, y: 180, width: 430, height: 430))
context.setFillColor(color(0.15, 0.72, 1.0, 0.08).cgColor)
context.fillEllipse(in: CGRect(x: 535, y: -150, width: 390, height: 390))
context.restoreGState()

context.saveGState()
context.setStrokeColor(color(0.45, 0.72, 1.0, 0.07).cgColor)
context.setLineWidth(1)
stride(from: 0, through: Int(width), by: 40).forEach { x in
    context.move(to: CGPoint(x: CGFloat(x), y: 0))
    context.addLine(to: CGPoint(x: CGFloat(x), y: height))
}
stride(from: 0, through: Int(height), by: 40).forEach { y in
    context.move(to: CGPoint(x: 0, y: CGFloat(y)))
    context.addLine(to: CGPoint(x: width, y: CGFloat(y)))
}
context.strokePath()
context.restoreGState()

let panel = NSBezierPath(
    roundedRect: NSRect(x: 54, y: 105, width: width - 108, height: 170),
    xRadius: 26,
    yRadius: 26
)
color(0.01, 0.025, 0.06, 0.55).setFill()
panel.fill()
color(0.35, 0.65, 1.0, 0.18).setStroke()
panel.lineWidth = 1
panel.stroke()

if let logo = NSImage(contentsOf: logoURL) {
    logo.draw(
        in: NSRect(x: 58, y: 331, width: 92, height: 92),
        from: .zero,
        operation: .sourceOver,
        fraction: 1
    )
}

drawText(
    "INSTALL OPEN S4KA",
    at: NSPoint(x: 170, y: 392),
    font: .systemFont(ofSize: 25, weight: .bold),
    color: .white
)
drawText(
    "Drag the app to Applications to get started",
    at: NSPoint(x: 171, y: 367),
    font: .systemFont(ofSize: 14, weight: .regular),
    color: color(0.78, 0.86, 0.98)
)
drawText(
    "OPEN S4KA  •  PLAY CLASSIC XBOX GAMES",
    at: NSPoint(x: width / 2, y: 35),
    font: .monospacedSystemFont(ofSize: 11, weight: .medium),
    color: color(0.62, 0.75, 0.92),
    centered: true
)

// A clear directional arrow sits between the two Finder icons.
context.saveGState()
context.setStrokeColor(color(0.25, 0.62, 1.0, 0.95).cgColor)
context.setFillColor(color(0.25, 0.62, 1.0, 0.95).cgColor)
context.setLineWidth(7)
context.setLineCap(.round)
context.move(to: CGPoint(x: 306, y: 191))
context.addLine(to: CGPoint(x: 445, y: 191))
context.strokePath()
context.move(to: CGPoint(x: 430, y: 215))
context.addLine(to: CGPoint(x: 462, y: 191))
context.addLine(to: CGPoint(x: 430, y: 167))
context.closePath()
context.fillPath()
context.restoreGState()

NSGraphicsContext.restoreGraphicsState()

guard let png = bitmap.representation(using: .png, properties: [:]) else {
    fputs("could not encode PNG\n", stderr)
    exit(EXIT_FAILURE)
}

do {
    try png.write(to: outputURL)
} catch {
    fputs("could not write background: \(error)\n", stderr)
    exit(EXIT_FAILURE)
}
