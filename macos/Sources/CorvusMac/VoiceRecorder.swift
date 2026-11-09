import AVFoundation
import CorvusCore
import Foundation

/// Records voice messages: AAC in MP4 (.m4a), mono, 44.1 kHz, 64 kbit/s — what the Signal apps record
/// and play everywhere (same choice as the Qt app's first preference).
@MainActor
final class VoiceRecorder: NSObject, AVAudioRecorderDelegate {
    var onDuration: ((Int64) -> Void)?
    var onFinished: ((_ path: String, _ durationMs: Int64) -> Void)?
    var onFailed: ((String) -> Void)?

    private var recorder: AVAudioRecorder?
    private var timer: Timer?
    private var discard = false

    var isRecording: Bool { recorder != nil }

    /// Asks for microphone access if needed; nil if recording is possible, otherwise why not.
    static func prepare() async -> String? {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: break
        case .notDetermined:
            if !(await AVCaptureDevice.requestAccess(for: .audio)) { return deniedText }
        default:
            return deniedText
        }
        if AVCaptureDevice.default(for: .audio) == nil { return "No microphone was found." }
        return nil
    }

    private static let deniedText =
        "Corvus may not use the microphone. Allow it in System Settings → Privacy & Security → Microphone."

    func start(directory: URL, baseName: String) {
        guard recorder == nil else { return }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let url = directory.appendingPathComponent(baseName + ".m4a")
            let settings: [String: Any] = [
                AVFormatIDKey: kAudioFormatMPEG4AAC,
                AVSampleRateKey: 44_100,
                AVNumberOfChannelsKey: 1, // speech
                AVEncoderBitRateKey: 64_000,
            ]
            let r = try AVAudioRecorder(url: url, settings: settings)
            r.delegate = self
            guard r.record(forDuration: TimeInterval(VoiceNote.maxDurationMs) / 1000) else { // stops by itself, result kept
                onFailed?("The recording could not be started.")
                return
            }
            recorder = r
            discard = false
            onDuration?(0)
            timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self, let r = self.recorder else { return }
                    self.onDuration?(Int64(r.currentTime * 1000))
                }
            }
        } catch {
            onFailed?(error.localizedDescription)
        }
    }

    /// Ends the recording; onFinished follows once the file is complete.
    func stop() {
        guard let r = recorder else { return }
        r.stop()
    }

    /// Ends the recording and deletes the file; onFinished does not follow.
    func cancel() {
        guard let r = recorder else { return }
        discard = true
        r.stop()
    }

    nonisolated func audioRecorderDidFinishRecording(_ r: AVAudioRecorder, successfully ok: Bool) {
        let url = r.url
        MainActor.assumeIsolated { finish(url: url, ok: ok) }
    }

    nonisolated func audioRecorderEncodeErrorDidOccur(_ r: AVAudioRecorder, error: Error?) {
        let text = error?.localizedDescription ?? "Unknown error"
        MainActor.assumeIsolated {
            let url = recorder?.url
            reset()
            if let url { try? FileManager.default.removeItem(at: url) }
            onFailed?(text)
        }
    }

    private func finish(url: URL, ok: Bool) {
        let dropped = discard
        reset()
        if dropped || !ok {
            try? FileManager.default.removeItem(at: url)
            if !dropped { onFailed?("The recording could not be completed.") }
            return
        }
        // The file's own length; the recorder's clock is reset by stop().
        let seconds = (try? AVAudioPlayer(contentsOf: url).duration) ?? 0
        onFinished?(url.path, Int64(seconds * 1000))
    }

    private func reset() {
        timer?.invalidate()
        timer = nil
        recorder = nil
        discard = false
    }
}
