/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * Project synchronization progress dialog.
 *
 * Shows nothing but a progress bar (with its percentage) and a Cancel button:
 * the window title says what is running, the bar is the only progress display
 * (no per-file / per-phase text), and the operation can be cancelled at any
 * time. Closing the dialog (title bar X or Esc) is treated as a cancellation
 * request, it never leaves the synchronization running invisibly.
 *
 * Nothing in here is synchronization specific, so the other long project wide
 * operations reuse it with their own title: searching every project file also
 * blocks the GUI thread and has to stay cancellable.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef PROJECTSYNCPROGRESSDIALOG_H
#define PROJECTSYNCPROGRESSDIALOG_H

#include <QDialog>

class QCloseEvent;
class QProgressBar;
class QPushButton;

class ProjectSyncProgressDialog : public QDialog
{
    Q_OBJECT

public:
    // title names the operation shown in the window title; the default is the
    // project synchronization this dialog was written for.
    explicit ProjectSyncProgressDialog(QWidget *parent = nullptr, const QString &title = QString());

    // Pumps the event loop so the Cancel button stays clickable while the
    // caller blocks in the synchronization.
    void setProgress(int step, int total);
    bool wasCancelled() const { return cancelled; }

public slots:
    // Asks the running operation to stop at its next cancellation point.
    void requestCancel();

protected:
    void closeEvent(QCloseEvent *event) override;
    // Esc must not hide the dialog: route it through the cancel request.
    void reject() override;

private:
    QProgressBar *progressBar = nullptr;
    QPushButton *cancelButton = nullptr;
    bool cancelled = false;
};

#endif // PROJECTSYNCPROGRESSDIALOG_H
