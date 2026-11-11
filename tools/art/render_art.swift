// SPDX-License-Identifier: GPL-3.0-or-later
// Original artwork of the title (no game or third-party assets): a home-screen tile (512x512) and a 16:9 background
// (3840x2160) of a sunny island with rolling hills, drawn with CoreGraphics. The background keeps its subject on the
// right: the PS5 shell puts the title name and the Play button on the left.
//
//   swift tools/art/render_art.swift <out dir>
// writes icon0.png, background.png and background.rgba (raw RGBA8 for tools/art/bc7_dds.cpp).
import CoreGraphics
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
let rgb = CGColorSpace(name: CGColorSpace.sRGB)!

func color(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: rgb, components: [CGFloat(r) / 255, CGFloat(g) / 255, CGFloat(b) / 255, a])!
}

func context(_ w: Int, _ h: Int) -> CGContext {
    let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: rgb,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.setShouldAntialias(true)
    c.interpolationQuality = .high
    return c
}

func linear(_ c: CGContext, _ stops: [(CGFloat, CGColor)], from: CGPoint, to: CGPoint) {
    let g = CGGradient(colorsSpace: rgb, colors: stops.map { $0.1 } as CFArray, locations: stops.map { $0.0 })!
    c.drawLinearGradient(g, start: from, end: to, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
}

func radial(_ c: CGContext, _ stops: [(CGFloat, CGColor)], center: CGPoint, radius: CGFloat) {
    let g = CGGradient(colorsSpace: rgb, colors: stops.map { $0.1 } as CFArray, locations: stops.map { $0.0 })!
    c.drawRadialGradient(g, startCenter: center, startRadius: 0, endCenter: center, endRadius: radius, options: [])
}

// A rolling hill: a smooth curve through `peaks` (x, height), closed down to the bottom edge, filled with a vertical
// gradient (lit top, darker foot).
func hill(_ c: CGContext, width w: CGFloat, base: CGFloat, end: CGFloat, peaks: [(CGFloat, CGFloat)], top: CGColor,
          foot: CGColor) {
    let p = CGMutablePath()
    p.move(to: CGPoint(x: -20, y: 0))
    p.addLine(to: CGPoint(x: -20, y: base))
    var prev = CGPoint(x: -20, y: base)
    for (x, h) in peaks + [(w + 20, end), (w + 40, end)] {
        let pt = CGPoint(x: x, y: h)
        let mid = CGPoint(x: (prev.x + pt.x) / 2, y: (prev.y + pt.y) / 2)
        p.addQuadCurve(to: mid, control: prev)
        prev = pt
    }
    p.addLine(to: CGPoint(x: w + 20, y: 0))
    p.closeSubpath()
    c.saveGState()
    c.addPath(p)
    c.clip()
    let maxH = peaks.map { $0.1 }.max() ?? base
    linear(c, [(0, foot), (1, top)], from: CGPoint(x: 0, y: 0), to: CGPoint(x: 0, y: maxH))
    c.restoreGState()
}

func tree(_ c: CGContext, x: CGFloat, y: CGFloat, s: CGFloat) {
    c.setFillColor(color(110, 78, 52))
    c.fill(CGRect(x: x - s * 0.07, y: y, width: s * 0.14, height: s * 0.55))
    for (dx, dy, r, k) in [(0.0, 0.85, 0.42, 0), (-0.25, 0.62, 0.30, 1), (0.25, 0.64, 0.31, 1), (0.0, 1.12, 0.28, 2)] {
        let shade = [color(58, 150, 78), color(46, 128, 66), color(84, 178, 92)][k]
        c.setFillColor(shade)
        c.fillEllipse(in: CGRect(x: x + CGFloat(dx) * s - CGFloat(r) * s, y: y + CGFloat(dy) * s - CGFloat(r) * s,
                                 width: CGFloat(r) * 2 * s, height: CGFloat(r) * 2 * s))
    }
}

func cloud(_ c: CGContext, x: CGFloat, y: CGFloat, s: CGFloat, alpha: CGFloat) {
    c.setFillColor(color(255, 255, 255, alpha))
    for (dx, dy, r) in [(0.0, 0.0, 0.5), (0.45, 0.12, 0.42), (-0.45, 0.08, 0.38), (0.15, 0.35, 0.4), (-0.2, 0.3, 0.33)] {
        c.fillEllipse(in: CGRect(x: x + CGFloat(dx) * s - CGFloat(r) * s, y: y + CGFloat(dy) * s - CGFloat(r) * s,
                                 width: CGFloat(r) * 2 * s, height: CGFloat(r) * 2 * s))
    }
}

func leaf(_ c: CGContext, center: CGPoint, size s: CGFloat, fill: CGColor, vein: CGColor) {
    let p = CGMutablePath()
    p.move(to: CGPoint(x: center.x, y: center.y - s))
    p.addCurve(to: CGPoint(x: center.x, y: center.y + s), control1: CGPoint(x: center.x + s * 0.95, y: center.y - s * 0.4),
               control2: CGPoint(x: center.x + s * 0.75, y: center.y + s * 0.7))
    p.addCurve(to: CGPoint(x: center.x, y: center.y - s), control1: CGPoint(x: center.x - s * 0.75, y: center.y + s * 0.7),
               control2: CGPoint(x: center.x - s * 0.95, y: center.y - s * 0.4))
    c.addPath(p)
    c.setFillColor(fill)
    c.fillPath()
    c.setStrokeColor(vein)
    c.setLineWidth(s * 0.07)
    c.setLineCap(.round)
    c.move(to: CGPoint(x: center.x, y: center.y - s * 1.25))
    c.addLine(to: CGPoint(x: center.x, y: center.y + s * 0.8))
    c.strokePath()
    for k in 0..<3 {
        let yy = center.y - s * 0.35 + CGFloat(k) * s * 0.38
        for side in [-1.0, 1.0] {
            c.move(to: CGPoint(x: center.x, y: yy))
            c.addLine(to: CGPoint(x: center.x + CGFloat(side) * s * 0.42, y: yy + s * 0.3))
        }
    }
    c.setLineWidth(s * 0.045)
    c.strokePath()
}

func text(_ c: CGContext, _ s: String, font: String, size: CGFloat, color col: CGColor, centerX: CGFloat, y: CGFloat,
          kern: CGFloat = 0) {
    let f = CTFontCreateWithName(font as CFString, size, nil)
    let attr: [NSAttributedString.Key: Any] = [.init(kCTFontAttributeName as String): f,
                                               .init(kCTForegroundColorAttributeName as String): col,
                                               .init(kCTKernAttributeName as String): kern]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attr))
    let w = CTLineGetTypographicBounds(line, nil, nil, nil)
    c.textPosition = CGPoint(x: centerX - CGFloat(w) / 2, y: y)
    CTLineDraw(line, c)
}

func save(_ c: CGContext, _ name: String, raw: Bool = false) {
    let img = c.makeImage()!
    let url = URL(fileURLWithPath: out).appendingPathComponent(name + ".png")
    let dst = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dst, img, nil)
    CGImageDestinationFinalize(dst)
    if raw {   // top-down RGBA8, opaque
        let data = Data(bytes: c.data!, count: c.bytesPerRow * c.height)
        try! data.write(to: URL(fileURLWithPath: out).appendingPathComponent(name + ".rgba"))
    }
}

func scene(_ c: CGContext, w: CGFloat, h: CGFloat, emblem: Bool) {
    // sky (CoreGraphics is bottom-up): warm horizon to clear blue
    linear(c, [(0, color(255, 206, 160)), (0.25, color(255, 232, 200)), (0.55, color(150, 210, 232)), (1, color(64, 138, 196))],
           from: CGPoint(x: 0, y: h * 0.30), to: CGPoint(x: 0, y: h))
    // sun glow, right
    radial(c, [(0, color(255, 250, 225, 1)), (0.16, color(255, 238, 180, 0.9)), (1, color(255, 222, 160, 0))],
           center: CGPoint(x: w * 0.76, y: h * 0.62), radius: h * 0.6)
    c.setFillColor(color(255, 252, 238))
    c.fillEllipse(in: CGRect(x: w * 0.76 - h * 0.07, y: h * 0.62 - h * 0.07, width: h * 0.14, height: h * 0.14))
    cloud(c, x: w * 0.58, y: h * 0.80, s: h * 0.09, alpha: 0.9)
    cloud(c, x: w * 0.93, y: h * 0.86, s: h * 0.07, alpha: 0.8)
    cloud(c, x: w * 0.36, y: h * 0.90, s: h * 0.055, alpha: 0.6)
    // sea up to the horizon (clipped: the gradient would otherwise extend over the whole picture)
    c.saveGState()
    c.clip(to: CGRect(x: 0, y: 0, width: w, height: h * 0.33))
    linear(c, [(0, color(30, 104, 150)), (1, color(120, 196, 214))], from: CGPoint(x: 0, y: 0), to: CGPoint(x: 0, y: h * 0.33))
    c.setFillColor(color(255, 255, 255, 0.22))
    for k in 0..<12 {
        c.fill(CGRect(x: w * (0.03 + 0.075 * CGFloat(k)), y: h * (0.27 - 0.045 * CGFloat(k % 4)), width: w * 0.045, height: h * 0.004))
    }
    c.restoreGState()
    // island hills, back to front, rising toward the right and leaving the sea open on the left
    hill(c, width: w, base: h * 0.30, end: h * 0.50, peaks: [(w * 0.34, h * 0.33), (w * 0.58, h * 0.47), (w * 0.82, h * 0.56)],
         top: color(158, 210, 146), foot: color(104, 166, 116))
    hill(c, width: w, base: h * 0.17, end: h * 0.40, peaks: [(w * 0.42, h * 0.24), (w * 0.66, h * 0.40), (w * 0.90, h * 0.43)],
         top: color(116, 194, 104), foot: color(72, 142, 82))
    hill(c, width: w, base: h * 0.05, end: h * 0.27, peaks: [(w * 0.46, h * 0.12), (w * 0.72, h * 0.27), (w * 0.95, h * 0.30)],
         top: color(136, 208, 98), foot: color(80, 152, 72))
    // sandy shore, bottom left
    hill(c, width: w, base: h * 0.07, end: 0, peaks: [(w * 0.16, h * 0.075), (w * 0.34, h * 0.035), (w * 0.5, 0)],
         top: color(242, 224, 172), foot: color(216, 192, 138))
    for (x, y, s) in [(0.62, 0.45, 0.07), (0.70, 0.48, 0.06), (0.86, 0.52, 0.065), (0.66, 0.36, 0.10), (0.78, 0.38, 0.09),
                      (0.92, 0.40, 0.08), (0.74, 0.24, 0.13), (0.88, 0.26, 0.12)] {
        tree(c, x: w * CGFloat(x), y: h * CGFloat(y), s: h * CGFloat(s))
    }
    if emblem {
        leaf(c, center: CGPoint(x: w * 0.5, y: h * 0.70), size: h * 0.17, fill: color(96, 196, 104), vein: color(240, 255, 230))
    }
}

// ---------------------------------------------------------------- icon0.png 512x512
do {
    let c = context(512, 512)
    scene(c, w: 512, h: 512, emblem: true)
    // banner with the project's own wording
    c.setFillColor(color(18, 52, 58))
    c.fill(CGRect(x: 0, y: 0, width: 512, height: 132))
    c.setFillColor(color(120, 210, 120))
    c.fill(CGRect(x: 0, y: 128, width: 512, height: 6))
    text(c, "NATIVE", font: "AvenirNext-Heavy", size: 74, color: color(245, 252, 240), centerX: 256, y: 44, kern: 4)
    text(c, "RECOMPILED  ·  PS5", font: "AvenirNext-DemiBold", size: 24, color: color(170, 225, 180), centerX: 256, y: 14, kern: 2)
    save(c, "icon0")
}

// ---------------------------------------------------------------- background 3840x2160 (pic0 / pic1)
do {
    let c = context(3840, 2160)
    scene(c, w: 3840, h: 2160, emblem: false)
    // soft shade on the left, where the shell draws the title name and the Play button
    linear(c, [(0, color(10, 30, 36, 0.55)), (0.45, color(10, 30, 36, 0.0))], from: CGPoint(x: 0, y: 0), to: CGPoint(x: 3840, y: 0))
    save(c, "background", raw: true)
}
print("art written to \(out)")
