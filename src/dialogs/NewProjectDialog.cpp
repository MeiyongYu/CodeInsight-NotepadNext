/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See NewProjectDialog.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "NewProjectDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStyle>
#include <QVBoxLayout>

namespace {
// Default location for the project data files: ~/.codeinsight (works on
// Windows, macOS and Linux alike since QDir::home() is cross platform).
QString defaultProjectDataPath()
{
    return QDir::home().absoluteFilePath(QStringLiteral(".codeinsight"));
}

// Room a path field keeps beyond its own text, in characters. A project path is
// routinely longer than the default value, and a field that only just fits what
// is already in it makes the user scroll to check where a path points - which is
// exactly what happened with "C:/Users/<user>/.codeinsight" sitting flush
// against the frame.
constexpr int ExtraPathCharacters = 20;

// Width for a path field: the text it starts with, the extra characters, and
// what the widget spends on its own frame and internal text margin.
int pathFieldWidthFor(const QLineEdit *edit, int extraColumns)
{
    const QFontMetrics metrics(edit->font());
    const int frame = 2 * edit->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, Q_NULLPTR, edit);
    return metrics.horizontalAdvance(edit->text())
        + extraColumns * metrics.averageCharWidth() + frame + 8;
}
}

NewProjectDialog::NewProjectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Project"));

    auto *formLayout = new QFormLayout;

    // --- project name ---
    // Default name and the whole edit follow the project naming convention:
    // English letters, digits, '.', '-' and '_' only (no spaces).
    nameEdit = new QLineEdit(tr("Untitled"));
    nameEdit->selectAll();
    nameEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9._-]+")), nameEdit));
    formLayout->addRow(tr("New project name:"), nameEdit);

    // --- source root (second row) ---
    sourceRootEdit = new QLineEdit(QDir::homePath());
    sourceRootEdit->setMinimumWidth(pathFieldWidthFor(sourceRootEdit, ExtraPathCharacters));
    QPushButton *sourceBrowse = new QPushButton(tr("Browse..."));
    connect(sourceBrowse, &QPushButton::clicked, this, &NewProjectDialog::browseSourceRoot);
    auto *sourceRow = new QHBoxLayout;
    sourceRow->addWidget(sourceRootEdit, 1);
    sourceRow->addWidget(sourceBrowse);
    formLayout->addRow(tr("Project sourcecode root path:"), sourceRow);

    // --- data path (third row) ---
    dataPathEdit = new QLineEdit(defaultProjectDataPath());
    dataPathEdit->setMinimumWidth(pathFieldWidthFor(dataPathEdit, ExtraPathCharacters));
    QPushButton *dataBrowse = new QPushButton(tr("Browse..."));
    connect(dataBrowse, &QPushButton::clicked, this, &NewProjectDialog::browseDataPath);
    auto *dataRow = new QHBoxLayout;
    dataRow->addWidget(dataPathEdit, 1);
    dataRow->addWidget(dataBrowse);
    formLayout->addRow(tr("Project data path:"), dataRow);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    okButton = buttonBox->button(QDialogButtonBox::Ok);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(formLayout);
    layout->addWidget(buttonBox);

    // The dialog width follows from the path fields above - they are the widest
    // thing in here and the only widgets that stretch, so the room they were
    // given is what the dialog grows by. Keep a floor for narrow translations.
    setMinimumWidth(qMax(520, minimumSizeHint().width()));

    connect(nameEdit, &QLineEdit::textChanged, this, &NewProjectDialog::validateInput);
    connect(dataPathEdit, &QLineEdit::textChanged, this, &NewProjectDialog::validateInput);
    connect(sourceRootEdit, &QLineEdit::textChanged, this, &NewProjectDialog::validateInput);
    validateInput();
}

QString NewProjectDialog::projectName() const
{
    return nameEdit->text().trimmed();
}

QString NewProjectDialog::dataPath() const
{
    return dataPathEdit->text().trimmed();
}

QString NewProjectDialog::sourceRoot() const
{
    return sourceRootEdit->text().trimmed();
}

void NewProjectDialog::setAcceptHandler(std::function<QString()> handler)
{
    acceptHandler = std::move(handler);
}

void NewProjectDialog::accept()
{
    if (acceptHandler) {
        const QString error = acceptHandler();
        if (!error.isEmpty()) {
            // Report it over this dialog and stay open: the user is one field
            // away from a valid project, so the form must not disappear.
            QMessageBox::warning(this, windowTitle(), error);
            return;
        }
    }

    QDialog::accept();
}

void NewProjectDialog::browseDataPath()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Where do you want to store the project data files?"), dataPathEdit->text());
    if (!dir.isEmpty())
        dataPathEdit->setText(QDir::toNativeSeparators(dir));
}

void NewProjectDialog::browseSourceRoot()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Project Source Directory - the main location of your source files"), sourceRootEdit->text());
    if (!dir.isEmpty())
        sourceRootEdit->setText(QDir::toNativeSeparators(dir));
}

void NewProjectDialog::validateInput()
{
    const bool ok = !projectName().isEmpty() && !dataPath().isEmpty() && !sourceRoot().isEmpty();
    okButton->setEnabled(ok);
}
