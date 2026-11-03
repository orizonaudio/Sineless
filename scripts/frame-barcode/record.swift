// Records the frame barcode's cells from the main display with ScreenCaptureKit, at the display's
// own size and refresh rate (run by record.sh; see the frame-timing skill).
//
//   swift record.swift grab <prefix> <count> <gap-ms>
//       Saves <count> whole frames, <gap-ms> apart, as <prefix>-<i>.gray (one byte of grey per
//       pixel, rows from the top), and prints {"width", "height"} for locate.
//   swift record.swift record <places.json> <seconds> <out.jsonl>
//       Writes a header line, then for each refresh the window server showed: {"t": ms} (its
//       display time on the host clock) with either "idle": true (nothing changed) or "cells": each
//       strip's two rows of greys (0 to 1) at the cells' centres found by locate.
//
// Needs Screen Recording permission for the terminal that runs it.
import AppKit
import CoreMedia
import CoreVideo
import Foundation
import ScreenCaptureKit

struct Place: Codable {
    let left: Double
    let top: Double
    let cell: Double
}

// What analyse.ts --locate writes: the strips, and the cells in each row (limits.ts).
struct Located: Codable {
    let rowCells: Int
    let places: [Place]
}

func milliseconds(_ ticks: UInt64) -> Double {
    var timebase = mach_timebase_info_data_t()
    mach_timebase_info(&timebase)
    return Double(ticks) * Double(timebase.numer) / Double(timebase.denom) / 1e6
}

final class Capture: NSObject, SCStreamOutput {
    let onFrame: (CVPixelBuffer?, SCFrameStatus, Double) -> Void

    init(onFrame: @escaping (CVPixelBuffer?, SCFrameStatus, Double) -> Void) {
        self.onFrame = onFrame
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer buffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(
                  buffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let info = attachments.first,
              let raw = info[.status] as? Int,
              let status = SCFrameStatus(rawValue: raw)
        else { return }
        let ticks = (info[.displayTime] as? UInt64) ?? 0
        onFrame(CMSampleBufferGetImageBuffer(buffer), status, milliseconds(ticks))
    }
}

// Grey (0 to 1) at a pixel of a BGRA buffer.
func grey(_ buffer: CVPixelBuffer, _ x: Int, _ y: Int) -> Double {
    guard let base = CVPixelBufferGetBaseAddress(buffer),
          x >= 0, y >= 0, x < CVPixelBufferGetWidth(buffer), y < CVPixelBufferGetHeight(buffer)
    else { return 0 }
    let pixel = base.advanced(by: y * CVPixelBufferGetBytesPerRow(buffer) + x * 4)
        .assumingMemoryBound(to: UInt8.self)
    return (Double(pixel[0]) + Double(pixel[1]) + Double(pixel[2])) / 765
}

// The main display's stream, set up but not started: its size in pixels and refresh rate.
func makeStream(capture: Capture) async throws -> (SCStream, Int, Int, Double) {
    let content = try await SCShareableContent.current
    // FRAME_BARCODE_DISPLAY: "builtin", "external" (the first that is not built in), or the main
    // display by default.
    let kind = ProcessInfo.processInfo.environment["FRAME_BARCODE_DISPLAY"] ?? "main"
    let chosen = content.displays.first { display in
        switch kind {
        case "builtin": return CGDisplayIsBuiltin(display.displayID) != 0
        case "external": return CGDisplayIsBuiltin(display.displayID) == 0
        default: return display.displayID == CGMainDisplayID()
        }
    }
    guard let display = chosen ?? content.displays.first
    else { throw NSError(domain: "record", code: 1) }
    let mode = CGDisplayCopyDisplayMode(display.displayID)
    let width = mode?.pixelWidth ?? display.width
    let height = mode?.pixelHeight ?? display.height
    let screen = NSScreen.screens.first {
        ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID)
            == display.displayID
    }
    let rate = Double(screen?.maximumFramesPerSecond ?? 60)
    let configuration = SCStreamConfiguration()
    configuration.width = width
    configuration.height = height
    configuration.pixelFormat = kCVPixelFormatType_32BGRA
    // No limit: every refresh. A limit of 1/rate dropped about one refresh in 20 at 60 Hz, on both
    // displays measured; with none, no refresh was missed.
    configuration.minimumFrameInterval = .zero
    configuration.queueDepth = 6
    configuration.showsCursor = false
    let stream = SCStream(
        filter: SCContentFilter(display: display, excludingWindows: []),
        configuration: configuration, delegate: nil)
    try stream.addStreamOutput(
        capture, type: .screen, sampleHandlerQueue: DispatchQueue(label: "record"))
    return (stream, width, height, rate)
}

let arguments = CommandLine.arguments
switch arguments.count > 1 ? arguments[1] : "" {
case "grab":
    let prefix = arguments[2]
    let count = Int(arguments[3]) ?? 2
    let gap = Double(arguments[4]) ?? 300
    let lock = NSLock()
    var saved = 0
    var last = -Double.infinity
    var size = (0, 0)
    let capture = Capture { buffer, status, time in
        guard status == .complete, let buffer else { return }
        lock.lock()
        defer { lock.unlock() }
        guard saved < count, time - last >= gap else { return }
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        let width = CVPixelBufferGetWidth(buffer)
        let height = CVPixelBufferGetHeight(buffer)
        var bytes = [UInt8](repeating: 0, count: width * height)
        for y in 0..<height {
            for x in 0..<width {
                bytes[y * width + x] = UInt8(min(255, grey(buffer, x, y) * 255))
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, .readOnly)
        FileManager.default.createFile(
            atPath: "\(prefix)-\(saved).gray", contents: Data(bytes))
        size = (width, height)
        last = time
        saved += 1
    }
    let (stream, _, _, _) = try await makeStream(capture: capture)
    try await stream.startCapture()
    // Up to ten seconds for the frames to come.
    for _ in 0..<100 {
        if lock.withLock({ saved == count }) { break }
        try await Task.sleep(nanoseconds: 100_000_000)
    }
    try await stream.stopCapture()
    print("{\"width\": \(size.0), \"height\": \(size.1)}")
case "record":
    let located = try JSONDecoder().decode(
        Located.self, from: Data(contentsOf: URL(fileURLWithPath: arguments[2])))
    let places = located.places
    let seconds = Double(arguments[3]) ?? 60
    FileManager.default.createFile(atPath: arguments[4], contents: nil)
    guard let out = FileHandle(forWritingAtPath: arguments[4]) else { exit(1) }
    let capture = Capture { buffer, status, time in
        var line: [String: Any] = ["t": time]
        if status == .idle {
            line["idle"] = true
        } else if status == .complete, let buffer {
            CVPixelBufferLockBaseAddress(buffer, .readOnly)
            line["cells"] = places.map { place in
                (0..<2).map { row in
                    (0..<located.rowCells).map { cell in
                        grey(buffer, Int(place.left + (Double(cell) + 0.5) * place.cell),
                             Int(place.top + (Double(row) + 0.5) * place.cell))
                    }
                }
            }
            CVPixelBufferUnlockBaseAddress(buffer, .readOnly)
        } else {
            return
        }
        if let data = try? JSONSerialization.data(withJSONObject: line) {
            out.write(data)
            out.write(Data("\n".utf8))
        }
    }
    let (stream, width, height, rate) = try await makeStream(capture: capture)
    let header: [String: Any] = [
        "width": width, "height": height, "rate": rate,
        "strips": places.map { ["left": $0.left, "top": $0.top, "cell": $0.cell] },
    ]
    // The header first; the samples follow once the stream starts.
    out.write(try JSONSerialization.data(withJSONObject: header))
    out.write(Data("\n".utf8))
    try await stream.startCapture()
    try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
    try await stream.stopCapture()
    out.closeFile()
default:
    FileHandle.standardError.write(Data("usage: record.swift grab|record ...\n".utf8))
    exit(2)
}
