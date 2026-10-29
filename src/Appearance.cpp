#include "Appearance.h"

#include <QSettings>
#include <QtGlobal>

Appearance Appearance::defaults(const QPalette &pal)
{
    Appearance a;
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    a.background = pal.color(QPalette::Base);
    a.outBg = QColor(0x2c, 0x6b, 0xed); // Signal blue
    a.outText = QColor(Qt::white);
    a.inBg = dark ? QColor(0x3a, 0x3a, 0x3c) : QColor(0xe9, 0xe9, 0xeb);
    a.inText = dark ? QColor(0xee, 0xee, 0xee) : QColor(0x1b, 0x1b, 0x1b);
    a.radius = 16;
    a.showAvatars = true;
    a.alwaysShowSend = false;
    a.generateLinkPreviews = true;
    a.sortAlphabetically = false;
#ifdef Q_OS_MACOS
    // Like Messages: own bubbles follow the system accent colour, slightly rounder.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    a.outBg = pal.color(QPalette::Accent);
#else
    a.outBg = pal.color(QPalette::Highlight);
#endif
    a.radius = 18;
#endif
    return a;
}

namespace {
struct Key {
    const char *name;
    QColor Appearance::*member;
};
const Key kKeys[] = {
    {"background", &Appearance::background}, {"outBg", &Appearance::outBg}, {"outText", &Appearance::outText},
    {"inBg", &Appearance::inBg},             {"inText", &Appearance::inText},
};
} // namespace

Appearance Appearance::load(const QPalette &pal)
{
    Appearance a = defaults(pal);
    QSettings s;
    s.beginGroup(QStringLiteral("appearance"));
    for (const Key &k : kKeys) {
        const QColor c(s.value(QLatin1String(k.name)).toString());
        if (c.isValid()) a.*(k.member) = c;
    }
    bool ok = false;
    const int r = s.value(QStringLiteral("radius")).toInt(&ok);
    if (ok) a.radius = qBound(0, r, 28);
    if (s.contains(QStringLiteral("showAvatars"))) a.showAvatars = s.value(QStringLiteral("showAvatars")).toBool();
    if (s.contains(QStringLiteral("sortAlphabetically")))
        a.sortAlphabetically = s.value(QStringLiteral("sortAlphabetically")).toBool();
    if (s.contains(QStringLiteral("alwaysShowSend")))
        a.alwaysShowSend = s.value(QStringLiteral("alwaysShowSend")).toBool();
    if (s.contains(QStringLiteral("generateLinkPreviews")))
        a.generateLinkPreviews = s.value(QStringLiteral("generateLinkPreviews")).toBool();
    const int icon = s.value(QStringLiteral("appIcon")).toInt(&ok);
    if (ok) a.appIcon = qBound(int(IconFollowTheme), icon, int(IconDark));
    return a;
}

void Appearance::save(const QPalette &pal) const
{
    const Appearance def = defaults(pal);
    QSettings s;
    s.beginGroup(QStringLiteral("appearance"));
    for (const Key &k : kKeys) {
        if ((*this).*(k.member) == def.*(k.member))
            s.remove(QLatin1String(k.name));
        else
            s.setValue(QLatin1String(k.name), ((*this).*(k.member)).name());
    }
    if (radius == def.radius)
        s.remove(QStringLiteral("radius"));
    else
        s.setValue(QStringLiteral("radius"), radius);
    if (showAvatars == def.showAvatars)
        s.remove(QStringLiteral("showAvatars"));
    else
        s.setValue(QStringLiteral("showAvatars"), showAvatars);
    if (sortAlphabetically == def.sortAlphabetically)
        s.remove(QStringLiteral("sortAlphabetically"));
    else
        s.setValue(QStringLiteral("sortAlphabetically"), sortAlphabetically);
    if (alwaysShowSend == def.alwaysShowSend)
        s.remove(QStringLiteral("alwaysShowSend"));
    else
        s.setValue(QStringLiteral("alwaysShowSend"), alwaysShowSend);
    if (generateLinkPreviews == def.generateLinkPreviews)
        s.remove(QStringLiteral("generateLinkPreviews"));
    else
        s.setValue(QStringLiteral("generateLinkPreviews"), generateLinkPreviews);
    if (appIcon == def.appIcon)
        s.remove(QStringLiteral("appIcon"));
    else
        s.setValue(QStringLiteral("appIcon"), appIcon);
}

void Appearance::reset()
{
    QSettings().remove(QStringLiteral("appearance"));
}

bool Appearance::operator==(const Appearance &o) const
{
    return background == o.background && outBg == o.outBg && outText == o.outText && inBg == o.inBg &&
           inText == o.inText && radius == o.radius && showAvatars == o.showAvatars &&
           sortAlphabetically == o.sortAlphabetically && alwaysShowSend == o.alwaysShowSend &&
           generateLinkPreviews == o.generateLinkPreviews && appIcon == o.appIcon;
}
