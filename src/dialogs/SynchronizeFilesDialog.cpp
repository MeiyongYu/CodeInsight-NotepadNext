/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See SynchronizeFilesDialog.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "SynchronizeFilesDialog.h"
#include "ProjectManager.h"
#include "ProjectSyncProgressDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

SynchronizeFilesDialog::SynchronizeFilesDialog(ProjectManager *projectManager, QWidget *parent)
    : QDialog(parent),
      manager(projectManager)
{
    setWindowTitle(tr("Synchronize Files"));
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("This synchronizes the project database with your sources files."), this));
    layout->setSpacing(12);

    auto *separatorLabel = new QLabel(tr("Database Updates"), this);
    layout->addWidget(separatorLabel);

    forceCheckBox = new QCheckBox(tr("Force all files to be re-parsed"), this);
    // Checked by default: this dialog is opened by hand, and a full re-parse is
    // what reaching for it usually means. Unticking it still gives the plain
    // incremental run.
    forceCheckBox->setChecked(true);
    layout->addWidget(forceCheckBox);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto *startButton = new QPushButton(tr("Start"), this);
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    buttonRow->addWidget(startButton);
    buttonRow->addWidget(cancelButton);
    layout->addLayout(buttonRow);

    connect(startButton, &QPushButton::clicked, this, &SynchronizeFilesDialog::start);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

void SynchronizeFilesDialog::start()
{
    const bool force = forceCheckBox->isChecked();

    // Count the work first so the progress bar has a proper range.
    const ProjectManager::SyncDelta delta = manager->computeDelta(force);
    if (delta.empty()) {
        accept(); // nothing to do
        return;
    }

    // Progress dialog with nothing but a progress bar and a Cancel button: no
    // per-file / per-phase text, and the run really stops when it is cancelled.
    ProjectSyncProgressDialog progress(this);
    progress.setProgress(0, ProjectManager::syncStepCount(delta));
    progress.show();
    qApp->processEvents();

    QString errorMessage;
    bool cancelled = false;
    const bool ok = manager->synchronize(delta, [&progress](int step, int totalSteps, const QString &) {
        progress.setProgress(step, totalSteps);
        return !progress.wasCancelled();
    }, &errorMessage, &cancelled);

    progress.accept();

    if (cancelled) {
        reject(); // cancelled: leave the dialog without reporting success
        return;
    }

    if (!ok)
        QMessageBox::warning(this, tr("Synchronize Files"), errorMessage.isEmpty()
                             ? tr("Synchronization failed.") : errorMessage);

    accept();
}
