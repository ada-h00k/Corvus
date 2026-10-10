#include "LinkDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSysInfo>
#include <QStandardPaths>
#include <QVBoxLayout>

LinkDialog::LinkDialog(const QString &executable, QWidget *parent)
    : QDialog(parent), m_exe(executable)
{
    setWindowTitle(tr("Link device"));
    setMinimumWidth(420);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("Open Signal on your phone → Settings → Linked devices →\n"
                                 "“Link new device” and scan the QR code.")));

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Device name:")));
    m_name = new QLineEdit(QStringLiteral("signal-cli-qt (%1)").arg(QSysInfo::machineHostName()));
    row->addWidget(m_name, 1);
    m_start = new QPushButton(tr("Start"));
    row->addWidget(m_start);
    lay->addLayout(row);

    m_qr = new QLabel;
    m_qr->setAlignment(Qt::AlignCenter);
    m_qr->setMinimumHeight(40);
    lay->addWidget(m_qr);

    auto *uriRow = new QHBoxLayout;
    m_uri = new QLineEdit;
    m_uri->setReadOnly(true);
    m_uri->setPlaceholderText(tr("Link URI"));
    auto *copy = new QPushButton(tr("Copy"));
    uriRow->addWidget(m_uri, 1);
    uriRow->addWidget(copy);
    lay->addLayout(uriRow);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    auto *cancel = new QPushButton(tr("Cancel"));
    lay->addWidget(cancel, 0, Qt::AlignRight);

    connect(m_start, &QPushButton::clicked, this, &LinkDialog::startLinking);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_uri->text()); });
    connect(cancel, &QPushButton::clicked, this, &LinkDialog::reject);
}

LinkDialog::~LinkDialog() { killProcess(); }

void LinkDialog::startLinking()
{
    const QString name = m_name->text().trimmed();
    if (name.isEmpty()) return;
    killProcess();
    m_buf.clear();
    m_qr->clear();
    m_uri->clear();
    m_status->setText(tr("Waiting for signal-cli …"));
    m_start->setEnabled(false);
    m_name->setEnabled(false);

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &LinkDialog::onStdout);
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { onFinished(code); });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_status->setText(tr("signal-cli could not be started: %1").arg(m_proc->errorString()));
            m_start->setEnabled(true);
            m_name->setEnabled(true);
        }
    });
    m_proc->start(m_exe, {QStringLiteral("link"), QStringLiteral("-n"), name});
}

void LinkDialog::onStdout()
{
    m_buf += m_proc->readAllStandardOutput();
    for (const QByteArray &line : m_buf.split('\n')) {
        const QString s = QString::fromUtf8(line).trimmed();
        if (s.startsWith(QLatin1String("sgnl://")) || s.startsWith(QLatin1String("tsdevice:"))) {
            showUri(s);
            break;
        }
    }
}

void LinkDialog::showUri(const QString &uri)
{
    if (m_uri->text() == uri) return;
    m_uri->setText(uri);

    const QString qrencode = QStandardPaths::findExecutable(QStringLiteral("qrencode"));
    if (!qrencode.isEmpty()) {
        QProcess q;
        q.start(qrencode, {QStringLiteral("-t"), QStringLiteral("PNG"), QStringLiteral("-s"), QStringLiteral("6"),
                           QStringLiteral("-m"), QStringLiteral("2"), QStringLiteral("-o"), QStringLiteral("-"), uri});
        if (q.waitForFinished(5000) && q.exitCode() == 0) {
            QPixmap pm;
            if (pm.loadFromData(q.readAllStandardOutput(), "PNG")) {
                m_qr->setPixmap(pm);
                m_status->setText(tr("Please scan the QR code with your phone …"));
                return;
            }
        }
    }
    m_qr->setText(tr("To display the QR code, please install “qrencode”\n"
                     "(e.g. apt/brew install qrencode) or copy the URI into a QR generator."));
    m_status->setText(tr("Waiting for confirmation on your phone …"));
}

void LinkDialog::onFinished(int exitCode)
{
    if (exitCode == 0) {
        accept();
        return;
    }
    const QString err = m_proc ? QString::fromUtf8(m_proc->readAllStandardError()).trimmed() : QString();
    m_status->setText(tr("Linking failed (exit code %1).\n%2").arg(exitCode).arg(err));
    m_start->setEnabled(true);
    m_name->setEnabled(true);
}

void LinkDialog::reject()
{
    killProcess();
    QDialog::reject();
}

void LinkDialog::killProcess()
{
    if (!m_proc) return;
    QProcess *p = m_proc;
    m_proc = nullptr;
    p->disconnect(this);
    if (p->state() != QProcess::NotRunning) {
        p->terminate();
        if (!p->waitForFinished(2000)) {
            p->kill();
            p->waitForFinished(1000);
        }
    }
    p->deleteLater();
}
