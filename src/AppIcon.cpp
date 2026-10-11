#include "AppIcon.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

namespace AppIcon {

QPixmap render(int size, int unread)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    const qreal s = size;

    // background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2c, 0x6b, 0xed));
    p.drawRoundedRect(QRectF(s * 0.04, s * 0.04, s * 0.92, s * 0.92), s * 0.22, s * 0.22);

    // chat bubble with a small tail
    QPainterPath bubble;
    bubble.addEllipse(QRectF(s * 0.20, s * 0.22, s * 0.60, s * 0.50));
    QPainterPath tail;
    tail.moveTo(s * 0.30, s * 0.62);
    tail.lineTo(s * 0.24, s * 0.82);
    tail.lineTo(s * 0.46, s * 0.68);
    tail.closeSubpath();
    p.setBrush(Qt::white);
    p.drawPath(bubble.united(tail));

    // three dots
    p.setBrush(QColor(0x2c, 0x6b, 0xed));
    const qreal r = s * 0.045;
    for (int i = 0; i < 3; ++i) p.drawEllipse(QPointF(s * (0.37 + 0.13 * i), s * 0.47), r, r);

    if (unread > 0) {
        const qreal d = s * 0.50;
        const QRectF badge(s - d - s * 0.01, s * 0.01, d, d);
        p.setBrush(Qt::white);
        p.drawEllipse(badge.adjusted(-s * 0.03, -s * 0.03, s * 0.03, s * 0.03));
        p.setBrush(QColor(0xe5, 0x39, 0x35));
        p.drawEllipse(badge);
        if (size >= 32) {
            QFont f;
            f.setBold(true);
            f.setPixelSize(int(d * (unread > 99 ? 0.40 : unread > 9 ? 0.52 : 0.68)));
            p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, unread > 99 ? QStringLiteral("99+") : QString::number(unread));
        }
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
