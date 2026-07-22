//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Accelerate
import AppKit
import CoreAudio
import Foundation
import os.lock

private let mirageAudioIOProc: AudioDeviceIOProc = {
    _, _, inputData, _, _, _, context in
    guard let context else { return noErr }
    let service = Unmanaged<SystemAudioSpectrumService>.fromOpaque(context).takeUnretainedValue()
    service.ingest(inputData)
    return noErr
}

private let mirageAudioDeviceListener: AudioObjectPropertyListenerProc = {
    _, _, _, context in
    guard let context else { return noErr }
    let service = Unmanaged<SystemAudioSpectrumService>.fromOpaque(context).takeUnretainedValue()
    service.outputDeviceChanged()
    return noErr
}

final class SystemAudioSpectrumService {
    static let shared = SystemAudioSpectrumService()

    var onSpectrum: (([Float]) -> Void)?

    private let controlQueue = DispatchQueue(label: "cn.laobamac.Mirage.audio.control")
    private let analysisQueue = DispatchQueue(label: "cn.laobamac.Mirage.audio.analysis", qos: .userInteractive)
    private var timer: DispatchSourceTimer?
    private var wantsRunning = false
    private var running = false
    private var listenerInstalled = false

    private var tap = AudioObjectID(kAudioObjectUnknown)
    private var aggregate = AudioObjectID(kAudioObjectUnknown)
    private var ioProc: AudioDeviceIOProcID?
    private var format = AudioStreamBasicDescription()

    private static let fftSize = 4096
    private static let ringSize = 8192
    private static let binCount = 64
    private var ringLeft = [Float](repeating: 0, count: ringSize)
    private var ringRight = [Float](repeating: 0, count: ringSize)
    private var ringWrite = 0
    private var ringFilled = 0
    private var lastInputNanos: UInt64 = 0
    private var inputLock = os_unfair_lock_s()

    private var dftSetup: vDSP_DFT_Setup?
    private var hann = [Float](repeating: 0, count: fftSize)
    private var smoothedLeft = [Float](repeating: 0, count: binCount)
    private var smoothedRight = [Float](repeating: 0, count: binCount)
    private var bandEdges = [Int](repeating: 0, count: binCount + 1)
    private var bandGain = [Float](repeating: 0, count: binCount)
    private var analysisLeft = [Float](repeating: 0, count: fftSize)
    private var analysisRight = [Float](repeating: 0, count: fftSize)
    private var imaginaryInput = [Float](repeating: 0, count: fftSize)
    private var realOutputLeft = [Float](repeating: 0, count: fftSize)
    private var imaginaryOutputLeft = [Float](repeating: 0, count: fftSize)
    private var realOutputRight = [Float](repeating: 0, count: fftSize)
    private var imaginaryOutputRight = [Float](repeating: 0, count: fftSize)
    private var sampleRate: Float = 48_000

    private init() {
        dftSetup = vDSP_DFT_zop_CreateSetup(nil, vDSP_Length(Self.fftSize), .FORWARD)
        vDSP_hann_window(&hann, vDSP_Length(Self.fftSize), Int32(vDSP_HANN_DENORM))
        rebuildBandLayout(sampleRate: sampleRate)
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationDidBecomeActive),
            name: NSApplication.didBecomeActiveNotification,
            object: nil)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
        if let dftSetup { vDSP_DFT_DestroySetup(dftSetup) }
    }

    func setEnabled(_ enabled: Bool) {
        controlQueue.async { [weak self] in
            guard let self else { return }
            self.wantsRunning = enabled
            if enabled {
                self.startCapture()
            } else {
                self.stopCapture()
                self.publishSilence()
            }
        }
    }

    @objc private func applicationDidBecomeActive() {
        controlQueue.async { [weak self] in
            guard let self, self.wantsRunning, !self.running else { return }
            self.startCapture()
        }
    }

    fileprivate func outputDeviceChanged() {
        controlQueue.async { [weak self] in
            guard let self, self.wantsRunning else { return }
            self.stopCapture()
            self.startCapture()
        }
    }

    fileprivate func ingest(_ inputData: UnsafePointer<AudioBufferList>) {
        let currentFormat = format
        guard currentFormat.mFormatID == kAudioFormatLinearPCM,
              currentFormat.mBitsPerChannel == 32,
              currentFormat.mFormatFlags & kAudioFormatFlagIsFloat != 0 else { return }

        let list = UnsafeMutableAudioBufferListPointer(
            UnsafeMutablePointer<AudioBufferList>(mutating: inputData))
        guard !list.isEmpty else { return }
        let nonInterleaved = currentFormat.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        let channels = max(Int(currentFormat.mChannelsPerFrame), 1)
        let firstChannels = max(Int(list[0].mNumberChannels), 1)
        let frames: Int
        if nonInterleaved {
            frames = Int(list[0].mDataByteSize) / (MemoryLayout<Float>.size * firstChannels)
        } else {
            let bytesPerFrame = currentFormat.mBytesPerFrame > 0
                ? Int(currentFormat.mBytesPerFrame)
                : MemoryLayout<Float>.size * channels
            frames = Int(list[0].mDataByteSize) / bytesPerFrame
        }
        guard frames > 0 else { return }

        os_unfair_lock_lock(&inputLock)
        defer { os_unfair_lock_unlock(&inputLock) }
        for frame in 0..<frames {
            let left: Float
            let right: Float
            if nonInterleaved {
                guard let leftData = list[0].mData else { continue }
                let leftSamples = leftData.assumingMemoryBound(to: Float.self)
                left = leftSamples[frame * firstChannels]
                if list.count > 1, let rightData = list[1].mData {
                    right = rightData.assumingMemoryBound(to: Float.self)[frame]
                } else if firstChannels > 1 {
                    right = leftSamples[frame * firstChannels + 1]
                } else {
                    right = left
                }
            } else {
                guard let data = list[0].mData else { continue }
                let samples = data.assumingMemoryBound(to: Float.self)
                left = samples[frame * channels]
                right = channels > 1 ? samples[frame * channels + 1] : left
            }
            ringLeft[ringWrite] = left.isFinite ? left : 0
            ringRight[ringWrite] = right.isFinite ? right : 0
            ringWrite = (ringWrite + 1) & (Self.ringSize - 1)
            ringFilled = min(ringFilled + 1, Self.ringSize)
        }
        lastInputNanos = DispatchTime.now().uptimeNanoseconds
    }

    private func startCapture() {
        guard wantsRunning, !running else { return }
        installDeviceListener()

        let description = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        description.name = "Mirage Audio Spectrum"
        description.isPrivate = true
        description.muteBehavior = .unmuted

        var newTap = AudioObjectID(kAudioObjectUnknown)
        var status = AudioHardwareCreateProcessTap(description, &newTap)
        guard status == noErr, newTap != kAudioObjectUnknown else {
            NSLog("[Mirage] System audio tap failed: %d", status)
            return
        }
        tap = newTap

        var tapUID: CFString = "" as CFString
        var uidSize = UInt32(MemoryLayout<CFString>.size)
        var uidAddress = AudioObjectPropertyAddress(
            mSelector: kAudioTapPropertyUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        status = withUnsafeMutablePointer(to: &tapUID) {
            AudioObjectGetPropertyData(tap, &uidAddress, 0, nil, &uidSize, $0)
        }
        guard status == noErr else {
            NSLog("[Mirage] System audio tap UID failed: %d", status)
            stopCapture()
            return
        }

        var tapFormat = AudioStreamBasicDescription()
        var formatSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        var formatAddress = AudioObjectPropertyAddress(
            mSelector: kAudioTapPropertyFormat,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        status = AudioObjectGetPropertyData(tap, &formatAddress, 0, nil, &formatSize, &tapFormat)
        guard status == noErr,
              tapFormat.mFormatID == kAudioFormatLinearPCM,
              tapFormat.mBitsPerChannel == 32,
              tapFormat.mFormatFlags & kAudioFormatFlagIsFloat != 0 else {
            NSLog("[Mirage] Unsupported system audio format: %d", status)
            stopCapture()
            return
        }
        format = tapFormat
        sampleRate = Float(tapFormat.mSampleRate)
        rebuildBandLayout(sampleRate: sampleRate)

        let one = NSNumber(value: 1)
        let zero = NSNumber(value: 0)
        let subTap: [String: Any] = [
            kAudioSubTapUIDKey: tapUID,
            kAudioSubTapDriftCompensationKey: one
        ]
        let aggregateDescription: [String: Any] = [
            kAudioAggregateDeviceNameKey: "Mirage Audio Spectrum",
            kAudioAggregateDeviceUIDKey: "cn.laobamac.Mirage.audio.\(UUID().uuidString)",
            kAudioAggregateDeviceIsPrivateKey: one,
            kAudioAggregateDeviceTapAutoStartKey: zero,
            kAudioAggregateDeviceTapListKey: [subTap]
        ]
        var newAggregate = AudioObjectID(kAudioObjectUnknown)
        status = AudioHardwareCreateAggregateDevice(aggregateDescription as CFDictionary, &newAggregate)
        guard status == noErr, newAggregate != kAudioObjectUnknown else {
            NSLog("[Mirage] Audio aggregate creation failed: %d", status)
            stopCapture()
            return
        }
        aggregate = newAggregate

        let context = Unmanaged.passUnretained(self).toOpaque()
        status = AudioDeviceCreateIOProcID(aggregate, mirageAudioIOProc, context, &ioProc)
        guard status == noErr, ioProc != nil else {
            NSLog("[Mirage] Audio IO proc creation failed: %d", status)
            stopCapture()
            return
        }
        status = AudioDeviceStart(aggregate, ioProc)
        guard status == noErr else {
            NSLog("[Mirage] System audio capture start failed: %d", status)
            stopCapture()
            return
        }

        running = true
        resetInput()
        startTimer()
        NSLog("[Mirage] System audio capture started")
    }

    private func stopCapture() {
        timer?.cancel()
        timer = nil
        analysisQueue.sync {}
        if aggregate != kAudioObjectUnknown, let ioProc {
            AudioDeviceStop(aggregate, ioProc)
            AudioDeviceDestroyIOProcID(aggregate, ioProc)
        }
        ioProc = nil
        if aggregate != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregate)
            aggregate = kAudioObjectUnknown
        }
        if tap != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tap)
            tap = kAudioObjectUnknown
        }
        running = false
        resetInput()
    }

    private func installDeviceListener() {
        guard !listenerInstalled else { return }
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let context = Unmanaged.passUnretained(self).toOpaque()
        if AudioObjectAddPropertyListener(
            AudioObjectID(kAudioObjectSystemObject), &address,
            mirageAudioDeviceListener, context) == noErr {
            listenerInstalled = true
        }
    }

    private func startTimer() {
        let timer = DispatchSource.makeTimerSource(queue: analysisQueue)
        timer.schedule(deadline: .now(), repeating: 1.0 / 30.0, leeway: .milliseconds(2))
        timer.setEventHandler { [weak self] in self?.analyzeAndPublish() }
        self.timer = timer
        timer.resume()
    }

    private func resetInput() {
        os_unfair_lock_lock(&inputLock)
        ringLeft.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
        ringRight.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
        ringWrite = 0
        ringFilled = 0
        lastInputNanos = 0
        os_unfair_lock_unlock(&inputLock)
        smoothedLeft.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
        smoothedRight.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
    }

    private func analyzeAndPublish() {
        guard let dftSetup else { return }
        analysisLeft.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
        analysisRight.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
        let now = DispatchTime.now().uptimeNanoseconds
        var fresh = false

        os_unfair_lock_lock(&inputLock)
        let filled = min(ringFilled, Self.fftSize)
        if lastInputNanos > 0, now >= lastInputNanos,
           now - lastInputNanos <= 250_000_000, filled > 0 {
            fresh = true
            let start = (ringWrite - filled + Self.ringSize) & (Self.ringSize - 1)
            let offset = Self.fftSize - filled
            for index in 0..<filled {
                let source = (start + index) & (Self.ringSize - 1)
                analysisLeft[offset + index] = ringLeft[source]
                analysisRight[offset + index] = ringRight[source]
            }
        }
        os_unfair_lock_unlock(&inputLock)

        if fresh {
            vDSP_vmul(analysisLeft, 1, hann, 1, &analysisLeft, 1, vDSP_Length(Self.fftSize))
            vDSP_vmul(analysisRight, 1, hann, 1, &analysisRight, 1, vDSP_Length(Self.fftSize))
            Self.transform(analysisLeft, setup: dftSetup, imaginaryInput: imaginaryInput,
                           realOutput: &realOutputLeft, imaginaryOutput: &imaginaryOutputLeft)
            Self.transform(analysisRight, setup: dftSetup, imaginaryInput: imaginaryInput,
                           realOutput: &realOutputRight, imaginaryOutput: &imaginaryOutputRight)
        }
        let dt: Float = 1.0 / 30.0
        var output = [Float](repeating: 0, count: Self.binCount * 2)
        for bin in 0..<Self.binCount {
            let targetLeft = fresh ? response(real: realOutputLeft,
                                              imaginary: imaginaryOutputLeft, bin: bin) : 0
            let targetRight = fresh ? response(real: realOutputRight,
                                               imaginary: imaginaryOutputRight, bin: bin) : 0
            smoothedLeft[bin] = smooth(previous: smoothedLeft[bin], current: targetLeft, dt: dt)
            smoothedRight[bin] = smooth(previous: smoothedRight[bin], current: targetRight, dt: dt)
            output[bin] = smoothedLeft[bin]
            output[Self.binCount + bin] = smoothedRight[bin]
        }
        onSpectrum?(output)
    }

    private static func transform(_ input: [Float], setup: vDSP_DFT_Setup,
                                  imaginaryInput: [Float],
                                  realOutput: inout [Float],
                                  imaginaryOutput: inout [Float]) {
        input.withUnsafeBufferPointer { realInput in
            imaginaryInput.withUnsafeBufferPointer { imaginaryInput in
                realOutput.withUnsafeMutableBufferPointer { realOutput in
                    imaginaryOutput.withUnsafeMutableBufferPointer { imaginaryOutput in
                        vDSP_DFT_Execute(
                            setup,
                            realInput.baseAddress!, imaginaryInput.baseAddress!,
                            realOutput.baseAddress!, imaginaryOutput.baseAddress!)
                    }
                }
            }
        }
    }

    private func response(real: [Float], imaginary: [Float], bin: Int) -> Float {
        let low = bandEdges[bin]
        let high = max(bandEdges[bin + 1], low + 1)
        var magnitude: Float = 0
        for index in low..<high {
            magnitude += hypotf(real[index], imaginary[index])
        }
        magnitude = magnitude / Float(high - low) * (2.0 / Float(Self.fftSize))
        let compensated = max(magnitude * bandGain[bin], 1.0e-12)
        let unit = min(max((20 * log10f(compensated) + 100) / 92, 0), 1)
        if unit <= 0.5 {
            return 0.5 * powf(unit * 2, 1.6)
        }
        return 1 - 0.5 * powf((1 - unit) * 2, 1.6)
    }

    private func smooth(previous: Float, current: Float, dt: Float) -> Float {
        let time: Float = current > previous ? 0.030 : 0.140
        let factor = 1 - expf(-dt / time)
        return previous + factor * (current - previous)
    }

    private func rebuildBandLayout(sampleRate: Float) {
        let anchors: [(Float, Float)] = [
            (10, 0), (60, 2), (125, 5), (250, 10), (500, 21),
            (1_000, 32), (2_000, 38), (3_000, 46), (8_000, 54),
            (12_000, 60), (16_000, 64)
        ]
        let maximumFrequency = min(16_000, sampleRate * 0.5)
        let maximumBin = max(65, min(Self.fftSize / 2,
            Int(ceil(maximumFrequency * Float(Self.fftSize) / sampleRate))))

        func frequency(for band: Float) -> Float {
            if band <= anchors[0].1 { return anchors[0].0 }
            for index in 1..<anchors.count where band <= anchors[index].1 {
                let lower = anchors[index - 1]
                let upper = anchors[index]
                let amount = (band - lower.1) / (upper.1 - lower.1)
                return expf(logf(lower.0) + (logf(upper.0) - logf(lower.0)) * amount)
            }
            return anchors.last!.0
        }

        for bin in 0..<Self.binCount {
            let frequency = min(frequency(for: Float(bin) - 0.5), maximumFrequency)
            var next = max(1, Int(ceil(frequency * Float(Self.fftSize) / sampleRate)))
            if bin > 0, next <= bandEdges[bin - 1] { next = bandEdges[bin - 1] + 1 }
            let remaining = Self.binCount - bin
            if next + remaining > maximumBin { next = maximumBin - remaining }
            bandEdges[bin] = next
        }
        bandEdges[Self.binCount] = maximumBin
        for bin in 0..<Self.binCount {
            let upperFrequency = Float(bandEdges[bin + 1]) * sampleRate / Float(Self.fftSize)
            bandGain[bin] = powf(upperFrequency / 1_000, 1.15)
        }
    }

    private func publishSilence() {
        onSpectrum?([Float](repeating: 0, count: Self.binCount * 2))
    }
}
