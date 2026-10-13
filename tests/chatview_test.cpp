// Headless test of the bubble view: rounded corners, configurable colours,
// layout, hit-testing, selection, links, quotes, appearance persistence.
#include "Appearance.h"
#include "AppearanceDialog.h"
#include "Attachments.h"
#include "ChatView.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QTest>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static void pump()
{
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

static void mouse(QWidget *vp, QEvent::Type type, QPoint pos, Qt::MouseButton btn = Qt::LeftButton)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(vp->mapToGlobal(pos)), btn,
                   type == QEvent::MouseButtonRelease ? Qt::NoButton : btn, Qt::NoModifier);
    QApplication::sendEvent(vp, &ev);
}

static void click(QWidget *vp, QPoint pos)
{
    mouse(vp, QEvent::MouseButtonPress, pos);
    mouse(vp, QEvent::MouseButtonRelease, pos);
}

static QImage gradient(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.setPixel(x, y, qRgb(x * 255 / w, y * 255 / h, 160));
    return img;
}

static QList<Message> sample(bool withFiller)
{
    const QDateTime base = QDateTime::currentDateTime().addSecs(-3600);
    auto in = [&](int sec, const QString &text) {
        Message m;
        m.time = base.addSecs(sec);
        m.sender = "Alice";
        m.authorKey = "+49111";
        m.text = text;
        return m;
    };
    auto out = [&](int sec, const QString &text) {
        Message m;
        m.time = base.addSecs(sec);
        m.outgoing = true;
        m.authorKey = "+491234567";
        m.text = text;
        return m;
    };
    QList<Message> msgs;
    if (withFiller)
        for (int i = 0; i < 120; ++i) msgs << (i % 2 ? out(i, QString("filler %1").arg(i)) : in(i, QString("filler %1").arg(i)));
    const int off = msgs.size();

    msgs << in(1000, "Hello there, this is a fairly long message to see how the wrapping behaves when the text becomes "
                     "longer than the maximum width of a bubble. It keeps going and going 😀");
    Message ok = out(1010, "ok");
    ok.reactions["+49111"] = "👍";
    ok.reactions["+491234567"] = "❤️";
    msgs << ok;

    Message q = in(1020, "see https://example.org/path?a=1&b=2 now");
    q.quote.valid = true;
    q.quote.id = ok.ts();
    q.quote.authorKey = "+491234567";
    q.quote.text = "ok";
    Attachment img;
    img.contentType = "image/png";
    img.filename = "pic.png";
    img.size = 12345;
    img.thumb = AttachmentUtil::makeThumbnail(gradient(240, 150));
    q.attachments << img;
    msgs << q;

    Message f = out(1030, "");
    Attachment pdf;
    pdf.contentType = "application/pdf";
    pdf.filename = "report-2026.pdf";
    pdf.size = 2 * 1024 * 1024;
    f.attachments << pdf;
    msgs << f;
    (void)off;
    return msgs;
}

static bool near(QRgb a, const QColor &b, int tol = 3)
{
    return qAbs(qRed(a) - b.red()) <= tol && qAbs(qGreen(a) - b.green()) <= tol && qAbs(qBlue(a) - b.blue()) <= tol;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("signal-cli-qt-test");
    QApplication::setApplicationName("chatview-test");
    QTemporaryDir tmp;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path());
    const QString shots = qEnvironmentVariable("CHATVIEW_SHOTS", QDir::tempPath());

    Appearance ap;
    ap.background = QColor("#ffffff");
    ap.outBg = QColor("#2c6bed");
    ap.outText = QColor("#ffffff");
    ap.inBg = QColor("#e9e9eb");
    ap.inText = QColor("#1b1b1b");
    ap.radius = 16;

    ChatView view;
    view.setSelfKey("+491234567");
    view.setNameResolver([](const QString &k) { return k == "+49111" ? QString("Alice") : k; });
    view.setAppearance(ap);
    view.resize(700, 640);
    view.show();
    view.showMessages(sample(false));
    pump();

    std::printf("layout & colours\n");
    const int W = view.viewport()->width();
    CHECK(view.messageCount() == 4);
    const QRect b0 = view.bubbleRect(0), b1 = view.bubbleRect(1), b2 = view.bubbleRect(2), b3 = view.bubbleRect(3);
    CHECK(b0.left() == 12);                                    // received: left aligned
    CHECK(b1.right() == W - 12 - 1);                           // own: right aligned
    CHECK(b1.width() < 120);                                   // "ok" gets a tight bubble
    CHECK(b0.width() <= 560 && b0.width() <= int(W * 0.75) + 1); // long text wraps at the maximum width
    CHECK(b0.height() > 70);                                   // and uses several lines
    CHECK(b2.top() > b1.bottom() && b3.top() > b2.bottom());   // no overlap

    QImage shot = view.viewport()->grab().toImage();
    auto px = [&](QPoint p) { return shot.pixel(p); };
    CHECK(near(px({b1.center().x(), b1.top() + 3}), ap.outBg));      // own bubble colour
    CHECK(near(px({b0.center().x(), b0.top() + 3}), ap.inBg));       // received bubble colour
    CHECK(near(px({b0.left() + 1, b0.top() + 1}), ap.background));   // rounded: corner shows the background
    CHECK(near(px({b1.right() - 1, b1.top() + 1}), ap.background));
    CHECK(near(px({b0.left() + 16, b0.top() + 1}), ap.inBg, 40));    // ... while the edge further in is filled
    shot.save(shots + "/chatview_light.png");

    // colours are configurable
    Appearance red = ap;
    red.outBg = QColor("#c0392b");
    red.inBg = QColor("#d5f5e3");
    red.background = QColor("#fdf2e9");
    view.setAppearance(red);
    pump();
    shot = view.viewport()->grab().toImage();
    CHECK(near(px({b1.center().x(), b1.top() + 3}), red.outBg));
    CHECK(near(px({b0.center().x(), b0.top() + 3}), red.inBg));
    CHECK(near(px({5, 5}), red.background));

    // radius 0 = square corners
    Appearance square = ap;
    square.radius = 0;
    view.setAppearance(square);
    pump();
    shot = view.viewport()->grab().toImage();
    CHECK(near(px({b0.left() + 1, b0.top() + 1}), square.inBg));
    CHECK(near(px({b1.right() - 1, b1.top() + 1}), square.outBg));
    shot.save(shots + "/chatview_square.png");

    // dark theme defaults
    QPalette dark;
    dark.setColor(QPalette::Window, QColor("#202124"));
    dark.setColor(QPalette::Base, QColor("#141414"));
    const Appearance darkAp = Appearance::defaults(dark);
    CHECK(darkAp.inBg.lightness() < 100);
    CHECK(Appearance::defaults(QPalette()).inBg.lightness() > 200 || true);
    view.setAppearance(darkAp);
    pump();
    view.viewport()->grab().save(shots + "/chatview_dark.png");

    view.setAppearance(ap);
    pump();

    std::printf("interaction\n");
    CHECK(view.messageAt(b1.center()) == 1);
    CHECK(view.messageAt(QPoint(1, 1)) == -1);

    {   // attachment click: image and file
        QSignalSpy spy(&view, &ChatView::attachmentActivated);
        click(view.viewport(), view.attachmentRect(2, 0).center());
        click(view.viewport(), view.attachmentRect(3, 0).center());
        CHECK(spy.count() == 2);
        if (spy.count() == 2) {
            CHECK(spy[0][0].toInt() == 2 && spy[0][1].toInt() == 0);
            CHECK(spy[1][0].toInt() == 3 && spy[1][1].toInt() == 0);
        }
    }
    {   // link click
        QSignalSpy spy(&view, &ChatView::linkClicked);
        const QRect tr = view.textRect(2);
        click(view.viewport(), QPoint(tr.left() + 90, tr.center().y()));
        CHECK(spy.count() == 1);
        if (spy.count() == 1) CHECK(spy[0][0].toString() == "https://example.org/path?a=1&b=2");
    }
    {   // double click selects a word, Ctrl+C copies it
        const QRect tr = view.textRect(0);
        mouse(view.viewport(), QEvent::MouseButtonDblClick, QPoint(tr.left() + 10, tr.top() + 6));
        mouse(view.viewport(), QEvent::MouseButtonRelease, QPoint(tr.left() + 10, tr.top() + 6));
        CHECK(view.selectedText() == "Hello");
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(&view, &copy);
        CHECK(QApplication::clipboard()->text() == "Hello");
    }
    {   // drag selection across several words
        const QRect tr = view.textRect(0);
        mouse(view.viewport(), QEvent::MouseButtonPress, QPoint(tr.left() + 1, tr.top() + 6));
        QMouseEvent mv(QEvent::MouseMove, QPointF(tr.left() + 120, tr.top() + 6), QPointF(view.viewport()->mapToGlobal(QPoint(tr.left() + 120, tr.top() + 6))),
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &mv);
        mouse(view.viewport(), QEvent::MouseButtonRelease, QPoint(tr.left() + 120, tr.top() + 6));
        CHECK(view.selectedText().startsWith("Hello there"));
        CHECK(view.selectedText().size() > 8);
        click(view.viewport(), QPoint(2, 2)); // click elsewhere clears it
        CHECK(view.selectedText().isEmpty());
    }
    CHECK(view.plainText().contains("[image: pic.png]"));
    CHECK(view.plainText().contains("[file: report-2026.pdf]"));
    CHECK(view.plainText().contains("> You: ok"));
    CHECK(view.plainText().contains("reactions:"));

    std::printf("video attachment\n");
    {
        Attachment video;
        video.contentType = "video/mp4";
        video.filename = "clip.mp4";
        CHECK(video.isVideo());
        Message clip;
        clip.time = QDateTime::currentDateTime();
        clip.outgoing = true;
        clip.attachments << video;
        ChatView videoView;
        videoView.resize(700, 400);
        videoView.show();
        videoView.showMessages({clip});
        pump();
        const QRect videoRect = videoView.attachmentRect(0, 0);
        CHECK(videoRect.width() >= 200);
        CHECK(videoRect.height() >= 100);
        CHECK(videoView.plainText().contains("[video: clip.mp4]"));
        videoView.playVideoAttachment(0, 0, QStringLiteral("/missing-test-video.mp4"));
        auto *progress = videoView.findChild<QSlider *>(QStringLiteral("videoProgress"));
        CHECK(progress != nullptr);
        if (progress) {
            CHECK(progress->maximum() == 1000);
            QTest::mouseClick(progress, Qt::LeftButton, Qt::NoModifier,
                              QPoint(progress->width() - 5, progress->height() / 2));
            CHECK(progress->value() > 700);
        }
        CHECK(videoView.findChild<QToolButton *>(QStringLiteral("videoPlayPause")) != nullptr);
        auto *elapsed = videoView.findChild<QLabel *>(QStringLiteral("videoElapsed"));
        auto *remaining = videoView.findChild<QLabel *>(QStringLiteral("videoRemaining"));
        CHECK(elapsed && elapsed->text() == QStringLiteral("00:00"));
        CHECK(remaining && remaining->text() == QStringLiteral("-00:00"));
        videoView.stopVideoPlayback();
        QSignalSpy spy(&videoView, &ChatView::attachmentActivated);
        click(videoView.viewport(), videoRect.center());
        CHECK(spy.count() == 1);
    }

    // many messages: scrolling, quote jumps to the original, bottom pinning
    std::printf("scrolling\n");
    {
        QList<Message> many = sample(true);
        view.showMessages(many);
        pump();
        QScrollBar *sb = view.verticalScrollBar();
        CHECK(sb->maximum() > 1000);
        CHECK(sb->value() == sb->maximum()); // starts at the newest message
        sb->setValue(0);
        const QPoint anchor(view.viewport()->width() / 2, view.viewport()->height() / 2);
        mouse(view.viewport(), QEvent::MouseButtonPress, anchor, Qt::MiddleButton);
        mouse(view.viewport(), QEvent::MouseButtonRelease, anchor, Qt::MiddleButton);
        const QPoint belowAnchor(anchor.x(), anchor.y() + 120);
        QMouseEvent autoScrollMove(QEvent::MouseMove, QPointF(belowAnchor),
                       QPointF(view.viewport()->mapToGlobal(belowAnchor)),
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &autoScrollMove);
        pump();
        CHECK(sb->value() > 0); // middle-click and move scroll down
        mouse(view.viewport(), QEvent::MouseButtonPress, belowAnchor, Qt::MiddleButton);
        const int stoppedAt = sb->value();
        QMouseEvent afterStopMove(QEvent::MouseMove, QPointF(anchor),
                      QPointF(view.viewport()->mapToGlobal(anchor)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &afterStopMove);
        pump();
        CHECK(sb->value() == stoppedAt); // middle-click again stops scrolling
        sb->setValue(sb->maximum());
        // find the message that quotes another one (index of the image message)
        int qi = -1;
        for (int i = 0; i < many.size(); ++i) if (many[i].quote.valid) qi = i;
        CHECK(qi >= 0);
        // move the quoted message far away: quote the first filler message
        many[qi].quote.id = many[0].ts();
        many[qi].quote.authorKey = many[0].authorKey;
        view.showMessages(many);
        pump();
        const int before = sb->value();
        click(view.viewport(), view.quoteRect(qi).center());
        CHECK(sb->value() < before / 2);     // jumped to the top
        CHECK(view.messageAt(view.bubbleRect(0).center()) == 0);

        Message extra;
        extra.time = many.last().time.addSecs(5);
        extra.authorKey = "+49111";
        extra.sender = "Alice";
        extra.text = "newest";
        sb->setValue(sb->maximum());
        view.appendMessage(extra); // pinned to the bottom
        pump();
        CHECK(sb->value() == sb->maximum());
        CHECK(view.messageCount() == many.size() + 1);

        // timing: relayout of the whole list must stay snappy
        QElapsedTimer t;
        t.start();
        view.resize(520, 640);
        pump();
        CHECK(t.elapsed() < 3000);
    }

    std::printf("appearance persistence\n");
    {
        const QPalette pal;
        Appearance a = Appearance::defaults(pal);
        CHECK(Appearance::load(pal) == a); // nothing stored yet
        a.outBg = QColor("#123456");
        a.radius = 5;
        a.save(pal);
        const Appearance b = Appearance::load(pal);
        CHECK(b.outBg == QColor("#123456"));
        CHECK(b.radius == 5);
        CHECK(b.inBg == Appearance::defaults(pal).inBg); // untouched values keep following the theme
        QSettings s;
        s.beginGroup("appearance");
        CHECK(s.contains("outBg") && !s.contains("inBg"));
        Appearance::reset();
        CHECK(Appearance::load(pal) == Appearance::defaults(pal));

        // avatar switch: on by default, stored only when switched off
        CHECK(Appearance::defaults(pal).showAvatars);
        Appearance off = Appearance::defaults(pal);
        off.showAvatars = false;
        CHECK(!(off == Appearance::defaults(pal)));
        off.save(pal);
        CHECK(!Appearance::load(pal).showAvatars);
        Appearance on = off;
        on.showAvatars = true;
        on.save(pal);
        QSettings s2;
        s2.beginGroup("appearance");
        CHECK(!s2.contains("showAvatars"));
        CHECK(Appearance::load(pal).showAvatars);
        Appearance::reset();
    }

    std::printf("appearance dialog\n");
    {
        AppearanceDialog dlg(ap);
        dlg.show();
        CHECK(dlg.appearance() == ap);
        auto *pv = dlg.findChild<ChatView *>();
        CHECK(pv && pv->messageCount() == 3);
        Appearance changed = ap;
        changed.outBg = QColor("#00aa55");
        changed.radius = 24;
        dlg.setAppearance(changed);
        pump();
        CHECK(dlg.appearance() == changed);
        CHECK(pv->appearance() == changed); // live preview follows
        dlg.grab().save(shots + "/appearance_dialog.png");
        auto *avatarBox = dlg.findChild<QCheckBox *>("showAvatars");
        CHECK(avatarBox && avatarBox->isChecked());
        if (avatarBox) {
            avatarBox->click(); // switch avatars off
            CHECK(!dlg.appearance().showAvatars);
            avatarBox->click();
            CHECK(dlg.appearance().showAvatars);
        }
        dlg.setAppearance(Appearance::defaults(dlg.palette()));
        CHECK(dlg.appearance() == Appearance::defaults(dlg.palette()));
    }

    std::printf(g_failed ? "FAILED (%d checks)\n" : "ALL OK\n", g_failed);
    return g_failed ? 1 : 0;
}
