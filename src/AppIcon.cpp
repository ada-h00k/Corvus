#include "AppIcon.h"

#include <QFont>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QStyleHints>
#include <QSvgRenderer>

// Ensure the embedded icon resource is linked into the application.
static void initIconsResource()
{
    Q_INIT_RESOURCE(icons);
}

namespace AppIcon {

static Mode s_mode = Mode::FollowTheme;

void setMode(Mode mode)
{
    s_mode = mode;
}

bool themeIsDark()
{
    switch (QGuiApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Dark: return true;
    case Qt::ColorScheme::Light: return false;
    default: return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    }
}

bool prefersDark()
{
    if (s_mode != Mode::FollowTheme) return s_mode == Mode::Dark;
    return themeIsDark();
}

// The artwork in packaging/, embedded via resources/icons.qrc.
static QSvgRenderer &artwork(const char *alias)
{
    initIconsResource();
    static QHash<QByteArray, QSvgRenderer *> renderers;
    QSvgRenderer *&r = renderers[alias];
    if (!r) r = new QSvgRenderer(QStringLiteral(":/icons/") + QLatin1String(alias));
    return *r;
}

static QPixmap paint(QSvgRenderer &svg, int size, int unread)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    const qreal s = size;
    svg.render(&p, QRectF(0, 0, s, s));

    if (unread > 0) {
        // Keep the badge legible in small system-tray icons.
        const qreal d = s * 0.62;
        const QRectF badge(s - d - s * 0.01, s * 0.01, d, d);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(badge.adjusted(-s * 0.03, -s * 0.03, s * 0.03, s * 0.03));
        p.setBrush(QColor(0xe5, 0x39, 0x35));
        p.drawEllipse(badge);

        const QString label = unread > 99
            ? (size < 32 ? QStringLiteral("99") : QStringLiteral("99+"))
            : QString::number(unread);
        QFont f;
        f.setBold(true);
        const int fontPx = size < 32
            ? (unread > 9 ? 7 : 8)
            : int(d * (unread > 99 ? 0.32 : unread > 9 ? 0.46 : 0.60));
        f.setPixelSize(fontPx);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(badge, Qt::AlignCenter, label);
    }
    return pm;
}

QPixmap render(int size, int unread)
{
    return render(size, unread, prefersDark());
}

QPixmap render(int size, int unread, bool dark)
{
    return paint(artwork(dark ? "app-dark.svg" : "app-light.svg"), size, unread);
}

QPixmap renderTray(int size, int unread, bool dark)
{
    return paint(artwork(dark ? "tray-dark.svg" : "tray-light.svg"), size, unread);
}

QIcon app()
{
    // Qt's Wayland plugin (xdg-toplevel-icon, seen with Qt 6.12) paints the window icon into a
    // straight-alpha buffer but announces it as ARGB8888, which Wayland defines as premultiplied.
    // Half-transparent edge pixels then come out as garbage (black / cyan specks in the title bar).
    // Handing it premultiplied values disguised as straight alpha makes the buffer hold what the
    // compositor expects.
    const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    QIcon icon;
    for (int sz : {16, 22, 24, 32, 48, 64, 128, 256}) {
        if (!wayland) {
            icon.addPixmap(render(sz));
            continue;
        }
        QImage img = render(sz).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        img.reinterpretAsFormat(QImage::Format_ARGB32);
        icon.addPixmap(QPixmap::fromImage(img));
    }
    return icon;
}

QIcon tray(int unread)
{
    QIcon icon;
    // The tray sits on the panel, so it always contrasts with the theme (light raven on a dark
    // theme, dark raven on a light one); the App icon setting only picks the tile.
    const bool dark = themeIsDark();
    for (int sz : {16, 22, 24, 32, 48, 64, 128}) icon.addPixmap(renderTray(sz, unread, dark));
    return icon;
}

} // namespace AppIcon
