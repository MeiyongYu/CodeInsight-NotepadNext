/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * "Synchronize Files" dialog: explains the sync, offers "Force all files to
 * be re-parsed", Start / Cancel. Start runs an incremental (or forced full)
 * ctags+cscope synchronization with a progress dialog that shows nothing but a
 * progress bar and can be cancelled at any time.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef SYNCHRONIZEFILESDIALOG_H
#define SYNCHRONIZEFILESDIALOG_H

#include <QDialog>

class QCheckBox;
class ProjectManager;

class SynchronizeFilesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SynchronizeFilesDialog(ProjectManager *projectManager, QWidget *parent = nullptr);

private slots:
    void start();

private:
    ProjectManager *manager = nullptr;
    QCheckBox *forceCheckBox = nullptr;
};

#endif // SYNCHRONIZEFILESDIALOG_H
