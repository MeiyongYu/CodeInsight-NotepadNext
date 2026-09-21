/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * ProjectPanelDock: the project content management dock shown on the right
 * side of the editor area while a project is open. Three tabs, in order:
 *
 *   1. Project Symbols  - symbol + file list, filter box, double click jumps
 *   2. Project Files    - file/dir/size/modified list, filter box, double
 *                         click opens the file
 *   3. Folders          - directory tree (left) + first level file list
 *                         (right); the directory follows the file currently
 *                         open in the editor
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef PROJECTPANELDOCK_H
#define PROJECTPANELDOCK_H

#include <QDockWidget>

class QLineEdit;
class QTabWidget;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class ProjectManager;
class SymbolTreeModel;

class ProjectPanelDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit ProjectPanelDock(ProjectManager *projectManager, QWidget *parent = nullptr);

    // Re-read the project file list from the manager.
    void refreshFiles();
    // Re-read the project symbol table from the manager.
    void refreshSymbols();
    // Reset the Folders tab to the project source root (expanded one level).
    void resetFolders();
    // Make the Folders tab follow the given file (editor switched).
    void trackEditorFile(const QString &filePath);

signals:
    // Symbol double clicked in "Project Symbols" (absolute path, 1-based line).
    void jumpToSymbolRequested(const QString &filePath, int line);
    // File double clicked in "Project Files".
    void openFileRequested(const QString &filePath);

private:
    // Show only the symbols matching the filter box (empty: every symbol).
    void applySymbolFilter(const QString &filter);

    // --- Folders page helpers ---
    // Lazily fill the subdirectory children of a Directory tree item
    // (level-by-level expansion).
    void fillFolderChildren(QTreeWidgetItem *dirItem);
    // First-level listing (dirs + files) of dir in the right-hand list.
    void populateFolderFileList(const QString &dir);
    // Expand the Directory tree level by level down to dir and select it.
    void revealDirectoryInFolderTree(const QString &dir);
    // Apply the last tracked editor file to the Folders page (no-op unless
    // the dock is visible and the Folders tab is the current one).
    void syncFoldersToEditorFile();

    ProjectManager *manager = nullptr;

    // --- Project Symbols page ---
    QLineEdit *symbolFilter = nullptr;
    QTreeView *symbolTree = nullptr;
    // Rows are handed out on demand, so the list has no cap: every matching
    // symbol is reachable through the scrollbar (see SymbolTreeModel).
    SymbolTreeModel *symbolModel = nullptr;

    // --- Project Files page ---
    QLineEdit *fileFilter = nullptr;
    QTreeWidget *fileTree = nullptr;

    // --- Folders page ---
    QLineEdit *folderPathEdit = nullptr;
    QTreeWidget *folderTree = nullptr;
    QTreeWidget *folderFileList = nullptr;
    QString folderRoot;
    // Last file reported by the editor; applied when the Folders tab shows.
    QString pendingEditorFile;
    int foldersTabIndex = -1;

    QTabWidget *tabs = nullptr;
};

#endif // PROJECTPANELDOCK_H
