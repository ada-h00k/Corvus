// Voice messages: format choice, names, durations and the signal-cli version check.
#include "VoiceRecorder.h"

#include <QTest>

class VoiceRecorderTest : public QObject
{
    Q_OBJECT
private slots:
    void prefersAacInMp4()
    {
        const auto all = [](QMediaFormat::FileFormat, QMediaFormat::AudioCodec) { return true; };
        const auto f = VoiceRecording::chooseFormat(all);
        QVERIFY(f);
        QCOMPARE(f->extension, QStringLiteral("m4a"));
        QCOMPARE(f->file, QMediaFormat::MPEG4);
        QCOMPARE(f->codec, QMediaFormat::AudioCodec::AAC);
    }

    void fallsBackInOrder()
    {
        using FF = QMediaFormat::FileFormat;
        using AC = QMediaFormat::AudioCodec;
        // no AAC: Opus in Ogg
        auto opus = VoiceRecording::chooseFormat([](FF f, AC c) { return f == QMediaFormat::Ogg && c == AC::Opus; });
        QVERIFY(opus);
        QCOMPARE(opus->extension, QStringLiteral("ogg"));
        QCOMPARE(opus->codec, AC::Opus);
        // Vorbis only
        auto vorbis = VoiceRecording::chooseFormat([](FF f, AC c) { return f == QMediaFormat::Ogg && c == AC::Vorbis; });
        QVERIFY(vorbis && vorbis->codec == AC::Vorbis);
        // only uncompressed audio
        auto wav = VoiceRecording::chooseFormat([](FF f, AC c) { return f == QMediaFormat::Wave && c == AC::Wave; });
        QVERIFY(wav);
        QCOMPARE(wav->extension, QStringLiteral("wav"));
        // nothing at all
        QVERIFY(!VoiceRecording::chooseFormat([](FF, AC) { return false; }));
    }

    void namesTheFileByTime()
    {
        const QDateTime t(QDate(2026, 10, 27), QTime(14, 5, 9));
        QCOMPARE(VoiceRecording::baseName(t), QStringLiteral("voice-message-2026-10-27-14-05-09"));
    }

    void formatsDurations()
    {
        QCOMPARE(VoiceRecording::durationText(0), QStringLiteral("0:00"));
        QCOMPARE(VoiceRecording::durationText(7400), QStringLiteral("0:07"));
        QCOMPARE(VoiceRecording::durationText(59999), QStringLiteral("0:59"));
        QCOMPARE(VoiceRecording::durationText(12 * 60 * 1000 + 3000), QStringLiteral("12:03"));
        QCOMPARE(VoiceRecording::durationText(3723000), QStringLiteral("1:02:03"));
        QCOMPARE(VoiceRecording::durationText(-5), QStringLiteral("0:00"));
    }

    void voiceNotesNeedSignalCli_0_14_2()
    {
        QVERIFY(VoiceRecording::supportsVoiceNote("0.14.2"));
        QVERIFY(VoiceRecording::supportsVoiceNote("0.14.9"));
        QVERIFY(VoiceRecording::supportsVoiceNote("0.15.0"));
        QVERIFY(VoiceRecording::supportsVoiceNote("signal-cli 0.14.9"));
        QVERIFY(!VoiceRecording::supportsVoiceNote("0.14.1"));
        QVERIFY(!VoiceRecording::supportsVoiceNote("0.13.12"));
        QVERIFY(!VoiceRecording::supportsVoiceNote(QString()));
        QVERIFY(!VoiceRecording::supportsVoiceNote("unknown"));
    }
};

QTEST_MAIN(VoiceRecorderTest)
#include "voice_recorder_test.moc"
