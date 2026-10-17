// Draws the Cam Studio icon for iOS: the same mark as scripts/make-icon.ps1 (white camera on a blue
// tile, same proportions and colours) as the single 1024 px app icon. iOS rounds the corners itself,
// so the tile fills the square, and the image has no alpha channel.
//   swift scripts/make-icon-ios.swift        (on a Mac, from the repo root)
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let s: CGFloat = 1024
let space = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: Int(s), height: Int(s), bitsPerComponent: 8, bytesPerRow: 0, space: space,
                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
// Top-left origin, like System.Drawing in the Windows script.
ctx.translateBy(x: 0, y: s)
ctx.scaleBy(x: 1, y: -1)

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat) -> CGColor {
    CGColor(colorSpace: space, components: [r / 255, g / 255, b / 255, 1])!
}

// Tile with a soft vertical gradient.
let gradient = CGGradient(colorsSpace: space, colors: [rgb(76, 160, 255), rgb(0, 95, 204)] as CFArray, locations: [0, 1])!
ctx.drawLinearGradient(gradient, start: .zero, end: CGPoint(x: 0, y: s), options: [])

// Camera body + viewfinder wedge.
ctx.setFillColor(rgb(255, 255, 255))
ctx.addPath(CGPath(roundedRect: CGRect(x: s * 0.14, y: s * 0.30, width: s * 0.50, height: s * 0.40),
                   cornerWidth: s * 0.09, cornerHeight: s * 0.09, transform: nil))
ctx.fillPath()
ctx.addLines(between: [
    CGPoint(x: s * 0.66, y: s * 0.44), CGPoint(x: s * 0.86, y: s * 0.33),
    CGPoint(x: s * 0.86, y: s * 0.67), CGPoint(x: s * 0.66, y: s * 0.56),
])
ctx.closePath()
ctx.fillPath()

// Lens.
ctx.setFillColor(rgb(0, 95, 204))
let r = s * 0.115
ctx.fillEllipse(in: CGRect(x: s * 0.39 - r, y: s * 0.50 - r, width: 2 * r, height: 2 * r))

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
let out = root.appendingPathComponent("ios/Resources/Assets.xcassets/AppIcon.appiconset/AppIcon.png")
try FileManager.default.createDirectory(at: out.deletingLastPathComponent(), withIntermediateDirectories: true)
let dest = CGImageDestinationCreateWithURL(out as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
guard CGImageDestinationFinalize(dest) else { fatalError("could not write \(out.path)") }
print("Wrote \(out.path)")
