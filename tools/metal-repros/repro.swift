// Creates a compute pipeline from a metallib, the step at which Veach's large kernels crash Metal's compiler
// service (XPC_ERROR_CONNECTION_INTERRUPTED). Usage: xcrun swift repro.swift <kernel.metallib> <function>
import Metal
import Foundation

let args = CommandLine.arguments
guard args.count == 3 else {
    print("usage: xcrun swift repro.swift <kernel.metallib> <function>")
    exit(2)
}
guard let device = MTLCreateSystemDefaultDevice() else {
    print("no Metal device")
    exit(2)
}
print("device: \(device.name)")
do {
    let library = try device.makeLibrary(URL: URL(fileURLWithPath: args[1]))
    guard let function = library.makeFunction(name: args[2]) else {
        print("no function '\(args[2])' in \(args[1])")
        exit(2)
    }
    let start = Date()
    _ = try device.makeComputePipelineState(function: function)
    print("pipeline '\(args[2])' created in \(String(format: "%.1f", Date().timeIntervalSince(start))) s")
} catch {
    print("pipeline '\(args[2])' failed: \(error)")
    exit(1)
}
