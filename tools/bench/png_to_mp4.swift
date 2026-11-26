// Encodes a numbered PNG sequence (frame_0000.png, ...) into an H.264 MP4 with AVFoundation (no ffmpeg needed).
// Usage: xcrun swift tools/bench/png_to_mp4.swift <frames directory> <out.mp4> [fps]
import AVFoundation
import AppKit
import Foundation

let args = CommandLine.arguments
guard args.count >= 3 else {
    print("usage: xcrun swift png_to_mp4.swift <frames directory> <out.mp4> [fps]")
    exit(2)
}
let directory = URL(fileURLWithPath: args[1])
let output = URL(fileURLWithPath: args[2])
let fps = Int32(args.count > 3 ? Int(args[3]) ?? 30 : 30)
let frames = try FileManager.default.contentsOfDirectory(atPath: directory.path)
    .filter { $0.hasSuffix(".png") }.sorted()
guard let first = NSImage(contentsOf: directory.appendingPathComponent(frames[0])),
      let firstImage = first.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
    print("cannot read \(frames[0])")
    exit(1)
}
let width = firstImage.width
let height = firstImage.height
try? FileManager.default.removeItem(at: output)
let writer = try AVAssetWriter(outputURL: output, fileType: .mp4)
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: width, AVVideoHeightKey: height,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: width * height * 12],
])
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32ARGB, kCVPixelBufferWidthKey as String: width,
    kCVPixelBufferHeightKey as String: height,
])
writer.add(input)
writer.startWriting()
writer.startSession(atSourceTime: .zero)
for (index, name) in frames.enumerated() {
    guard let image = NSImage(contentsOf: directory.appendingPathComponent(name))?
        .cgImage(forProposedRect: nil, context: nil, hints: nil) else { continue }
    var buffer: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &buffer)
    guard let pixels = buffer else { continue }
    CVPixelBufferLockBaseAddress(pixels, [])
    let context = CGContext(data: CVPixelBufferGetBaseAddress(pixels), width: width, height: height, bitsPerComponent: 8,
                            bytesPerRow: CVPixelBufferGetBytesPerRow(pixels), space: CGColorSpaceCreateDeviceRGB(),
                            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue)
    context?.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    CVPixelBufferUnlockBaseAddress(pixels, [])
    while !input.isReadyForMoreMediaData { usleep(1000) }
    adaptor.append(pixels, withPresentationTime: CMTime(value: CMTimeValue(index), timescale: fps))
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
print("wrote \(output.path) (\(frames.count) frames, \(width)x\(height), \(fps) fps)")
