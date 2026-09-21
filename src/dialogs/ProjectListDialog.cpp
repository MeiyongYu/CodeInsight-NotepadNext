/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See ProjectListDialog.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ProjectListDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

ProjectListDialog::ProjectListDialog(const QStringList &projectNames, const QHash<QString, QString> &projectPaths,
                                     bool removeMode, QWidget *parent)
    : QDialog(parent),
      removeMode(removeMode)
{
    setWindowTitle(removeMode ? tr("Remove Project") : tr("Open Project"));
    setMinimumSize(480, 360);

    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Project Name:"), this));

    nameEdit = new QLineEdit(this);
    nameEdit->setReadOnly(true);
    layout->addWidget(nameEdit);

    listWidget = new QListWidget(this);
    for (const QString &name : projectNames) {
        QString entry = name;
        const QString path = projectPaths.value(name);
        if (!path.isEmpty())
            entry += QStringLiteral("  (%1)").arg(QDir::toNativeSeparators(path));
        QListWidgetItem *item = new QListWidgetItem(entry, listWidget);
        item->setData(Qt::UserRole, name);
        listWidget->addItem(item);
    }
    layout->addWidget(listWidget, 1);

    // An empty list is exactly the situation "browse.." exists for, so say so
    // instead of letting the user face an empty box with a dead OK button.
    if (projectNames.isEmpty()) {
        // One literal on purpose: lupdate matches the string it finds in tr()
        // against the .ts, and a concatenation is one more thing that can stop
        // matching.
        auto *hint = new QLabel(tr("No projects are registered. Create one via Project -> New Project..., or use \"Browse..\" to open an existing project file (*.codeinsightprj)."), this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    okButton = buttonBox->button(QDialogButtonBox::Ok);
    okButton->setText(removeMode ? tr("Remove") : tr("OK"));
    okButton->setEnabled(false);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // "browse.." belongs in the lower left corner, opposite the OK/Cancel pair:
    // it is the way out when the list above it cannot help, so it must not be
    // mistaken for one of them. Remove mode has no use for it.
    auto *buttonRow = new QHBoxLayout();
    if (!removeMode) {
        browseButton = new QPushButton(tr("Browse.."), this);
        browseButton->setObjectName(QStringLiteral("browseProjectButton"));
        browseButton->setAutoDefault(false);
        connect(browseButton, &QPushButton::clicked, this, &ProjectListDialog::browseForProjectFile);
        buttonRow->addWidget(browseButton);
    }
    buttonRow->addStretch(1);
    buttonRow->addWidget(buttonBox);
    layout->addLayout(buttonRow);

    connect(listWidget, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        nameEdit->setText(item->data(Qt::UserRole).toString());
        okButton->setEnabled(true);
    });
    connect(listWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        nameEdit->setText(item->data(Qt::UserRole).toString());
        accept(); // double click opens/removes right away
    });
}

QString ProjectListDialog::selectedProject() const
{
    return nameEdit->text();
}

void ProjectListDialog::setBrowseHandler(std::function<BrowseOutcome(const QString &, QString *)> handler)
{
    browseHandler = std::move(handler);
}

void ProjectListDialog::setStartDirectory(const QString &dir)
{
    startDirectory = dir;
}

void ProjectListDialog::setProjectFileChooser(ProjectFileChooser chooser)
{
    projectFileChooser = std::move(chooser);
}

// "browse..": pick a <name>.codeinsightprj straight from the file system. The
// project it names is registered and opened by the handler, from inside this
// dialog's own event loop - only a project that is really open closes the
// window. A chooser that was closed without a pick, a user who cancels while
// the current project's files are being closed, and a project file that cannot
// be used all leave the window where it is.
void ProjectListDialog::browseForProjectFile()
{
    const QString startDir = startDirectory.isEmpty() ? QDir::homePath() : startDirectory;
    const QString projectFile =
        projectFileChooser
            ? projectFileChooser(startDir)
            : QFileDialog::getOpenFileName(this, tr("Open Project"), startDir,
                                           tr("Project files (*.codeinsightprj);;All files (*)"));
    if (projectFile.isEmpty())
        return; // closed without a pick: nothing was touched

    QString errorMessage;
    const BrowseOutcome outcome = browseHandler ? browseHandler(projectFile, &errorMessage)
                                                : BrowseOutcome::Failed;
    switch (outcome) {
    case BrowseOutcome::Opened:
        accept(); // the project is open: this window has done its job
        return;
    case BrowseOutcome::Cancelled:
        return; // the user backed out - not a failure, so no message either
    case BrowseOutcome::Failed:
        QMessageBox::warning(this, windowTitle(),
                             errorMessage.isEmpty() ? tr("The project file could not be opened.") : errorMessage);
        return;
    }
}
