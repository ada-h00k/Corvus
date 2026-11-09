import AppKit
import CorvusCore
import ImageIO
import SwiftUI

/// Shows a sticker (or any image) and plays it when it is animated: WebP, APNG and GIF through
/// ImageIO's CGAnimateImageDataWithBlock. Still images are simply shown. Playback stops when the
/// view goes away (lazy grids and lists remove off-screen views).
struct AnimatedStickerView: NSViewRepresentable {
    let data: Data
    var animate = true

    func makeNSView(context: Context) -> NSImageView {
        let v = NSImageView()
        v.imageScaling = .scaleProportionallyUpOrDown
        v.animates = false
        v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        v.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
        context.coordinator.show(data, in: v, animate: animate)
        return v
    }

    func updateNSView(_ v: NSImageView, context: Context) {
        if context.coordinator.data != data || context.coordinator.animating != animate {
            context.coordinator.show(data, in: v, animate: animate)
        }
    }

    static func dismantleNSView(_ v: NSImageView, coordinator: Coordinator) { coordinator.stop() }

    func makeCoordinator() -> Coordinator { Coordinator() }

    @MainActor
    final class Coordinator {
        var data = Data()
        var animating = false
        private var stopFlag: StopFlag?

        /// Shared with ImageIO's callback: setting `stop` ends the animation at the next frame.
        final class StopFlag: @unchecked Sendable { var stop = false }

        func show(_ d: Data, in v: NSImageView, animate: Bool) {
            stop()
            data = d
            animating = animate
            v.image = NSImage(data: d)  // first frame / still image right away
            guard animate, let src = CGImageSourceCreateWithData(d as CFData, nil), CGImageSourceGetCount(src) > 1 else { return }
            let flag = StopFlag()
            stopFlag = flag
            weak var view = v
            CGAnimateImageDataWithBlock(d as CFData, nil) { _, image, stop in
                if flag.stop || view == nil {
                    stop.pointee = true
                    return
                }
                view?.image = NSImage(cgImage: image, size: .zero)
            }
        }

        func stop() {
            stopFlag?.stop = true
            stopFlag = nil
        }
    }
}

/// A sticker inside a message: no bubble, about 160 pt, animated once its image is loaded.
struct StickerAttachmentView: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let index: Int
    let attachment: Attachment
    static let side: CGFloat = 160

    var body: some View {
        let token = model.attachmentToken(message, index)
        Group {
            if let data = model.animatedGifs[token] {
                AnimatedStickerView(data: data)
            } else if !attachment.thumb.isEmpty, let image = NSImage(data: attachment.thumb) {
                Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
            } else {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(Theme.field)
                    .overlay { Image(systemName: "face.dashed").font(.largeTitle).foregroundStyle(.secondary) }
            }
        }
        .frame(width: Self.side, height: Self.side)
        .onAppear { model.loadAnimatedGif(message, index) }
        .help("Sticker")
        .accessibilityLabel("Sticker")
    }
}

/// Installed sticker packs: one scrolling grid with a section per pack, pack shortcuts at the bottom
/// and "+" to install a pack from a signal.art link. Choosing a sticker sends it.
struct StickerPicker: View {
    @Environment(AppModel.self) private var model
    let chosen: () -> Void
    @State private var query = ""

    var body: some View {
        let packs = model.stickerPacks.filter { $0.matches(query) }
        VStack(spacing: 6) {
            TextField("Search sticker packs …", text: $query)
                .textFieldStyle(.roundedBorder)
                .help("Search installed sticker packs by title, author or emoji")
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 8) {
                        ForEach(packs) { pack in
                            Text(pack.title)
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.secondary)
                                .padding(.top, 4)
                                .id(pack.id)
                            LazyVGrid(columns: Array(repeating: GridItem(.fixed(72), spacing: 6), count: 4), spacing: 6) {
                                ForEach(pack.stickers, id: \.id) { s in
                                    StickerCell(packId: pack.id, sticker: s) {
                                        model.sendSticker(packId: pack.id, stickerId: s.id)
                                        chosen()
                                    }
                                }
                            }
                        }
                        if packs.isEmpty && model.stickerPacksLoaded {
                            Text(model.stickerPacks.isEmpty
                                 ? "No sticker packs installed. Add one with + below."
                                 : "No sticker packs match your search.")
                                .foregroundStyle(.secondary)
                                .frame(maxWidth: .infinity)
                                .padding(.top, 30)
                        }
                    }
                    .padding(.horizontal, 2)
                }
                .frame(maxHeight: .infinity)
                if let s = model.stickerStatus {
                    Text(s.text)
                        .font(.caption)
                        .foregroundStyle(s.error ? Color.red : Color.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                Divider()
                HStack(spacing: 4) {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 4) {
                            ForEach(packs) { pack in
                                Button { withAnimation { proxy.scrollTo(pack.id, anchor: .top) } } label: {
                                    PackIcon(pack: pack)
                                }
                                .buttonStyle(.borderless)
                                .help(pack.author.isEmpty ? pack.title : "\(pack.title) – \(pack.author)")
                            }
                        }
                    }
                    Button { Task { await model.addStickerPack() } } label: {
                        Image(systemName: "plus").font(.system(size: 15, weight: .bold)).frame(width: 32, height: 32)
                    }
                    .buttonStyle(.borderless)
                    .help("Add sticker pack")
                    .accessibilityLabel("Add sticker pack")
                }
                .frame(height: 40)
            }
        }
        .padding(10)
        .frame(width: 340, height: 440)
        .onAppear { model.loadStickerPacks() }
    }
}

private struct StickerCell: View {
    @Environment(AppModel.self) private var model
    let packId: String
    let sticker: StickerPack.Sticker
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Group {
                if let data = model.stickerImages[AppModel.stickerKey(packId, sticker.id)] {
                    AnimatedStickerView(data: data, animate: hovering) // like Signal: animate under the pointer
                } else if !sticker.emoji.isEmpty {
                    Text(sticker.emoji).font(.system(size: 30)).opacity(0.4)
                } else {
                    ProgressView().controlSize(.small)
                }
            }
            .frame(width: 66, height: 66)
            .padding(3)
            .background(RoundedRectangle(cornerRadius: 8).fill(hovering ? Theme.field : Color.clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .onAppear { model.requestStickerImage(packId, sticker.id) }
        .help(sticker.emoji)
    }
}

private struct PackIcon: View {
    @Environment(AppModel.self) private var model
    let pack: StickerPack

    var body: some View {
        Group {
            if let first = pack.stickers.first, let data = model.stickerImages[AppModel.stickerKey(pack.id, first.id)],
               let image = NSImage(data: data) {
                Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
            } else {
                Text(String(pack.title.prefix(1))).font(.headline)
            }
        }
        .frame(width: 30, height: 30)
        .padding(2)
        .onAppear { if let first = pack.stickers.first { model.requestStickerImage(pack.id, first.id) } }
    }
}
