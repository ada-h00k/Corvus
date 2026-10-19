import SwiftUI

/// Search GIPHY (or browse trending GIFs) and pick one to send. Includes the required attribution and
/// GIPHY's analytics pings (onload when a preview is shown, onsent after sending).
struct GiphyPicker: View {
    let apiKey: String
    let onSelect: (URL, URL?) -> Void
    @Environment(\.dismiss) private var dismiss

    struct Gif: Identifiable {
        let id: String
        let title: String
        let preview: URL
        let original: URL
        let loadedAnalytics: URL?
        let sentAnalytics: URL?
    }

    @State private var query = ""
    @State private var status = "Loading trending GIFs …"
    @State private var gifs: [Gif] = []
    @State private var previews: [String: Data] = [:]
    @State private var searchTask: Task<Void, Never>?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TextField("Search GIFs", text: $query)
                .textFieldStyle(.roundedBorder)
                .onSubmit { run(debounce: false) }
                .onChange(of: query) { _, _ in run(debounce: true) }
            Text(status).font(.caption).foregroundStyle(.secondary)
            ScrollView {
                LazyVGrid(columns: Array(repeating: GridItem(.fixed(150), spacing: 6), count: 3), spacing: 6) {
                    ForEach(gifs) { g in
                        Button {
                            dismiss()
                            onSelect(g.original, g.sentAnalytics)
                        } label: {
                            ZStack {
                                RoundedRectangle(cornerRadius: 6).fill(Color.secondary.opacity(0.15))
                                if let data = previews[g.id] {
                                    AnimatedImage(data: data)
                                } else {
                                    ProgressView().controlSize(.small)
                                }
                            }
                            .frame(width: 150, height: 120)
                            .clipShape(RoundedRectangle(cornerRadius: 6))
                        }
                        .buttonStyle(.plain)
                        .disabled(previews[g.id] == nil)
                        .help(g.title)
                        .accessibilityLabel(g.title.isEmpty ? "GIF" : g.title)
                    }
                }
            }
            HStack {
                Link("Powered by GIPHY", destination: URL(string: "https://giphy.com")!).font(.caption)
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding()
        .frame(width: 500, height: 520)
        .onAppear { run(debounce: false) }
        .onDisappear { searchTask?.cancel() }
    }

    private func run(debounce: Bool) {
        searchTask?.cancel()
        let q = String(query.trimmingCharacters(in: .whitespaces).prefix(50))
        searchTask = Task {
            if debounce { try? await Task.sleep(for: .milliseconds(350)) }
            guard !Task.isCancelled else { return }
            await search(q)
        }
    }

    private static func get(_ url: URL) async throws -> Data {
        var req = URLRequest(url: url)
        req.setValue("corvus", forHTTPHeaderField: "User-Agent")
        let (data, resp) = try await URLSession.shared.data(for: req)
        if let code = (resp as? HTTPURLResponse)?.statusCode, code != 200 {
            throw URLError(.badServerResponse, userInfo: [NSLocalizedDescriptionKey: "HTTP \(code)"])
        }
        return data
    }

    private func search(_ q: String) async {
        gifs = []
        previews = [:]
        status = q.isEmpty ? "Loading trending GIFs …" : "Searching GIPHY …"
        var c = URLComponents(string: "https://api.giphy.com")!
        c.path = q.isEmpty ? "/v1/gifs/trending" : "/v1/gifs/search"
        c.queryItems = [URLQueryItem(name: "api_key", value: apiKey)] + (q.isEmpty ? [] : [URLQueryItem(name: "q", value: q)])
            + [URLQueryItem(name: "limit", value: "18"), URLQueryItem(name: "bundle", value: "messaging_non_clips"),
               URLQueryItem(name: "lang", value: "en")]
        let root: [String: Any]
        do {
            root = (try JSONSerialization.jsonObject(with: try await Self.get(c.url!))) as? [String: Any] ?? [:]
        } catch {
            if !Task.isCancelled { status = "GIPHY search failed: \(error.localizedDescription)" }
            return
        }
        guard !Task.isCancelled else { return }
        func https(_ s: Any?) -> URL? {
            guard let s = s as? String, let u = URL(string: s), u.scheme == "https" else { return nil }
            return u
        }
        var result: [Gif] = []
        for (i, v) in ((root["data"] as? [[String: Any]]) ?? []).enumerated() {
            let images = v["images"] as? [String: Any] ?? [:]
            let preview = https((images["fixed_width_small"] as? [String: Any])?["url"]) ?? https((images["fixed_width"] as? [String: Any])?["url"])
            guard let preview, let original = https((images["original"] as? [String: Any])?["url"]) else { continue }
            let analytics = v["analytics"] as? [String: Any] ?? [:]
            result.append(Gif(id: (v["id"] as? String ?? "") + "-\(i)", title: v["title"] as? String ?? "", preview: preview,
                              original: original,
                              loadedAnalytics: https((analytics["onload"] as? [String: Any])?["url"]),
                              sentAnalytics: https((analytics["onsent"] as? [String: Any])?["url"])))
        }
        gifs = result
        status = result.isEmpty ? "No GIFs found." : "Select a GIF to send."
        await withTaskGroup(of: (String, Data?, URL?).self) { group in
            for g in result {
                group.addTask { (g.id, try? await Self.get(g.preview), g.loadedAnalytics) }
            }
            for await (id, data, analytics) in group {
                guard !Task.isCancelled, let data else { continue }
                previews[id] = data
                if let analytics { Task.detached { _ = try? await Self.get(analytics) } }
            }
        }
    }
}
