/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See ProjectSyncProgressDialog.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ProjectSyncProgressDialog.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

ProjectSyncProgressDialog::ProjectSyncProgressDialog(QWidget *parent, const QString &title)
    : QDialog(parent)
{
    setWindowTitle(title.isEmpty() ? tr("Synchronizing project") : title);
    setWindowModality(Qt::WindowModal);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);

    // The bar is the only progress display; QProgressBar shows the percentage
    // by default ("42%").
    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setMinimumWidth(320);
    layout->addWidget(progressBar);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setAutoDefault(false);
    buttonRow->addWidget(cancelButton);
    layout->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, this, &ProjectSyncProgressDialog::requestCancel);

    // A compact, non-resizable, bar-only window that still grows if a label
    // (e.g. "Cancelling...") needs more room.
    layout->setSizeConstraint(QLayout::SetFixedSize);
}

void ProjectSyncProgressDialog::setProgress(int step, int total)
{
    progressBar->setRange(0, qMax(1, total));
    progressBar->setValue(qBound(0, step, progressBar->maximum()));
    // The synchronization runs on the GUI thread, so the event loop has to be
    // pumped for the dialog to repaint and for the Cancel click to arrive.
    QCoreApplication::processEvents();
}

void ProjectSyncProgressDialog::requestCancel()
{
    if (cancelled)
        return;

    cancelled = true;
    // Keep the dialog visible until the operation really stopped, but make it
    // obvious that the request has been registered.
    cancelButton->setEnabled(false);
    cancelButton->setText(tr("Cancelling..."));
}

void ProjectSyncProgressDialog::closeEvent(QCloseEvent *event)
{
    // Closing the window is a cancellation request, never "hide and keep
    // synchronizing in the background".
    requestCancel();
    event->ignore();
}

void ProjectSyncProgressDialog::reject()
{
    // Esc goes through reject(): ask for cancellation instead of hiding.
    requestCancel();
}
