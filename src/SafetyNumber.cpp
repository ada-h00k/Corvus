#include "SafetyNumber.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

namespace SafetyNumber {

QString Identity::digits() const { return digitsOnly(safetyNumber); }

bool Identity::hasSafetyNumber() const { return digits().size() == kDigits; }

QString digitsOnly(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text)
        if (c >= QLatin1Char('0') && c <= QLatin1Char('9')) out += c;
    return out;
}

QString format(const QString &safetyNumber)
{
    const QString d = digitsOnly(safetyNumber);
    if (d.size() != kDigits) return safetyNumber;
    QString out;
    for (int i = 0; i < kDigits; i += 5) {
        if (i > 0) out += (i % 20 == 0) ? QLatin1Char('\n') : QLatin1Char(' ');
        out += d.mid(i, 5);
    }
    return out;
}

bool matches(const QString &a, const QString &b)
{
    const QString da = digitsOnly(a);
    return da.size() == kDigits && da == digitsOnly(b);
}

QByteArray scannableBytes(const QString &base64)
{
    const QByteArray text = base64.trimmed().toLatin1();
    if (text.isEmpty()) return {};
    const auto decoded = QByteArray::fromBase64Encoding(text, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? decoded.decoded : QByteArray();
}

QList<Identity> parse(const QJsonValue &result)
{
    QList<Identity> list;
    const QJsonArray array = result.isArray() ? result.toArray() : QJsonArray{result};
    for (const QJsonValue &v : array) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Identity id;
        id.number = o.value(QStringLiteral("number")).toString();
        id.uuid = o.value(QStringLiteral("uuid")).toString();
        id.fingerprint = o.value(QStringLiteral("fingerprint")).toString();
        id.safetyNumber = o.value(QStringLiteral("safetyNumber")).toString();
        id.scannable = o.value(QStringLiteral("scannableSafetyNumber")).toString();
        id.trustLevel = o.value(QStringLiteral("trustLevel")).toString();
        id.addedMs = static_cast<qint64>(o.value(QStringLiteral("addedTimestamp")).toDouble());
        if (id.number.isEmpty() && id.uuid.isEmpty() && id.safetyNumber.isEmpty()) continue;
        list << id;
    }
    return list;
}

int pick(const QList<Identity> &identities, const QString &recipient)
{
    int best = -1;
    bool bestExact = false;
    for (int i = 0; i < identities.size(); ++i) {
        const Identity &id = identities.at(i);
        if (!id.hasSafetyNumber()) continue;
        const bool exact = !recipient.isEmpty() && (id.number == recipient || id.uuid == recipient);
        if (best < 0 || (exact && !bestExact) ||
            (exact == bestExact && id.addedMs > identities.at(best).addedMs)) {
            best = i;
            bestExact = exact;
        }
    }
    return best;
}

Change evaluate(const QString &storedDigits, const Identity &now)
{
    if (!now.hasSafetyNumber()) return Change::None;
    if (storedDigits.isEmpty()) return now.isUntrusted() ? Change::Changed : Change::None;
    return digitsOnly(storedDigits) == now.digits() ? Change::None : Change::Changed;
}

bool isUntrustedIdentityError(const QString &error)
{
    return error.contains(QLatin1String("untrusted identity"), Qt::CaseInsensitive);
}

QString trustLabel(const QString &trustLevel)
{
    if (trustLevel == QLatin1String("TRUSTED_VERIFIED"))
        return QCoreApplication::translate("SafetyNumber", "Verified");
    if (trustLevel == QLatin1String("TRUSTED_UNVERIFIED"))
        return QCoreApplication::translate("SafetyNumber", "Not verified");
    if (trustLevel == QLatin1String("UNTRUSTED"))
        return QCoreApplication::translate("SafetyNumber", "Not trusted (the key has changed)");
    return trustLevel.isEmpty() ? QCoreApplication::translate("SafetyNumber", "Unknown") : trustLevel;
}

} // namespace SafetyNumber
