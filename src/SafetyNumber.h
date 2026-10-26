#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QString>

// Safety numbers as reported by signal-cli ("listIdentities").
namespace SafetyNumber {

struct Identity {
    QString number;
    QString uuid;
    QString fingerprint;
    QString safetyNumber;   // as delivered by signal-cli (digits, possibly grouped)
    QString scannable;      // base64 of the data inside the QR code ("scannableSafetyNumber")
    QString trustLevel;     // TRUSTED_VERIFIED, TRUSTED_UNVERIFIED, UNTRUSTED, ...
    qint64 addedMs = 0;

    QString digits() const;
    bool hasSafetyNumber() const;   // exactly 60 digits
    bool isVerified() const { return trustLevel == QLatin1String("TRUSTED_VERIFIED"); }
    bool isUntrusted() const { return trustLevel == QLatin1String("UNTRUSTED"); }
};

constexpr int kDigits = 60;

// Keeps the digits only ("12345 67890 …" -> "1234567890…").
QString digitsOnly(const QString &text);
// 60 digits -> 12 groups of five, four groups per line (like the official clients).
// Anything else is returned unchanged.
QString format(const QString &safetyNumber);
// Both texts hold the same 60 digits (spaces, line breaks and other separators are ignored).
bool matches(const QString &a, const QString &b);

// The bytes the official apps put into (and read from) their QR code: signal-cli reports them
// base64-encoded. Empty if the text is not valid base64.
QByteArray scannableBytes(const QString &base64);

// Result of "listIdentities" (array of objects).
QList<Identity> parse(const QJsonValue &result);
// The identity to show for `recipient` (number or UUID): the entry of that recipient with a
// usable safety number, newest first. -1 if there is none.
int pick(const QList<Identity> &identities, const QString &recipient);

// Has the safety number of a contact changed since we last saw (or the user last accepted) it?
// `storedDigits` is the number remembered for the contact, empty if we have never seen one.
// signal-cli marks a key that replaced a trusted one as UNTRUSTED; that counts as a change even
// when nothing is remembered yet.
enum class Change { None, Changed };
Change evaluate(const QString &storedDigits, const Identity &now);

// signal-cli refuses to send to a contact whose new key was not accepted yet
// (message: Untrusted Identity for "+49…").
bool isUntrustedIdentityError(const QString &error);

QString trustLabel(const QString &trustLevel); // translated

} // namespace SafetyNumber
