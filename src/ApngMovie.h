#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QRect>
#include <QTimer>

// Qt ships no APNG image plugin: QImage decodes only the first frame of an
// animated PNG, so QMovie freezes on frame one. That is exactly why Signal's
// animated stickers stood still — the Signal sticker spec mandates APNG for
// animated stickers (not GIF/WebP), while our GIF/WebP playback kept working.
// This player parses the APNG chunk structure itself, rebuilds every frame as
// a standalone PNG that Qt's regular PNG decoder can read, composites them
// onto one canvas (dispose/blend per the APNG spec) and steps through the
// frames with a timer. Frames are decoded one at a time while they are shown
// (like a QMovie with CacheNone), so long animations stay cheap in memory.
class ApngMovie : public QObject
{
    Q_OBJECT
public:
    // Returns a player for animated PNG data or nullptr when the data is not
    // an APNG (static PNG, GIF, WebP, …) or any frame fails to decode.
    // Callers start playback with start().
    static ApngMovie *fromData(const QByteArray &data, QObject *parent = nullptr);

    ~ApngMovie() override;

    void start();
    void stop();
    bool isRunning() const { return m_running; }
    bool isPaused() const { return m_paused; }
    void setPaused(bool paused);

    // Fully composited canvas as it is currently on display.
    QPixmap currentPixmap() const { return m_canvas; }

signals:
    void updated();

private:
    struct Frame {
        QByteArray png;    // the frame rebuilt as a standalone PNG (tiny, decoded on demand)
        QRect region;      // placement of that region on the canvas
        int delayMs = 100;
        int disposeOp = 0; // 0 keep, 1 clear the region, 2 restore previous
        int blendOp = 0;   // 0 replace the region, 1 alpha over
    };

    explicit ApngMovie(QObject *parent);
    bool parse(const QByteArray &data);
    void compose(int index);
    void tick();

    QList<Frame> m_frames;
    QImage m_canvasImage;
    QImage m_previousImage;
    QPixmap m_canvas;
    QTimer m_timer;
    int m_index = -1;
    int m_loopsLeft = -1; // -1 = loop forever (acTL num_plays 0)
    bool m_running = false;
    bool m_paused = false;
};
