// SPDX-License-Identifier: GPL-3.0-or-later
// Home-screen art from the game's official artwork, for the user's own build: the Pokopia logo over the key art for the
// tile, the key art for the backgrounds. tools/art/official_art.sh downloads the two pictures into build/ (ignored by
// git): they belong to Nintendo / The Pokémon Company and are never committed.
//
//   swift tools/art/official_art.swift <logo.png> <key-art.jpg> <out dir>
// writes icon0.png (512x512), background.png and background.rgba (3840x2160, raw RGBA8 for tools/art/bc7_dds.cpp).
import CoreGraphics
import CoreImage
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count == 4 else {
    FileHandle.standardError.write("usage: official_art.swift <logo.png> <key-art.jpg> <out dir>\n".data(using: .utf8)!)
    exit(2)
}
let out = args[3]
let rgb = CGColorSpace(name: CGColorSpace.sRGB)!

func load(_ path: String) -> CGImage {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
        FileHandle.standardError.write("cannot read \(path)\n".data(using: .utf8)!)
        exit(1)
    }
    return img
}

func color(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: rgb, components: [CGFloat(r) / 255, CGFloat(g) / 255, CGFloat(b) / 255, a])!
}

func context(_ w: Int, _ h: Int) -> CGContext {
    let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: rgb,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.interpolationQuality = .high
    return c
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

// The middle part of img with the aspect ratio w / h (CGImage crops are top-down).
func centred(_ img: CGImage, aspect: CGFloat) -> CGImage {
    let iw = CGFloat(img.width), ih = CGFloat(img.height)
    let r = iw / ih > aspect
        ? CGRect(x: ((iw - ih * aspect) / 2).rounded(), y: 0, width: (ih * aspect).rounded(), height: ih)
        : CGRect(x: 0, y: ((ih - iw / aspect) / 2).rounded(), width: iw, height: (iw / aspect).rounded())
    return img.cropping(to: r)!
}

func blurred(_ img: CGImage, sigma: Double) -> CGImage {
    let ci = CIImage(cgImage: img)
    let b = ci.clampedToExtent().applyingGaussianBlur(sigma: sigma).cropped(to: ci.extent)
    return CIContext(options: [.workingColorSpace: rgb, .outputColorSpace: rgb]).createCGImage(b, from: ci.extent)!
}

let logo = load(args[1])
let art = load(args[2])

// ---------------------------------------------------------------- icon0.png 512x512
// The logo over the middle of the key art, softened and lightened behind the logo so that the logo reads at tile size.
do {
    let c = context(512, 512)
    c.draw(blurred(centred(art, aspect: 1), sigma: 5), in: CGRect(x: 0, y: 0, width: 512, height: 512))
    let veil = CGGradient(colorsSpace: rgb, colors: [color(255, 250, 230, 0.55), color(255, 250, 230, 0)] as CFArray,
                          locations: [0, 1])!
    c.drawRadialGradient(veil, startCenter: CGPoint(x: 256, y: 256), startRadius: 0, endCenter: CGPoint(x: 256, y: 256),
                         endRadius: 300, options: [])
    let lw: CGFloat = 484, lh = lw * CGFloat(logo.height) / CGFloat(logo.width)
    c.draw(logo, in: CGRect(x: (512 - lw) / 2, y: (512 - lh) / 2, width: lw, height: lh))
    save(c, "icon0")
}

// ---------------------------------------------------------------- background 3840x2160 (pic0 / pic1)
// The key art, 16:9 out of its middle (its left and right edges fade to cream), with the shade on the left of the
// repository's own art: the shell draws the title name and the Play button there.
do {
    let c = context(3840, 2160)
    c.draw(centred(art, aspect: 16.0 / 9.0), in: CGRect(x: 0, y: 0, width: 3840, height: 2160))
    let shade = CGGradient(colorsSpace: rgb, colors: [color(10, 30, 36, 0.55), color(10, 30, 36, 0)] as CFArray,
                           locations: [0, 0.45])!
    c.drawLinearGradient(shade, start: CGPoint(x: 0, y: 0), end: CGPoint(x: 3840, y: 0),
                         options: [.drawsAfterEndLocation])
    save(c, "background", raw: true)
}
print("art written to \(out)")
