#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>

class QCheckBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

// "New poll" like the official clients: a question, 2..=10 options and the choice
// between single and multiple votes. The limits follow signal-cli / the Signal apps:
// 200 characters for the question, 100 for each option.
class PollDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PollDialog(QWidget *parent = nullptr);

    QString question() const;
    QStringList options() const; // trimmed, non-empty entries
    bool allowMultiple() const;

private:
    void addOptionRow(const QString &text = QString());
    void updateRows();        // renumber the placeholders, enable/disable buttons
    void send();

    QGridLayout *m_optionsLayout = nullptr;
    QList<QLineEdit *> m_optionEdits;
    QList<QToolButton *> m_optionRemoves;
    QLineEdit *m_questionEdit = nullptr;
    QPushButton *m_addOption = nullptr;
    QCheckBox *m_multiple = nullptr;
    QPushButton *m_send = nullptr;
    QLabel *m_error = nullptr;
};
