/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * "Add and Remove Project Files" dialog.
 *
 * Layout (per the feature spec / mockup):
 *   - "File Name" path edit (Enter navigates the Directory tree)
 *   - Directory tree (left) | first level "File Name" list (right)
 *   - "Project Files (N)" list of absolute paths (bottom)
 *   - right button column: Close / Add / Add All / Add Tree / Remove Tree /
 *     Remove File / Remove All
 *
 * Every change goes straight into the ProjectManager, which keeps the
 * in-memory list and <project>.filelist in sync. On Close the caller
 * triggers an incremental synchronize (ctags + cscope).
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef PROJECTFILESDIALOG_H
#define PROJECTFILESDIALOG_H

#include <QDialog>

class QListWidget;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class ProjectManager;

class ProjectFilesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProjectFilesDialog(ProjectManager *projectManager, QWidget *parent = nullptr);

private slots:
    void onPathEdited();
    void onDirectoryExpanded(QTreeWidgetItem *item);
    void onDirectoryItemClicked(QTreeWidgetItem *item, int column);
    void onFileListSelectionChanged();
    void onFileEntryActivated(QTreeWidgetItem *item, int column);
    void addSelected();
    void addAllInCurrentDir();
    void addTreeFromCurrentDir();
    void removeSelectedFromTree();
    void removeSelectedFromProject();
    void removeAllFromProject();

private:
    void setCurrentDir(const QString &dir);
    void populateFileList(const QString &dir);
    void refreshProjectList();
    QStringList selectedProjectFiles() const;
    QString currentFileListDir() const;

    ProjectManager *manager = nullptr;
    QLineEdit *pathEdit = nullptr;
    QTreeWidget *dirTree = nullptr;
    QTreeWidget *fileList = nullptr;
    QListWidget *projectList = nullptr;
    QPushButton *addButton = nullptr;
    QPushButton *addAllButton = nullptr;
    QPushButton *addTreeButton = nullptr;
    QPushButton *removeTreeButton = nullptr;
    QPushButton *removeFileButton = nullptr;
    QPushButton *removeAllButton = nullptr;
    QString currentDir;
};

#endif // PROJECTFILESDIALOG_H
