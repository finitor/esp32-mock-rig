// audio_loopcheck: play a sample-counting ramp into a CoreAudio device and
// record it back, both through AudioQueue, then write the recording as a
// 16-bit mono WAV for ramp_check.py. macOS only. With the mock in the monitor
// route, every lost, repeated or reordered sample shows exactly.
//
//   swiftc -O -o host/build/audio_loopcheck host/tools/audio_loopcheck.swift
//   host/build/audio_loopcheck "Mock Radio" 120 loop.wav
//   python3 host/tools/ramp_check.py loop.wav
//
// Sixteen 10 ms buffers each way: with fewer, a loaded Mac drops input and
// fills the gap with zeros, which looks like the board's fault and is not.
import AudioToolbox
import CoreAudio
import Foundation

let args = CommandLine.arguments
let want = args[1], secs = Double(args[2])!, outPath = args[3]

func prop<T>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector, _ v: inout T) {
    var a = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var sz = UInt32(MemoryLayout<T>.size)
    AudioObjectGetPropertyData(id, &a, 0, nil, &sz, &v)
}
var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
var sz: UInt32 = 0
AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &sz)
var ids = [AudioObjectID](repeating: 0, count: Int(sz) / 4)
AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &sz, &ids)
var uid: CFString? = nil
for id in ids {
    var name: CFString = "" as CFString
    prop(id, kAudioObjectPropertyName, &name)
    if (name as String).contains(want) {
        var u: CFString = "" as CFString
        prop(id, kAudioDevicePropertyDeviceUID, &u)
        uid = u
        print("device \(name) uid \(u)")
    }
}
guard let devUID = uid else { print("no device matching \(want)"); exit(1) }

var fmt = AudioStreamBasicDescription(mSampleRate: 48000, mFormatID: kAudioFormatLinearPCM,
    mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
    mBytesPerPacket: 2, mFramesPerPacket: 1, mBytesPerFrame: 2, mChannelsPerFrame: 1, mBitsPerChannel: 16, mReserved: 0)
let q = DispatchQueue(label: "audio")
let frames = 480
var counter = 0
var recorded = [Int16]()
recorded.reserveCapacity(Int(secs * 48000) + 48000)
let lock = NSLock()

var outQ: AudioQueueRef? = nil
AudioQueueNewOutputWithDispatchQueue(&outQ, &fmt, 0, q) { aq, buf in
    let p = buf.pointee.mAudioData.assumingMemoryBound(to: Int16.self)
    for i in 0..<frames { p[i] = Int16(counter % 16384 - 8192); counter += 1 }
    buf.pointee.mAudioDataByteSize = UInt32(frames * 2)
    AudioQueueEnqueueBuffer(aq, buf, 0, nil)
}
var inQ: AudioQueueRef? = nil
AudioQueueNewInputWithDispatchQueue(&inQ, &fmt, 0, q) { aq, buf, _, _, _ in
    let n = Int(buf.pointee.mAudioDataByteSize) / 2
    let p = buf.pointee.mAudioData.assumingMemoryBound(to: Int16.self)
    lock.lock(); recorded.append(contentsOf: UnsafeBufferPointer(start: p, count: n)); lock.unlock()
    AudioQueueEnqueueBuffer(aq, buf, 0, nil)
}
var u: CFString = devUID
AudioQueueSetProperty(outQ!, kAudioQueueProperty_CurrentDevice, &u, UInt32(MemoryLayout<CFString>.size))
AudioQueueSetProperty(inQ!, kAudioQueueProperty_CurrentDevice, &u, UInt32(MemoryLayout<CFString>.size))
for _ in 0..<16 {
    var b: AudioQueueBufferRef? = nil
    AudioQueueAllocateBuffer(outQ!, UInt32(frames * 2), &b)
    let p = b!.pointee.mAudioData.assumingMemoryBound(to: Int16.self)
    for i in 0..<frames { p[i] = Int16(counter % 16384 - 8192); counter += 1 }
    b!.pointee.mAudioDataByteSize = UInt32(frames * 2)
    AudioQueueEnqueueBuffer(outQ!, b!, 0, nil)
    var r: AudioQueueBufferRef? = nil
    AudioQueueAllocateBuffer(inQ!, UInt32(frames * 2), &r)
    AudioQueueEnqueueBuffer(inQ!, r!, 0, nil)
}
print("start out:", AudioQueueStart(outQ!, nil), "in:", AudioQueueStart(inQ!, nil))
let t0 = Date()
Thread.sleep(forTimeInterval: secs)
let el = Date().timeIntervalSince(t0)
AudioQueueStop(inQ!, true); AudioQueueStop(outQ!, true)
lock.lock(); let rec = recorded; lock.unlock()
print(String(format: "played %d, recorded %d samples in %.3f s (%.1f per s)", counter, rec.count, el, Double(rec.count) / el))

var d = Data()
func le32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
func le16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
d.append(contentsOf: Array("RIFF".utf8)); le32(UInt32(36 + rec.count * 2)); d.append(contentsOf: Array("WAVEfmt ".utf8))
le32(16); le16(1); le16(1); le32(48000); le32(96000); le16(2); le16(16)
d.append(contentsOf: Array("data".utf8)); le32(UInt32(rec.count * 2))
rec.withUnsafeBytes { d.append(contentsOf: $0) }
try! d.write(to: URL(fileURLWithPath: outPath))
