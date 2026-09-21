/*
 * This file is part of Notepad Next.
 * Copyright 2019 Justin Dailey
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PROJECTMAINWINDOW_H
#define PROJECTMAINWINDOW_H

#include <QObject>

#include "ProjectListDialog.h"

class MainWindow;
class QAction;
class QMenu;
class ScintillaNext;
class CtagsSymbolManager;
class ProjectManager;
class ProjectPanelDock;
class ContextTracker;
class SearchResultsDock;

// One visited position of the jump history (see the ring inside ProjectMainWindow).
// Kept at file scope rather than nested in the class: moc cannot parse a nested
// struct that carries a member initialiser inside a Q_OBJECT class body.
struct JumpHistoryEntry
{
    QString file; // absolute path; an entry with an empty file is not recorded
    int line = 1; // 1-based
};

// The jump history is a fixed ring of this many entries.
constexpr int JumpHistoryCapacity = 40;

// The codeinsight project feature of the main window: the Project menu and
// panel, the function list, Ctrl+Click symbol jumping with its history and the
// offscreen self check. It is a QObject attached to one MainWindow and reaches
// the window's private app/dockedEditor/ui through the friend declaration in
// MainWindow.h, so MainWindow itself stays close to upstream: a thin
// forwarding layer only.
class ProjectMainWindow : public QObject
{
    Q_OBJECT

public:
    explicit ProjectMainWindow(MainWindow *mainWindow);

    // Creates the Project menu, the right-side project panel and the toolbar
    // jump actions. Called once from the MainWindow constructor, after
    // restoreSettings(), so the toolbar action is not dropped by a
    // Gui/ToolBar repopulation.
    void setupProjectFeature();

    // The project manager, for UI that has to follow the project lifecycle
    // (the find dialog greys out "Find All in Project Files" while no project is open).
    ProjectManager *getProjectManager() const { return projectManager; }

    // The editor showing filePath, opening the file when it is not open yet.
    ScintillaNext *editorForFilePath(const QString &filePath);

    // restoreWindowState() hook: the project panel follows the project
    // lifecycle, not the saved window state - hide it unless a project is open.
    void hideProjectPanelIfNoProject();

private slots:
    // Attached inside the editor pane, not a dock: one list per editor, wired
    // up when the editor is added; refreshed whenever it becomes active.
    void setupFunctionList(ScintillaNext *editor);
    void updateFunctionList(ScintillaNext *editor);

private:
    // Click on a symbol in the function list: switch to the editor and place the
    // target line in the upper-middle area of the view
    void jumpFunctionList(ScintillaNext *editor, int lineNumber);
    // updateFunctionList() plus a dropped ctags result, for changed files
    void reparseFunctionList(ScintillaNext *editor);
    static QString ctagsLanguageFor(const QString &languageName);

    // ---------- codeinsight project management ----------
    // Ctrl+Click (or the toolbar button) on a symbol: look it up in the
    // current file's function list first, then in the project symbol table.
    void jumpToSymbolAt(ScintillaNext *editor, int position);
    // The toolbar button right of "Jump to Symbol": every place that calls the
    // symbol under the caret. One call site jumps right away, several open the
    // same chooser the symbol jump uses.
    void findCallersAt(ScintillaNext *editor, int position);
    // Modal "ctags + cscope over the project delta" run with a progress bar.
    bool runProjectSynchronize(bool force);
    // Open the file if needed, switch to it and place the line upper-middle.
    // Returns false when the file could not be opened, so the caller can tell a
    // real jump from a no-op (the jump history only records real jumps).
    bool jumpToProjectSymbol(const QString &filePath, int line);

    // ---------- jump history (toolbar back / forward) ----------
    // The whole history is one head index into the ring plus the entries a walk
    // has already written: a slot that was never used is empty, and an empty
    // slot is the only thing that stops a walk. Nothing counts entries.

    // The position a jump starts from: the caret (or an explicit position) of
    // the given editor, as a history entry.
    JumpHistoryEntry jumpPositionFor(ScintillaNext *editor, int position = -1) const;
    // Records one user jump. Both ends are reduced to a file+line key first,
    // then each half is skipped when it already equals the entry the head sits
    // on: that covers the source of a run of jumps (it is the previous
    // destination) and the destination of a jump that lands where the caret
    // already is (a double click reports one and the same jump twice).
    void recordJump(const JumpHistoryEntry &source, const JumpHistoryEntry &destination);
    // Pushes one entry in front of the head.
    void pushJumpEntry(const JumpHistoryEntry &entry);
    // True when that slot holds a recorded position (its file is not empty).
    bool jumpSlotFilled(int index) const;
    // Index-only moves: back/forward never add or rewrite entries. False when
    // the neighbouring slot in that direction was never written to.
    bool moveJumpHeadBack();
    bool moveJumpHeadForward();
    void goBackJump();
    void goForwardJump();
    // Shows a recorded position again; false when its file cannot be opened any
    // more, in which case the caller drops the stale entry.
    bool navigateToHistoryEntry(const JumpHistoryEntry &entry);
    void clearJumpHistory();
    void updateJumpActionStates();
    // Offscreen regression hook (NOTEPADNEXT_PROJECT_SELFCHECK=1): drives the
    // whole project lifecycle without user interaction and quits the app.
    void runProjectSelfCheck();
    // Timing probe for one real (large) project, see the comment at the
    // definition: NOTEPADNEXT_SYNC_PROBE=<source root>.
    void runSyncProbe();
    // Drives the real "Synchronize Files" dialog of the probe project and
    // prints how the progress bar advances (see the definition).
    void runSyncUiProbe();
    // Time how long the GUI thread needs to load the symbol database and to
    // rebuild the symbol panel of an already parsed project
    // (NOTEPADNEXT_SYMBOL_PROBE=1 uses the shadow project of runSyncProbe,
    // NOTEPADNEXT_SYMBOL_PROBE=<project folder or .codeinsightprj> probes that
    // real project instead).
    void runSymbolProbe();

    MainWindow *window = Q_NULLPTR;

    CtagsSymbolManager *ctagsManager = Q_NULLPTR;
    QAction *functionListAction = Q_NULLPTR;

    // project management
    ProjectManager *projectManager = Q_NULLPTR;
    ProjectPanelDock *projectPanel = Q_NULLPTR;
    // Drives the Context view of the relation panel from the caret of the
    // active editor (see ContextTracker).
    ContextTracker *contextTracker = Q_NULLPTR;
    // The dock holding the search results and the relation panel. The Context
    // view lives on the latter, so opening a project brings that page up.
    SearchResultsDock *searchResultsDock = Q_NULLPTR;
    QMenu *projectMenu = Q_NULLPTR;
    QAction *currentProjectAction = Q_NULLPTR;
    QAction *newProjectAction = Q_NULLPTR;
    QAction *openProjectAction = Q_NULLPTR;
    QAction *closeProjectAction = Q_NULLPTR;
    QAction *removeProjectAction = Q_NULLPTR;
    QAction *addRemoveProjectFilesAction = Q_NULLPTR;
    QAction *synchronizeFilesAction = Q_NULLPTR;
    QAction *jumpBackAction = Q_NULLPTR;
    QAction *jumpForwardAction = Q_NULLPTR;
    QAction *jumpToSymbolAction = Q_NULLPTR;
    QAction *findCallersAction = Q_NULLPTR;

    // Jump history ring, addressed by one head. ring[head] is where the user
    // stands; walking back moves the index upwards (head + 1, wrapping 39 -> 0)
    // and forward moves it downwards (head - 1, wrapping 0 -> 39), the
    // mirroring the ring has to respect. A slot whose file is empty was never
    // filled in, and that is what a walk stops at.
    JumpHistoryEntry jumpHistoryRing[JumpHistoryCapacity];
    int jumpHistoryHead = 0; // index of the entry the user is standing on

    void newProjectDialog();
    void openProjectDialog();
    // Registers and opens a <name>.codeinsightprj that the user located by hand
    // through "Open Project" -> "browse..". Everything happens before the dialog
    // closes, so the outcome decides whether it stays on screen: Opened once the
    // project is really open, Cancelled when the user backed out of closing the
    // current project's files, Failed with errorMessage otherwise.
    ProjectListDialog::BrowseOutcome openProjectFromFile(const QString &projectFile, QString *errorMessage);
    // Where "browse.." starts: the open project's folder, else the first
    // registered project's folder, else the home directory.
    QString projectBrowserStartDirectory() const;
    void closeProjectRequested();
    // Closing a project also closes every file of it that is open: the files
    // listed in the project plus whatever lives below its source root. Untitled
    // buffers and files outside the project stay. Returns false when the user
    // cancels the save prompt, in which case nothing at all is closed.
    bool closeProjectFiles();
    void removeProjectDialog();
    void addRemoveProjectFilesDialog();
    void synchronizeProjectFilesDialog();
    void updateProjectActions();
};

#endif // PROJECTMAINWINDOW_H
