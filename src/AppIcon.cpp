#include "AppIcon.h"

#include <QFont>
#include <QPainter>
#include <QSvgRenderer>

// Ensure the embedded icon resource is linked into the application.
static void initIconsResource()
{
    Q_INIT_RESOURCE(icons);
}

namespace AppIcon {

QPixmap render(int size, int unread)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    const qreal s = size;

    // packaging/wren.svg, embedded via resources/icons.qrc
    static QSvgRenderer renderer((initIconsResource(), QStringLiteral(":/icons/wren.svg")));
    renderer.render(&p, QRectF(0, 0, s, s));

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

QIcon app()
{
    QIcon icon;
    for (int sz : {16, 22, 24, 32, 48, 64, 128, 256}) icon.addPixmap(render(sz));
    return icon;
}

QIcon tray(int unread)
{
    QIcon icon;
    for (int sz : {16, 22, 24, 32, 48, 64, 128}) icon.addPixmap(render(sz, unread));
    return icon;
}

} // namespace AppIcon
