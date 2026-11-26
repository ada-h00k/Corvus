import AppKit
import CoreImage.CIFilterBuiltins
import CorvusCore
import SwiftUI

/// Links this Mac as a secondary device: runs `corvus-cli link -n <name>` and shows the
/// returned URI as a QR code (rendered with Core Image, no qrencode needed).
struct LinkView: View {
    let executable: String
    let onFinish: (Bool) -> Void

    @State private var name = "corvus (\(Host.current().localizedName ?? "Mac"))"
    @State private var uri = ""
    @State private var status = ""
    @State private var running = false
    @State private var process: Process?
    @State private var input: FileHandle?          // corvus-cli reads the history transfer answer here
    @State private var transferOffered = false
    @State private var transferAnswered = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Link device").font(.title2.bold())
            Text("Open Signal on your phone → Settings → Linked devices → “Link new device” and scan the QR code.")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Text("Device name:")
                TextField("Device name", text: $name).disabled(running)
                Button("Start") { start() }.disabled(running || name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            ZStack {
                RoundedRectangle(cornerRadius: 8).fill(Color.white)
                if let qr = qrImage {
                    Image(nsImage: qr).interpolation(.none).resizable().scaledToFit().padding(12)
                } else if running {
                    ProgressView()
                }
            }
            .frame(width: 260, height: 260)
            .frame(maxWidth: .infinity)
            HStack {
                TextField("Link URI", text: .constant(uri)).textFieldStyle(.roundedBorder)
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(uri, forType: .string)
                }
                .disabled(uri.isEmpty)
            }
            Text(status).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if transferOffered && !transferAnswered {
                // The phone offers its recent history (Signal's synchronized start), like Signal Desktop asks.
                VStack(alignment: .leading, spacing: 8) {
                    Text("Your phone offers to transfer the recent chat history (last 45 days) to this computer.")
                        .fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Button("Transfer history") { answerTransfer(true) }.keyboardShortcut(.defaultAction)
                        Button("Continue without") { answerTransfer(false) }
                    }
                }
            }
            HStack {
                Spacer()
                Button("Cancel") { cancel() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 460)
    }

    private var qrImage: NSImage? {
        guard !uri.isEmpty else { return nil }
        let f = CIFilter.qrCodeGenerator()
        f.message = Data(uri.utf8)
        f.correctionLevel = "M"
        guard let out = f.outputImage?.transformed(by: CGAffineTransform(scaleX: 8, y: 8)) else { return nil }
        let rep = NSCIImageRep(ciImage: out)
        let img = NSImage(size: rep.size)
        img.addRepresentation(rep)
        return img
    }

    private func start() {
        uri = ""
        status = "Waiting for \(BackendInfo.name(forExecutable: executable)) …"
        running = true
        let p = Process()
        SignalCli.configure(p, executable: executable, arguments: ["link", "-n", name.trimmingCharacters(in: .whitespaces)])
        p.environment = SignalCli.childEnvironment()
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        let stdin = Pipe()
        p.standardInput = stdin
        input = stdin.fileHandleForWriting
        transferOffered = false
        transferAnswered = false
        var buffer = ""
        out.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil; return }
            let s = String(decoding: d, as: UTF8.self)
            Task { @MainActor in
                buffer += s
                // Complete lines only; the trailing partial one stays in the buffer.
                while let nl = buffer.firstIndex(of: "\n") {
                    let t = buffer[..<nl].trimmingCharacters(in: .whitespaces)
                    buffer.removeSubrange(...nl)
                    handleLine(t)
                }
            }
        }
        p.terminationHandler = { proc in
            let code = proc.terminationStatus
            let e = String(decoding: err.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            Task { @MainActor in
                guard process === proc else { return }
                process = nil
                running = false
                if code == 0 {
                    onFinish(true)
                } else {
                    status = "Linking failed (exit code \(code)).\n\(e)"
                }
            }
        }
        do {
            try p.run()
            process = p
        } catch {
            running = false
            status = "\(BackendInfo.name(forExecutable: executable)) could not be started: \(error.localizedDescription)"
        }
    }

    /// corvus-cli reports the phone's transfer offer and the import progress after the QR code was scanned.
    private func handleLine(_ t: String) {
        if t.isEmpty { return }
        if t == "history-offered" {
            if !transferOffered {
                transferOffered = true
                status = "Waiting for your choice …"
            }
        } else if t.hasPrefix("history-progress ") {
            status = "Importing chat history … (\(t.dropFirst(17)) messages)"
        } else if t.hasPrefix("history-done") {
            let count = t.dropFirst(12).trimmingCharacters(in: .whitespaces)
            status = count.isEmpty ? "Chat history transferred." : "Chat history transferred (\(count) messages)."
        } else if t == "history-skip" {
            status = "Linked without chat history."
        } else if t.hasPrefix("history-error ") {
            status = "The chat history transfer failed, linking continues without it:\n\(t.dropFirst(14))"
        } else if t.hasPrefix("sgnl://") || t.hasPrefix("tsdevice:") {
            if uri != t {
                uri = t
                status = "Please scan the QR code with your phone …"
            }
        }
    }

    private func answerTransfer(_ transfer: Bool) {
        guard !transferAnswered, let input else { return }
        transferAnswered = true
        status = transfer ? "Transferring chat history …" : "Linked without chat history."
        try? input.write(contentsOf: Data((transfer ? "transfer\n" : "skip\n").utf8))
    }

    private func cancel() {
        if let p = process {
            process = nil
            p.terminate()
        }
        onFinish(false)
    }
}
