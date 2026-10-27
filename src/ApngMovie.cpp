#include "ApngMovie.h"

#include <QPainter>

namespace {

// APNG dispose/blend operations (values as defined by the APNG spec).
// dispose 0 ("none") needs no special handling: the canvas simply stays as-is.
// blend 0 ("source") is the fallback branch of the ternary in compose().
constexpr int kDisposeBackground = 1;
constexpr int kDisposePrevious = 2;
constexpr int kBlendOver = 1;

// Signal stickers are 512x512; anything far beyond that cannot come from a
// sticker and would only serve to burn memory during decode.
constexpr int kMaxCanvasDim = 2048;

QByteArray pngSignature()
{
    return QByteArrayLiteral("\x89PNG\r\n\x1a\n");
}

// CRC-32 over the chunk type + payload. Qt's PNG decoder rejects critical
// chunks with a bad CRC, so the frame chunks rebuilt below carry a real one.
quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    const int size = data.size();
    for (int i = 0; i < size; ++i)
        crc = table[(crc ^ quint32(quint8(data.at(i)))) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void putBigEndian(char *out, quint32 value)
{
    out[0] = char(value >> 24);
    out[1] = char(value >> 16);
    out[2] = char(value >> 8);
    out[3] = char(value);
}

quint32 readBigEndian(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    return (quint32(quint8(data.at(offset))) << 24) | (quint32(quint8(data.at(offset + 1))) << 16) |
           (quint32(quint8(data.at(offset + 2))) << 8) | quint32(quint8(data.at(offset + 3)));
}

quint32 readBigEndian16(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    return (quint32(quint8(data.at(offset))) << 8) | quint32(quint8(data.at(offset + 1)));
}

QByteArray pngChunk(const char *type, const QByteArray &payload)
{
    QByteArray out;
    char header[4];
    putBigEndian(header, quint32(payload.size()));
    out.append(header, 4);
    out.append(type);
    out.append(payload);
    char crc[4];
    putBigEndian(crc, crc32(QByteArray(type) + payload));
    out.append(crc, 4);
    return out;
}

// One frame's compressed scanlines only exist inside the APNG container.
// Wrapping them in a fresh IHDR (+ palette chunks) yields a normal PNG that
// Qt decodes with its regular PNG handler — no filter or colour-format math
// is duplicated here.
QByteArray framePng(int width, int height, int depth, int colorType, int interlace,
                    const QByteArray &plte, const QByteArray &trns, const QByteArray &zlibData)
{
    QByteArray ihdr;
    ihdr.resize(13);
    putBigEndian(ihdr.data(), quint32(width));
    putBigEndian(ihdr.data() + 4, quint32(height));
    ihdr[8] = char(depth);
    ihdr[9] = char(colorType);
    ihdr[10] = 0; // compression: deflate
    ihdr[11] = 0; // filter method: per-spec
    ihdr[12] = char(interlace);
    QByteArray png = pngSignature() + pngChunk("IHDR", ihdr);
    if (!plte.isEmpty()) png += pngChunk("PLTE", plte);
    if (!trns.isEmpty()) png += pngChunk("tRNS", trns);
    png += pngChunk("IDAT", zlibData);
    png += pngChunk("IEND", QByteArray());
    return png;
}

} // namespace

ApngMovie::ApngMovie(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ApngMovie::tick);
}

ApngMovie::~ApngMovie()
{
    m_timer.stop();
}

ApngMovie *ApngMovie::fromData(const QByteArray &data, QObject *parent)
{
    // Cheap early-out: every APNG is a PNG; GIF/WebP/… bail out immediately.
    if (!data.startsWith(pngSignature())) return nullptr;
    ApngMovie *movie = new ApngMovie(parent);
    if (!movie->parse(data)) {
        delete movie;
        return nullptr;
    }
    return movie;
}

bool ApngMovie::parse(const QByteArray &data)
{
    struct Chunk {
        QByteArray type;
        QByteArray payload;
    };
    QList<Chunk> chunks;

    int pos = pngSignature().size();
    while (pos + 8 <= data.size()) {
        const quint32 length = readBigEndian(data, pos);
        const qint64 end = qint64(pos) + 12 + qint64(length);
        if (end > data.size()) return false; // truncated chunk table
        chunks.append({data.mid(pos + 4, 4), data.mid(pos + 8, int(length))});
        pos = int(end);
        if (chunks.constLast().type == QByteArrayLiteral("IEND")) break;
    }
    if (chunks.isEmpty() || chunks.constLast().type != QByteArrayLiteral("IEND")) return false;

    int width = 0, height = 0, depth = 0, colorType = 0, interlace = 0;
    bool haveIhdr = false, haveActl = false;
    quint32 numPlays = 0;
    QByteArray plte, trns;
    QByteArray posterPng; // validated below, but not part of the animation

    // fcTL markers split the data stream into frames. A segment with no fcTL
    // is the poster image (default image without animation role); IDAT data
    // always spans the full canvas, fdAT data spans the fcTL region.
    struct Segment {
        QByteArray fc;
        QByteArray zlib;
        bool isDefaultImage = false;
    };
    QList<Segment> segments;
    QByteArray curFc, curZlib;
    bool curIsDefault = false;

    for (const Chunk &chunk : chunks) {
        if (chunk.type == QByteArrayLiteral("IHDR")) {
            if (chunk.payload.size() < 13) return false;
            width = int(readBigEndian(chunk.payload, 0));
            height = int(readBigEndian(chunk.payload, 4));
            depth = int(quint8(chunk.payload.at(8)));
            colorType = int(quint8(chunk.payload.at(9)));
            interlace = int(quint8(chunk.payload.at(12)));
            haveIhdr = true;
        } else if (chunk.type == QByteArrayLiteral("PLTE")) {
            plte = chunk.payload;
        } else if (chunk.type == QByteArrayLiteral("tRNS")) {
            trns = chunk.payload;
        } else if (chunk.type == QByteArrayLiteral("acTL")) {
            if (chunk.payload.size() >= 8) {
                numPlays = readBigEndian(chunk.payload, 4);
                haveActl = true;
            }
        } else if (chunk.type == QByteArrayLiteral("fcTL")) {
            if (!curFc.isEmpty() || !curZlib.isEmpty())
                segments.append({curFc, curZlib, curIsDefault});
            curFc = chunk.payload;
            curZlib = QByteArray();
            curIsDefault = false;
        } else if (chunk.type == QByteArrayLiteral("IDAT")) {
            if (curZlib.isEmpty()) curIsDefault = true;
            curZlib += chunk.payload;
        } else if (chunk.type == QByteArrayLiteral("fdAT")) {
            if (chunk.payload.size() >= 4) curZlib += chunk.payload.mid(4);
        } else if (chunk.type == QByteArrayLiteral("IEND")) {
            break;
        }
    }
    if (!curFc.isEmpty() || !curZlib.isEmpty())
        segments.append({curFc, curZlib, curIsDefault});

    if (!haveIhdr || !haveActl || width <= 0 || height <= 0) return false;
    if (width > kMaxCanvasDim || height > kMaxCanvasDim) return false;
    // The layouts Qt's PNG decoder handles natively; anything exotic stays on
    // the static first-frame path.
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false;
    if (colorType != 0 && colorType != 2 && colorType != 3 && colorType != 4 && colorType != 6)
        return false;
    if (interlace > 1) return false;

    for (const Segment &segment : segments) {
        if (segment.fc.isEmpty()) {
            // Poster image (default image without fcTL): not part of the
            // animation, but it is what a plain PNG decode would show.
            if (!posterPng.isEmpty()) return false;
            posterPng = framePng(width, height, depth, colorType, interlace,
                                 plte, trns, segment.zlib);
            continue;
        }
        if (segment.fc.size() < 26 || segment.zlib.isEmpty()) return false;
        if (m_frames.size() >= 1024) return false; // absurd, refuse politely

        Frame frame;
        int fw = int(readBigEndian(segment.fc, 4));
        int fh = int(readBigEndian(segment.fc, 8));
        const int fx = int(readBigEndian(segment.fc, 12));
        const int fy = int(readBigEndian(segment.fc, 16));
        const int delayNum = int(readBigEndian16(segment.fc, 20));
        const int delayDen = int(readBigEndian16(segment.fc, 22));
        frame.disposeOp = int(quint8(segment.fc.at(24)));
        frame.blendOp = int(quint8(segment.fc.at(25)));
        if (segment.isDefaultImage) {
            // The IDAT image always covers the whole canvas, whatever the
            // (spec-wise redundant) fcTL claims.
            fw = width;
            fh = height;
        }
        if (fw <= 0 || fh <= 0 || fx < 0 || fy < 0 || fx + fw > width || fy + fh > height)
            return false;
        frame.region = QRect(fx, fy, fw, fh);
        // delay_den 0 means "treat as 1/100 s"; 0 ms would busy-loop the timer.
        const int den = delayDen > 0 ? delayDen : 100;
        int delay = int(qRound(double(delayNum) * 1000.0 / double(den)));
        if (delay <= 0) delay = 100;
        frame.delayMs = delay;

        frame.png = framePng(frame.region.width(), frame.region.height(),
                             depth, colorType, interlace, plte, trns, segment.zlib);
        m_frames.append(frame);
    }

    // A single frame has nothing to animate — the caller keeps its static path.
    if (m_frames.size() < 2) return false;

    // Prove the container is decodable before claiming the animation: Qt must
    // read one rebuilt frame (and the poster, when present). The rest decodes
    // on demand while it is displayed, keeping memory bounded for long loops.
    if (QImage::fromData(m_frames.first().png, "PNG").isNull()) return false;
    if (!posterPng.isEmpty() && QImage::fromData(posterPng, "PNG").isNull()) return false;

    m_canvasImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    m_canvasImage.fill(Qt::transparent);
    m_index = -1;
    m_loopsLeft = numPlays > 0 ? int(numPlays) : -1;
    return true;
}

void ApngMovie::compose(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    const Frame &frame = m_frames.at(index);

    // Frames are decoded while they are on display (QMovie-with-CacheNone
    // style); a broken mid-loop frame freezes us on the last good canvas.
    const QImage content = QImage::fromData(frame.png, "PNG");
    if (content.isNull()) {
        stop();
        return;
    }

    // Frame n-1's dispose rule decides what the canvas looks like before n.
    if (index == 0) {
        m_canvasImage.fill(Qt::transparent);
    } else {
        const Frame &previous = m_frames.at(index - 1);
        if (previous.disposeOp == kDisposePrevious) {
            m_canvasImage = m_previousImage;
        } else if (previous.disposeOp == kDisposeBackground) {
            QPainter eraser(&m_canvasImage);
            eraser.setCompositionMode(QPainter::CompositionMode_Source);
            eraser.fillRect(previous.region, Qt::transparent);
        }
    }
    if (frame.disposeOp == kDisposePrevious)
        m_previousImage = m_canvasImage.copy();

    QPainter painter(&m_canvasImage);
    // Blend 0 replaces the region including its alpha, 1 composites over it.
    painter.setCompositionMode(frame.blendOp == kBlendOver
                                   ? QPainter::CompositionMode_SourceOver
                                   : QPainter::CompositionMode_Source);
    painter.drawImage(frame.region, content);
    painter.end();

    m_canvas = QPixmap::fromImage(m_canvasImage);
}

void ApngMovie::tick()
{
    if (!m_running || m_paused || m_frames.isEmpty()) return;
    int next = m_index + 1;
    if (next >= m_frames.size()) {
        // acTL num_plays counts the loops after the first pass; 0 means forever.
        if (m_loopsLeft >= 0) {
            if (m_loopsLeft == 0) {
                stop(); // stay on the last frame
                return;
            }
            --m_loopsLeft;
        }
        next = 0;
    }
    m_index = next;
    compose(m_index);
    emit updated();
    m_timer.start(qMax(m_frames.at(m_index).delayMs, 10));
}

void ApngMovie::start()
{
    if (m_running || m_frames.isEmpty()) return;
    m_running = true;
    m_paused = false;
    m_index = -1;
    tick(); // draw frame 0 right away and schedule the first timer tick
}

void ApngMovie::stop()
{
    m_running = false;
    m_timer.stop(); // the last composed frame stays visible
}

void ApngMovie::setPaused(bool paused)
{
    if (m_paused == paused) return;
    m_paused = paused;
    if (!m_running) return;
    if (paused)
        m_timer.stop();
    else if (m_index >= 0)
        m_timer.start(qMax(m_frames.at(m_index).delayMs, 10));
}
