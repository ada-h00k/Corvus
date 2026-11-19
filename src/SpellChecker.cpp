#include "SpellChecker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>

#ifdef HAVE_HUNSPELL
#  if __has_include(<hunspell/hunspell.hxx>)
#    include <hunspell/hunspell.hxx>
#  else
#    include <hunspell.hxx>
#  endif
#endif

namespace {

// ------------------------------------------------------------------ dictionaries on disk

QStringList dictionaryDirs()
{
    QStringList dirs;

    const QString dicPath = QString::fromLocal8Bit(qgetenv("DICPATH"));
    if (!dicPath.isEmpty())
        dirs += dicPath.split(QDir::listSeparator(), Qt::SkipEmptyParts);

    // The user's own dictionaries: ~/.local/share/corvus/corvus/dictionaries
    // (inside the Flatpak: ~/.var/app/<id>/data/corvus/corvus/dictionaries).
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/dictionaries");

    const QStringList dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &base : dataDirs) {
        dirs << base + QStringLiteral("/hunspell")
             << base + QStringLiteral("/myspell")
             << base + QStringLiteral("/myspell/dicts");
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    dirs << appDir + QStringLiteral("/dictionaries")
         << appDir + QStringLiteral("/../share/hunspell")
         << appDir + QStringLiteral("/../Resources/dictionaries");

    dirs << QStringLiteral("/app/share/hunspell")
         << QStringLiteral("/usr/share/hunspell")
         << QStringLiteral("/usr/share/myspell")
         << QStringLiteral("/usr/share/myspell/dicts")
         << QStringLiteral("/usr/local/share/hunspell")
         << QStringLiteral("/opt/homebrew/share/hunspell")
         << QStringLiteral("/Library/Spelling");

    QStringList result;
    QSet<QString> seen;
    for (const QString &d : std::as_const(dirs)) {
        const QString clean = QDir::cleanPath(d);
        if (clean.isEmpty() || seen.contains(clean)) continue;
        seen.insert(clean);
        if (QDir(clean).exists()) result << clean;
    }
    return result;
}

QString normalizedCode(QString code)
{
    code.replace(QLatin1Char('-'), QLatin1Char('_'));
    return code;
}

// code ("de_DE", "de_DE_frami", "fr") -> path without the .dic/.aff extension.
// The first folder in the search order wins.
QMap<QString, QString> findDictionaries()
{
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z]{2,3}([_-][A-Za-z0-9]+)*$"));
    QMap<QString, QString> found;
    const QStringList dirs = dictionaryDirs();
    for (const QString &dir : dirs) {
        const QDir d(dir);
        const QStringList dics = d.entryList({QStringLiteral("*.dic")}, QDir::Files, QDir::Name);
        for (const QString &file : dics) {
            const QString base = file.left(file.size() - 4);
            if (!valid.match(base).hasMatch()) continue; // hyph_de_DE.dic, th_*, …
            const QString path = d.filePath(base);
            if (!QFileInfo::exists(path + QStringLiteral(".aff"))) continue;
            const QString code = normalizedCode(base);
            if (!found.contains(code)) found.insert(code, path);
        }
    }
    return found;
}

// Picks the dictionary that fits the system language best.
[[maybe_unused]] QString autoLanguage(const QStringList &codes)
{
    if (codes.isEmpty()) return {};

    auto shortest = [](QStringList list) {
        std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        return list.first();
    };

    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &ui : uiLanguages) {
        const QStringList parts = normalizedCode(ui).split(QLatin1Char('_'));
        const QString lang = parts.value(0).toLower();
        if (lang.isEmpty()) continue;

        if (parts.size() > 1) {
            // "de_DE" before "de_AT"; a variant such as "de_DE_frami" is fine.
            const QString exact = lang + QLatin1Char('_') + parts.at(1).toUpper();
            QStringList hits;
            for (const QString &c : codes)
                if (c.compare(exact, Qt::CaseInsensitive) == 0
                    || c.startsWith(exact + QLatin1Char('_'), Qt::CaseInsensitive))
                    hits << c;
            if (!hits.isEmpty()) return shortest(hits);
        }

        QStringList sameLanguage;
        for (const QString &c : codes)
            if (c.compare(lang, Qt::CaseInsensitive) == 0
                || c.startsWith(lang + QLatin1Char('_'), Qt::CaseInsensitive))
                sameLanguage << c;
        if (!sameLanguage.isEmpty()) return shortest(sameLanguage);
    }
    return {};
}

// ------------------------------------------------------------------ words

bool isWordChar(QChar c)
{
    return c.isLetter() || c.isMark();
}

bool isJoiner(QChar c)
{
    return c == QLatin1Char('\'') || c == QChar(0x2019) || c == QLatin1Char('-');
}

QString personalFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/spellcheck-personal.txt");
}

} // namespace

// ===================================================================== SpellChecker

struct SpellChecker::Impl
{
#ifdef HAVE_HUNSPELL
    std::unique_ptr<Hunspell> hunspell;
    QStringEncoder encoder;
    QStringDecoder decoder;
#endif
    QString requested; // what the user chose ("" = automatic)
    QString language;  // what is loaded
    QSet<QString> personal; // lower case
    QSet<QString> ignored;  // lower case, this session only

#ifdef HAVE_HUNSPELL
    bool setEncoding(const QString &name)
    {
        QString n = name.trimmed().toUpper();
        if (n == QLatin1String("UTF-8") || n == QLatin1String("UTF8")) {
            encoder = QStringEncoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
            decoder = QStringDecoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        } else if (n == QLatin1String("ISO8859-1") || n == QLatin1String("ISO-8859-1") || n == QLatin1String("LATIN1")) {
            encoder = QStringEncoder(QStringConverter::Latin1, QStringConverter::Flag::Stateless);
            decoder = QStringDecoder(QStringConverter::Latin1, QStringConverter::Flag::Stateless);
        } else {
            n.replace(QRegularExpression(QStringLiteral("^ISO8859-")), QStringLiteral("ISO-8859-"));
            const QByteArray raw = n.toLatin1();
            encoder = QStringEncoder(raw.constData(), QStringConverter::Flag::Stateless);
            decoder = QStringDecoder(raw.constData(), QStringConverter::Flag::Stateless);
        }
        return encoder.isValid() && decoder.isValid();
    }
#endif

    void reload()
    {
        language.clear();
#ifdef HAVE_HUNSPELL
        hunspell.reset();
        const QMap<QString, QString> dictionaries = findDictionaries();
        QString code = requested;
        if (code.isEmpty() || !dictionaries.contains(normalizedCode(code)))
            code = autoLanguage(dictionaries.keys());
        if (code.isEmpty()) return;
        code = normalizedCode(code);

        const QString base = dictionaries.value(code);
        auto hs = std::make_unique<Hunspell>(QFile::encodeName(base + QStringLiteral(".aff")).constData(),
                                             QFile::encodeName(base + QStringLiteral(".dic")).constData());
        const std::string encoding = hs->get_dict_encoding();
        if (!setEncoding(QString::fromStdString(encoding))) return;
        hunspell = std::move(hs);
        language = code;
#endif
    }

    void loadPersonal()
    {
        QFile f(personalFile());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        while (!f.atEnd()) {
            const QString w = QString::fromUtf8(f.readLine()).trimmed();
            if (!w.isEmpty()) personal.insert(w.toLower());
        }
    }

    void savePersonal() const
    {
        QDir().mkpath(QFileInfo(personalFile()).absolutePath());
        QSaveFile f(personalFile());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QStringList words = personal.values();
        words.sort();
        for (const QString &w : std::as_const(words)) f.write(w.toUtf8() + '\n');
        f.commit();
    }

    // The same word in the form Hunspell wants it.
    static QString clean(QString w)
    {
        w.replace(QChar(0x2019), QLatin1Char('\''));
        return w;
    }

#ifdef HAVE_HUNSPELL
    bool hunspellAccepts(const QString &w)
    {
        const QByteArray enc = encoder(w);
        if (encoder.hasError()) return false;
        return hunspell->spell(std::string(enc.constData(), size_t(enc.size())));
    }
#endif
};

SpellChecker::SpellChecker(QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>())
{
    d->loadPersonal();
    d->reload();
}

SpellChecker::~SpellChecker() = default;

bool SpellChecker::available()
{
#ifdef HAVE_HUNSPELL
    return true;
#else
    return false;
#endif
}

QStringList SpellChecker::availableLanguages()
{
    QStringList codes = findDictionaries().keys();
    std::sort(codes.begin(), codes.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(languageTitle(a), languageTitle(b)) < 0;
    });
    return codes;
}

QString SpellChecker::languageTitle(const QString &code)
{
    const QStringList parts = normalizedCode(code).split(QLatin1Char('_'));
    QString name = parts.value(0);
    QString territory;

    const QLocale locale(parts.size() > 1 ? parts.at(0) + QLatin1Char('_') + parts.at(1) : parts.at(0));
    if (locale.language() == QLocale::C || locale.language() == QLocale::AnyLanguage) return code;

    name = locale.nativeLanguageName();
    if (name.isEmpty()) return code;
    name[0] = name.at(0).toUpper();
    if (parts.size() > 1) territory = locale.nativeTerritoryName();

    QStringList detail;
    if (!territory.isEmpty()) detail << territory;
    if (parts.size() > 2) detail << parts.mid(2).join(QLatin1Char(' '));
    return detail.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, detail.join(QStringLiteral(", ")));
}

void SpellChecker::setLanguage(const QString &code)
{
    if (d->requested == code && !d->language.isEmpty()) return;
    d->requested = code;
    d->reload();
    emit changed();
}

QString SpellChecker::language() const
{
    return d->language;
}

QString SpellChecker::requestedLanguage() const
{
    return d->requested;
}

bool SpellChecker::isReady() const
{
#ifdef HAVE_HUNSPELL
    return d->hunspell != nullptr;
#else
    return false;
#endif
}

bool SpellChecker::isCorrect(const QString &word) const
{
#ifdef HAVE_HUNSPELL
    if (!d->hunspell) return true;
    const QString w = Impl::clean(word);
    if (w.isEmpty() || w.size() > 64) return true; // Hunspell is not meant for very long strings

    const QString lower = w.toLower();
    if (d->personal.contains(lower) || d->ignored.contains(lower)) return true;
    if (d->hunspellAccepts(w)) return true;

    // "E-Mail-Adresse", "Rock-'n'-Roll": fine if every part is.
    if (w.contains(QLatin1Char('-'))) {
        const QStringList parts = w.split(QLatin1Char('-'), Qt::SkipEmptyParts);
        if (parts.size() > 1) {
            for (const QString &part : parts) {
                if (part.size() < 2) continue;
                const QString pl = part.toLower();
                if (d->personal.contains(pl) || d->ignored.contains(pl)) continue;
                if (!d->hunspellAccepts(part)) return false;
            }
            return true;
        }
    }
    return false;
#else
    Q_UNUSED(word)
    return true;
#endif
}

QStringList SpellChecker::suggestions(const QString &word, int max) const
{
    QStringList result;
#ifdef HAVE_HUNSPELL
    if (!d->hunspell) return result;
    const QByteArray enc = d->encoder(Impl::clean(word));
    if (d->encoder.hasError()) return result;

    const std::vector<std::string> raw = d->hunspell->suggest(std::string(enc.constData(), size_t(enc.size())));
    for (const std::string &s : raw) {
        const QString text = d->decoder(QByteArrayView(s.data(), qsizetype(s.size())));
        if (text.isEmpty() || result.contains(text)) continue;
        result << text;
        if (result.size() >= max) break;
    }
#else
    Q_UNUSED(word)
    Q_UNUSED(max)
#endif
    return result;
}

void SpellChecker::addToDictionary(const QString &word)
{
    const QString w = Impl::clean(word).trimmed().toLower();
    if (w.isEmpty() || d->personal.contains(w)) return;
    d->personal.insert(w);
    d->savePersonal();
    emit changed();
}

void SpellChecker::ignoreWord(const QString &word)
{
    const QString w = Impl::clean(word).trimmed().toLower();
    if (w.isEmpty()) return;
    d->ignored.insert(w);
    emit changed();
}

QList<QPair<int, int>> SpellChecker::checkableWords(const QString &text)
{
    // Addresses and e-mail addresses are not prose.
    static const QRegularExpression urlLike(
        QStringLiteral(R"((?:[A-Za-z][A-Za-z0-9+.-]*://|www\.)\S+|\S+@\S+\.\S+)"));
    QList<QPair<int, int>> urlRanges;
    auto it = urlLike.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        urlRanges.append({int(m.capturedStart()), int(m.capturedLength())});
    }

    QList<QPair<int, int>> words;
    const int n = text.size();
    int i = 0;
    while (i < n) {
        if (!isWordChar(text.at(i))) { ++i; continue; }

        const int start = i;
        while (i < n) {
            if (isWordChar(text.at(i))) {
                ++i;
            } else if (isJoiner(text.at(i)) && i > start && i + 1 < n && isWordChar(text.at(i + 1))) {
                ++i;
            } else {
                break;
            }
        }
        const int length = i - start;

        // Text inside an address.
        bool skip = false;
        for (const auto &r : std::as_const(urlRanges)) {
            if (start >= r.first && start < r.first + r.second) { skip = true; break; }
        }
        if (skip) continue;

        // Part of something that is not a plain word: abc123, snake_case, @name, a/b, file.txt, #tag.
        const QChar prev = start > 0 ? text.at(start - 1) : QChar();
        const QChar next = i < n ? text.at(i) : QChar();
        if (prev.isDigit() || next.isDigit()) continue;
        if (prev == QLatin1Char('_') || next == QLatin1Char('_')) continue;
        if (prev == QLatin1Char('@') || prev == QLatin1Char('#') || prev == QLatin1Char('/') || prev == QLatin1Char('\\')) continue;
        if (next == QLatin1Char('/') || next == QLatin1Char('\\') || next == QLatin1Char('@')) continue;
        if (next == QLatin1Char('.') && i + 1 < n && (text.at(i + 1).isLetterOrNumber())) continue;
        if (prev == QLatin1Char('.') && start >= 2 && text.at(start - 2).isLetterOrNumber()) continue;

        if (length < 2 || length > 64) continue;

        // Abbreviations such as "USB" or "HTTP".
        const QString word = text.mid(start, length);
        if (word == word.toUpper() && word != word.toLower()) continue;

        words.append({start, length});
    }
    return words;
}

bool SpellChecker::wordAt(const QString &text, int position, int *start, int *length)
{
    const auto words = checkableWords(text);
    for (const auto &w : words) {
        if (position >= w.first && position <= w.first + w.second) {
            if (start) *start = w.first;
            if (length) *length = w.second;
            return true;
        }
    }
    return false;
}

// ================================================================== SpellHighlighter

SpellHighlighter::SpellHighlighter(QTextDocument *document, SpellChecker *checker)
    : QSyntaxHighlighter(document), m_checker(checker)
{
    if (m_checker)
        connect(m_checker, &SpellChecker::changed, this, [this] { if (m_active) rehighlight(); });
}

void SpellHighlighter::setActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    rehighlight();
}

void SpellHighlighter::setCursorPosition(int position)
{
    if (m_cursor == position) return;
    const int old = m_cursor;
    m_cursor = position;
    if (!m_active) return;

    QTextDocument *doc = document();
    if (!doc) return;
    QTextBlock oldBlock = old >= 0 ? doc->findBlock(old) : QTextBlock();
    QTextBlock newBlock = doc->findBlock(position);
    if (oldBlock.isValid()) rehighlightBlock(oldBlock);
    if (newBlock.isValid() && newBlock != oldBlock) rehighlightBlock(newBlock);
}

void SpellHighlighter::highlightBlock(const QString &text)
{
    if (!m_active || !m_checker || !m_checker->isReady() || text.isEmpty()) return;

    const QTextBlock block = currentBlock();
    const int blockStart = block.position();

    // Text that must not be checked because of its formatting (mentions, code).
    QList<QPair<int, int>> skipRanges;
    if (m_skip) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && m_skip(f.charFormat()))
                skipRanges.append({f.position() - blockStart, f.length()});
        }
    }

    QTextCharFormat mark;
    mark.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    mark.setUnderlineColor(QColor(0xdc, 0x32, 0x2f));

    const auto words = SpellChecker::checkableWords(text);
    for (const auto &w : words) {
        const int start = w.first, length = w.second;
        if (m_cursor >= 0 && m_cursor == blockStart + start + length) continue; // still typing it

        bool skip = false;
        for (const auto &r : std::as_const(skipRanges)) {
            if (start < r.first + r.second && start + length > r.first) { skip = true; break; }
        }
        if (skip) continue;

        if (!m_checker->isCorrect(text.mid(start, length)))
            setFormat(start, length, mark);
    }
}
