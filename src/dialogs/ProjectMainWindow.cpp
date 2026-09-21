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

#include "ProjectMainWindow.h"
#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "NotepadNextApplication.h"
#include "ApplicationSettings.h"
#include "DockedEditor.h"
#include "EditorPane.h"
#include "EditorManager.h"
#include "LuaExtension.h"

#include "FunctionListWidget.h"
#include "CtagsSymbolManager.h"
#include "ProjectManager.h"
#include "ProjectPanelDock.h"
#include "SymbolTreeModel.h"
#include "SymbolJumpFilter.h"
#include "ContextPanel.h"
#include "ContextTracker.h"
#include "RelationForm.h"
#include "SearchResultsDock.h"

#include "NewProjectDialog.h"
#include "ProjectListDialog.h"
#include "ProjectFilesDialog.h"
#include "ProjectSyncProgressDialog.h"
#include "SmartFindReplaceDialog.h"
#include "SynchronizeFilesDialog.h"

#include <QFile>
#include <QDockWidget>
#include <QProgressBar>
#include <QDialog>
#include <QMenu>
#include <QMessageBox>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QSet>
#include <QCheckBox>
#include <QDir>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTreeView>
#include <QTreeWidget>
#include <QShortcut>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QApplication>
#include <QScrollBar>
#include <QToolButton>
#include <QPainter>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>

// Opt-in diagnostics for the function list: set NOTEPADNEXT_FUNCTIONLIST_DEBUG=1
// to trace why the panel stays empty (which language was detected, which ctags
// binary is used and how many symbols came back).
static bool functionListDebugEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("NOTEPADNEXT_FUNCTIONLIST_DEBUG");
    return enabled;
}

ProjectMainWindow::ProjectMainWindow(MainWindow *mainWindow) :
    QObject(mainWindow),
    window(mainWindow)
{
    // ---------- Function list (attached inside the editor, not a dock) ----------
    ctagsManager = new CtagsSymbolManager(this);

    functionListAction = window->ui->menuView->addAction(tr("函数列表"));
    functionListAction->setCheckable(true);
    functionListAction->setChecked(window->app->getSettings()->value("FunctionList/Visible", true).toBool());
    functionListAction->setObjectName(QStringLiteral("actionFunctionList"));
    connect(functionListAction, &QAction::toggled, this, [this](bool on) {
        window->app->getSettings()->setValue("FunctionList/Visible", on);
        for (ScintillaNext *editor : window->dockedEditor->editors()) {
            updateFunctionList(editor);
        }
    });

    // The language and file name maps of ctags are probed in the background at
    // start up. Re-evaluate every open file once they are there, so that a panel
    // which had to be guessed before the probes finished ends up in the right
    // state instead of staying wrong.
    connect(ctagsManager, &CtagsSymbolManager::knowledgeReady, this, [this]() {
        qInfo(Q_FUNC_INFO);
        const QVector<ScintillaNext *> editors = window->dockedEditor->editors();
        for (ScintillaNext *editor : editors)
            updateFunctionList(editor);
    });

    // Every editor gets its own function list wiring as it is added, and every
    // activation refreshes it. Both signals come from the main window side, so
    // MainWindow needs no extra calls in addEditor()/activateEditor().
    connect(window->dockedEditor, &DockedEditor::editorAdded, this, &ProjectMainWindow::setupFunctionList);
    connect(window, &MainWindow::editorActivated, this, &ProjectMainWindow::updateFunctionList);
}

// The project panel follows the project lifecycle, not the saved window state:
// restoreState() resurrects its visibility when the previous session ended with
// a project open, but a fresh start has no project yet (projects are only
// opened through the UI), so the panel would sit there empty.
void ProjectMainWindow::hideProjectPanelIfNoProject()
{
    if (projectPanel && projectManager && !projectManager->hasProject())
        projectPanel->hide();
}

void ProjectMainWindow::setupFunctionList(ScintillaNext *editor)
{
    EditorPane *pane = EditorPane::paneForEditor(editor);
    if (pane == Q_NULLPTR)
        return;

    FunctionListWidget *funcList = pane->functionList();

    // Click a symbol -> switch to this editor and jump to the line. A user jump,
    // so it is recorded (requirement 6); the source is where the click started.
    connect(funcList, &FunctionListWidget::jumpToLineRequested, this, [this, editor](int lineNumber) {
        const JumpHistoryEntry source = jumpPositionFor(editor);
        jumpFunctionList(editor, lineNumber);
        recordJump(source, JumpHistoryEntry{editor->getFilePath(), qMax(1, lineNumber)});
    });

    // Regenerate the symbol list when the file is saved/reloaded/renamed or when
    // its language changes. The cached result is dropped first, otherwise the
    // refresh below would consider the file already done. The symbols currently
    // on screen stay until the new ones arrive, so the panel does not flicker.
    connect(editor, &ScintillaNext::saved, this, [this, editor]() { reparseFunctionList(editor); });
    connect(editor, &ScintillaNext::reloaded, this, [this, editor]() { reparseFunctionList(editor); });
    connect(editor, &ScintillaNext::renamed, this, [this, editor]() { reparseFunctionList(editor); });
    // Language changed -> re-evaluate visibility/contents
    connect(editor, &ScintillaNext::lexerChanged, this, [this, editor]() { reparseFunctionList(editor); });

    // ---------- project management hooks ----------
    // Ctrl+LeftClick on a symbol -> jump (file function list, project table).
    SymbolJumpFilter *jumpFilter = new SymbolJumpFilter(editor, editor);
    editor->viewport()->installEventFilter(jumpFilter);
    connect(jumpFilter, &SymbolJumpFilter::symbolClicked, this, [this, editor](int position) {
        jumpToSymbolAt(editor, position);
    });

    // A saved project file goes into the background fileChangeList; a single
    // auto-sync thread drains the list (Save All feeds the same path).
    connect(editor, &ScintillaNext::saved, this, [this, editor]() {
        if (projectManager && projectManager->hasProject() && editor->isFile()
            && projectManager->isProjectFile(editor->getFilePath())) {
            projectManager->enqueueFileChange(editor->getFilePath());
        }
    });

    // File closed: clear the list and stop any in-flight background ctags
    // process. Only this file's request is cancelled - other editors may have
    // their own parse running at the same time.
    connect(editor, &ScintillaNext::closed, this, [this, editor]() {
        EditorPane *p = EditorPane::paneForEditor(editor);
        if (p)
            p->functionList()->clearSymbols();
        if (editor->isFile())
            ctagsManager->cancel(editor->getFilePath());
    });

    // Deliver results for this editor (queued from the worker thread).
    // The generation token guarantees only the newest request's result is applied.
    QPointer<ScintillaNext> editorGuard = editor;
    connect(ctagsManager, &CtagsSymbolManager::symbolsReady, this,
            [this, editorGuard](int generation, const QString &filePath, const QVector<FunctionSymbol> &symbols) {
                Q_UNUSED(generation)
                if (editorGuard.isNull())
                    return;
                ScintillaNext *editor = editorGuard.data();
                if (!editor->isFile() || editor->getFilePath() != filePath)
                    return;
                EditorPane *p = EditorPane::paneForEditor(editor);
                if (p == Q_NULLPTR)
                    return;

                // Order matters: clearing the busy flag first keeps the hint from
                // coming back when the list below turns out to be empty.
                FunctionListWidget *funcList = p->functionList();
                funcList->setBusy(false);
                funcList->setSymbols(symbols);

                // Visibility is not touched here on purpose: the panel was opened
                // when the parse was requested, once ctags had claimed the file.
                // A result without symbols therefore leaves it open and shows a
                // hint, which is what makes the panel appear the moment a file is
                // opened instead of a moment later.
                if (functionListDebugEnabled()) {
                    qInfo("FunctionList[debug]: %d symbol(s) delivered to %s, panel requested=%d",
                          int(symbols.size()), qUtf8Printable(filePath),
                          int(p->isFunctionListRequested()));

                    // Offscreen test hook: NOTEPADNEXT_FUNCTIONLIST_AUTOJUMP=1
                    // simulates a click on the first symbol so that the jump
                    // (switch editor + scroll position) can be verified without
                    // real mouse input.
                    if (!symbols.isEmpty() && qEnvironmentVariableIsSet("NOTEPADNEXT_FUNCTIONLIST_AUTOJUMP")) {
                        jumpFunctionList(editor, symbols.first().line);
                    }
                }
            });

    updateFunctionList(editor);
}

// The target line is placed about one third down the view ("upper middle"):
// SCI_GOTOLINE alone pins it to the very top edge, which hides all context
// above the function. One third keeps the signature comfortably high while the
// surrounding code (the body above, includes, callers) stays visible.
void ProjectMainWindow::jumpFunctionList(ScintillaNext *editor, int lineNumber)
{
    window->dockedEditor->switchToEditor(editor);

    const sptr_t target = lineNumber - 1;
    editor->gotoLine(target);

    const sptr_t visible = editor->linesOnScreen();
    const sptr_t offset = qMax<sptr_t>(1, visible / 3);
    editor->setFirstVisibleLine(qMax<sptr_t>(0, target - offset));
    editor->grabFocus();

    if (functionListDebugEnabled()) {
        qInfo("FunctionList[debug]: jump to line %d -> firstVisible=%d linesOnScreen=%d position=%.2f",
              lineNumber, int(editor->firstVisibleLine()) + 1, int(visible),
              visible > 0 ? double(target - editor->firstVisibleLine()) / visible : 0.0);
    }
}

// Single entry point of the function list. It is deliberately idempotent and
// cheap - the expensive part (ctags itself) always runs in the background - so
// it can be called on every file open, tab switch, save and menu toggle:
//
//   * the panel is opened as soon as ctags says it can handle the file, well
//     before any symbol exists, so switching to a file feels instant;
//   * the symbol list then fills in when the background parse delivers.
void ProjectMainWindow::updateFunctionList(ScintillaNext *editor)
{
    if (editor == Q_NULLPTR)
        return;

    EditorPane *pane = EditorPane::paneForEditor(editor);
    if (pane == Q_NULLPTR)
        return;

    FunctionListWidget *funcList = pane->functionList();
    funcList->setFileLabel(editor->getName());

    // Feature switch (view menu action). There is no language whitelist any more:
    // the language is resolved per file and ctags itself decides what it can
    // parse, so every language either project knows about is covered by
    // construction instead of by a table that has to be maintained.
    const bool enabled = functionListAction != Q_NULLPTR && functionListAction->isChecked();
    pane->setFunctionListEnabled(enabled);

    // Unsaved buffers have no file on disk yet, nothing can be parsed for them.
    const QString filePath = editor->isFile() ? editor->getFilePath() : QString();
    const QString ctagsLanguage = ctagsLanguageFor(editor->languageName);
    const bool supported = enabled && !filePath.isEmpty()
            && CtagsSymbolManager::canParse(filePath, ctagsLanguage);

    pane->setFunctionListVisible(supported);

    if (functionListDebugEnabled()) {
        qInfo("FunctionList[debug]: file=%s language=\"%s\" ctagsLanguage=\"%s\" supported=%d panel=%d symbols=%d pending=%d parsed=%d",
              qUtf8Printable(editor->getName()),
              qUtf8Printable(editor->languageName),
              ctagsLanguage.isEmpty() ? "(ctags auto-detect)" : qUtf8Printable(ctagsLanguage),
              int(supported), int(pane->isFunctionListRequested()),
              funcList->symbolCount(), int(ctagsManager->isPending(filePath)),
              int(ctagsManager->hasParsed(filePath)));
    }

    if (!supported) {
        // Nothing to show, and no cached result may survive: without this a file
        // switched off via the menu would still count as parsed and would come
        // back empty when the feature is switched on again.
        if (!filePath.isEmpty())
            ctagsManager->invalidate(filePath);
        funcList->setBusy(false);
        funcList->clearSymbols();
        return;
    }

    // The list is already up to date (or on its way): this is what keeps tab
    // switching from re-running ctags for every file over and over.
    if (ctagsManager->isPending(filePath) || ctagsManager->hasParsed(filePath))
        return;

    // Announce the parse while the panel is still empty, so the wait is visible.
    if (funcList->symbolCount() == 0)
        funcList->setBusy(true);

    ctagsManager->request(filePath, ctagsLanguage);
}

// The file changed on disk (or its language was changed), so whatever ctags said
// about it before is void.
void ProjectMainWindow::reparseFunctionList(ScintillaNext *editor)
{
    if (editor == Q_NULLPTR)
        return;

    if (editor->isFile())
        ctagsManager->invalidate(editor->getFilePath());

    updateFunctionList(editor);
}

// ---------------------------------------------------------------------------
// codeinsight project management
// ---------------------------------------------------------------------------

void ProjectMainWindow::setupProjectFeature()
{
    projectManager = new ProjectManager(window->app->getSettings(), window);

    // Right-side content management panel (Symbols | Files | Folders).
    projectPanel = new ProjectPanelDock(projectManager, window);
    projectPanel->hide();
    window->addDockWidget(Qt::RightDockWidgetArea, projectPanel);

    // ---- Context view of the relation panel ----
    // It shows the file the symbol under the caret of the active editor is
    // defined in. ContextTracker owns the "caret -> word -> symbol table ->
    // panel" path and decides when there is nothing to do.
    searchResultsDock = window->findChild<SearchResultsDock *>();

    if (searchResultsDock != Q_NULLPTR) {
        contextTracker = new ContextTracker(projectManager, searchResultsDock->contextPanel(), this);
        connect(window, &MainWindow::editorActivated, contextTracker, &ContextTracker::setCurrentEditor);
        contextTracker->setCurrentEditor(window->currentEditor());

        // The relation forms: the dock builds and lines them up; the caret
        // feeds the unlocked ones, and the refresh button of a form asks for
        // the symbol under the caret right now.
        searchResultsDock->initRelationPanel(projectManager);
        connect(searchResultsDock, &SearchResultsDock::relationFormRefreshRequested, this,
                [this](RelationForm *form) {
                    QString name;
                    QString filePath;
                    int line = 0;

                    if (contextTracker != Q_NULLPTR && contextTracker->currentSymbol(&name, &filePath, &line))
                        form->setRootSymbol(name, filePath, line, true);
                });
        connect(contextTracker, &ContextTracker::symbolAtCaret,
                searchResultsDock, &SearchResultsDock::updateUnlockedRelationRoots);
        // A double click on a relation row opens the symbol's file in the
        // editor and puts the line in the upper middle of the view (the same
        // jump the function list and the symbol panel use).
        connect(searchResultsDock, &SearchResultsDock::relationFormOpenRequested, this,
                [this](const QString &filePath, int line) {
                    jumpToProjectSymbol(filePath, line);
                });
    }

    // ---- "Project" menu, placed to the left of "Help" ----
    projectMenu = new QMenu(tr("Project"), window);
    projectMenu->setObjectName(QStringLiteral("menuProject"));

    currentProjectAction = projectMenu->addAction(tr("Current Project:"));
    currentProjectAction->setEnabled(false); // turns normal once a project is open
    projectMenu->addSeparator();

    newProjectAction = projectMenu->addAction(tr("New Project..."));
    newProjectAction->setObjectName(QStringLiteral("actionNewProject"));
    connect(newProjectAction, &QAction::triggered, this, &ProjectMainWindow::newProjectDialog);

    openProjectAction = projectMenu->addAction(tr("Open Project..."));
    openProjectAction->setObjectName(QStringLiteral("actionOpenProject"));
    connect(openProjectAction, &QAction::triggered, this, &ProjectMainWindow::openProjectDialog);

    closeProjectAction = projectMenu->addAction(tr("Close Project"));
    closeProjectAction->setObjectName(QStringLiteral("actionCloseProject"));
    connect(closeProjectAction, &QAction::triggered, this, &ProjectMainWindow::closeProjectRequested);

    removeProjectAction = projectMenu->addAction(tr("Remove Project..."));
    removeProjectAction->setObjectName(QStringLiteral("actionRemoveProject"));
    connect(removeProjectAction, &QAction::triggered, this, &ProjectMainWindow::removeProjectDialog);

    projectMenu->addSeparator();

    addRemoveProjectFilesAction = projectMenu->addAction(tr("Add and Remove Project Files..."));
    addRemoveProjectFilesAction->setObjectName(QStringLiteral("actionAddRemoveProjectFiles"));
    connect(addRemoveProjectFilesAction, &QAction::triggered, this, &ProjectMainWindow::addRemoveProjectFilesDialog);

    synchronizeFilesAction = projectMenu->addAction(tr("Synchronize Files..."));
    synchronizeFilesAction->setObjectName(QStringLiteral("actionSynchronizeFiles"));
    connect(synchronizeFilesAction, &QAction::triggered, this, &ProjectMainWindow::synchronizeProjectFilesDialog);

    // "Project Settings" is intentionally NOT added (per spec, not needed yet).
    window->ui->menuBar->insertMenu(window->ui->menuHelp->menuAction(), projectMenu);

    // ---- manager -> panel wiring ----
    connect(projectManager, &ProjectManager::projectOpened, this, [this](const QString &) {
        projectPanel->refreshFiles();
        projectPanel->refreshSymbols();
        projectPanel->resetFolders();
        projectPanel->show();
        projectPanel->raise();
        if (auto *editor = window->currentEditor(); editor && editor->isFile())
            projectPanel->trackEditorFile(editor->getFilePath());

        // A project is what the relation panel is for: its Context view reads
        // the symbol table, so the panel comes up with the project instead of
        // waiting for the search results to be used.
        if (searchResultsDock != Q_NULLPTR)
            searchResultsDock->showRelationPanel();

        updateProjectActions();
    });
    connect(projectManager, &ProjectManager::projectClosed, this, [this]() {
        projectPanel->hide();
        // Requirement 5: the jump history lives and dies with the project.
        clearJumpHistory();
        updateProjectActions();
    });
    connect(projectManager, &ProjectManager::projectFileListChanged, projectPanel, &ProjectPanelDock::refreshFiles);
    connect(projectManager, &ProjectManager::projectSymbolsUpdated, projectPanel, &ProjectPanelDock::refreshSymbols);
    connect(projectPanel, &ProjectPanelDock::openFileRequested, window, &MainWindow::openFile);
    // A jump from the project symbol panel is a user jump like any other, so it
    // goes into the same history (requirement 6).
    connect(projectPanel, &ProjectPanelDock::jumpToSymbolRequested, this, [this](const QString &filePath, int line) {
        const JumpHistoryEntry source = jumpPositionFor(window->currentEditor());
        if (jumpToProjectSymbol(filePath, line))
            recordJump(source, JumpHistoryEntry{filePath, qMax(1, line)});
    });

    // The Folders tab follows the directory of the active editor. The dock
    // remembers the file even while hidden / on another tab and applies it as
    // soon as the Folders page becomes visible.
    connect(window, &MainWindow::editorActivated, this, [this](ScintillaNext *editor) {
        if (editor->isFile())
            projectPanel->trackEditorFile(editor->getFilePath());
    });

    updateProjectActions();

    // ---- toolbar buttons: jump history back / forward ----
    // Left of the symbol jump, in the same blue so the three read as one family.
    // Both start disabled: there is nothing to walk back to yet. Back/forward
    // only move the head index, they never add a record (requirement 4).
    QPixmap jumpBackPm(16, 16);
    jumpBackPm.fill(Qt::transparent);
    {
        QPainter painter(&jumpBackPm);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2A, 0x6D, 0xB8));
        painter.drawPolygon(QPolygon() << QPoint(1, 8) << QPoint(8, 1) << QPoint(8, 5) << QPoint(15, 5)
                                       << QPoint(15, 11) << QPoint(8, 11) << QPoint(8, 15));
    }
    jumpBackAction = window->ui->mainToolBar->addAction(QIcon(jumpBackPm), tr("Jump Back"));
    jumpBackAction->setObjectName(QStringLiteral("actionJumpBack"));
    jumpBackAction->setToolTip(tr("Jump Back: go back through the jump history"));
    connect(jumpBackAction, &QAction::triggered, this, &ProjectMainWindow::goBackJump);

    QPixmap jumpForwardPm(16, 16);
    jumpForwardPm.fill(Qt::transparent);
    {
        QPainter painter(&jumpForwardPm);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2A, 0x6D, 0xB8));
        painter.drawPolygon(QPolygon() << QPoint(15, 8) << QPoint(8, 1) << QPoint(8, 5) << QPoint(1, 5)
                                       << QPoint(1, 11) << QPoint(8, 11) << QPoint(8, 15));
    }
    jumpForwardAction = window->ui->mainToolBar->addAction(QIcon(jumpForwardPm), tr("Jump Forward"));
    jumpForwardAction->setObjectName(QStringLiteral("actionJumpForward"));
    jumpForwardAction->setToolTip(tr("Jump Forward: go forward through the jump history"));
    connect(jumpForwardAction, &QAction::triggered, this, &ProjectMainWindow::goForwardJump);

    updateJumpActionStates();

    // ---- toolbar button: same effect as Ctrl+Click on a symbol ----
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    {
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing);
        // small document
        painter.setPen(QPen(QColor(0x60, 0x60, 0x60)));
        painter.setBrush(QColor(0xF7, 0xF7, 0xF7));
        painter.drawRect(2, 1, 8, 12);
        painter.setPen(QColor(0xAA, 0xAA, 0xAA));
        for (int y = 4; y <= 10; y += 3)
            painter.drawLine(4, y, 8, y);
        // jump arrow
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2A, 0x6D, 0xB8));
        const QPolygon arrow = QPolygon() << QPoint(8, 5) << QPoint(15, 10) << QPoint(11, 10) << QPoint(11, 15) << QPoint(8, 15);
        painter.drawPolygon(arrow);
    }
    jumpToSymbolAction = window->ui->mainToolBar->addAction(QIcon(pm), tr("Jump to Symbol"));
    jumpToSymbolAction->setObjectName(QStringLiteral("actionJumpToSymbol"));
    connect(jumpToSymbolAction, &QAction::triggered, this, [this]() {
        if (auto *editor = window->currentEditor())
            jumpToSymbolAt(editor, static_cast<int>(editor->currentPos()));
    });

    // ---- toolbar button: where is the symbol under the caret called? ----
    // Sits directly right of the symbol jump. Icon: the "fx" reference box with
    // a red up arrow leaving it, i.e. "up to the call site".
    QPixmap callersPm(16, 16);
    callersPm.fill(Qt::transparent);
    {
        QPainter painter(&callersPm);
        painter.setRenderHint(QPainter::Antialiasing);

        // the "fx" reference box
        const QRect box(0, 4, 10, 10);
        painter.setPen(QPen(QColor(0x9A, 0x7B, 0x2F)));
        painter.setBrush(QColor(0xFF, 0xFD, 0xF3));
        painter.drawRect(box.x(), box.y(), box.width() - 1, box.height() - 1);
        // Arial without antialiasing: at 9 px the default font of a toolbar
        // size icon smears "fx" into a blob, and ClearType adds colored fringes
        // on the transparent background - plain and unaliased stays legible.
        // A platform without Arial falls back to its default sans serif.
        QFont fxFont(QStringLiteral("Arial"));
        fxFont.setStyleStrategy(QFont::NoAntialias);
        fxFont.setItalic(true);
        fxFont.setBold(true);
        fxFont.setPixelSize(9);
        painter.setFont(fxFont);
        painter.setPen(QColor(0x18, 0x18, 0x18));
        painter.drawText(box, Qt::AlignCenter, QStringLiteral("fx"));

        // the red up arrow
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xCC, 0x1B, 0x1B));
        painter.drawRect(QRect(11, 6, 2, 9)); // shaft
        painter.drawPolygon(QPolygon() << QPoint(12, 1) << QPoint(8, 6) << QPoint(15, 6)); // head
    }
    findCallersAction = window->ui->mainToolBar->addAction(QIcon(callersPm), tr("Find Callers"));
    findCallersAction->setObjectName(QStringLiteral("actionFindCallers"));
    findCallersAction->setToolTip(tr("Find Callers: jump to the places that call this symbol"));
    connect(findCallersAction, &QAction::triggered, this, [this]() {
        if (auto *editor = window->currentEditor())
            findCallersAt(editor, static_cast<int>(editor->currentPos()));
    });

    // "Smart Rename..." in the editor context menu (MainWindow.ui's menu does
    // not hold it, the same way it does not hold the Project menu). The action
    // has to exist before any menu is built, which is why it is created here and
    // not with the dialog it opens.
    SmartRenameDialog::installEditorContextMenu(window);

    // Offscreen regression: run the whole project lifecycle automatically.
    if (qEnvironmentVariableIsSet("NOTEPADNEXT_PROJECT_SELFCHECK")) {
        // NOTEPADNEXT_SYNC_PROBE turns the run into a timing probe for one real
        // project (see runSyncProbe) instead of the regular self check;
        // NOTEPADNEXT_SYMBOL_PROBE only measures the symbol panel rebuild of an
        // already parsed project (see runSymbolProbe).
        if (qEnvironmentVariableIsSet("NOTEPADNEXT_SYMBOL_PROBE"))
            QTimer::singleShot(2000, this, &ProjectMainWindow::runSymbolProbe);
        else if (qEnvironmentVariableIsSet("NOTEPADNEXT_SYNC_PROBE"))
            QTimer::singleShot(2000, this, &ProjectMainWindow::runSyncProbe);
        else
            QTimer::singleShot(2000, this, &ProjectMainWindow::runProjectSelfCheck);
    }
}

void ProjectMainWindow::updateProjectActions()
{
    const bool has = projectManager && projectManager->hasProject();
    const QString name = has ? projectManager->currentProjectName() : QString();
    currentProjectAction->setText(has ? tr("Current Project:  %1").arg(name) : tr("Current Project:"));
    // Normal color while a project is open; gray only when there is none.
    currentProjectAction->setEnabled(has);
    closeProjectAction->setEnabled(has);
    removeProjectAction->setEnabled(!projectManager->availableProjects().isEmpty());
    addRemoveProjectFilesAction->setEnabled(has);
    synchronizeFilesAction->setEnabled(has);
    // Stays available even with an empty list: that is the situation the
    // dialog's "browse.." button exists for (see openProjectDialog).
    openProjectAction->setEnabled(true);
}

void ProjectMainWindow::newProjectDialog()
{
    NewProjectDialog dialog(window);
    // Creating runs from inside the dialog's own event loop instead of after it
    // returned: a refused attempt ("A project named ... already exists", a data
    // path that cannot be created, ...) then leaves the dialog on screen with
    // everything the user typed, so it costs one edit rather than the whole form.
    dialog.setAcceptHandler([this, &dialog]() -> QString {
        QString errorMessage;
        if (projectManager->createProject(dialog.projectName(), dialog.dataPath(), dialog.sourceRoot(), &errorMessage))
            return QString();
        return errorMessage;
    });
    if (dialog.exec() != QDialog::Accepted)
        return; // Cancel: do nothing

    // Per spec: right after creating, let the user add files to the project.
    addRemoveProjectFilesDialog();
}

void ProjectMainWindow::openProjectDialog()
{
    const QStringList names = projectManager->availableProjects();

    QHash<QString, QString> paths;
    for (const QString &name : names)
        paths.insert(name, projectManager->projectPath(name));

    // No "nothing to pick from" early return any more. An empty list is one of
    // the two cases this window serves - the global project list lost its
    // entries (reinstalled machine, fresh settings file) while the project
    // folders are still on disk - and the "browse.." button in its lower left
    // corner is how that case is handled. Everything else is a plain pick from
    // the list below, which then runs exactly as it did before.
    ProjectListDialog dialog(names, paths, /*removeMode=*/false, window);
    dialog.setStartDirectory(projectBrowserStartDirectory());
    dialog.setBrowseHandler([this](const QString &projectFile, QString *errorMessage) {
        return openProjectFromFile(projectFile, errorMessage);
    });
    if (dialog.exec() != QDialog::Accepted)
        return;

    // A "browse.." run that went through has registered and opened the project
    // from inside the dialog's own event loop and leaves no name behind here.
    const QString name = dialog.selectedProject();
    if (name.isEmpty())
        return;

    // Opening another project closes the current one, so its files go too.
    if (projectManager->hasProject() && projectManager->currentProjectName() != name
        && !closeProjectFiles())
        return; // cancelled while saving: stay on the current project

    QString errorMessage;
    if (!projectManager->openProject(name, &errorMessage))
        QMessageBox::warning(window, tr("Open Project"), errorMessage);
}

// Registers a project file the user located by hand - "Open Project" ->
// "browse.." - and opens it. This is the way back for a machine whose global
// project list is gone while the <name>.codeinsight folders are still there.
// Runs from inside the dialog's event loop, so "everything or nothing" is the
// only useful answer: Opened closes the window, Cancelled and Failed leave it
// untouched, the latter with the reason for the user to act on.
ProjectListDialog::BrowseOutcome ProjectMainWindow::openProjectFromFile(const QString &projectFile, QString *errorMessage)
{
    QString name;
    QString failMessage;
    // Judge the file before touching anything: an unreadable name or a file
    // this folder cannot load later must not register an entry first.
    if (!ProjectManager::projectIdentityFromFile(projectFile, &name, nullptr, &failMessage)) {
        if (errorMessage)
            *errorMessage = failMessage;
        return ProjectListDialog::BrowseOutcome::Failed;
    }

    // Opening another project closes the current one, so its files go too,
    // exactly like picking an entry from the list. A stop at the save prompt
    // means "leave everything as it is", so the registration deliberately comes
    // after this point - a cancelled run must not leave a new list entry behind.
    if (projectManager->hasProject() && projectManager->currentProjectName() != name
        && !closeProjectFiles()) {
        return ProjectListDialog::BrowseOutcome::Cancelled;
    }

    if (!projectManager->registerProject(projectFile, nullptr, &failMessage)) {
        if (errorMessage)
            *errorMessage = failMessage;
        return ProjectListDialog::BrowseOutcome::Failed;
    }
    updateProjectActions(); // the list gained an entry; "Remove Project" may wake up

    if (!projectManager->openProject(name, &failMessage)) {
        if (errorMessage)
            *errorMessage = failMessage;
        return ProjectListDialog::BrowseOutcome::Failed;
    }

    return ProjectListDialog::BrowseOutcome::Opened;
}

// Where the "browse.." chooser starts: next to the project that is open, else
// next to a project that is already known, else at home - in every case a
// folder the user recognizes rather than whatever the last file dialog used.
QString ProjectMainWindow::projectBrowserStartDirectory() const
{
    if (projectManager->hasProject())
        return projectManager->projectDir();

    const QStringList names = projectManager->availableProjects();
    for (const QString &name : names) {
        const QString path = projectManager->projectPath(name);
        if (!path.isEmpty() && QDir(path).exists())
            return path;
    }

    return QDir::homePath();
}

void ProjectMainWindow::closeProjectRequested()
{
    // The files of the project are closed together with it; a cancel at the
    // save prompt means "leave everything as it is", project included.
    if (!closeProjectFiles())
        return;

    projectManager->closeProject();
}

// Canonical, forward slashed absolute path. That is the shape the project keeps
// its file list in, so the two can be compared directly. Files that no longer
// exist fall back to their plain absolute path.
static QString projectComparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(canonical.isEmpty() ? info.absoluteFilePath()
                                                                         : canonical));
}

static bool sameProjectPath(const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

static bool pathInsideDirectory(const QString &path, const QString &directory)
{
    if (path.isEmpty() || directory.isEmpty())
        return false;
    if (sameProjectPath(path, directory))
        return true;

    QString prefix = directory;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
#ifdef Q_OS_WIN
    return path.startsWith(prefix, Qt::CaseInsensitive);
#else
    return path.startsWith(prefix);
#endif
}

// Which open editors belong to the project: the files it lists plus everything
// living below its source root. Untitled buffers and files from outside stay
// open - they have nothing to do with this project.
bool ProjectMainWindow::closeProjectFiles()
{
    if (projectManager == Q_NULLPTR || !projectManager->hasProject() || window->dockedEditor == Q_NULLPTR)
        return true;

    // Read while the project is still open: closing it drops the file list and
    // the source root.
    QStringList listedFiles;
    const QStringList projectFiles = projectManager->projectFiles();
    listedFiles.reserve(projectFiles.size());
    for (const QString &file : projectFiles)
        listedFiles.append(projectComparablePath(file));
    const QString sourceRoot = projectComparablePath(projectManager->sourceRoot());

    QVector<ScintillaNext *> projectEditors;
    const QVector<ScintillaNext *> openEditors = window->dockedEditor->editors();
    for (ScintillaNext *editor : openEditors) {
        if (editor == Q_NULLPTR || !editor->isFile())
            continue; // untitled / temporary buffers are never part of a project

        const QString path = projectComparablePath(editor->getFilePath());
        bool belongs = pathInsideDirectory(path, sourceRoot);
        for (int i = 0; !belongs && i < listedFiles.size(); ++i)
            belongs = sameProjectPath(path, listedFiles.at(i));
        if (belongs)
            projectEditors.append(editor);
    }

    if (projectEditors.isEmpty())
        return true;

    // One prompt for all of them, exactly like closing the tabs by hand.
    if (!window->checkEditorsBeforeClose(projectEditors))
        return false; // cancelled: neither the files nor the project are touched

    for (ScintillaNext *editor : projectEditors)
        editor->close();

    // Never leave the window without a document: the tab bar, the function list
    // and the folder tree all expect one. Same rule as closing the last tab.
    if (window->editorCount() == 0) {
        if (window->app->getSettings()->exitOnLastTabClosed())
            window->close();
        else
            window->newFile();
    }

    return true;
}

void ProjectMainWindow::removeProjectDialog()
{
    const QStringList names = projectManager->availableProjects();
    if (names.isEmpty())
        return;

    QHash<QString, QString> paths;
    for (const QString &name : names)
        paths.insert(name, projectManager->projectPath(name));

    ProjectListDialog dialog(names, paths, /*removeMode=*/true, window);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = dialog.selectedProject();
    if (name.isEmpty())
        return;

    const auto reply = QMessageBox::question(window, tr("Remove Project"),
                                             tr("Delete project \"%1\" including all of its data files?\n%2")
                                                 .arg(name, QDir::toNativeSeparators(projectManager->projectPath(name))));
    if (reply != QMessageBox::Yes)
        return;

    // Removing the project that is open closes it as well, so its files go too.
    if (projectManager->hasProject() && projectManager->currentProjectName() == name
        && !closeProjectFiles())
        return; // cancelled while saving: nothing is removed

    QString errorMessage;
    if (!projectManager->removeProject(name, &errorMessage))
        QMessageBox::warning(window, tr("Remove Project"), errorMessage);
    else
        updateProjectActions();
}

void ProjectMainWindow::addRemoveProjectFilesDialog()
{
    if (!projectManager->hasProject())
        return;

    ProjectFilesDialog dialog(projectManager, window);
    dialog.exec();

    // Closing the dialog triggers "Synchronize Files" on the changed part of
    // the list (per spec). If nothing changed this is a no-op.
    runProjectSynchronize(/*force=*/false);
}

void ProjectMainWindow::synchronizeProjectFilesDialog()
{
    if (!projectManager->hasProject())
        return;

    SynchronizeFilesDialog dialog(projectManager, window);
    dialog.exec();
}

bool ProjectMainWindow::runProjectSynchronize(bool force)
{
    if (!projectManager || !projectManager->hasProject())
        return false;

    projectManager->saveFileList();
    const ProjectManager::SyncDelta delta = projectManager->computeDelta(force);
    if (delta.empty())
        return true;

    // Progress dialog with nothing but a progress bar and a Cancel button: no
    // per-file / per-phase text, and the run really stops when it is cancelled.
    ProjectSyncProgressDialog progress(window);
    progress.setProgress(0, ProjectManager::syncStepCount(delta));
    progress.show();
    qApp->processEvents();

    QString errorMessage;
    bool cancelled = false;
    const bool ok = projectManager->synchronize(delta, [&progress](int step, int totalSteps, const QString &) {
        progress.setProgress(step, totalSteps);
        return !progress.wasCancelled();
    }, &errorMessage, &cancelled);

    progress.accept(); // done: close the dialog

    if (cancelled) {
        window->ui->statusBar->showMessage(tr("Synchronization cancelled."), 5000);
        return false;
    }

    if (!ok)
        QMessageBox::warning(window, tr("Synchronize Files"),
                             errorMessage.isEmpty() ? tr("Synchronization failed.") : errorMessage);
    return ok;
}

// The editor that shows filePath, opening the file when it is not open yet.
// Returns null when the path resolves to no editor (empty path, or a history
// entry whose file has since been deleted), so the caller can treat the
// navigation as a no-op instead of trying to open a file that is not there.
ScintillaNext *ProjectMainWindow::editorForFilePath(const QString &filePath)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath))
        return Q_NULLPTR;

    // Already open? Compare canonical paths, the panel stores normalized ones.
    const QFileInfo targetInfo(filePath);
    for (ScintillaNext *editor : window->dockedEditor->editors()) {
        if (editor->isFile() && QFileInfo(editor->getFilePath()).canonicalFilePath() == targetInfo.canonicalFilePath())
            return editor;
    }

    window->openFile(filePath);
    for (ScintillaNext *editor : window->dockedEditor->editors()) {
        if (editor->isFile() && QFileInfo(editor->getFilePath()).canonicalFilePath() == targetInfo.canonicalFilePath())
            return editor;
    }
    return Q_NULLPTR;
}

bool ProjectMainWindow::jumpToProjectSymbol(const QString &filePath, int line)
{
    ScintillaNext *target = editorForFilePath(filePath);
    if (target == Q_NULLPTR)
        return false;
    jumpFunctionList(target, qMax(1, line));
    return true;
}

// ---------- jump history (toolbar back / forward) ----------

// The key a position is stored and compared under. One and the same file
// reaches the jump history in two spellings: the project tables and the tags
// database hand out forward slashes ("D:/proj/a.c") while
// ScintillaNext::getFilePath() returns native separators ("D:\proj\a.c").
// With two spellings around, "the source is where the head already points"
// never matched, so every jump stored one position too many and a back press
// looked like it did nothing. canonicalFilePath() also settles case and
// symlinks for a file that exists; one that is gone keeps its absolute path,
// so the entry is still recognised (and then skipped when navigating).
static QString jumpEntryKey(const QString &file)
{
    if (file.isEmpty())
        return QString();

    const QFileInfo info(file);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(canonical.isEmpty() ? info.absoluteFilePath() : canonical));
}

JumpHistoryEntry ProjectMainWindow::jumpPositionFor(ScintillaNext *editor, int position) const
{
    JumpHistoryEntry entry;
    if (editor == Q_NULLPTR)
        return entry;
    if (position < 0)
        position = static_cast<int>(editor->currentPos());
    // An untitled buffer carries no path and could never be navigated back to,
    // so only real files become entries (pushJumpEntry drops the empty ones).
    if (editor->isFile())
        entry.file = editor->getFilePath();
    entry.line = static_cast<int>(editor->lineFromPosition(position)) + 1;
    return entry;
}

void ProjectMainWindow::pushJumpEntry(const JumpHistoryEntry &entry)
{
    // Every stored entry is keyed, so the equality tests in recordJump compare
    // like with like no matter how the caller spelled the path.
    JumpHistoryEntry keyed = entry;
    keyed.file = jumpEntryKey(keyed.file);
    if (keyed.file.isEmpty())
        return; // nothing that could ever be navigated to

    // The new entry goes in front of the head, i.e. one slot downwards in the
    // ring, and the head follows it there. The index walks downwards and wraps
    // around (0 -> 39, the index reversal the ring has to respect). Nothing
    // else is maintained: a walk in either direction reads the neighbouring
    // slot straight off the ring.
    jumpHistoryHead = (jumpHistoryHead - 1 + JumpHistoryCapacity) % JumpHistoryCapacity;
    jumpHistoryRing[jumpHistoryHead] = keyed;
}

void ProjectMainWindow::recordJump(const JumpHistoryEntry &source, const JumpHistoryEntry &destination)
{
    // Both ends are keyed first: the comparisons below must not be defeated by
    // the path spelling (see jumpEntryKey).
    const JumpHistoryEntry from{jumpEntryKey(source.file), source.line};
    const JumpHistoryEntry to{jumpEntryKey(destination.file), destination.line};

    // A jump that ends exactly where it started moves nothing the user could
    // walk back to, so it is not a record either. (Clicking the symbol the
    // caret already sits on is the everyday case.) Skipping it also avoids the
    // pair of identical entries that would otherwise cost one fruitless press.
    if (!from.file.isEmpty() && from.file == to.file && from.line == to.line)
        return;

    // Requirement 2c: the source is only added when it differs from the entry
    // the head already points at. Consecutive jumps share the position the
    // previous jump landed on, so without this check every jump would duplicate
    // it.
    //
    // The destination is skipped under the same condition, and that part is not
    // cosmetic: a symbol list is wired to both clicked and doubleClicked, so a
    // double click reports one and the same jump twice - the second report
    // arrives with source == destination == the head. Recording it puts an
    // entry in the ring that only repeats where the caret already is, and back
    // then has to be pressed twice before anything moves.
    // Both tests read the head before anything is pushed, which is what makes
    // "the landing is already the head" detectable. An empty head slot (a
    // history that has not started yet) equals nothing.
    const bool headFilled = jumpSlotFilled(jumpHistoryHead);
    const bool sourceIsHead = headFilled
            && jumpHistoryRing[jumpHistoryHead].file == from.file
            && jumpHistoryRing[jumpHistoryHead].line == from.line;
    const bool destinationIsHead = headFilled
            && jumpHistoryRing[jumpHistoryHead].file == to.file
            && jumpHistoryRing[jumpHistoryHead].line == to.line;

    if (!sourceIsHead)
        pushJumpEntry(from);
    if (!destinationIsHead)
        pushJumpEntry(to);
    updateJumpActionStates();
}

// A slot holds a recorded position only once something was written to it. Both
// the arrow states and the walk test exactly this, so a hole in the ring - a
// slot never used - is what a direction runs out at.
bool ProjectMainWindow::jumpSlotFilled(int index) const
{
    return !jumpHistoryRing[index].file.isEmpty();
}

bool ProjectMainWindow::moveJumpHeadBack()
{
    const int next = (jumpHistoryHead + 1) % JumpHistoryCapacity;
    if (!jumpSlotFilled(next))
        return false; // nothing was ever recorded in that direction
    jumpHistoryHead = next;
    return true;
}

bool ProjectMainWindow::moveJumpHeadForward()
{
    const int next = (jumpHistoryHead - 1 + JumpHistoryCapacity) % JumpHistoryCapacity;
    if (!jumpSlotFilled(next))
        return false; // nothing was ever recorded in that direction
    jumpHistoryHead = next;
    return true;
}

// Walking back/forward moves the index only - a walk is never recorded itself
// (requirements 3/4). The loop exists because a recorded file may have been
// deleted behind our back: such an entry is struck out (its slot is emptied)
// and the walk carries on in the same direction until it lands on something
// that really opens. The hole it leaves is a hole for good, so that direction
// may no longer be walkable all the way back - the accepted simplification. The
// attempt cap only keeps a ring whose entries all went stale from spinning.
void ProjectMainWindow::goBackJump()
{
    int from = jumpHistoryHead;
    for (int attempt = 0; attempt < JumpHistoryCapacity; ++attempt) {
        const int next = (from + 1) % JumpHistoryCapacity;
        if (!jumpSlotFilled(next))
            break;
        if (navigateToHistoryEntry(jumpHistoryRing[next])) {
            jumpHistoryHead = next;
            break;
        }
        jumpHistoryRing[next] = JumpHistoryEntry();
        from = next;
    }
    updateJumpActionStates();
}

void ProjectMainWindow::goForwardJump()
{
    int from = jumpHistoryHead;
    for (int attempt = 0; attempt < JumpHistoryCapacity; ++attempt) {
        const int next = (from - 1 + JumpHistoryCapacity) % JumpHistoryCapacity;
        if (!jumpSlotFilled(next))
            break;
        if (navigateToHistoryEntry(jumpHistoryRing[next])) {
            jumpHistoryHead = next;
            break;
        }
        jumpHistoryRing[next] = JumpHistoryEntry();
        from = next;
    }
    updateJumpActionStates();
}

// Show a recorded position again. Deliberately the same mechanics as a fresh
// jump (open the file, switch to it, place the line upper-middle) but it never
// touches the history: a back/forward does not become a new record. False means
// the file cannot be opened any more, and the caller drops the entry.
bool ProjectMainWindow::navigateToHistoryEntry(const JumpHistoryEntry &entry)
{
    ScintillaNext *target = editorForFilePath(entry.file);
    if (target == Q_NULLPTR)
        return false;
    jumpFunctionList(target, qMax(1, entry.line));
    return true;
}

void ProjectMainWindow::clearJumpHistory()
{
    jumpHistoryHead = 0;
    for (JumpHistoryEntry &entry : jumpHistoryRing)
        entry = JumpHistoryEntry();
    updateJumpActionStates();
}

void ProjectMainWindow::updateJumpActionStates()
{
    // Enabled iff the neighbouring slot in that direction holds a recorded
    // position. The head is the only state the history keeps, so this is the
    // whole rule behind the grey arrows.
    if (jumpBackAction)
        jumpBackAction->setEnabled(jumpSlotFilled((jumpHistoryHead + 1) % JumpHistoryCapacity));
    if (jumpForwardAction)
        jumpForwardAction->setEnabled(
                jumpSlotFilled((jumpHistoryHead - 1 + JumpHistoryCapacity) % JumpHistoryCapacity));
}

// One row of the chooser: the text shown in the list plus the position it
// points at, so the preview pane under the list can load it.
struct JumpTarget
{
    QString text;
    QString file;  // absolute path; empty = nothing to preview
    int line = 1;  // 1-based
};

// The preview pane always shows this many characters of code: the chooser is
// sized so they fit (see chooseJumpTarget).
static constexpr int JumpPreviewColumns = 100;

// Pixel width the pane needs for `columns` characters of code: the text itself
// plus the line number margin and the vertical scrollbar, which takes its space
// out of the viewport whenever it shows up. Measured with the pane's own font,
// so a different editor font or size still yields <JumpPreviewColumns> columns.
static int jumpPreviewWidthForColumns(ScintillaNext *preview, int columns)
{
    const QByteArray sample(columns, '0');
    const int text = static_cast<int>(preview->textWidth(STYLE_DEFAULT, sample.constData()));
    const int scroll = preview->verticalScrollBar()->sizeHint().width();
    return static_cast<int>(preview->marginLeft()) + static_cast<int>(preview->marginWidthN(0)) + text
           + scroll;
}

// Read-only preview shown under the list. Font, line numbers and the target
// line highlight mirror the editors (see EditorManager::setupEditor) so the
// pane looks like the buffer the jump lands in. The decorators the editors add
// (auto completion, smart highlighter, ...) are deliberately left out: the pane
// is read-only and only lives as long as the chooser is open.
static ScintillaNext *createJumpPreview(QWidget *parent, NotepadNextApplication *app)
{
    auto *preview = new ScintillaNext(QString(), parent);
    preview->setObjectName(QStringLiteral("jumpPreview"));

    ApplicationSettings *settings = app->getSettings();
    preview->styleSetFore(STYLE_DEFAULT, 0x000000);
    preview->styleSetBack(STYLE_DEFAULT, 0xFFFFFF);
    preview->styleSetSize(STYLE_DEFAULT, settings->fontSize());
    preview->styleSetFont(STYLE_DEFAULT, settings->fontName().toUtf8().data());
    preview->styleClearAll();

    preview->styleSetFore(STYLE_LINENUMBER, 0x808080);
    preview->styleSetBack(STYLE_LINENUMBER, 0xE4E4E4);

    preview->setMarginLeft(2);
    preview->setMarginWidthN(0, 30); // line numbers
    // No fold margin: the pane never folds anything, and its markers showed up
    // as a column of empty circles beside the line numbers.
    preview->setMarginWidthN(2, 0);

    // The caret line is what marks the target, exactly as in the editors.
    preview->setCaretLineVisible(true);
    preview->setCaretLineVisibleAlways(true);
    preview->setElementColour(SC_ELEMENT_CARET_LINE_BACK, 0xFFFFE8E8);
    preview->setCaretWidth(2);
    preview->setScrollWidth(1);
    preview->setScrollWidthTracking(true);
    preview->setReadOnly(true);

    return preview;
}

// Load target into the preview and centre the marked line. loadedFile tracks
// what the pane currently holds, so stepping through rows of the same file only
// moves the caret line instead of reloading the document.
static void showJumpPreview(ScintillaNext *preview, NotepadNextApplication *app,
                            const JumpTarget &target, QString *loadedFile)
{
    if (preview == Q_NULLPTR)
        return;

    if (target.file.isEmpty()) {
        // Nothing to preview (a local symbol in an untitled buffer): empty the
        // pane. Asking the pane itself rather than the loadedFile bookkeeping
        // keeps this correct after a failed load, which leaves text behind
        // without recording a file.
        if (preview->length() > 0) {
            preview->setReadOnly(false);
            preview->clearAll();
            preview->setReadOnly(true);
        }
        loadedFile->clear();
        return;
    }

    if (*loadedFile != target.file) {
        QByteArray content;
        // An already open editor wins over the file on disk: it may hold unsaved
        // changes and it is what the jump itself will show.
        if (ScintillaNext *open = app->getEditorManager()->getEditorByFilePath(target.file)) {
            content = open->get_text_range(0, static_cast<int>(open->length()));
        } else {
            QFile file(target.file);
            if (!file.open(QIODevice::ReadOnly)) {
                preview->setReadOnly(false);
                preview->setText(QStringLiteral("// %1").arg(target.file).toUtf8().constData());
                preview->setReadOnly(true);
                loadedFile->clear(); // retry if the file shows up later
                return;
            }
            content = file.readAll();
        }

        preview->setReadOnly(false);
        preview->setText(content.constData());
        preview->setReadOnly(true);
        app->setEditorLanguage(preview,
                               app->detectLanguageFromExtension(QFileInfo(target.file).suffix()));
        // The language script (src/scripts/init.lua) turns the fold margin back
        // on for every language it applies, so it has to go off again here.
        preview->setMarginWidthN(2, 0);
        *loadedFile = target.file;
    }

    // Scintilla counts lines from zero, JumpTarget from one.
    const int line = qMax(1, target.line);
    preview->gotoLine(line - 1);
    const int visible = static_cast<int>(preview->linesOnScreen());
    if (visible > 2)
        preview->setFirstVisibleLine(qMax(0, line - 1 - visible / 2));
    preview->setXOffset(0);
}

// Chooser for multiple jump targets, with a preview of the highlighted row.
// The list expands to show up to 10 entries; more than 10 get a vertical
// scrollbar, fewer show at their natural height. Double click / Enter accept,
// Esc / the window X cancel (returns -1). Clicking a row loads its file into
// the preview pane below and marks the target line, so a wrong entry can be
// spotted before the jump happens.
static int chooseJumpTarget(NotepadNextApplication *app, QWidget *parent, const QString &title,
                            const QVector<JumpTarget> &targets)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);

    auto *list = new QListWidget(&dialog);
    for (const JumpTarget &target : targets)
        list->addItem(target.text);
    list->setCurrentRow(0);
    list->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *preview = createJumpPreview(&dialog, app);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    layout->addWidget(list);
    layout->addWidget(preview, 1);

    // Height: min(item count, 10) rows; a scrollbar appears beyond ten.
    const int visibleRows = qMin(targets.size(), 10);
    int rowHeight = list->sizeHintForRow(0);
    if (rowHeight <= 0)
        rowHeight = list->fontMetrics().height() + 6;
    const int listHeight = 2 * list->frameWidth() + rowHeight * visibleRows + 2;
    list->setFixedHeight(listHeight);

    // Width: the code area always shows exactly 100 characters, measured from
    // the pane's own font and margins so another editor font or size still
    // yields 100 columns. Line numbers and the scrollbar sit outside those 100
    // and are added on top. The list uses the same width and scrolls
    // horizontally when an entry (they carry full file paths) does not fit.
    const int width = jumpPreviewWidthForColumns(preview, JumpPreviewColumns) + 8;
    dialog.resize(width, listHeight + qMax(160, rowHeight * 14) + 12);

    QString loadedFile;
    auto showRow = [&](int row) {
        if (row >= 0 && row < targets.size())
            showJumpPreview(preview, app, targets.at(row), &loadedFile);
        // Setting the pane's language puts the Lua extension in "pane mode"
        // (LuaExtension::setEditor). Point it back at the window's editor right
        // away: the pane is destroyed with the dialog and a pointer left behind
        // in there would dangle.
        if (auto *window = qobject_cast<MainWindow *>(parent)) {
            if (ScintillaNext *editor = window->currentEditor())
                LuaExtension::Instance().setEditor(editor);
        }
    };
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) { showRow(row); });
    QObject::connect(list, &QListWidget::itemClicked, &dialog,
                     [&](QListWidgetItem *item) { showRow(list->row(item)); });

    // Show the first row right away, then once more when the dialog is on
    // screen: only then does the pane know how many lines fit, which is what
    // centring the target line needs.
    showRow(0);
    QTimer::singleShot(0, &dialog, [&]() { showRow(list->currentRow()); });

    // Double-click jumps right away; Enter confirms the current item; the
    // dialog rejects via Esc or the title bar X (returning -1).
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog, list](QListWidgetItem *item) {
        list->setCurrentItem(item);
        dialog.accept();
    });
    auto *enterShortcut = new QShortcut(QKeySequence(Qt::Key_Return), &dialog);
    enterShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(enterShortcut, &QShortcut::activated, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted)
        return -1;

    const QListWidgetItem *current = list->currentItem();
    return current ? list->row(current) : -1;
}

void ProjectMainWindow::jumpToSymbolAt(ScintillaNext *editor, int position)
{
    if (editor == Q_NULLPTR || position < 0)
        return;

    // Resolve the word under the click.
    const Sci_CharacterRange range = editor->wordAtPosition(position);
    if (range.cpMin == INVALID_POSITION || range.cpMax <= range.cpMin)
        return;
    const QString word = QString::fromUtf8(editor->get_text_range(range.cpMin, range.cpMax));
    if (word.isEmpty())
        return;

    // Where this jump starts, kept for the jump history. Only a jump that really
    // happens records it - every early return above/below means "no jump"
    // (requirement 2b).
    const JumpHistoryEntry source = jumpPositionFor(editor, position);

    // 1. The current file's function list.
    QVector<FunctionSymbol> localMatches;
    if (EditorPane *pane = EditorPane::paneForEditor(editor))
        localMatches = pane->functionList()->findSymbols(word);

    if (!localMatches.isEmpty()) {
        int choice = 0;
        if (localMatches.size() > 1) {
            QVector<JumpTarget> targets;
            for (const FunctionSymbol &sym : localMatches) {
                targets.append({QStringLiteral("%1 (line %2)").arg(sym.name).arg(sym.line),
                                editor->getFilePath(), qMax(1, sym.line)});
            }
            choice = chooseJumpTarget(window->app, window, tr("Go to Symbol"), targets);
            if (choice < 0)
                return; // cancelled
        }
        const int targetLine = qMax(1, localMatches.at(choice).line);
        jumpFunctionList(editor, targetLine);
        recordJump(source, JumpHistoryEntry{editor->getFilePath(), targetLine});
        return;
    }

    // 2. The open project's symbol table (if any).
    if (!projectManager || !projectManager->hasProject())
        return;

    const QVector<ProjectManager::ProjectSymbol> projectMatches = projectManager->lookupSymbols(word);
    if (projectMatches.isEmpty())
        return; // nothing found anywhere: do nothing, per spec

    int choice = 0;
    if (projectMatches.size() > 1) {
        QVector<JumpTarget> targets;
        for (const ProjectManager::ProjectSymbol &sym : projectMatches) {
            targets.append({QStringLiteral("%1 (%2:%3)").arg(sym.name, QDir::toNativeSeparators(sym.file)).arg(qMax(1, sym.line)),
                            sym.file, qMax(1, sym.line)});
        }
        choice = chooseJumpTarget(window->app, window, tr("Go to Symbol"), targets);
        if (choice < 0)
            return; // cancelled / closed
    }
    const QString targetFile = projectMatches.at(choice).file;
    const int targetLine = qMax(1, projectMatches.at(choice).line);
    if (jumpToProjectSymbol(targetFile, targetLine))
        recordJump(source, JumpHistoryEntry{targetFile, targetLine});
}

void ProjectMainWindow::findCallersAt(ScintillaNext *editor, int position)
{
    if (editor == Q_NULLPTR || position < 0 || projectManager == Q_NULLPTR)
        return;

    // The symbol the caret sits on, resolved exactly like the symbol jump does.
    const Sci_CharacterRange range = editor->wordAtPosition(position);
    if (range.cpMin == INVALID_POSITION || range.cpMax <= range.cpMin)
        return;
    const QString word = QString::fromUtf8(editor->get_text_range(range.cpMin, range.cpMax));
    if (word.isEmpty())
        return;

    // Where this jump starts, for the jump history (requirement 6).
    const JumpHistoryEntry source = jumpPositionFor(editor, position);

    QString errorMessage;
    const QVector<ProjectManager::ProjectSymbol> callers = projectManager->findCallers(word, &errorMessage);

    if (callers.isEmpty()) {
        // Neither "nothing calls it" nor "cannot answer" deserves a modal box:
        // the editor keeps the focus and the status bar explains the outcome.
        window->ui->statusBar->showMessage(errorMessage.isEmpty()
                                       ? tr("No call site of \"%1\" found.").arg(word)
                                       : errorMessage,
                                   8000);
        return;
    }

    // A single call site jumps right away; several use the very chooser the
    // symbol jump uses, so double click / Enter / Esc behave identically.
    int choice = 0;
    if (callers.size() > 1) {
        QVector<JumpTarget> targets;
        for (const ProjectManager::ProjectSymbol &site : callers) {
            targets.append({QStringLiteral("%1 (%2:%3)").arg(site.name, QDir::toNativeSeparators(site.file)).arg(qMax(1, site.line)),
                            site.file, qMax(1, site.line)});
        }
        choice = chooseJumpTarget(window->app, window, tr("Callers of \"%1\"").arg(word), targets);
        if (choice < 0)
            return; // cancelled
    }

    const QString targetFile = callers.at(choice).file;
    const int targetLine = qMax(1, callers.at(choice).line);
    if (jumpToProjectSymbol(targetFile, targetLine))
        recordJump(source, JumpHistoryEntry{targetFile, targetLine});
}

// Timing probe for one real project, used to find out where a synchronization
// of a large project spends its time (the regular self check only exercises a
// handful of files). Run with:
//   NOTEPADNEXT_PROJECT_SELFCHECK=1
//   NOTEPADNEXT_SYNC_PROBE=<source root>            (e.g. D:/code/linux-5.10.201-vct)
//   NOTEPADNEXT_SYNC_PROBE_LIST=<a real .filelist>  (copied into the shadow project)
//   NOTEPADNEXT_SYNC_PROBE_DIR=<scratch dir>        (default D:/tmp/syncprobe)
// It registers a shadow project next to the real sources, then forces a
// synchronization twice - cold (no databases yet) and warm (databases and the
// synced snapshot already there) - and prints the time every phase took. The
// user's own project folder is never touched: all databases land in the
// scratch directory.
// Measures what the GUI thread still has to do once a synchronization reaches
// 100%: the manager loads every symbol of the database into memory and the
// project panel turns every match into a row. Two ways in:
//
//   NOTEPADNEXT_SYMBOL_PROBE=1  the shadow project of runSyncProbe, which
//                               needs NOTEPADNEXT_SYNC_PROBE_DIR
//   NOTEPADNEXT_SYMBOL_PROBE=<folder or .codeinsightprj of a real project>
//                               that project ("D:/proj/linux-vct.codeinsight",
//                               ~490k symbols) - the only way to get numbers
//                               for a real one
//
// Both run in the self-check sandbox (NOTEPADNEXT_PROJECT_SELFCHECK): the real
// project is registered in that run only and then opened where it lies, so its
// folder is only read and the user's own registration never changes. Running
// without QT_QPA_PLATFORM=offscreen shows the real cost in a real window.
void ProjectMainWindow::runSymbolProbe()
{
    const QString srcRoot = qEnvironmentVariable("NOTEPADNEXT_SYNC_PROBE");
    const QString shadowRoot = qEnvironmentVariable("NOTEPADNEXT_SYNC_PROBE_DIR");
    const QString requested = qEnvironmentVariable("NOTEPADNEXT_SYMBOL_PROBE");
    QString name = QStringLiteral("SyncProbe");
    QString error;

    if (!requested.isEmpty() && requested != QLatin1String("1")) {
        // A folder is accepted as well as the project file itself: one
        // <name>.codeinsightprj next to the databases is the normal layout.
        const QFileInfo given(requested);
        QString projectFile = given.absoluteFilePath();
        if (given.isDir()) {
            const QFileInfoList files =
                QDir(projectFile).entryInfoList({QStringLiteral("*.codeinsightprj")}, QDir::Files);
            projectFile = files.size() == 1 ? files.first().absoluteFilePath() : QString();
        }
        QString probeName;
        if (projectFile.isEmpty()
            || !ProjectManager::projectIdentityFromFile(projectFile, &probeName, nullptr, &error)) {
            qInfo("SymbolProbe: no project file at '%s': %s", qUtf8Printable(requested), qUtf8Printable(error));
            QTimer::singleShot(300, qApp, &QCoreApplication::quit);
            return;
        }
        if (!projectManager->availableProjects().contains(probeName, Qt::CaseInsensitive)
            && !projectManager->registerProject(projectFile, nullptr, &error)) {
            qInfo("SymbolProbe: registerProject failed: %s", qUtf8Printable(error));
            QTimer::singleShot(300, qApp, &QCoreApplication::quit);
            return;
        }
        name = probeName;
    } else {
        if (shadowRoot.isEmpty()) {
            qInfo("SymbolProbe: NOTEPADNEXT_SYNC_PROBE_DIR is not set");
            QTimer::singleShot(300, qApp, &QCoreApplication::quit);
            return;
        }
        // Register the (already parsed) shadow project of runSyncProbe if this
        // process has not seen it yet - createProject() only adds the settings
        // entry and writes the config, the databases stay as they are.
        if (!projectManager->availableProjects().contains(name, Qt::CaseInsensitive)) {
            if (!projectManager->createProject(name, shadowRoot, srcRoot, &error)) {
                qInfo("SymbolProbe: createProject failed: %s", qUtf8Printable(error));
                QTimer::singleShot(300, qApp, &QCoreApplication::quit);
                return;
            }
            projectManager->closeProject();
        }
    }
    if (!projectManager->openProject(name, &error)) {
        qInfo("SymbolProbe: openProject failed: %s", qUtf8Printable(error));
        QTimer::singleShot(300, qApp, &QCoreApplication::quit);
        return;
    }

    // openProject() parses the tags file on a worker thread and lets the GUI
    // thread refresh the panel when it is done; wait until the count settles.
    QElapsedTimer waiting;
    waiting.start();
    int lastCount = -1;
    while (waiting.elapsed() < 180000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const int count = projectManager->symbolCount();
        if (count > 0 && count == lastCount)
            break;
        lastCount = count;
    }
    qInfo("SymbolProbe: %d symbols loaded (waited %lld ms)", lastCount,
          static_cast<long long>(waiting.elapsed()));

    auto *tree = projectPanel->findChild<QTreeView *>(QStringLiteral("projectSymbolTree"));
    auto *filterEdit = projectPanel->findChild<QLineEdit *>(QStringLiteral("projectSymbolFilter"));
    QAbstractItemModel *rows = tree ? tree->model() : Q_NULLPTR;
    // Every symbol of the database has to be a row: no cap, nothing hidden, the
    // whole result reachable through the scrollbar.
    QElapsedTimer timer;
    timer.start();
    projectPanel->refreshSymbols();
    qInfo("SymbolProbe: refreshSymbols took %lld ms (rows=%d of %d symbols)",
          static_cast<long long>(timer.elapsed()), rows ? rows->rowCount() : -1,
          projectManager->symbolCount());
    if (filterEdit) {
        // The filter box, driven like a user drives it: one keyword, then back
        // to the whole list.
        timer.restart();
        filterEdit->setText(QStringLiteral("skb"));
        qInfo("SymbolProbe: filter 'skb' took %lld ms (rows=%d)", static_cast<long long>(timer.elapsed()),
              rows ? rows->rowCount() : -1);
        timer.restart();
        filterEdit->clear();
        qInfo("SymbolProbe: clearing the filter took %lld ms (rows=%d)",
              static_cast<long long>(timer.elapsed()), rows ? rows->rowCount() : -1);
    }

    qInfo("SymbolProbe: finished");
    QTimer::singleShot(300, qApp, &QCoreApplication::quit);
}

void ProjectMainWindow::runSyncProbe()
{
    const QString srcRoot = qEnvironmentVariable("NOTEPADNEXT_SYNC_PROBE");
    const QString listSource = qEnvironmentVariable("NOTEPADNEXT_SYNC_PROBE_LIST");
    const QString shadowRoot = qEnvironmentVariable("NOTEPADNEXT_SYNC_PROBE_DIR");
    const QString name = QStringLiteral("SyncProbe");
    const QString dir = shadowRoot + QLatin1Char('/') + name + QStringLiteral(".codeinsight");
    QString error;

    QDir(shadowRoot).removeRecursively();
    QDir().mkpath(shadowRoot);

    if (!projectManager->createProject(name, shadowRoot, srcRoot, &error)) {
        qInfo("SyncProbe: createProject failed: %s", qUtf8Printable(error));
        return;
    }

    // createProject() adopts the empty project right away and closeProject()
    // writes the in-memory (still empty) list back to the .filelist, so the
    // order matters: close first, then write the real list, then open it.
    projectManager->closeProject();
    if (!listSource.isEmpty()) {
        QFile in(listSource);
        QFile out(dir + QLatin1Char('/') + name + QStringLiteral(".filelist"));
        if (in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(in.readAll());
            out.close();
        }
    }

    if (!projectManager->openProject(name, &error)) {
        qInfo("SyncProbe: openProject failed: %s", qUtf8Printable(error));
        return;
    }
    qInfo("SyncProbe: files=%lld", static_cast<long long>(projectManager->projectFiles().size()));

    const auto runOnce = [&](const char *label) {
        const ProjectManager::SyncDelta delta = projectManager->computeDelta(/*force=*/true);
        qInfo("SyncProbe[%s]: added=%lld removed=%lld steps=%d", label,
              static_cast<long long>(delta.added.size()),
              static_cast<long long>(delta.removed.size()),
              ProjectManager::syncStepCount(delta));

        QElapsedTimer timer;
        timer.start();
        qint64 lastMs = 0;
        int lastStep = -1;
        bool cancelled = false;
        QString message;
        const bool ok = projectManager->synchronize(delta, [&](int step, int total, const QString &what) {
            if (step == lastStep)
                return true; // one phase polls every 150 ms; that is not news
            lastStep = step;
            const qint64 now = timer.elapsed();
            qInfo("SyncProbe[%s]: step %d/%d  +%lld ms  at %lld ms  %s", label, step, total,
                  static_cast<long long>(now - lastMs), static_cast<long long>(now),
                  qUtf8Printable(what));
            lastMs = now;
            return true;
        }, &message, &cancelled);
        qInfo("SyncProbe[%s]: ok=%d cancelled=%d elapsed=%lld ms err=%s", label, ok, cancelled,
              static_cast<long long>(timer.elapsed()), qUtf8Printable(message));
    };

    runOnce("cold");
    runOnce("warm");
    runSyncUiProbe();

    qInfo("SyncProbe: finished");
    QTimer::singleShot(300, qApp, &QCoreApplication::quit);
}

// The user reaches a synchronization through the "Synchronize Files" dialog:
// Start runs synchronize() from the GUI thread and pumps events every 150 ms so
// the modal progress dialog can repaint. This drives that exact path (dialog
// open, Start clicked, progress watched) and prints every change of the bar, so
// a stall shows up as a gap in the trace. The project has to be the shadow one
// prepared by runSyncProbe().
void ProjectMainWindow::runSyncUiProbe()
{
    QElapsedTimer clock;
    int lastValue = -1;
    bool sawProgress = false;
    int round = 0;

    auto *watch = new QTimer(this);
    watch->setInterval(200);
    connect(watch, &QTimer::timeout, this, [&clock, &lastValue, &sawProgress, &round]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == Q_NULLPTR)
            return;
        auto *bar = dialog->findChild<QProgressBar *>();
        if (bar == Q_NULLPTR)
            return;
        sawProgress = true;
        if (bar->value() == lastValue)
            return;
        lastValue = bar->value();
        qInfo("SyncProbe[ui%d]: %d/%d at %lld ms", round, bar->value(), bar->maximum(),
              static_cast<long long>(clock.elapsed()));
    });

    // Clicks Start whenever the (re-opened) Synchronize Files dialog is up and
    // no run is in flight yet, so the loop below can drive it twice in a row
    // exactly like the user does.
    auto *clicker = new QTimer(this);
    clicker->setInterval(600);
    connect(clicker, &QTimer::timeout, this, []() {
        QWidget *modal = QApplication::activeModalWidget();
        if (modal == Q_NULLPTR || qobject_cast<ProjectSyncProgressDialog *>(modal) != Q_NULLPTR)
            return; // nothing up, or a run is already going
        auto *dialog = qobject_cast<QDialog *>(modal);
        if (dialog == Q_NULLPTR)
            return;
        for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Start")) {
                qInfo("SyncProbe[ui]: clicking Start");
                button->click();
                return;
            }
        }
    });

    clock.start();
    watch->start();
    clicker->start();
    for (round = 1; round <= 2; ++round) {
        lastValue = -1;
        sawProgress = false;
        QElapsedTimer roundClock;
        roundClock.start();
        {
            SynchronizeFilesDialog dialog(projectManager, window);
            dialog.exec();
        }
        qInfo("SyncProbe[ui%d]: dialog closed after %lld ms (last=%d, progress=%d)",
              round, static_cast<long long>(roundClock.elapsed()), lastValue, sawProgress);
    }
    clicker->stop();
    watch->stop();
    qInfo("SyncProbe[ui]: both dialogs finished after %lld ms", static_cast<long long>(clock.elapsed()));

    // What the "100%" step still leaves for the GUI thread: the signal emitted
    // right after it is a direct connection into the panel, which hands the
    // whole table to its model (the rows are built on demand, so this is a walk
    // over the symbols and nothing more - see SymbolTreeModel).
    {
        QElapsedTimer timer;
        timer.start();
        const int count = projectManager->symbols().size();
        projectPanel->refreshSymbols();
        qInfo("SyncProbe[ui]: refreshSymbols(%d symbols) took %lld ms", count,
              static_cast<long long>(timer.elapsed()));
    }
}

// Offscreen regression: create -> add files -> synchronize -> symbols ->
// jump (file local + project) -> auto sync on save -> re-parse a modified file
// -> cancel a run -> close -> remove.
// Prints "ProjectSelfCheck: ALL PASSED" or the failing steps, then quits.
void ProjectMainWindow::runProjectSelfCheck()
{
    QStringList failures;
    auto check = [&failures](bool ok, const char *what) {
        qInfo("ProjectSelfCheck: %-42s %s", what, ok ? "PASS" : "FAIL");
        if (!ok)
            failures.append(what);
    };
    auto waitUntil = [](const std::function<bool()> &cond, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        while (!cond()) {
            if (timer.elapsed() > timeoutMs)
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(20);
        }
        return true;
    };
    // Modals raised from inside a modal loop are not always reported by
    // QApplication::activeModalWidget() (the offscreen plugin used by this self
    // check does not report them), so waiting for one through that call alone
    // means waiting forever inside somebody else's event loop. A dialog that is
    // in exec() is by definition a visible top level window, which is true on
    // every platform, so ask for that and keep activeModalWidget() as the first,
    // cheapest attempt.
    const auto visibleNewProjectDialog = []() -> NewProjectDialog * {
        if (auto *dialog = qobject_cast<NewProjectDialog *>(QApplication::activeModalWidget()))
            return dialog;
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (auto *dialog = qobject_cast<NewProjectDialog *>(w); dialog != Q_NULLPTR && dialog->isVisible())
                return dialog;
        }
        return Q_NULLPTR;
    };
    const auto visibleMessageBox = []() -> QMessageBox * {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            return box;
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (auto *box = qobject_cast<QMessageBox *>(w); box != Q_NULLPTR && box->isVisible())
                return box;
        }
        return Q_NULLPTR;
    };
    // Drives the modal jump chooser from inside the self check: as soon as it
    // is up, double click the given row (0 based) - exactly what the user does.
    // A stuck dialog would hang the run, so a watchdog rejects it instead.
    auto driveChooser = [](int row) {
        auto *timer = new QTimer(qApp);
        timer->setProperty("ticks", 0);
        timer->setInterval(30);
        QObject::connect(timer, &QTimer::timeout, timer, [timer, row]() {
            const int ticks = timer->property("ticks").toInt() + 1;
            timer->setProperty("ticks", ticks);
            if (ticks > 200) { // ~6 s: give up instead of blocking the self check
                timer->stop();
                timer->deleteLater();
                if (QWidget *modal = QApplication::activeModalWidget())
                    modal->close();
                return;
            }
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog == Q_NULLPTR)
                return;
            auto *list = dialog->findChild<QListWidget *>();
            if (list == Q_NULLPTR || list->count() <= row)
                return;
            timer->stop();
            timer->deleteLater();

            list->setCurrentRow(row);
            QListWidgetItem *item = list->item(row);
            const QPoint pos = list->visualItemRect(item).center();
            const QPoint global = list->viewport()->mapToGlobal(pos);
            QMouseEvent press(QEvent::MouseButtonPress, pos, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, pos, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &release);
            QMouseEvent dbl(QEvent::MouseButtonDblClick, pos, global,
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &dbl);
        });
        timer->start();
    };
    // Number of tag entries for a symbol: a stale entry after a re-parse shows
    // up as a second line here.
    auto tagEntries = [](const QString &tagsFile, const QString &name) {
        QFile f(tagsFile);
        int count = 0;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray prefix = name.toUtf8() + '\t';
            while (!f.atEnd()) {
                if (f.readLine().startsWith(prefix))
                    ++count;
            }
        }
        return count;
    };

    // --- test data ---
    const QString base = QDir::temp().absoluteFilePath(QStringLiteral("npn_projcheck"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);
    const QString dataPath = QDir(base).absoluteFilePath(QStringLiteral("data"));
    const QString srcDir = QDir(base).absoluteFilePath(QStringLiteral("src"));
    const QString srcSub = QDir(srcDir).absoluteFilePath(QStringLiteral("sub"));
    QDir().mkpath(dataPath);
    QDir().mkpath(srcDir);
    QDir().mkpath(srcSub);
    auto writeFile = [](const QString &path, const QString &content) {
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(content.toUtf8());
    };
    writeFile(QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c")),
              "#include <stdio.h>\n\nvoid alpha(void)\n{\n    printf(\"alpha\\n\");\n}\n");
    // beta() calls alpha(): the Find Callers regression below needs a call site
    // (line 7), and the prototype on line 3 must not count as one.
    writeFile(QDir(srcDir).absoluteFilePath(QStringLiteral("beta.c")),
              "#include <stdio.h>\n\nvoid alpha(void);\n\nint beta(int x)\n{\n    alpha();\n    return x + 1;\n}\n");
    // gamma() calls beta() twice, from two different lines: the multiple-call-
    // site case of Find Callers (one entry per line, so the two lines stay
    // apart while a nested double call on one line would collapse into one).
    writeFile(QDir(srcSub).absoluteFilePath(QStringLiteral("gamma.c")),
              "int gamma(void)\n{\n    int a = beta(1);\n    return a + beta(2);\n}\n");

    // --- Project menu localization ---
    // Unlike the other menus this one is built in C++ (setupProjectFeature)
    // instead of MainWindow.ui, so every entry has to go through tr() to reach
    // i18n/*.ts - the very mechanism the menus from the .ui file use. Comparing
    // the live widgets against QCoreApplication::translate() catches an entry
    // that was hard coded with QStringLiteral(): it would keep its English text
    // under every language.
    //
    // The context is the one the entries are actually built with: the menu
    // lives in this class, so its tr() calls are recorded under
    // "ProjectMainWindow" in the .ts files. Asking under "MainWindow" would
    // only ever find the source strings and would flag every translated entry
    // as hard coded.
    {
        const auto menuText = [](const char *source) {
            return QCoreApplication::translate("ProjectMainWindow", source);
        };

        check(projectMenu->title() == menuText("Project"),
              "menu title is translatable");

        struct MenuEntry { QAction *action; const char *source; };
        const MenuEntry menuEntries[] = {
            {newProjectAction, "New Project..."},
            {openProjectAction, "Open Project..."},
            {closeProjectAction, "Close Project"},
            {removeProjectAction, "Remove Project..."},
            {addRemoveProjectFilesAction, "Add and Remove Project Files..."},
            {synchronizeFilesAction, "Synchronize Files..."},
        };
        int hardCoded = 0;
        for (const MenuEntry &entry : menuEntries) {
            if (entry.action->text() != menuText(entry.source))
                ++hardCoded;
        }
        check(hardCoded == 0, "every menu entry is translatable");

        // No project open at this point: the header line shows the plain label.
        check(currentProjectAction->text() == menuText("Current Project:"),
              "menu header label is translatable");

        // Logged as well so a localized run can be eyeballed.
        qInfo("ProjectSelfCheck: menu \"%s\": %s | %s | %s | %s | %s | %s | %s",
              qUtf8Printable(projectMenu->title()),
              qUtf8Printable(currentProjectAction->text()),
              qUtf8Printable(newProjectAction->text()),
              qUtf8Printable(openProjectAction->text()),
              qUtf8Printable(closeProjectAction->text()),
              qUtf8Printable(removeProjectAction->text()),
              qUtf8Printable(addRemoveProjectFilesAction->text()),
              qUtf8Printable(synchronizeFilesAction->text()));
    }

    // --- create project ---
    QString errorMessage;
    const bool projectCreated =
        projectManager->createProject(QStringLiteral("ProjCheck"), dataPath, srcDir, &errorMessage);
    if (!projectCreated) {
        qInfo("ProjectSelfCheck: create error: base=%s srcDir=%s srcExists=%d dataExists=%d error=%s",
              qUtf8Printable(base), qUtf8Printable(srcDir), int(QFileInfo::exists(srcDir)),
              int(QFileInfo::exists(dataPath)), qUtf8Printable(errorMessage));
    }
    check(projectCreated, "create project");

    check(QFileInfo::exists(projectManager->projectConfigFilePath()), ".codeinsightprj created");
    check(projectPanel->isVisible(), "panel visible after create");
    check(currentProjectAction->text().contains(QStringLiteral("ProjCheck")), "menu shows current project");
    check(currentProjectAction->text()
              == QCoreApplication::translate("ProjectMainWindow", "Current Project:  %1")
                     .arg(QStringLiteral("ProjCheck")),
          "menu header follows the translation");

    // --- a refused "New Project..." keeps the dialog and the typed data ---
    // The user's path exactly: press OK on a name that is already taken, get the
    // error over the still open form, dismiss it and carry on editing what was
    // already typed - the attempt must not throw the form away.
    {
        struct NewProjectProbe {
            QPointer<NewProjectDialog> dialog;
            QPointer<QLineEdit> nameField;
            QPointer<QLineEdit> sourceField;
            QPointer<QLineEdit> dataField;
            int stage = 0;
            int ticks = 0;
            int beats = 0;
            int statusPrints = 0;
            bool warningOverDialog = false;
            bool dialogKeptWhileWarning = false;
            bool sameDialogAfterWarning = false;
            bool okStillEnabled = false;
            QString warningText;
            QString nameAfter;
            QString sourceAfter;
            QString dataAfter;
        };
        NewProjectProbe probe;

        // Pressing OK blocks: the dialog judges the input from inside its own event
        // loop, and a refused creation opens a warning that runs a loop of its own.
        // A timer slot cannot be re-entered while it is on the stack, so clicking
        // from the driver's own slot would freeze the driver for exactly as long as
        // the warning is up - it would never see the warning it is meant to dismiss.
        // Queue the press instead: the driver's slot returns, and the driver keeps
        // running inside the modal loop that the press opens.
        const auto clickLater = [](QAbstractButton *button) {
            if (button == Q_NULLPTR)
                return;
            QMetaObject::invokeMethod(button, [button]() { button->click(); }, Qt::QueuedConnection);
        };

        auto *driver = new QTimer(this);
        driver->setInterval(30);
        connect(driver, &QTimer::timeout, this, [&probe, dataPath, srcSub, visibleNewProjectDialog, visibleMessageBox, clickLater]() {
            // While the scenario drags on, say what the probe can actually see: a
            // probe waiting for a window that never shows up looks exactly like a
            // hung self check, and this is what tells the two apart. Silent while
            // the probe is quick, which is the normal case.
            if (++probe.beats % 34 == 0 && probe.statusPrints < 12) {
                ++probe.statusPrints;
                QStringList tops;
                const QWidgetList all = QApplication::topLevelWidgets();
                for (QWidget *w : all) {
                    if (w->isVisible())
                        tops.append(QString::fromLatin1(w->metaObject()->className()));
                }
                qInfo("ProjectSelfCheck: new project probe stage=%d visible=[%s] activeModal=%s",
                      probe.stage, qUtf8Printable(tops.join(QStringLiteral(","))),
                      QApplication::activeModalWidget() != Q_NULLPTR ? "yes" : "no");
            }
            if (probe.stage == 0) {
                auto *dialog = visibleNewProjectDialog();
                if (dialog == Q_NULLPTR)
                    return;
                const QList<QLineEdit *> fields = dialog->findChildren<QLineEdit *>();
                if (fields.size() < 3)
                    return;
                // Identify the three edits by what they hold, not by their position
                // in the object tree: which one is the source root is part of the
                // dialog's contract, the order they were built in is not.
                for (QLineEdit *edit : fields) {
                    if (edit->text().endsWith(QStringLiteral(".codeinsight")))
                        probe.dataField = edit;
                    else if (edit->text() == QDir::homePath())
                        probe.sourceField = edit;
                }
                for (QLineEdit *edit : fields) {
                    if (edit != probe.dataField && edit != probe.sourceField)
                        probe.nameField = edit;
                }
                if (probe.nameField == Q_NULLPTR || probe.sourceField == Q_NULLPTR || probe.dataField == Q_NULLPTR)
                    return;
                probe.dialog = dialog;
                // "ProjCheck" already exists, so createProject refuses it. The other
                // two fields point somewhere real, so the refusal is about the name.
                probe.nameField->setText(QStringLiteral("ProjCheck"));
                probe.sourceField->setText(QDir::toNativeSeparators(srcSub));
                probe.dataField->setText(QDir::toNativeSeparators(dataPath));
                ++probe.stage;
                auto *box = dialog->findChild<QDialogButtonBox *>();
                auto *ok = box != Q_NULLPTR ? box->button(QDialogButtonBox::Ok) : Q_NULLPTR;
                qInfo("ProjectSelfCheck: new project probe typed [name=%s source=%s data=%s] okEnabled=%d",
                      qUtf8Printable(probe.nameField->text()),
                      qUtf8Printable(probe.sourceField->text()),
                      qUtf8Printable(probe.dataField->text()),
                      int(ok != Q_NULLPTR && ok->isEnabled()));
                clickLater(ok);
                return;
            }
            if (probe.stage == 1) {
                auto *warning = visibleMessageBox();
                if (warning == Q_NULLPTR)
                    return;
                probe.warningText = warning->text();
                probe.warningOverDialog = warning->parentWidget() == probe.dialog.data();
                probe.dialogKeptWhileWarning = probe.dialog != Q_NULLPTR && probe.dialog->isVisible();
                ++probe.stage;
                clickLater(warning->button(QMessageBox::Ok));
                return;
            }
            if (probe.stage == 2) {
                auto *dialog = visibleNewProjectDialog();
                if (dialog == Q_NULLPTR)
                    return;
                probe.sameDialogAfterWarning = (dialog == probe.dialog.data());
                if (probe.nameField != Q_NULLPTR && probe.sourceField != Q_NULLPTR && probe.dataField != Q_NULLPTR) {
                    probe.nameAfter = probe.nameField->text();
                    probe.sourceAfter = probe.sourceField->text();
                    probe.dataAfter = probe.dataField->text();
                }
                auto *box = dialog->findChild<QDialogButtonBox *>();
                probe.okStillEnabled = box != Q_NULLPTR && box->button(QDialogButtonBox::Ok)->isEnabled();
                ++probe.stage;
                if (box != Q_NULLPTR)
                    box->button(QDialogButtonBox::Cancel)->click(); // leave as the user would
            }
        });
        auto *watchdog = new QTimer(this);
        watchdog->setInterval(500);
        connect(watchdog, &QTimer::timeout, this, [&probe, driver, watchdog]() {
            if (++probe.ticks <= 20 || probe.stage >= 3)
                return; // 10 s is plenty; a probe that already finished needs nothing
            // Stuck: abandon the scenario instead of hanging the whole self check.
            driver->stop();
            watchdog->stop();
            qInfo("ProjectSelfCheck: new project probe gave up at stage %d - closing the modal window",
                  probe.stage);
            // Close by walking the top level windows: an abandoned probe must not
            // leave a modal loop behind for the rest of the self check to sit in.
            const QWidgetList tops = QApplication::topLevelWidgets();
            for (QWidget *w : tops) {
                if (w->isVisible() && w->isModal())
                    w->close();
            }
        });
        driver->start();
        watchdog->start();
        newProjectAction->trigger(); // blocks in the dialog's event loop
        driver->stop();
        watchdog->stop();
        driver->deleteLater();
        watchdog->deleteLater();

        check(probe.stage >= 3, "the new project dialog was driven through a refusal");
        check(probe.warningText
                  == QCoreApplication::translate("ProjectManager",
                                                 "A project named \"%1\" already exists.")
                         .arg(QStringLiteral("ProjCheck")),
              "the refusal reports the name clash to the user");
        check(probe.warningOverDialog && probe.dialogKeptWhileWarning,
              "the warning appears over the still open New Project dialog");
        check(probe.sameDialogAfterWarning, "the same dialog comes back after the warning");
        check(probe.nameAfter == QStringLiteral("ProjCheck")
                  && probe.sourceAfter == QDir::toNativeSeparators(srcSub)
                  && probe.dataAfter == QDir::toNativeSeparators(dataPath),
              "the typed values are still there to edit");
        check(probe.okStillEnabled, "OK is still usable for another attempt");
    }

    // --- add files (tree) and synchronize ---
    window->openFile(QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c")));
    // A non-language file: must be filtered out by addFiles.
    writeFile(QDir(srcDir).absoluteFilePath(QStringLiteral("notes.txt")), "not source\n");
    QStringList treeFiles;
    QDirIterator it(srcDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        treeFiles.append(QDir::cleanPath(it.next()));
    bool changed = false;
    projectManager->addFiles(treeFiles, &changed);
    check(changed && projectManager->projectFiles().size() == 3, "add tree -> 3 files");
    check(!projectManager->isProjectFile(QDir(srcDir).absoluteFilePath(QStringLiteral("notes.txt"))), "non-language file filtered out");

    // --- hidden folders are tool/build state, not project sources ---
    // Adding a tree used to drag in whatever a build had parked next to the
    // sources ("D:\code\NotepadNext\.cmake-cache\..."), and those files then sat
    // in the project file list as if they were source.
    {
        const QString hiddenDir = QDir(srcDir).absoluteFilePath(QStringLiteral(".cmake-cache"));
        const QString hiddenFile = QDir(hiddenDir).absoluteFilePath(QStringLiteral("generated.c"));
        const QString visibleFile = QDir(srcDir).absoluteFilePath(QStringLiteral("generated.c"));
        QDir().mkpath(hiddenDir);
        writeFile(hiddenFile, "int built_here(void);\n");
        writeFile(visibleFile, "int built_here(void);\n");

        bool hiddenChanged = false;
        projectManager->addFiles({hiddenFile}, &hiddenChanged);
        check(!hiddenChanged && !projectManager->isProjectFile(hiddenFile),
              "a file in a hidden folder is not added");

        // The very same file one level up is accepted, so the check above is
        // about the hidden folder and not about the name or the extension.
        bool visibleChanged = false;
        projectManager->addFiles({visibleFile}, &visibleChanged);
        check(visibleChanged && projectManager->isProjectFile(visibleFile),
              "the same file outside a hidden folder is still added");

        // "." says "this folder" and ".." "the folder above"; both are path
        // spelling, not hidden folders, so "./src/main.c" keeps working.
        check(!ProjectManager::isUnderHiddenFolder(QStringLiteral("./beta.c")),
              "./ is not a hidden folder");
        check(!ProjectManager::isUnderHiddenFolder(QStringLiteral("../beta.c")),
              ".. is not a hidden folder");
        check(ProjectManager::isUnderHiddenFolder(QStringLiteral(".git/config.c")),
              "a folder whose name starts with a dot is hidden");
        check(!ProjectManager::isUnderHiddenFolder(QStringLiteral("src/sub/.hidden.c")),
              "a file whose own name starts with a dot is still a file");

        // Leave the fixture as it was: the rest of the run counts these files
        // and synchronizes them.
        projectManager->removeFiles({visibleFile});
        QFile::remove(visibleFile);
        QFile::remove(hiddenFile);
        QDir().rmdir(hiddenDir);
        check(projectManager->projectFiles().size() == 3, "the hidden folder check left the file list alone");
    }

    // --- the file browser does not offer what addFiles refuses ---
    {
        const QString browseDir = QDir(base).absoluteFilePath(QStringLiteral("browse"));
        QDir().mkpath(QDir(browseDir).absoluteFilePath(QStringLiteral(".cache")));
        QDir().mkpath(QDir(browseDir).absoluteFilePath(QStringLiteral("keep")));

        ProjectFilesDialog browser(projectManager, window);
        auto *pathField = browser.findChild<QLineEdit *>();
        check(pathField != Q_NULLPTR, "the file browser exposes its path field");
        if (pathField != Q_NULLPTR) {
            pathField->setText(QDir::toNativeSeparators(browseDir));
            QMetaObject::invokeMethod(&browser, "onPathEdited"); // navigate to the fixture
        }

        bool listedHidden = false;
        bool listedVisible = false;
        for (QTreeWidget *tree : browser.findChildren<QTreeWidget *>()) {
            for (QTreeWidgetItemIterator it(tree); *it != Q_NULLPTR; ++it) {
                const QString name = (*it)->text(0);
                if (name == QStringLiteral(".cache"))
                    listedHidden = true;
                else if (name == QStringLiteral("keep"))
                    listedVisible = true;
            }
        }
        check(!listedHidden, "the file browser does not list a hidden folder");
        check(listedVisible, "the file browser still lists a normal folder");
    }

    // Project name convention: spaces etc. are rejected.
    check(!ProjectManager::validateProjectName(QStringLiteral("Bad Name")), "project name rejects spaces");
    check(ProjectManager::validateProjectName(QStringLiteral("MyProj-1.0")), "project name accepts letters/digits/._-");
    check(QFileInfo::exists(projectManager->fileListFilePath()), ".filelist written");

    check(runProjectSynchronize(false), "synchronize runs");
    check(QFileInfo::exists(projectManager->ctagsFilePath()), ".ctags written");
    check(QFileInfo::exists(projectManager->cscopeFilePath()), ".cscope written");
    check(waitUntil([this]() { return projectManager->symbols().size() >= 3; }, 10000), "symbols parsed (>=3)");
    check(!projectManager->lookupSymbols(QStringLiteral("alpha")).isEmpty(), "lookup alpha");
    check(!projectManager->lookupSymbols(QStringLiteral("gamma")).isEmpty(), "lookup gamma (subdir file)");

    // --- project jump: open beta.c via the symbol table ---
    const ProjectManager::ProjectSymbol gammaSym = projectManager->lookupSymbols(QStringLiteral("gamma")).first();
    jumpToProjectSymbol(gammaSym.file, gammaSym.line);
    check(window->currentEditor() && window->currentEditor()->getFilePath().endsWith(QStringLiteral("gamma.c")), "project jump opened gamma.c");
    check(window->currentEditor()->lineFromPosition(window->currentEditor()->currentPos()) + 1 == gammaSym.line, "jump landed on symbol line");

    // --- Folders tab: follows the editor file, expands level by level ---
    {
        auto *panelTabs = projectPanel->findChild<QTabWidget *>(QStringLiteral("projectTabs"));
        auto *foldersTree = projectPanel->findChild<QTreeWidget *>(QStringLiteral("projectFolderTree"));
        auto *foldersList = projectPanel->findChild<QTreeWidget *>(QStringLiteral("projectFolderFileList"));
        check(panelTabs && foldersTree && foldersList, "folders widgets found");
        if (panelTabs && foldersTree && foldersList) {
            panelTabs->setCurrentIndex(panelTabs->count() - 1); // Folders is the last tab
            projectPanel->trackEditorFile(QDir(srcSub).absoluteFilePath(QStringLiteral("gamma.c")));

            QTreeWidgetItem *cur = foldersTree->currentItem();
            check(cur && cur->data(0, Qt::UserRole).toString() == QDir::cleanPath(srcSub),
                  "folders tab switched to the editor file directory");
            check(cur && cur->parent() && cur->parent()->isExpanded(),
                  "directory tree expanded level by level");
            check(foldersList->topLevelItemCount() >= 1, "file list refreshed for the directory");
        }
    }

    // --- panel click sequence: A, B, then A again ---
    // Regression for "the second click on a row does not jump": after a jump
    // the editor holds the focus, so the panel has to react to plain clicks.
    {
        auto *panelTabs = projectPanel->findChild<QTabWidget *>(QStringLiteral("projectTabs"));
        auto *symbolTree = projectPanel->findChild<QTreeView *>(QStringLiteral("projectSymbolTree"));
        if (panelTabs)
            panelTabs->setCurrentIndex(0); // Project Symbols
        QCoreApplication::processEvents();

        const auto symbolRows = [symbolTree]() {
            return symbolTree && symbolTree->model() ? symbolTree->model()->rowCount() : 0;
        };
        check(symbolRows() >= 2, "symbol tree populated for clicking");
        if (symbolRows() >= 2) {
            auto clickRow = [](QTreeView *tree, int row) {
                const QModelIndex index = tree->model()->index(row, SymbolTreeModel::SymbolColumn);
                if (!index.isValid())
                    return;
                tree->scrollTo(index);
                QCoreApplication::processEvents();
                const QPoint pos = tree->visualRect(index).center();
                const QPoint global = tree->viewport()->mapToGlobal(pos);
                QMouseEvent press(QEvent::MouseButtonPress, pos, global,
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, pos, global,
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &release);
                QCoreApplication::processEvents();
            };
            auto clickLandedOnRow = [this, symbolTree](int row) {
                const QModelIndex index = symbolTree->model()->index(row, SymbolTreeModel::SymbolColumn);
                if (!index.isValid() || !window->currentEditor())
                    return false;
                const int wantLine = index.data(SymbolTreeModel::LineRole).toInt();
                const QString wantFile =
                    QFileInfo(index.data(SymbolTreeModel::PathRole).toString()).canonicalFilePath();
                const int gotLine = window->currentEditor()->lineFromPosition(window->currentEditor()->currentPos()) + 1;
                const QString gotFile = QFileInfo(window->currentEditor()->getFilePath()).canonicalFilePath();
                return gotFile == wantFile && gotLine == wantLine;
            };

            clickRow(symbolTree, 0);
            check(clickLandedOnRow(0), "click #1 on row 0 jumps");
            clickRow(symbolTree, 1);
            check(clickLandedOnRow(1), "click #2 on row 1 jumps");
            clickRow(symbolTree, 0);
            check(clickLandedOnRow(0), "click #3 back on row 0 jumps again");
        }
    }

    // --- file-local jump via the function list path ---
    ScintillaNext *alphaEditor = Q_NULLPTR;
    const QFileInfo alphaInfo(QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c")));
    for (ScintillaNext *editor : window->dockedEditor->editors()) {
        if (editor->isFile() && QFileInfo(editor->getFilePath()).canonicalFilePath() == alphaInfo.canonicalFilePath())
            alphaEditor = editor;
    }
    if (alphaEditor) {
        window->dockedEditor->switchToEditor(alphaEditor);
        check(waitUntil([this]() { return ctagsManager->hasParsed(window->currentEditor()->getFilePath()); }, 15000), "function list parsed alpha.c");
        const Sci_CharacterRange range = alphaEditor->wordAtPosition(static_cast<int>(alphaEditor->currentPos()));
        Q_UNUSED(range);
        // jump to beta() through the project table (alpha.c has no beta symbol)
        jumpToProjectSymbol(projectManager->lookupSymbols(QStringLiteral("beta")).first().file, 3);
        check(window->currentEditor()->getFilePath().endsWith(QStringLiteral("beta.c")), "jump switched to beta.c");
    } else {
        check(false, "alpha.c editor found");
    }

    // --- Find Callers: cscope answers "who calls this symbol?" ---
    // The database built by the synchronization holds beta.c:7 -> alpha() and
    // the two beta() calls in gamma.c.
    if (alphaEditor) {
        check(findCallersAction != Q_NULLPTR && !findCallersAction->icon().isNull(), "Find Callers toolbar icon drawn");
        const QVector<ProjectManager::ProjectSymbol> alphaCallers = projectManager->findCallers(QStringLiteral("alpha"));
        check(alphaCallers.size() == 1, "findCallers(alpha) -> one call site");
        check(!alphaCallers.isEmpty() && alphaCallers.first().name == QStringLiteral("beta"), "call site reported as beta()");
        check(!alphaCallers.isEmpty() && alphaCallers.first().file.endsWith(QStringLiteral("beta.c")), "call site is in beta.c");
        check(!alphaCallers.isEmpty() && alphaCallers.first().line == 7, "call site on line 7");
        check(projectManager->findCallers(QStringLiteral("no_such_symbol_zz")).isEmpty(), "unknown symbol has no callers");

        // Several call sites: one entry each, sorted by file and line. This is
        // the list the chooser shows, so the count has to be exact.
        const QVector<ProjectManager::ProjectSymbol> betaCallers = projectManager->findCallers(QStringLiteral("beta"));
        check(betaCallers.size() == 2, "findCallers(beta) -> two call sites");
        check(betaCallers.size() == 2 && betaCallers.first().line == 3 && betaCallers.last().line == 4,
              "call sites on gamma.c lines 3 and 4");
        check(betaCallers.size() == 2 && betaCallers.first().file.endsWith(QStringLiteral("gamma.c")),
              "call sites are in gamma.c");

        // The toolbar entry point: with the caret on the definition of alpha()
        // in alpha.c the single call site must be taken right away.
        window->dockedEditor->switchToEditor(alphaEditor);
        const int alphaCaret = static_cast<int>(alphaEditor->positionFromLine(2)) + 5; // "void alpha(void)"
        findCallersAt(alphaEditor, alphaCaret);
        check(window->currentEditor() && window->currentEditor()->getFilePath().endsWith(QStringLiteral("beta.c")),
              "Find Callers switched to beta.c");
        check(window->currentEditor() && window->currentEditor()->lineFromPosition(window->currentEditor()->currentPos()) + 1 == 7,
              "Find Callers landed on the call site");

        // Several call sites (beta() is called from two lines of gamma.c): the
        // chooser the symbol jump uses has to appear, and double clicking its
        // second row must land on the second call site.
        ScintillaNext *betaEditor = window->currentEditor();
        check(betaEditor != Q_NULLPTR && betaEditor->getFilePath().endsWith(QStringLiteral("beta.c")),
              "beta.c is current for the chooser test");
        if (betaEditor) {
            const int betaCaret = static_cast<int>(betaEditor->positionFromLine(4)) + 4; // "int beta(int x)"
            driveChooser(1);
            findCallersAt(betaEditor, betaCaret);
            check(window->currentEditor() && window->currentEditor()->getFilePath().endsWith(QStringLiteral("gamma.c")),
                  "chooser jumped into the caller file");
            check(window->currentEditor() && window->currentEditor()->lineFromPosition(window->currentEditor()->currentPos()) + 1 == 4,
                  "chooser jumped to the double clicked call site");
        }
    } else {
        check(false, "alpha.c editor found for Find Callers");
    }

    // --- relation forms: the row of windows next to the Context view ---
    {
        // Earlier tests left the search results tab in front; the row lives on
        // the relation panel tab, so bring that one back up first.
        searchResultsDock->showRelationPanel();

        const QList<RelationForm *> relationForms = searchResultsDock->relationForms();
        check(searchResultsDock != Q_NULLPTR && relationForms.size() == 2,
              "relation forms: two built with the page");

        // The page is whatever the Context view sits on; the row it fills
        // starts a couple of pixels below the tab names.
        QWidget *page = searchResultsDock->contextPanel()->parentWidget();
        const QRect pageArea = page->rect().adjusted(0, 2, 0, 0);

        // The row fills the page: the windows sit shoulder to shoulder from
        // the left edge, each as tall as the page, and the last one ends
        // exactly at the right edge. isHidden() and not isVisible(): a page
        // behind the other tab of the stack is not visible, but its row is
        // still laid out and that is what is being checked here.
        auto rowOfWindows = [context = searchResultsDock->contextPanel(), pageArea]() {
            QList<QWidget *> row;
            for (QObject *child : context->parentWidget()->children()) {
                if (auto *w = qobject_cast<QWidget *>(child);
                        w != Q_NULLPTR && !w->isHidden() && w->y() == pageArea.top()
                        && w->height() == pageArea.height() && w->width() > 0)
                    row.append(w);
            }
            std::sort(row.begin(), row.end(), [](QWidget *a, QWidget *b) { return a->x() < b->x(); });
            return row;
        };
        auto rowFillsPage = [rowOfWindows, pageArea]() {
            const QList<QWidget *> row = rowOfWindows();

            if (row.isEmpty())
                return false;

            int x = pageArea.left();
            for (QWidget *w : row) {
                if (w->x() != x)
                    return false;
                x += w->width();
            }
            return x == pageArea.right() + 1;
        };
        // The default row starts with the Context view at the left edge - a
        // fact of the layout, not of the filling (the reorder test below moves
        // windows around on purpose).
        {
            const QList<QWidget *> row = rowOfWindows();
            check(rowFillsPage() && !row.isEmpty()
                      && qobject_cast<ContextPanel *>(row.first()) != Q_NULLPTR,
                  "the row of windows fills the page, Context view first");
        }

        if (!relationForms.isEmpty()) {
            RelationForm *form = relationForms.first();
            QTreeWidget *tree = form->tree();

            check(form->isLocked(), "relation form is locked by default");

            // The boundaries between the header labels are drag handles: all
            // three columns take a drag (a stretch column cannot be grabbed),
            // and the row of headers ends exactly at the tree's right edge.
            QHeaderView *headerView = tree->header();
            {
                bool interactive = true;
                for (int column = 0; column < 3; ++column)
                    interactive = interactive
                        && headerView->sectionResizeMode(column) == QHeaderView::Interactive;
                const int spread = headerView->sectionSize(0) + headerView->sectionSize(1)
                    + headerView->sectionSize(2);
                check(interactive && qAbs(spread - tree->viewport()->width()) <= 1,
                      "relation columns: the header boundaries are drag handles and the columns fill the tree");
            }

            // A drag on the boundary between the first two headers reshapes
            // the columns and they keep filling the tree. The form in the row
            // is too narrow for a widening drag (the last column would hit
            // its floor), so the gesture runs on a wide form of its own.
            {
                RelationForm dragForm(projectManager);
                dragForm.resize(600, 400);
                dragForm.show();
                QApplication::processEvents();

                QHeaderView *dragHeader = dragForm.tree()->header();
                const int width0 = dragHeader->sectionSize(0);
                const int width1 = dragHeader->sectionSize(1);
                const int spreadBefore = width0 + width1 + dragHeader->sectionSize(2);
                // The grab zone sits on the left column's side of the
                // boundary, so the press lands a few pixels before the second
                // section starts.
                const int grip = dragHeader->sectionViewportPosition(1) - 3;
                const QPoint gripLocal(grip, dragHeader->height() / 2);
                const QPoint gripGlobal = dragHeader->mapToGlobal(gripLocal);
                QMouseEvent gripPress(QEvent::MouseButtonPress, gripLocal, gripGlobal,
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(dragHeader->viewport(), &gripPress);
                QMouseEvent gripMove(QEvent::MouseMove, gripLocal + QPoint(40, 0), gripGlobal + QPoint(40, 0),
                                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(dragHeader->viewport(), &gripMove);
                QMouseEvent gripRelease(QEvent::MouseButtonRelease, gripLocal + QPoint(40, 0),
                                        gripGlobal + QPoint(40, 0),
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(dragHeader->viewport(), &gripRelease);
                const int spreadAfter = dragHeader->sectionSize(0) + dragHeader->sectionSize(1)
                    + dragHeader->sectionSize(2);
                check((dragHeader->sectionSize(0) != width0 || dragHeader->sectionSize(1) != width1)
                          && spreadAfter == spreadBefore,
                      "a drag on a header boundary reshapes the columns and they still fill the tree");
            }

            // The root row is the symbol, and its next level comes up with it:
            // alpha() is called once, from beta() at beta.c:7.
            form->setRootSymbol(QStringLiteral("alpha"),
                                QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c")), 3);
            check(tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->text(0) == QStringLiteral("alpha"),
                  "relation tree: the root row is the symbol");
            check(tree->topLevelItem(0)->childCount() == 1,
                  "relation tree: the root's next level came up with it");
            check(tree->topLevelItem(0)->childCount() == 1
                      && tree->topLevelItem(0)->child(0)->text(0) == QStringLiteral("beta")
                      && tree->topLevelItem(0)->child(0)->text(1) == QStringLiteral("7"),
                  "relation tree: the call site is beta() at beta.c:7");

            // The pointer over a symbol row shows the arrow: the view has a
            // cursor of its own, so the rim's resize cursor never sticks to
            // the rows.
            check(tree->cursor().shape() == Qt::ArrowCursor,
                  "hovering a symbol row keeps the arrow cursor");

            // A double click on a row asks for the symbol to be opened in the
            // editor - it never expands the row, that is the indicator's job.
            {
                QTreeWidgetItem *rootItem = tree->topLevelItem(0);
                // Start from the row folded: if the double click expanded it,
                // it would be doing the indicator's work.
                rootItem->setExpanded(false);
                QApplication::processEvents();

                bool openRequested = false;
                QString openedFile;
                int openedLine = 0;
                connect(form, &RelationForm::symbolOpenRequested, this,
                        [&](const QString &filePath, int line) {
                            openRequested = true;
                            openedFile = filePath;
                            openedLine = line;
                        },
                        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection));

                const QRect rowRect = tree->visualItemRect(rootItem);
                const QPoint local = rowRect.center();
                const QPoint global = tree->viewport()->mapToGlobal(local);
                QMouseEvent rowPress(QEvent::MouseButtonPress, local, global,
                                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &rowPress);
                QMouseEvent rowRelease(QEvent::MouseButtonRelease, local, global,
                                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &rowRelease);
                QMouseEvent rowDblClick(QEvent::MouseButtonDblClick, local, global,
                                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &rowDblClick);
                QApplication::sendEvent(tree->viewport(), &rowRelease);
                QApplication::processEvents();

                check(!rootItem->isExpanded(),
                      "a double click on a symbol row does not expand it");
                check(openRequested
                          && openedFile == QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c"))
                          && openedLine == 3,
                      "a double click on a symbol row asks the editor to open it");
            }

            // The flip side query at the manager level: beta() calls alpha().
            const QVector<ProjectManager::ProjectSymbol> betaCallees =
                projectManager->findCallees(QStringLiteral("beta"));
            check(betaCallees.size() == 1 && betaCallees.first().name == QStringLiteral("alpha")
                      && betaCallees.first().line == 7,
                  "findCallees(beta) -> alpha at beta.c:7");

            // gamma() calls nobody's attention (nothing calls gamma), so
            // "Called" leaves it without children; flipped to "Call" its two
            // calls of beta() are the next level.
            form->setRootSymbol(QStringLiteral("gamma"),
                                QDir(srcDir).absoluteFilePath(QStringLiteral("gamma.c")), 1);
            check(tree->topLevelItem(0)->childCount() == 0,
                  "Called mode: gamma() has no callers, so no next level");
            if (QToolButton *callSegment = form->findChild<QToolButton *>(QStringLiteral("relationFormModeCall"))) {
                QToolButton *calledSegment = form->findChild<QToolButton *>(QStringLiteral("relationFormModeCalled"));
                check(calledSegment != nullptr && calledSegment->isChecked() && !callSegment->isChecked(),
                      "the capsule lights the mode the tree is in");
                callSegment->click();
                check(!form->isCalledMode() && tree->topLevelItem(0)->text(0) == QStringLiteral("gamma"),
                      "the mode flip rebuilt the tree from the root");
                check(tree->topLevelItem(0)->childCount() == 2,
                      "Call mode: gamma()'s two calls of beta() are the next level");
                calledSegment->click(); // back to Called for the tests below
            } else {
                check(false, "relation form has a mode capsule");
            }

            // A click on a row shows the symbol in the Context view.
            form->setRootSymbol(QStringLiteral("beta"),
                                QDir(srcDir).absoluteFilePath(QStringLiteral("beta.c")), 5);
            if (QTreeWidgetItem *row = tree->topLevelItem(0)->child(1)) {
                const QRect itemRect = tree->visualItemRect(row);
                const QPoint local = itemRect.center();
                QMouseEvent press(QEvent::MouseButtonPress, local, tree->viewport()->mapToGlobal(local),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, local, tree->viewport()->mapToGlobal(local),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &release);
                check(searchResultsDock->contextPanel()->currentFilePath().endsWith(QStringLiteral("gamma.c")),
                      "clicking a relation row shows the symbol in the Context view");
            } else {
                check(false, "relation tree: beta() has its two call sites as rows");
            }

            // The caret feeds the unlocked forms, and only those.
            if (QToolButton *lockButton = form->findChild<QToolButton *>(QStringLiteral("relationFormLockButton"))) {
                lockButton->setChecked(false);
                check(!form->isLocked(), "the lock opened");
                searchResultsDock->updateUnlockedRelationRoots(QStringLiteral("beta"),
                                                               QDir(srcDir).absoluteFilePath(QStringLiteral("beta.c")), 5);
                check(tree->topLevelItem(0)->text(0) == QStringLiteral("beta")
                          && tree->topLevelItem(0)->childCount() == 2,
                      "the unlocked form took the symbol as its root");
                check(relationForms.last()->tree()->topLevelItemCount() == 0,
                      "the locked form did not follow");
            } else {
                check(false, "relation form has a lock button");
            }

            // New: one more form, to the right, and the row still fills.
            const int formsBefore = searchResultsDock->relationForms().size();
            if (QToolButton *newButton = form->findChild<QToolButton *>(QStringLiteral("relationFormNewButton"))) {
                check(form->findChild<QWidget *>(QStringLiteral("relationFormFooter"))
                              ->styleSheet()
                              .contains(QStringLiteral("relationFormNewButton")),
                      "the New button wears the framed, clickable look");
                newButton->click();
                check(searchResultsDock->relationForms().size() == formsBefore + 1,
                      "New built one more relation form");
                RelationForm *newest = searchResultsDock->relationForms().last();
                check(newest->x() > form->x(), "the new form sits to the right of its requester");
                check(rowFillsPage(), "the row still fills the page after New");

                // A width drag on a rim is paid for by the neighbour on that
                // side - the Context view included - so the row keeps its
                // total width. The first form has the room for it here; the
                // form the New button built sits at its minimum already.
                auto rowTotalWidth = [context = searchResultsDock->contextPanel(), pageArea]() {
                    int sum = 0;
                    for (QObject *child : context->parentWidget()->children()) {
                        if (auto *w = qobject_cast<QWidget *>(child);
                                w != Q_NULLPTR && !w->isHidden() && w->y() == pageArea.top()
                                && w->height() == pageArea.height())
                            sum += w->width();
                    }
                    return sum;
                };
                const int totalBeforeDrag = rowTotalWidth();
                const int draggedWidthBefore = form->width();

                const QPoint rimLocal(1, form->height() / 2);
                const QPoint rimGlobal = form->mapToGlobal(rimLocal);
                QMouseEvent rimPress(QEvent::MouseButtonPress, rimLocal, rimGlobal,
                                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(form, &rimPress);
                QMouseEvent rimMove(QEvent::MouseMove, rimLocal, rimGlobal + QPoint(30, 0),
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(form, &rimMove);
                QMouseEvent rimRelease(QEvent::MouseButtonRelease, rimLocal, rimGlobal + QPoint(30, 0),
                                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(form, &rimRelease);
                check(rowFillsPage() && rowTotalWidth() == totalBeforeDrag
                          && form->width() == draggedWidthBefore - 30,
                      "a width drag on a rim is compensated by the neighbour");

                // The header drags the whole window to another boundary, and
                // the row reorders around the drop. Dragged all the way to the
                // left edge, the window has to come out first in the row.
                QWidget *formHeader = newest->findChild<QWidget *>(QStringLiteral("relationFormHeader"));
                if (formHeader != Q_NULLPTR) {
                    const QPoint headerLocal(40, 5);
                    const QPoint headerGlobal = formHeader->mapToGlobal(headerLocal);
                    QMouseEvent headerPress(QEvent::MouseButtonPress, headerLocal, headerGlobal,
                                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(formHeader, &headerPress);
                    // The first move sets the reference point, the second one
                    // does the dragging.
                    QMouseEvent headerMove0(QEvent::MouseMove, headerLocal, headerGlobal,
                                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(formHeader, &headerMove0);
                    QMouseEvent headerMove(QEvent::MouseMove, headerLocal, headerGlobal - QPoint(2000, 0),
                                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(formHeader, &headerMove);
                    QMouseEvent headerRelease(QEvent::MouseButtonRelease, headerLocal, headerGlobal - QPoint(2000, 0),
                                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(formHeader, &headerRelease);

                    QList<RelationForm *> byX = searchResultsDock->relationForms();
                    std::sort(byX.begin(), byX.end(), [](RelationForm *a, RelationForm *b) { return a->x() < b->x(); });
                    check(byX.first() == newest, "the window dropped at the boundary it was dragged to");
                    check(rowFillsPage(), "the row still fills the page after the reorder");
                } else {
                    check(false, "relation form has a header");
                }

                // The X takes the form out again, the rest re-fills the page.
                if (QToolButton *closeButton = newest->findChild<QToolButton *>(QStringLiteral("relationFormCloseButton"))) {
                    closeButton->click();
                    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    check(searchResultsDock->relationForms().size() == formsBefore,
                          "closing removed the form");
                    check(rowFillsPage(), "the row still fills the page after the close");
                } else {
                    check(false, "relation form has a close button");
                }
            } else {
                check(false, "relation form has a New button");
            }
        }

        // --- the last form of the row refuses to leave ---
        {
            QList<RelationForm *> rowForms = searchResultsDock->relationForms();
            QToolButton *closeButton = rowForms.last()->findChild<QToolButton *>(QStringLiteral("relationFormCloseButton"));

            if (closeButton != Q_NULLPTR && rowForms.size() >= 2) {
                // Closing down to one form is still allowed.
                closeButton->click();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                rowForms = searchResultsDock->relationForms();
                check(rowForms.size() == 1, "closing down to the last form leaves one");

                // The last one's X answers with silence: with it gone there
                // would be no New button left to build the row up again.
                QToolButton *lastClose = rowForms.first()->findChild<QToolButton *>(QStringLiteral("relationFormCloseButton"));
                if (lastClose != Q_NULLPTR) {
                    lastClose->click();
                    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    check(searchResultsDock->relationForms().size() == 1, "the last form ignores its close click");
                    check(rowFillsPage(), "the row still fills the page after the refused close");
                } else {
                    check(false, "the last form has a close button");
                }

                // The next block builds its layout from two forms, so the row
                // is brought back up here.
                QToolButton *newButton = rowForms.first()->findChild<QToolButton *>(QStringLiteral("relationFormNewButton"));
                if (newButton != Q_NULLPTR) {
                    newButton->click();
                    check(searchResultsDock->relationForms().size() == 2, "the New button rebuilds the row from the last form");
                } else {
                    check(false, "the last form has a New button");
                }
            } else {
                check(false, "relation forms closeable down to the last");
            }
        }

        // --- a changed layout is saved into the settings and comes back ---
        {
            QList<RelationForm *> rowForms = searchResultsDock->relationForms();

            // One more window, one form flipped to Call and unlocked, the
            // other one's columns dragged: a layout only a save can carry.
            QToolButton *newButton = rowForms.first()->findChild<QToolButton *>(QStringLiteral("relationFormNewButton"));
            if (newButton != Q_NULLPTR) {
                newButton->click();
                rowForms = searchResultsDock->relationForms();
            }
            check(rowForms.size() == 3, "the layout to save has three relation forms");

            rowForms.at(0)->setCalledMode(false);
            rowForms.at(0)->setLocked(false);
            rowForms.at(1)->setColumnWidths({120, 70, 220});
            searchResultsDock->saveRelationPanelLayout();

            ApplicationSettings *settings = qobject_cast<NotepadNextApplication *>(qApp)->getSettings();
            check(settings->value("RelationPanel/windowCount").toInt() == 3,
                  "the saved layout carries the window count");
            check(settings->value("RelationPanel/widths").toString().split(',').size() == 4,
                  "the saved layout carries every window's width");
            check(settings->value("RelationPanel/form.0.mode").toString() == QStringLiteral("Call")
                      && !settings->value("RelationPanel/form.0.locked").toBool(),
                  "the saved layout carries a form's mode and lock");
            check(settings->value("RelationPanel/form.1.columns").toString() == QStringLiteral("120,70,220"),
                  "the saved layout carries the dragged column widths");

            // A fresh dock on its own reads the saved layout back, the way a
            // restarted Notepad Next does when the project opens.
            SearchResultsDock *restoredDock = new SearchResultsDock();
            restoredDock->initRelationPanel(projectManager);
            restoredDock->resize(1000, 400);
            restoredDock->showRelationPanel();
            QApplication::processEvents();
            QApplication::processEvents();

            const QList<RelationForm *> restoredForms = restoredDock->relationForms();
            check(restoredForms.size() == 3, "a fresh dock restores the saved window count");

            bool statesBack = restoredForms.size() == 3;
            if (statesBack) {
                statesBack = !restoredForms.at(0)->isCalledMode()
                    && !restoredForms.at(0)->isLocked()
                    && restoredForms.at(1)->isLocked()
                    && restoredForms.at(1)->columnWidths().at(0) == 120
                    && restoredForms.at(1)->columnWidths().at(1) == 70;
            }
            check(statesBack, "a fresh dock restores the modes, the locks and the dragged columns");

            // The widths come back scaled to whatever page the fresh dock
            // has; either way the row fills that page exactly.
            {
                QWidget *restoredPage = restoredDock->contextPanel()->parentWidget();
                int sum = 0;
                for (QObject *child : restoredPage->children()) {
                    if (auto *w = qobject_cast<QWidget *>(child); w != Q_NULLPTR && !w->isHidden())
                        sum += w->width();
                }
                check(sum == restoredPage->width(), "the restored row fills the restored page");
            }

            check(restoredDock->contextPanel()->isLocked() == searchResultsDock->contextPanel()->isLocked(),
                  "a fresh dock restores the Context view's lock");

            delete restoredDock;
        }
    }

    // --- the search results dock owns the full bottom of the window ---
    {
        // Nothing of its chrome can take the dock away from the bottom: no
        // floating, no dragging to (or inside) another dock area.
        const QDockWidget::DockWidgetFeatures dockFeatures = searchResultsDock->features();
        check(!(dockFeatures & QDockWidget::DockWidgetMovable)
                  && !(dockFeatures & QDockWidget::DockWidgetFloatable),
              "search results dock: title bar cannot drag it out of the bottom");

        // Without floatability the title bar shows no float button at all.
        QWidget *titleBar = searchResultsDock->findChild<QWidget *>(QStringLiteral("dockTabTitleBar"));
        QToolButton *floatButton = titleBar != Q_NULLPTR
            ? titleBar->findChild<QToolButton *>(QStringLiteral("dockFloatButton"))
            : Q_NULLPTR;
        check(floatButton != Q_NULLPTR && !floatButton->isVisible(),
              "search results dock: the float button is gone from the title bar");

        // The dock spans the window's full width at the bottom. A floated
        // dock may keep the very same geometry for a moment, so a dock of
        // its own (a window of its own) is never "in the bottom".
        auto dockSpansFullBottom = [this]() {
            if (!searchResultsDock->isVisible() || searchResultsDock->isWindow())
                return false;
            const QPoint winTopLeft = window->mapToGlobal(QPoint(0, 0));
            const QPoint dockTopLeft = searchResultsDock->mapToGlobal(QPoint(0, 0));
            return dockTopLeft.x() == winTopLeft.x()
                && searchResultsDock->width() == window->width();
        };
        check(dockSpansFullBottom(), "search results dock: occupies the full bottom of the window");
    }

    // --- auto sync: add a file, enqueue (as if saved), wait for the background thread ---
    writeFile(QDir(srcDir).absoluteFilePath(QStringLiteral("delta.c")),
              "int delta_func(void)\n{\n    return 7;\n}\n");
    projectManager->addFiles({QDir(srcDir).absoluteFilePath(QStringLiteral("delta.c"))});
    projectManager->enqueueFileChange(QDir(srcDir).absoluteFilePath(QStringLiteral("delta.c")));
    check(waitUntil([this]() { return !projectManager->lookupSymbols(QStringLiteral("delta_func")).isEmpty(); }, 30000),
          "auto-sync picked up delta.c");
    check(QFileInfo::exists(projectManager->cscopeFilePath()), "cscope db present after auto sync");

    // --- re-parse of a modified file: the old entries must not survive ---
    {
        const QString deltaPath = QDir(srcDir).absoluteFilePath(QStringLiteral("delta.c"));
        // delta_func moves from line 1 to line 2: a stale entry would keep
        // pointing at line 1 and the panel would list the symbol twice.
        writeFile(deltaPath, QStringLiteral("// shifted\nint delta_func(void)\n{\n    return 8;\n}\n"));
        projectManager->enqueueFileChange(deltaPath);
        check(waitUntil([this]() {
                  const QVector<ProjectManager::ProjectSymbol> hits = projectManager->lookupSymbols(QStringLiteral("delta_func"));
                  return hits.size() == 1 && hits.first().line == 2;
              }, 30000),
              "modified file re-parsed without stale entries");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("delta_func")) == 1,
              "tags file holds one delta_func entry");
        // The delta of a saved file removes and re-adds that one file. The tags
        // of every other file are not part of the run, so they have to survive
        // it - dropping the whole tags file here would lose them.
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("alpha")) > 0,
              "incremental re-parse keeps the other files' symbols");
    }

    // --- dropping a file rewrites the tags file in place, and the file it
    // leaves behind has to be one ctags can still append to. The old
    // implementation read the tags file with QIODevice::Text and wrote it back
    // raw, which turned every line ending into LF while the "!_TAG_FILE_SORTED"
    // claim stayed. ctags then never returns from an --append into that file:
    // it rewrites the file over and over at 100% CPU. That is what made a
    // forced synchronization of a large project stop at 1%. ---
    {
        const QString stalePath = QDir(srcDir).absoluteFilePath(QStringLiteral("stale.c"));
        writeFile(stalePath, QStringLiteral("int stale_func(void)\n{\n    return 9;\n}\n"));
        projectManager->addFiles({stalePath});
        projectManager->enqueueFileChange(stalePath);
        check(waitUntil([this]() {
                  return !projectManager->lookupSymbols(QStringLiteral("stale_func")).isEmpty();
              }, 30000),
              "stale.c is parsed before it is dropped");

        // The background sync has to be fully drained before the file below is
        // touched by hand, otherwise the two write to it at the same time.
        check(waitUntil([this]() {
                  const ProjectManager::SyncDelta pending = projectManager->computeDelta(false);
                  return pending.added.isEmpty() && pending.removed.isEmpty();
              }, 30000),
              "the auto sync drained before the tags file is prepared");

        // Put the tags file into exactly the state the old code left behind: LF
        // line endings plus the "!_TAG_FILE_SORTED" claim. ctags itself writes
        // CRLF and this program's own rewrite is what used to strip the CR.
        bool poisoned = false;
        {
            QFile tags(projectManager->ctagsFilePath());
            if (tags.open(QIODevice::ReadOnly)) {
                QByteArray content = tags.readAll();
                tags.close();
                content.replace("\r\n", "\n");

                QByteArray broken;
                broken.append("!_TAG_FILE_SORTED\t1\t/0=unsorted, 1=sorted, 2=foldcase/\n");
                for (const QByteArray &line : content.split('\n')) {
                    if (line.startsWith("!_TAG_FILE_SORTED"))
                        continue; // the claim above replaces whatever was there
                    broken.append(line);
                    broken.append('\n');
                }

                QFile out(projectManager->ctagsFilePath());
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    out.write(broken);
                    poisoned = broken.count('\n') > broken.count("\r\n");
                }
            }
        }
        check(poisoned, "tags file poisoned like the old code left it");

        // Gone from the project and from disk: removed without being re-added,
        // so step 1 rewrites the tags file instead of rebuilding it.
        projectManager->removeFiles({stalePath});
        QFile::remove(stalePath);
        const ProjectManager::SyncDelta delta = projectManager->computeDelta(false);
        check(delta.removed.contains(stalePath) && !delta.added.contains(stalePath),
              "dropped file is removed without being re-added");

        QString error;
        check(projectManager->synchronize(delta, [](int, int, const QString &) { return true; }, &error),
              "synchronization after the drop succeeded");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("stale_func")) == 0,
              "dropped file left no tag entries");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("alpha")) > 0,
              "in-place rewrite kept the unrelated symbols");

        // What the next ctags --append is going to read. The rewrite normalises
        // the endings to the platform's native style (CRLF on Windows, LF
        // elsewhere), so the check adapts to the host instead of hardcoding CRLF.
        int bareLf = 0;
        int bareCrlf = 0;
        int sortedClaim = 0;
        QFile tags(projectManager->ctagsFilePath());
        if (tags.open(QIODevice::ReadOnly)) {
            const QByteArray raw = tags.readAll();
            bareCrlf = raw.count("\r\n");
            bareLf = raw.count('\n') - bareCrlf;
            for (const QByteArray &line : raw.split('\n'))
                if (line.startsWith("!_TAG_FILE_SORTED"))
                    ++sortedClaim;
        }
#if defined(Q_OS_WIN)
        check(bareLf == 0, "in-place rewrite repaired the poisoned line endings");
#else
        check(bareCrlf == 0, "in-place rewrite repaired the poisoned line endings");
#endif
        check(sortedClaim == 0, "in-place rewrite drops the stale sorted claim");

        // The step the user actually takes: "Synchronize Files" with the force
        // box checked. ctags spinning shows up as the budget below expiring -
        // the progress callback is polled while ctags runs, so the run is
        // aborted instead of hanging the whole self check.
        const ProjectManager::SyncDelta forced = projectManager->computeDelta(/*force=*/true);
        QElapsedTimer budget;
        budget.start();
        QString forcedError;
        bool forcedCancelled = false;
        check(projectManager->synchronize(forced, [&budget](int, int, const QString &) {
                  return budget.elapsed() < 10000;
              }, &forcedError, &forcedCancelled)
                  && !forcedCancelled,
              "forced synchronization completes without ctags spinning");
    }

    // --- cancellation: the progress dialog request stops the run and the tags
    // file is rolled back to its previous state ---
    {
        const QString eps1 = QDir(srcDir).absoluteFilePath(QStringLiteral("eps1.c"));
        const QString eps2 = QDir(srcDir).absoluteFilePath(QStringLiteral("eps2.c"));
        writeFile(eps1, QStringLiteral("int eps1_func(void)\n{\n    return 1;\n}\n"));
        writeFile(eps2, QStringLiteral("int eps2_func(void)\n{\n    return 2;\n}\n"));
        projectManager->addFiles({eps1, eps2});

        const ProjectManager::SyncDelta delta = projectManager->computeDelta(false);
        check(delta.added.size() == 2, "cancel: two files pending");

        int reports = 0;
        bool cancelled = false;
        QString error;
        const bool ok = projectManager->synchronize(delta, [&reports](int, int, const QString &) {
            return ++reports < 3; // cancel once the second file comes up
        }, &error, &cancelled);
        check(!ok && cancelled, "cancel aborts the synchronization");
        check(error.isEmpty(), "cancel is not reported as an error");
        check(projectManager->computeDelta(false).added.size() == 2, "cancel leaves the delta pending");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("eps1_func")) == 0,
              "cancel rolled the tags file back");

        // The dialog wiring: a cancelled dialog returns false from the callback.
        ProjectSyncProgressDialog dialog;
        dialog.show();
        dialog.requestCancel();
        bool dialogCancelled = false;
        int dialogReports = 0;
        const bool dialogOk = projectManager->synchronize(delta, [&dialog, &dialogReports](int step, int total, const QString &) {
            ++dialogReports;
            dialog.setProgress(step, total);
            return !dialog.wasCancelled();
        }, &error, &dialogCancelled);
        dialog.accept();
        check(dialogReports > 0 && !dialogOk && dialogCancelled, "progress dialog cancel aborts the run");

        // The dialog carries a single short explanation line: one sentence that
        // says what the action does, with the "this normally happens
        // automatically..." tail gone.
        {
            SynchronizeFilesDialog syncDialog(projectManager, window);
            QString intro;
            for (QLabel *label : syncDialog.findChildren<QLabel *>()) {
                if (label->text().startsWith(QStringLiteral("This synchronizes")))
                    intro = label->text();
            }
            check(intro == QCoreApplication::translate(
                               "SynchronizeFilesDialog",
                               "This synchronizes the project database with your sources files."),
                  "sync dialog keeps only the short explanation");

            // The forced re-parse box is ticked the moment the dialog opens.
            auto *forceBox = syncDialog.findChild<QCheckBox *>();
            check(forceBox != Q_NULLPTR && forceBox->isChecked(),
                  "sync dialog force re-parse is checked by default");
        }

        // --- the New Project dialog leaves room for the paths it edits ---
        {
            NewProjectDialog newProjectDialog(window);
            newProjectDialog.show();
            newProjectDialog.resize(newProjectDialog.minimumSizeHint());
            QApplication::processEvents();

            QLineEdit *dataField = Q_NULLPTR;
            QLineEdit *sourceField = Q_NULLPTR;
            for (QLineEdit *edit : newProjectDialog.findChildren<QLineEdit *>()) {
                if (edit->text().endsWith(QStringLiteral(".codeinsight")))
                    dataField = edit;
                else if (edit->text() == QDir::homePath())
                    sourceField = edit;
            }
            check(dataField != Q_NULLPTR && sourceField != Q_NULLPTR,
                  "new project dialog exposes both path fields");

            // A path is longer than the value the field starts with, and a field
            // that only just fits that value makes the user scroll inside it to
            // see where it points.
            for (QLineEdit *field : {dataField, sourceField}) {
                if (field == Q_NULLPTR)
                    continue;
                const QFontMetrics pathMetrics(field->font());
                const int twenty = 20 * pathMetrics.averageCharWidth();
                check(field->width() >= pathMetrics.horizontalAdvance(field->text()) + twenty,
                      "new project path field leaves twenty characters of room");
            }
            newProjectDialog.close();
        }

        // --- pressing OK is judged inside the New Project dialog ---
        {
            NewProjectDialog newProjectDialog(window);
            newProjectDialog.show();
            QApplication::processEvents();

            QLineEdit *nameField = newProjectDialog.findChildren<QLineEdit *>().value(0);
            check(nameField != Q_NULLPTR, "new project dialog exposes the name field");
            if (nameField != Q_NULLPTR)
                nameField->setText(QStringLiteral("Taken"));

            int handlerCalls = 0;
            newProjectDialog.setAcceptHandler([&handlerCalls]() {
                ++handlerCalls;
                return QStringLiteral("A project named \"Taken\" already exists.");
            });

            // QMessageBox::warning runs its own event loop: the warning has to be
            // dismissed from a timer, otherwise this check would block forever.
            int warnings = 0;
            auto *dismisser = new QTimer(this);
            dismisser->setInterval(20);
            connect(dismisser, &QTimer::timeout, this, [&warnings, visibleMessageBox]() {
                auto *box = visibleMessageBox();
                if (box == Q_NULLPTR)
                    return;
                ++warnings;
                box->button(QMessageBox::Ok)->click();
            });
            const auto clickOk = [&newProjectDialog]() {
                auto *box = newProjectDialog.findChild<QDialogButtonBox *>();
                if (box != Q_NULLPTR)
                    box->button(QDialogButtonBox::Ok)->click();
            };

            dismisser->start();
            clickOk();
            dismisser->stop();
            dismisser->deleteLater();

            check(handlerCalls == 1, "the OK button asks before creating");
            check(warnings == 1, "a refused creation warns over the dialog");
            check(newProjectDialog.isVisible() && newProjectDialog.result() != QDialog::Accepted,
                  "a refused creation keeps the New Project dialog open");
            check(nameField != Q_NULLPTR && nameField->text() == QStringLiteral("Taken"),
                  "a refused creation keeps what the user typed");

            // The very same dialog closes as soon as the creation is accepted.
            newProjectDialog.setAcceptHandler([]() { return QString(); });
            clickOk();
            check(newProjectDialog.result() == QDialog::Accepted && newProjectDialog.isHidden(),
                  "an accepted creation closes the New Project dialog");
        }

        // A cancelled run must not break the next one.
        bool resyncCancelled = false;
        check(projectManager->synchronizeIncremental(false, ProjectManager::ProgressFn(), &error, &resyncCancelled),
              "synchronize works again after a cancel");
        check(!resyncCancelled, "re-sync was not cancelled");
        check(projectManager->computeDelta(false).empty(), "re-sync cleared the delta");
        check(projectManager->lookupSymbols(QStringLiteral("eps1_func")).size() == 1
                  && projectManager->lookupSymbols(QStringLiteral("eps2_func")).size() == 1,
              "both files have symbols after the re-sync");
    }

    // --- a burst of files is parsed in batched ctags runs ---
    {
        // Starting ctags once per file spends nearly all the time in process
        // creation (~150 ms per file on Windows): 40 files took about 6 s that
        // way and well under a second batched. The elapsed time is printed
        // rather than asserted so the check stays valid on slow machines, while
        // a per-file regression would show up as thousands of milliseconds.
        QString bulkError;
        QStringList bulk;
        for (int i = 0; i < 40; ++i) {
            const QString path = QDir(srcDir).absoluteFilePath(
                QStringLiteral("bulk%1.c").arg(i, 2, 10, QLatin1Char('0')));
            writeFile(path, QStringLiteral("int bulk_%1(void)\n{\n    return %1;\n}\n").arg(i));
            bulk.append(path);
        }
        projectManager->addFiles(bulk);
        const ProjectManager::SyncDelta bulkDelta = projectManager->computeDelta(false);
        check(bulkDelta.added.size() == 40, "bulk: 40 files are pending");

        QElapsedTimer bulkTimer;
        bulkTimer.start();
        const bool bulkOk = projectManager->synchronize(bulkDelta, ProjectManager::ProgressFn(), &bulkError);
        const qint64 bulkElapsed = bulkTimer.elapsed();
        check(bulkOk, "bulk synchronization succeeds");
        if (!bulkOk)
            qInfo("ProjectSelfCheck: bulk error: %s", qUtf8Printable(bulkError));
        qInfo("ProjectSelfCheck: 40 files synced in %lld ms (%d ctags run(s))",
              bulkElapsed, ProjectManager::ctagsBatchCount(bulkDelta.added.size()));
        check(projectManager->lookupSymbols(QStringLiteral("bulk_39")).size() == 1,
              "bulk files are indexed");
    }

    // --- a burst of saved files triggers a single synchronization ---
    {
        // synchronize() rebuilds the cscope database on every call (cscope has
        // no incremental mode), so the background auto-sync folds a burst of
        // changes into one run. Syncing per file rebuilt the database once per
        // saved file: 40 saves used to mean 40 rebuilds.
        QStringList burst;
        for (int i = 0; i < 40; ++i) {
            const QString path = QDir(srcDir).absoluteFilePath(
                QStringLiteral("burst%1.c").arg(i, 2, 10, QLatin1Char('0')));
            writeFile(path, QStringLiteral("int burst_%1(void)\n{\n    return %1;\n}\n").arg(i));
            burst.append(path);
        }
        projectManager->addFiles(burst);

        QString burstError;
        check(projectManager->synchronizeIncremental(false, ProjectManager::ProgressFn(), &burstError),
              "burst files are indexed before the burst");
        const bool settled = waitUntil([this]() {
            return projectManager->computeDelta(false).empty();
        }, 30000);
        check(settled, "burst: nothing pending before the burst");

        // Touch every file: what a "save all" or a big edit session does.
        for (int i = 0; i < burst.size(); ++i) {
            writeFile(burst.at(i), QStringLiteral("int burst_%1(void)\n{\n    return %1 + 1;\n}\n").arg(i));
        }

        int syncSignals = 0;
        const auto conn = connect(projectManager, &ProjectManager::projectSymbolsUpdated,
                                  this, [&syncSignals]() { ++syncSignals; });
        for (const QString &path : burst)
            projectManager->enqueueFileChange(path);

        const bool drained = waitUntil([this, &syncSignals]() {
            // The signal is emitted on the background thread and delivered here
            // through the event loop, so waiting for the delta alone would
            // disconnect before the queued deliveries are processed.
            return syncSignals > 0 && projectManager->computeDelta(false).empty();
        }, 60000);
        QThread::msleep(200); // let any further queued deliveries land
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        disconnect(conn);

        check(drained, "burst: the auto-sync drained");
        check(syncSignals > 0 && syncSignals <= 3,
              "burst: 40 saved files fold into one synchronization");
        qInfo("ProjectSelfCheck: 40 saved files -> %d synchronization(s), %d symbols",
              syncSignals, projectManager->lookupSymbols(QStringLiteral("burst_39")).size());
        check(projectManager->lookupSymbols(QStringLiteral("burst_39")).size() == 1,
              "burst files are re-indexed after the burst");
    }

    // --- joint keyword search in the symbol filter boxes ---
    {
        // Both symbol panels share SymbolFilter: the filter text is split on
        // whitespace and *every* keyword has to occur in the symbol name, so
        // "rec skb" finds recv_vlan_skb() while a single keyword keeps the old
        // substring behaviour. The names below make each property observable
        // (keyword order, case, repeated blanks, a keyword without a match).
        const QString keywordPath = QDir(srcDir).absoluteFilePath(QStringLiteral("recv_keywords.c"));
        writeFile(keywordPath,
                  "int recv_vlan_skb(void)\n{\n    return 1;\n}\n"
                  "int recv_skb(void)\n{\n    return 2;\n}\n"
                  "int send_skb(void)\n{\n    return 3;\n}\n"
                  "int recv_only(void)\n{\n    return 4;\n}\n");

        QStringList keywordFiles;
        keywordFiles.append(keywordPath);
        bool keywordChanged = false;
        projectManager->addFiles(keywordFiles, &keywordChanged);
        const ProjectManager::SyncDelta keywordDelta = projectManager->computeDelta(false);
        check(keywordDelta.added.size() == 1, "keyword test file is pending");
        QString keywordError;
        check(projectManager->synchronize(keywordDelta, ProjectManager::ProgressFn(), &keywordError),
              "keyword test file is indexed");
        if (!keywordError.isEmpty())
            qInfo("ProjectSelfCheck: keyword sync error: %s", qUtf8Printable(keywordError));

        auto shownNames = [](QTreeWidget *tree) {
            QStringList names;
            if (tree) {
                for (int i = 0; i < tree->topLevelItemCount(); ++i)
                    names.append(tree->topLevelItem(i)->text(0));
            }
            names.sort();
            return names;
        };
        // Same thing for the project symbol table, which is model based now:
        // asking the model for a row is how the panel gets one too.
        auto shownSymbolNames = [](QTreeView *tree) {
            QStringList names;
            if (tree && tree->model()) {
                for (int i = 0; i < tree->model()->rowCount(); ++i) {
                    names.append(tree->model()
                                     ->index(i, SymbolTreeModel::SymbolColumn)
                                     .data(Qt::DisplayRole)
                                     .toString());
                }
            }
            names.sort();
            return names;
        };
        const QStringList expectedRecv{QStringLiteral("recv_skb"), QStringLiteral("recv_vlan_skb")};

        // --- Project Symbols tab ---
        auto *symbolFilterEdit = projectPanel->findChild<QLineEdit *>(QStringLiteral("projectSymbolFilter"));
        auto *symbolTree = projectPanel->findChild<QTreeView *>(QStringLiteral("projectSymbolTree"));
        check(symbolFilterEdit != Q_NULLPTR && symbolTree != Q_NULLPTR && symbolTree->model() != Q_NULLPTR,
              "project symbol filter box is reachable");
        if (symbolFilterEdit && symbolTree && symbolTree->model()) {
            // Nothing is hidden any more. The list used to stop after a fixed
            // number of rows and report the rest in a trailing "... and N more
            // symbols" row, which left a real project's symbols unreachable
            // (Linux kernel: ~470k). Every match is a row now and the model
            // hands them out one at a time, so the scrollbar covers the whole
            // result.
            symbolFilterEdit->setText(QStringLiteral("recv"));
            const QStringList recvRows = shownSymbolNames(symbolTree);
            check(recvRows.size() == 3 && recvRows.contains(QStringLiteral("recv_only")),
                  "a filter lists every match, not a capped slice of them");

            // A filter that matches nothing first: this forces the list to be
            // rebuilt, so the assertions below never read a stale one.
            symbolFilterEdit->setText(QStringLiteral("zzz_nomatch"));
            check(symbolTree->model()->rowCount() == 0, "unmatched filter empties the symbol list");

            symbolFilterEdit->clear();
            const int allSymbols = projectManager->symbols().size();
            check(symbolTree->model()->rowCount() == allSymbols,
                  "empty filter lists every project symbol");
            check(!shownSymbolNames(symbolTree).filter(QStringLiteral("more"), Qt::CaseInsensitive).size(),
                  "no row stands in for symbols that did not fit");

            symbolFilterEdit->setText(QStringLiteral("rec skb"));
            const QStringList joint = shownSymbolNames(symbolTree);
            check(joint == expectedRecv, "joint search keeps only symbols holding both keywords");

            symbolFilterEdit->setText(QStringLiteral("skb rec"));
            check(shownSymbolNames(symbolTree) == joint, "joint search ignores the keyword order");

            symbolFilterEdit->setText(QStringLiteral("RECV   skb"));
            check(shownSymbolNames(symbolTree) == joint,
                  "joint search ignores case and repeated blanks");

            symbolFilterEdit->setText(QStringLiteral("send skb"));
            check(shownSymbolNames(symbolTree) == QStringList{QStringLiteral("send_skb")},
                  "joint search works with another keyword pair");

            symbolFilterEdit->setText(QStringLiteral("recv zzz_nomatch"));
            check(symbolTree->model()->rowCount() == 0,
                  "one unmatched keyword hides every entry");

            // A single keyword keeps the plain substring behaviour (the three
            // "recv" matches above), so nothing more to check here.
            symbolFilterEdit->clear();
            check(symbolTree->model()->rowCount() == allSymbols,
                  "clearing the filter restores the full list");
            qInfo("ProjectSelfCheck: joint filter \"rec skb\" -> %s",
                  qUtf8Printable(joint.join(QStringLiteral(", "))));
        }

        // --- function list panel: same matcher, same semantics ---
        window->openFile(keywordPath);
        const bool keywordParsed = waitUntil([this]() {
            return window->currentEditor() != Q_NULLPTR && window->currentEditor()->isFile()
                && ctagsManager->hasParsed(window->currentEditor()->getFilePath());
        }, 20000);
        check(keywordParsed, "keyword test file parsed for the function list");
        if (keywordParsed) {
            EditorPane *pane = EditorPane::paneForEditor(window->currentEditor());
            FunctionListWidget *list = pane ? pane->functionList() : Q_NULLPTR;
            QLineEdit *listFilter = list ? list->findChild<QLineEdit *>(QStringLiteral("functionListFilter")) : Q_NULLPTR;
            QTreeWidget *listTree = list ? list->findChild<QTreeWidget *>() : Q_NULLPTR;
            check(listFilter != Q_NULLPTR && listTree != Q_NULLPTR,
                  "function list filter box is reachable");
            if (listFilter && listTree) {
                listFilter->clear();
                check(listTree->topLevelItemCount() >= 4,
                      "function list shows every symbol of the file");

                listFilter->setText(QStringLiteral("rec skb"));
                check(shownNames(listTree) == expectedRecv,
                      "function list joint search keeps both-keyword symbols");

                listFilter->setText(QStringLiteral("send skb"));
                check(shownNames(listTree) == QStringList{QStringLiteral("send_skb")},
                      "function list joint search works with another keyword pair");

                listFilter->setText(QStringLiteral("recv zzz_nomatch"));
                check(listTree->topLevelItemCount() == 0,
                      "function list joint search filters everything out");

                listFilter->clear();
                check(listTree->topLevelItemCount() >= 4,
                      "clearing the function list filter restores the list");
            }
        }
    }

    // --- jump chooser: the preview pane follows the highlighted row ---
    {
        // The chooser is modal and builds its widgets itself, so it is driven
        // from a timer while it is up: highlight the second row, inspect the
        // preview pane, then close the dialog.
        const QString previewAlpha = QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c"));
        const QString previewGamma = QDir(srcSub).absoluteFilePath(QStringLiteral("gamma.c"));

        QVector<JumpTarget> targets;
        targets.append({QStringLiteral("alpha (alpha.c:3)"), previewAlpha, 3});
        targets.append({QStringLiteral("gamma (gamma.c:3)"), previewGamma, 3});

        struct PreviewProbe
        {
            bool ran = false;
            bool readOnly = false;
            bool caretLine = false;
            int margin = -1;
            int foldMargin = -1;
            int paneWidth = 0;
            int widthForColumns = 0;
            int listHeight = 0;
            int paneHeight = 0;
            int firstLine = -1;
            int secondLine = -1;
            QString firstText;
            QString secondText;
        };
        PreviewProbe probe;

        QTimer::singleShot(200, qApp, [&probe]() {
            QWidget *modal = QApplication::activeModalWidget();
            if (modal == Q_NULLPTR)
                return; // dialog already gone: nothing to inspect
            auto *list = modal->findChild<QListWidget *>();
            auto *preview = modal->findChild<ScintillaNext *>(QStringLiteral("jumpPreview"));
            if (list == Q_NULLPTR || preview == Q_NULLPTR)
                return;

            probe.ran = true;
            probe.readOnly = preview->readOnly();
            probe.caretLine = preview->caretLineVisible() && preview->caretLineVisibleAlways();
            probe.margin = static_cast<int>(preview->marginWidthN(0));
            probe.foldMargin = static_cast<int>(preview->marginWidthN(2));
            probe.paneWidth = preview->width();
            probe.widthForColumns = jumpPreviewWidthForColumns(preview, JumpPreviewColumns);
            probe.listHeight = list->height();
            probe.paneHeight = preview->height();

            const auto caretLine = [preview]() {
                return static_cast<int>(preview->lineFromPosition(preview->currentPos())) + 1;
            };
            const auto paneText = [preview]() {
                return QString::fromUtf8(preview->get_text_range(0, static_cast<int>(preview->length())));
            };

            list->setCurrentRow(0);
            QCoreApplication::processEvents();
            probe.firstLine = caretLine();
            probe.firstText = paneText();

            list->setCurrentRow(1);
            QCoreApplication::processEvents();
            probe.secondLine = caretLine();
            probe.secondText = paneText();
            // Read again after the second file loaded: setting the pane's
            // language must not bring the fold margin back.
            probe.foldMargin = static_cast<int>(preview->marginWidthN(2));

            modal->close();
        });

        const int previewChoice = chooseJumpTarget(window->app, window, QStringLiteral("PreviewSelfCheck"), targets);
        check(previewChoice == -1, "preview chooser: cancel returns -1");
        check(probe.ran, "preview chooser: the pane is part of the dialog");
        check(probe.readOnly, "preview chooser: the pane is read only");
        check(probe.caretLine, "preview chooser: the pane highlights the target line");
        check(probe.margin == 30, "preview chooser: the pane shows line numbers");
        check(probe.foldMargin == 0, "preview chooser: the pane has no fold markers");
        check(probe.paneWidth >= probe.widthForColumns && probe.paneWidth <= probe.widthForColumns + 16,
              "preview chooser: the code area is 100 characters wide");
        check(probe.paneHeight >= 120 && probe.listHeight > 0,
              "preview chooser: the pane gets room under the list");
        check(probe.firstLine == 3 && probe.firstText.contains(QStringLiteral("void alpha")),
              "preview chooser: the highlighted row loads its file and line");
        check(probe.secondLine == 3 && probe.secondText.contains(QStringLiteral("beta(1)")),
              "preview chooser: another row loads the other file and line");

        // The pane on its own: an unreadable file, then a target without a file
        // (a local symbol in an untitled buffer).
        ScintillaNext *pane = createJumpPreview(window, window->app);
        QString loaded;
        showJumpPreview(pane, window->app,
                        JumpTarget{QStringLiteral("missing"),
                                   QDir(srcDir).absoluteFilePath(QStringLiteral("does_not_exist.c")), 1},
                        &loaded);
        const QString missingText = QString::fromUtf8(pane->get_text_range(0, static_cast<int>(pane->length())));
        check(missingText.startsWith(QStringLiteral("// ")) && missingText.contains(QStringLiteral("does_not_exist.c")),
              "preview pane: an unreadable target still names the file");

        showJumpPreview(pane, window->app, JumpTarget{QStringLiteral("untitled"), QString(), 1}, &loaded);
        check(pane->length() == 0, "preview pane: a row without a file clears the pane");

        // Loading switched the Lua extension to the pane; hand it back before
        // the pane goes away.
        if (ScintillaNext *editor = window->currentEditor())
            LuaExtension::Instance().setEditor(editor);
        delete pane;
    }

    // --- closing the project closes the files of the project ---
    {
        // A file from outside the project and an untitled buffer: both survive.
        const QString outsider = QDir(base).absoluteFilePath(QStringLiteral("outside.c"));
        writeFile(outsider, QStringLiteral("int outsider(void)\n{\n    return 1;\n}\n"));
        window->openFile(outsider);
        window->newFile();

        // Created after the file list was built: below the source root but not
        // in the project file list, so only the "below the source root" rule
        // can catch this one.
        const QString loosePath = QDir(srcSub).absoluteFilePath(QStringLiteral("loose.c"));
        writeFile(loosePath, QStringLiteral("int loose(void)\n{\n    return 3;\n}\n"));
        window->openFile(loosePath);

        const QString alphaPath = QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c"));
        const QString gammaPath = QDir(srcSub).absoluteFilePath(QStringLiteral("gamma.c"));
        window->openFile(alphaPath);
        window->openFile(gammaPath);

        QStringList listedPaths;
        const QStringList projectFiles = projectManager->projectFiles();
        for (const QString &file : projectFiles)
            listedPaths.append(QFileInfo(file).canonicalFilePath());
        const QString root = QDir::cleanPath(QDir::fromNativeSeparators(srcDir));

        auto belongsToProject = [&listedPaths, &root](const QString &path) {
            if (listedPaths.contains(QFileInfo(path).canonicalFilePath()))
                return true;
            const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
            return p.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
#else
            return p.startsWith(root + QLatin1Char('/'));
#endif
        };
        auto openProjectFileCount = [this, &belongsToProject]() {
            int count = 0;
            for (ScintillaNext *editor : window->dockedEditor->editors()) {
                if (editor->isFile() && belongsToProject(editor->getFilePath()))
                    ++count;
            }
            return count;
        };
        auto hasFileOpen = [this](const QString &path) {
            const QString want = QFileInfo(path).canonicalFilePath();
            for (ScintillaNext *editor : window->dockedEditor->editors()) {
                if (editor->isFile() && QFileInfo(editor->getFilePath()).canonicalFilePath() == want)
                    return true;
            }
            return false;
        };
        auto untitledCount = [this]() {
            int count = 0;
            for (ScintillaNext *editor : window->dockedEditor->editors()) {
                if (!editor->isFile())
                    ++count;
            }
            return count;
        };

        check(openProjectFileCount() >= 3, "listed and loose project files are open");
        check(hasFileOpen(outsider), "the file outside the project is open");
        const int untitledBefore = untitledCount();

        // "Close Project" from the menu: the files go with it.
        closeProjectRequested();
        QCoreApplication::processEvents();

        check(!projectManager->hasProject(), "close project from the menu entry point");
        check(openProjectFileCount() == 0, "no project file left open after the close");
        check(!hasFileOpen(alphaPath) && !hasFileOpen(gammaPath) && !hasFileOpen(loosePath),
              "the listed files and the loose one are closed");
        check(hasFileOpen(outsider), "the file outside the project stays open");
        check(untitledCount() == untitledBefore, "untitled buffers stay open");
        check(window->editorCount() > 0, "the window keeps at least one document");
    }

    // --- close project ---
    projectManager->closeProject();
    check(!projectPanel->isVisible(), "panel hidden after close");
    check(projectManager->currentProjectFromConfig().isEmpty(), "CurrentProject cleared");

    // --- remove project ---
    check(projectManager->removeProject(QStringLiteral("ProjCheck"), &errorMessage), "remove project");
    check(!QDir(QDir(dataPath).absoluteFilePath(QStringLiteral("ProjCheck.codeinsight"))).exists(), "project folder deleted");

    // --- removing files takes their symbols out of both databases ---
    // The dialog's own buttons, in the order the user presses them: Remove File on
    // one entry, Remove File on the last one, Remove All, and Close (closing is
    // what runs the synchronization - ProjectMainWindow::addRemoveProjectFilesDialog).
    // No removal may leave symbols behind, in the tags file or in the cscope
    // database. Measured before these checks existed, on the linux-vct project (a
    // Remove All with 8796 files in the list): the *.cscope kept its 10.4 MB of
    // cross references for every one of them, the *.ctags still had 1503
    // "!_TAG_*" metadata lines (89 KB) while holding no symbol, and a file removed
    // from the list kept its call sites in the cscope database even after the
    // synchronization that was supposed to rebuild it.
    {
        const QString dropBase = QDir::temp().absoluteFilePath(QStringLiteral("npn_dropcheck"));
        QDir(dropBase).removeRecursively();
        QDir().mkpath(dropBase);
        const QString dropData = QDir(dropBase).absoluteFilePath(QStringLiteral("data"));
        const QString dropSrc = QDir(dropBase).absoluteFilePath(QStringLiteral("src"));
        QDir().mkpath(dropData);
        QDir().mkpath(dropSrc);

        const QString dropA = QDir(dropSrc).absoluteFilePath(QStringLiteral("dropa.c"));
        const QString dropB = QDir(dropSrc).absoluteFilePath(QStringLiteral("dropb.c"));
        // dropa.c calls drop_helper() itself: after the other file is gone, the
        // rebuilt database still has to answer for the file that stayed.
        writeFile(dropA, "int drop_helper(void);\n\nint drop_alpha(void)\n{\n    return drop_helper();\n}\n\nint drop_helper(void)\n{\n    return 1;\n}\n");
        // drop_beta() calls drop_alpha(): the cscope side needs a call site that
        // has to disappear together with the file that holds it.
        writeFile(dropB, "int drop_alpha(void);\n\nint drop_beta(void)\n{\n    return drop_alpha();\n}\n");

        check(projectManager->createProject(QStringLiteral("ProjDrop"), dropData, dropSrc, &errorMessage),
              "remove: the probe project is created");
        projectManager->addFiles({dropA, dropB});
        check(projectManager->projectFiles().size() == 2, "remove: two files in the probe project");
        check(runProjectSynchronize(false), "remove: the probe project synchronizes");

        QString dropError;
        auto callsFromDropB = [this, &dropError]() {
            dropError.clear();
            int found = 0;
            const QVector<ProjectManager::ProjectSymbol> sites =
                projectManager->findCallers(QStringLiteral("drop_alpha"), &dropError);
            for (const ProjectManager::ProjectSymbol &site : sites) {
                if (site.file.endsWith(QStringLiteral("dropb.c")))
                    ++found;
            }
            return found;
        };
        check(callsFromDropB() > 0, "remove: the call site is in the database before the removal");
        check(projectManager->findCallers(QStringLiteral("drop_helper"), &dropError).size() > 0,
              "remove: the remaining file's own call site is in the database before the removal");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("drop_beta")) > 0,
              "remove: the file to drop has symbols before the removal");

        // The real dialog, with real button presses. Buttons are found by their
        // translated text, so this keeps working under --translation.
        ProjectFilesDialog dialog(projectManager, window);
        auto pressButton = [](ProjectFilesDialog *target, const char *text) {
            const QString wanted = QCoreApplication::translate("ProjectFilesDialog", text);
            const QList<QPushButton *> buttons = target->findChildren<QPushButton *>();
            for (QPushButton *button : buttons) {
                if (button->text() == wanted) {
                    // A disabled button ignores click(), which is how the dialog
                    // refuses an action that does not apply.
                    if (!button->isEnabled())
                        return false;
                    button->click();
                    return true;
                }
            }
            return false;
        };

        QListWidget *listed = dialog.findChild<QListWidget *>();
        check(listed != Q_NULLPTR && listed->count() == 2, "remove: the dialog lists both project files");
        if (listed != Q_NULLPTR) {
            for (int row = 0; row < listed->count(); ++row) {
                if (listed->item(row)->text().endsWith(QStringLiteral("dropb.c")))
                    listed->item(row)->setSelected(true);
            }
        }

        check(pressButton(&dialog, "Remove File"), "remove file: the button is found and pressed");
        check(!projectManager->isProjectFile(dropB) && projectManager->projectFiles().size() == 1,
              "remove file: the entry left the project list");

        // The per-file path: the dialog only edits the list, and closing it is
        // what runs the synchronization (ProjectMainWindow::addRemoveProjectFilesDialog).
        // That is the step which has to take the removed file's symbols out.
        check(runProjectSynchronize(false), "remove file: closing the dialog synchronizes the removal");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("drop_beta")) == 0,
              "remove file: its tag entries are gone");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("drop_alpha")) > 0,
              "remove file: the symbols of the remaining file survive");
        check(callsFromDropB() == 0, "remove file: its call sites are gone from the cscope database");
        check(QFileInfo::exists(projectManager->cscopeFilePath()),
              "remove file: the cscope database stays for the rest of the project");
        check(projectManager->findCallers(QStringLiteral("drop_helper"), &dropError).size() > 0,
              "remove file: the rebuilt database still covers the remaining file");
        check(!QFileInfo::exists(projectManager->cscopeFilePath() + QStringLiteral(".new")),
              "remove file: the scratch database was moved into place, not left behind");

        // The same button on the last entry empties the list one file at a time.
        // This is the path where the synchronization still has work to do, so it
        // is the one that proves both databases follow an empty file list.
        if (listed != Q_NULLPTR) {
            for (int row = 0; row < listed->count(); ++row)
                listed->item(row)->setSelected(true);
        }
        check(pressButton(&dialog, "Remove File"), "remove last file: the button is found and pressed");
        check(projectManager->projectFiles().isEmpty(), "remove last file: the project has no files left");
        check(QFileInfo::exists(projectManager->cscopeFilePath())
                  && tagEntries(projectManager->ctagsFilePath(), QStringLiteral("drop_alpha")) > 0,
              "remove last file: the databases are still there before the closing sync");
        int metadataBefore = 0;
        {
            QFile tags(projectManager->ctagsFilePath());
            if (tags.open(QIODevice::ReadOnly | QIODevice::Text)) {
                while (!tags.atEnd()) {
                    if (tags.readLine().startsWith("!_TAG_"))
                        ++metadataBefore;
                }
            }
        }
        // The synchronization rework appends each ctags batch to the project
        // tags file and drops the "!_TAG_*" header block while doing so (one
        // header per batch would accumulate, and a stale "!_TAG_FILE_SORTED"
        // line is what made ctags loop forever on a later --append). So the
        // concatenated file carries no metadata to be dropped.
        check(metadataBefore == 0, "remove last file: the tags file carries no ctags metadata to be dropped");

        check(runProjectSynchronize(false), "remove last file: closing the dialog synchronizes the removal");
        check(projectManager->symbols().isEmpty(), "remove last file: no symbols are left to show");
        check(QFileInfo(projectManager->ctagsFilePath()).size() == 0,
              "remove last file: the tags file is empty, metadata included");
        check(!QFileInfo::exists(projectManager->cscopeFilePath()),
              "remove last file: the cscope database is gone with the last file");
        check(callsFromDropB() == 0 && !dropError.isEmpty(),
              "remove last file: looking up a call site reports the missing database");

        // "Remove All" is the removal with nothing left to rebuild the databases
        // from, so it cleans up at the press instead of waiting for the dialog to
        // be closed - which is the state the user looks at right after pressing.
        projectManager->addFiles({dropA});
        check(runProjectSynchronize(false), "remove all: the project synchronizes again");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("drop_alpha")) > 0
                  && QFileInfo::exists(projectManager->cscopeFilePath()),
              "remove all: the databases are back before the button is pressed");

        // Re-opened the way the user does it: a dialog reflects the file list it
        // is given, down to which of its buttons apply. The one from above was
        // built while the list was empty, so its "Remove All" is disabled and
        // click() would do nothing.
        ProjectFilesDialog reopened(projectManager, window);
        QListWidget *listedAgain = reopened.findChild<QListWidget *>();
        check(listedAgain != Q_NULLPTR && listedAgain->count() == 1,
              "remove all: the re-opened dialog lists the file again");

        check(pressButton(&reopened, "Remove All"), "remove all: the button is found and pressed");
        check(projectManager->projectFiles().isEmpty(), "remove all: the project has no files left");
        check(projectManager->symbols().isEmpty(), "remove all: no symbols are left to show");
        check(!QFileInfo::exists(projectManager->ctagsFilePath()),
              "remove all: the tags file is gone at the press");
        check(!QFileInfo::exists(projectManager->cscopeFilePath()),
              "remove all: the cscope database is gone at the press");
        check(runProjectSynchronize(false), "remove all: closing the dialog leaves it that way");

        // A file added to a project whose databases were dropped has to build them
        // again from nothing - the tags file was missing or empty. A tags file
        // ctags cannot extend is the failure that made a forced synchronization of
        // a large project stop at 1% (see the forced check above), so it is part
        // of this one.
        const QString dropC = QDir(dropSrc).absoluteFilePath(QStringLiteral("dropc.c"));
        writeFile(dropC, "int drop_gamma(void)\n{\n    return 3;\n}\n");
        projectManager->addFiles({dropC});
        check(runProjectSynchronize(false), "remove all: a new file synchronizes");
        check(!projectManager->lookupSymbols(QStringLiteral("drop_gamma")).isEmpty(),
              "remove all: the new file's symbol is parsed");
        check(tagEntries(projectManager->ctagsFilePath(), QStringLiteral("drop_gamma")) > 0,
              "remove all: the tags file is usable again");
        check(QFileInfo::exists(projectManager->cscopeFilePath()),
              "remove all: the cscope database is rebuilt");

        check(projectManager->removeProject(QStringLiteral("ProjDrop"), &errorMessage),
              "remove: the probe project is removed again");
        QDir(dropBase).removeRecursively();
    }

    // --- "Open Project" -> "browse.." ---------------------------------------
    // The reinstalled machine: the global project list lost its entries while
    // the project folder is still on disk. The window that lists projects has to
    // open even with an empty list (there was a message box instead), offer a
    // "browse.." button in its lower left corner, and turn a hand picked
    // <name>.codeinsightprj into a registered, open project. A cancelled or
    // refused pick must leave that window exactly as it is.
    {
        const QString browseName = QStringLiteral("ProjBrowse");
        const QString browseDir = QDir(dataPath).absoluteFilePath(browseName + QStringLiteral(".codeinsight"));
        const QString browsePrj = QDir(browseDir).absoluteFilePath(browseName + QStringLiteral(".codeinsightprj"));
        QDir().mkpath(browseDir);
        writeFile(browsePrj, QStringLiteral("[Project]\nName=ProjBrowse\nSourceRoot=%1\n")
                                 .arg(QDir::cleanPath(QDir::fromNativeSeparators(srcSub))));
        writeFile(QDir(browseDir).absoluteFilePath(browseName + QStringLiteral(".filelist")),
                  QDir::cleanPath(QDir::fromNativeSeparators(QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c"))))
                      + QLatin1Char('\n'));
        check(QFileInfo::exists(browsePrj), "browse: the project file is on disk");
        check(!projectManager->availableProjects().contains(browseName)
                  && !projectManager->hasProject(),
              "browse: the project is unknown and none is open");

        // A file that cannot be a project - the wrong suffix, nothing there at
        // all - is refused before anything is registered.
        const QString notAProject = QDir(srcDir).absoluteFilePath(QStringLiteral("alpha.c"));
        QString identityError;
        check(!ProjectManager::projectIdentityFromFile(notAProject, nullptr, nullptr, &identityError)
                  && !identityError.isEmpty(),
              "browse: a file that is not a project file is refused");
        check(!ProjectManager::projectIdentityFromFile(QDir(base).absoluteFilePath(QStringLiteral("gone.codeinsightprj")),
                                                       nullptr, nullptr, &identityError)
                  && !identityError.isEmpty(),
              "browse: a project file that is not there is refused");
        // The two sides of these comparisons are built by different helpers (a
        // stored, cleaned path versus absoluteFilePath()), so compare the folder
        // that is meant instead of the spelling.
        const auto sameFolder = [](const QString &a, const QString &b) {
            return QDir::cleanPath(QDir::fromNativeSeparators(a))
                       .compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive)
                   == 0;
        };

        QString identityName;
        QString identityDir;
        check(ProjectManager::projectIdentityFromFile(browsePrj, &identityName, &identityDir, &identityError)
                  && identityName == browseName && sameFolder(identityDir, browseDir),
              "browse: the name and the folder come from the file");

        // Where the button lives, and that Remove does not want it: checked on a
        // plain dialog, so a layout mistake fails here instead of in the driven
        // run below, where the geometry is gone by the time it can be looked at.
        {
            ProjectListDialog removeProbe(QStringList{}, QHash<QString, QString>{}, /*removeMode=*/true, nullptr);
            check(removeProbe.browseProjectFileButton() == Q_NULLPTR, "browse: the Remove window has no \"browse..\" button");

            ProjectListDialog openProbe(QStringList{}, QHash<QString, QString>{}, /*removeMode=*/false, nullptr);
            openProbe.show();
            QCoreApplication::processEvents();
            QPushButton *browse = openProbe.browseProjectFileButton();
            auto *buttonBox = openProbe.findChild<QDialogButtonBox *>();
            check(browse != Q_NULLPTR && buttonBox != Q_NULLPTR, "browse: the button and the OK/Cancel row are there");
            check(browse != Q_NULLPTR
                      && browse->text() == QCoreApplication::translate("ProjectListDialog", "Browse.."),
                  "browse: the button text is translatable");
            if (browse != Q_NULLPTR && buttonBox != Q_NULLPTR) {
                check(browse->mapTo(&openProbe, browse->rect().center()).x()
                          < buttonBox->mapTo(&openProbe, buttonBox->rect().center()).x(),
                      "browse: the button sits in the lower left corner");
            }
            openProbe.close();
        }

        // The third outcome, "the user backed out" - closing the current
        // project's files is a place to stop at - has to end the same way as a
        // closed chooser: this window stays, and nothing is said about it,
        // because backing out is not a failure. Driven at the widget, without
        // a modal loop: the chooser and the handler are both injected, so the
        // press returns straight away.
        {
            ProjectListDialog cancelProbe(QStringList{}, QHash<QString, QString>{}, /*removeMode=*/false, nullptr);
            cancelProbe.setProjectFileChooser([](const QString &) {
                return QStringLiteral("C:/nowhere/ProjBrowse.codeinsightprj");
            });
            cancelProbe.setBrowseHandler([](const QString &, QString *) {
                return ProjectListDialog::BrowseOutcome::Cancelled;
            });
            cancelProbe.show();
            QCoreApplication::processEvents();
            QPushButton *button = cancelProbe.browseProjectFileButton();
            check(button != Q_NULLPTR, "browse: the cancel case has a button to press");
            if (button != Q_NULLPTR) {
                button->click();
                QCoreApplication::processEvents();
                check(cancelProbe.isVisible() && cancelProbe.result() != QDialog::Accepted,
                      "browse: a cancelled run leaves the window open and unsaid");
            }
            cancelProbe.close();
        }

        // The window has to be reachable in the first place: with nothing
        // registered the menu entry used to be disabled (and the window used to
        // answer with a message box), which is exactly the state to get out of.
        updateProjectActions();
        check(openProjectAction->isEnabled(), "browse: \"Open Project...\" works with an empty project list");

        struct BrowseProbe {
            QPointer<ProjectListDialog> dialog;
            QString nextPick; // what the injected file chooser returns next
            QString warningText;
            int stage = 0;
            int beats = 0;
            int ticks = 0;
            bool sawWarning = false;
            bool sameDialogThroughout = true;
            bool dialogKeptWhileWarning = false;
            bool dialogKeptAfterCancel = false;
            bool sawWarningAfterCancel = false;
        } probe;

        // Pressing "browse.." blocks on everything it does (a warning runs its
        // own loop), so the button is pressed through the event queue: the
        // driver's slot returns, and the driver keeps ticking inside the modal
        // loop that the press opens. Same reason as the New Project probe.
        const auto clickLater = [](QAbstractButton *button) {
            if (button == Q_NULLPTR)
                return;
            QMetaObject::invokeMethod(button, [button]() { button->click(); }, Qt::QueuedConnection);
        };
        const auto visibleProjectListDialog = []() -> ProjectListDialog * {
            if (auto *dialog = qobject_cast<ProjectListDialog *>(QApplication::activeModalWidget()))
                return dialog;
            const QWidgetList tops = QApplication::topLevelWidgets();
            for (QWidget *w : tops) {
                if (auto *dialog = qobject_cast<ProjectListDialog *>(w); dialog != Q_NULLPTR && dialog->isVisible())
                    return dialog;
            }
            return Q_NULLPTR;
        };

        auto *driver = new QTimer(this);
        driver->setInterval(30);
        connect(driver, &QTimer::timeout, this, [&probe, browsePrj, visibleProjectListDialog, visibleMessageBox, clickLater]() {
            if (++probe.beats % 34 == 0) {
                qInfo("ProjectSelfCheck: browse probe stage=%d dialog=%s warning=%s",
                      probe.stage, probe.dialog != Q_NULLPTR ? "up" : "gone",
                      QApplication::activeModalWidget() != Q_NULLPTR ? "up" : "none");
            }
            if (probe.stage == 0) {
                auto *dialog = visibleProjectListDialog();
                if (dialog == Q_NULLPTR)
                    return;
                probe.dialog = dialog;
                // Point the chooser at a file that is not there: the pick has to
                // be reported, and the window has to stay.
                probe.nextPick = browsePrj + QStringLiteral(".typo");
                dialog->setProjectFileChooser([&probe](const QString &) { return probe.nextPick; });
                ++probe.stage;
                clickLater(dialog->browseProjectFileButton());
                return;
            }
            if (probe.stage == 1) {
                auto *warning = visibleMessageBox();
                if (warning == Q_NULLPTR)
                    return;
                probe.sawWarning = true;
                probe.warningText = warning->text();
                probe.dialogKeptWhileWarning = probe.dialog != Q_NULLPTR && probe.dialog->isVisible();
                ++probe.stage;
                clickLater(warning->button(QMessageBox::Ok));
                return;
            }
            if (probe.stage == 2) {
                auto *dialog = visibleProjectListDialog();
                if (dialog == Q_NULLPTR)
                    return; // would mean the window closed on a refused pick
                probe.sameDialogThroughout = probe.sameDialogThroughout && dialog == probe.dialog.data();
                // Closed the chooser without a pick: window stays, no message.
                probe.nextPick.clear();
                ++probe.stage;
                clickLater(dialog->browseProjectFileButton());
                return;
            }
            if (probe.stage == 3) {
                auto *dialog = visibleProjectListDialog();
                if (dialog == Q_NULLPTR)
                    return;
                probe.sawWarningAfterCancel = visibleMessageBox() != Q_NULLPTR;
                probe.dialogKeptAfterCancel = dialog->isVisible();
                probe.sameDialogThroughout = probe.sameDialogThroughout && dialog == probe.dialog.data();
                // The real pick: this one is supposed to register the project,
                // open it and close the window.
                probe.nextPick = browsePrj;
                ++probe.stage;
                clickLater(dialog->browseProjectFileButton());
            }
        });
        auto *watchdog = new QTimer(this);
        watchdog->setInterval(500);
        connect(watchdog, &QTimer::timeout, this, [&probe, driver, watchdog]() {
            if (++probe.ticks <= 20 || probe.stage >= 4)
                return; // 10 s is plenty; a finished probe needs nothing
            driver->stop();
            watchdog->stop();
            qInfo("ProjectSelfCheck: browse probe gave up at stage %d - closing the modal windows", probe.stage);
            const QWidgetList tops = QApplication::topLevelWidgets();
            for (QWidget *w : tops) {
                if (w->isVisible() && w->isModal())
                    w->close();
            }
        });
        driver->start();
        watchdog->start();
        openProjectAction->trigger(); // blocks in the dialog's event loop
        driver->stop();
        watchdog->stop();
        driver->deleteLater();
        watchdog->deleteLater();

        check(probe.stage >= 4, "browse: the window was driven through a failed, a cancelled and a real pick");
        check(probe.sawWarning, "browse: a refused pick is reported to the user");
        check(probe.dialogKeptWhileWarning, "browse: the window stays open under that message");
        check(probe.warningText
                  == QCoreApplication::translate("ProjectManager", "Project file was not found: %1")
                         .arg(QDir::toNativeSeparators(browsePrj + QStringLiteral(".typo"))),
              "browse: the refusal says which file was not found");
        check(probe.dialogKeptAfterCancel && !probe.sawWarningAfterCancel,
              "browse: closing the chooser without a pick leaves the window alone");
        check(probe.sameDialogThroughout, "browse: every step happened in the same window");
        check(probe.dialog.isNull(), "browse: the window closed after the project was opened");

        check(projectManager->hasProject() && projectManager->currentProjectName() == browseName,
              "browse: the picked project is the open one");
        check(projectManager->availableProjects().contains(browseName)
                  && sameFolder(projectManager->projectPath(browseName), browseDir),
              "browse: the name and its folder were written to the global list");
        check(sameFolder(projectManager->projectPath(browseName), projectManager->projectDir()),
              "browse: the global entry and the open project point at the same folder");
        check(QFileInfo(projectManager->sourceRoot()).canonicalFilePath()
                  == QFileInfo(srcSub).canonicalFilePath(),
              "browse: the source root was read from the project file");
        {
            const QStringList files = projectManager->projectFiles();
            check(files.size() == 1 && QFileInfo(files.value(0)).fileName() == QStringLiteral("alpha.c"),
                  "browse: the file list next to the project file was loaded too");
        }

        // Same name, another folder: the entry that is already there wins, so a
        // project that is only registered once cannot be hidden by a second one.
        const QString clashDir = QDir(base).absoluteFilePath(QStringLiteral("elsewhere"));
        QDir().mkpath(clashDir + QLatin1Char('/') + browseName + QStringLiteral(".codeinsight"));
        const QString clashPrj = QDir(clashDir).absoluteFilePath(browseName + QStringLiteral(".codeinsight/")
                                                                 + browseName + QStringLiteral(".codeinsightprj"));
        writeFile(clashPrj, QStringLiteral("[Project]\nName=ProjBrowse\nSourceRoot=%1\n")
                                .arg(QDir::cleanPath(QDir::fromNativeSeparators(srcDir))));
        QString clashError;
        check(!projectManager->registerProject(clashPrj, nullptr, &clashError) && !clashError.isEmpty(),
              "browse: the same name in another folder is refused with a message");
        check(sameFolder(projectManager->projectPath(browseName), browseDir),
              "browse: the refusal left the registered folder where it was");
        QString identityClashName;
        check(ProjectManager::projectIdentityFromFile(clashPrj, &identityClashName, nullptr, nullptr)
                  && identityClashName == browseName,
              "browse: the refused file was a valid project file - the clash was the reason");

        check(projectManager->removeProject(browseName, &errorMessage),
              "browse: the probe project is removed again");
        check(!projectManager->availableProjects().contains(browseName)
                  && !projectManager->hasProject(),
              "browse: nothing of it is left in the global list");
    }

    // ---- jump history: one head index, ring walk, stale entries dropped ----
    // Exercised mostly on the ring itself: recordJump()/pushJumpEntry() and the
    // index movers touch no editor, so the semantics can be checked without a
    // real jump and without a project. The last two sections drive real files,
    // because a walk has to prove it opens what it lands on.
    {
        clearJumpHistory();
        check(jumpHistoryHead == 0 && !jumpBackAction->isEnabled() && !jumpForwardAction->isEnabled(),
              "jump history: starts empty with both toolbar buttons disabled");

        check(jumpBackAction && jumpForwardAction
                  && jumpBackAction->objectName() == QStringLiteral("actionJumpBack")
                  && jumpForwardAction->objectName() == QStringLiteral("actionJumpForward"),
              "jump history: the back/forward toolbar buttons exist");
        {
            const QList<QAction *> actions = window->ui->mainToolBar->actions();
            const int back = actions.indexOf(jumpBackAction);
            const int forward = actions.indexOf(jumpForwardAction);
            const int jump = actions.indexOf(jumpToSymbolAction);
            check(back >= 0 && forward == back + 1 && jump == forward + 1,
                  "jump history: back/forward sit directly left of the jump button");
        }

        // Two real files for the sections that have to observe a navigation,
        // plus a path that is guaranteed not to exist.
        const QString navA = QDir(base).absoluteFilePath(QStringLiteral("nav_a.c"));
        const QString navB = QDir(base).absoluteFilePath(QStringLiteral("nav_b.c"));
        const QString goneFile = QDir(base).absoluteFilePath(QStringLiteral("nav_gone.c"));
        writeFile(navA, QStringLiteral("int nav_a;\n"));
        writeFile(navB, QStringLiteral("int nav_b;\n"));
        QFile::remove(goneFile);

        // The entries carry the same key the ring stores, so the comparisons
        // below stay literal. Most of them point at files that do not exist on
        // purpose: recording them is pure ring arithmetic.
        const JumpHistoryEntry a{jumpEntryKey(QStringLiteral("/tmp/a.c")), 10};
        const JumpHistoryEntry b{jumpEntryKey(QStringLiteral("/tmp/b.c")), 20};
        const JumpHistoryEntry c{jumpEntryKey(QStringLiteral("/tmp/c.c")), 30};

        // First jump: both ends are recorded, the head lands on the destination
        // and the source sits one slot towards the older side.
        recordJump(a, b);
        check(jumpHistoryRing[jumpHistoryHead].line == b.line,
              "jump history: a first jump puts the head on the destination");
        check(jumpHistoryRing[(jumpHistoryHead + 1) % JumpHistoryCapacity].line == a.line,
              "jump history: the source sits one slot towards the older side");
        check(jumpBackAction->isEnabled() && !jumpForwardAction->isEnabled(),
              "jump history: back is available, forward is not");

        // Consecutive jump: its source is the head, so only the destination is
        // written - no duplicate of the previous landing is left behind.
        recordJump(b, c);
        check(jumpHistoryRing[jumpHistoryHead].line == c.line
                  && jumpHistoryRing[(jumpHistoryHead + 1) % JumpHistoryCapacity].line == b.line,
              "jump history: a consecutive jump does not duplicate the source");

        // A second report of the very same jump must leave nothing behind. It
        // happens for real: a symbol list is wired to clicked *and*
        // doubleClicked, so a double click reports the same jump twice and the
        // second report arrives with source == destination == the head. Storing
        // it puts an entry in the ring that only repeats the current position,
        // and back then has to be pressed twice before anything moves.
        {
            const int before = jumpHistoryHead;
            recordJump(jumpHistoryRing[jumpHistoryHead], jumpHistoryRing[jumpHistoryHead]);
            check(jumpHistoryHead == before,
                  "jump history: a repeated report of the same jump adds no record");

            // Clicking the symbol the caret already sits on: the jump goes
            // nowhere, so nothing is recorded.
            const JumpHistoryEntry inPlace{jumpEntryKey(QStringLiteral("/tmp/g.c")), 70};
            recordJump(inPlace, inPlace);
            check(jumpHistoryHead == before,
                  "jump history: a jump that ends where it started adds no record");

            check(moveJumpHeadBack() && jumpHistoryRing[jumpHistoryHead].line == b.line,
                  "jump history: one back press reaches the position before the jump");
            moveJumpHeadForward();
        }

        // A jump that lands where the head already points: only the source is
        // new, and a second copy of the landing would cost one fruitless press.
        {
            const JumpHistoryEntry e{jumpEntryKey(QStringLiteral("/tmp/e.c")), 50};
            recordJump(e, c);
            check(jumpHistoryRing[jumpHistoryHead].line == e.line,
                  "jump history: a landing that equals the head keeps only the source");
            check(moveJumpHeadBack() && jumpHistoryRing[jumpHistoryHead].line == c.line,
                  "jump history: back from such a jump returns to the landing");
        }

        // The same position arrives in two spellings - the project tables
        // normalize to "D:/x/y.c", ScintillaNext::getFilePath() returns the
        // native "D:\x\y.c" - and the dedup only works when both collapse to
        // one key. This is the bug that made the first back press do nothing.
        check(jumpEntryKey(QStringLiteral("D:/tmp/jumpcheck/x.c"))
                  == jumpEntryKey(QDir::toNativeSeparators(QStringLiteral("D:/tmp/jumpcheck/x.c"))),
              "jump history: one file spelled two ways is one entry key");
        {
            clearJumpHistory();
            recordJump(a, b);
            recordJump(b, c);
            const JumpHistoryEntry nativeSource{QDir::toNativeSeparators(c.file), c.line};
            recordJump(nativeSource, JumpHistoryEntry{jumpEntryKey(QStringLiteral("/tmp/f.c")), 60});
            check(jumpHistoryRing[jumpHistoryHead].line == 60,
                  "jump history: a source spelled the native way is still the head");
            check(jumpHistoryRing[(jumpHistoryHead + 1) % JumpHistoryCapacity].line == c.line,
                  "jump history: that source was not recorded a second time");
        }

        // Back/forward only move the head - they never add a record (req 3/4),
        // and they stop as soon as the next slot was never written to.
        {
            clearJumpHistory();
            recordJump(a, b);
            recordJump(b, c);
            check(moveJumpHeadBack() && jumpHistoryRing[jumpHistoryHead].line == b.line,
                  "jump history: back walks to the previous destination");
            check(moveJumpHeadBack() && jumpHistoryRing[jumpHistoryHead].line == a.line,
                  "jump history: back again reaches the first source");
            check(!moveJumpHeadBack(), "jump history: back stops at an empty slot");
            check(moveJumpHeadForward() && jumpHistoryRing[jumpHistoryHead].line == b.line,
                  "jump history: forward walks to the newer entry");
            check(moveJumpHeadForward() && jumpHistoryRing[jumpHistoryHead].line == c.line,
                  "jump history: forward reaches the newest entry again");
            check(!moveJumpHeadForward(), "jump history: forward stops at an empty slot");
            check(jumpHistoryRing[(jumpHistoryHead + 1) % JumpHistoryCapacity].line == b.line,
                  "jump history: the whole walk left the recorded entries alone");
            check(!jumpForwardAction->isEnabled() && jumpBackAction->isEnabled(),
                  "jump history: standing on the newest entry only back is available");
        }

        // A new jump after walking back writes over the slot the entries ahead
        // lived in, so only the source of the new jump is left behind.
        {
            clearJumpHistory();
            recordJump(a, b);
            recordJump(b, c);
            moveJumpHeadBack();
            moveJumpHeadBack();
            const JumpHistoryEntry d{jumpEntryKey(QStringLiteral("/tmp/d.c")), 40};
            recordJump(a, d);
            check(jumpHistoryRing[jumpHistoryHead].line == d.line,
                  "jump history: the new destination is the head");
            check(moveJumpHeadBack() && jumpHistoryRing[jumpHistoryHead].line == a.line,
                  "jump history: the source of the new jump is the only thing behind it");
        }

        // Ring capacity: 40 slots. Pushing past it overwrites the oldest entry,
        // and once every slot is used the index is free to keep cycling - the
        // walk is only stopped by an empty slot, and there is none left.
        {
            clearJumpHistory();
            for (int i = 0; i < JumpHistoryCapacity + 5; ++i)
                pushJumpEntry(JumpHistoryEntry{QStringLiteral("/tmp/n%1.c").arg(i), i + 1});
            check(jumpHistoryRing[jumpHistoryHead].line == JumpHistoryCapacity + 5,
                  "jump history: the head is the newest entry");
            const int newest = jumpHistoryHead;
            for (int i = 0; i < JumpHistoryCapacity; ++i)
                moveJumpHeadBack();
            check(jumpHistoryHead == newest,
                  "jump history: a full ring keeps cycling - the index comes back around");
            for (int i = 0; i < JumpHistoryCapacity - 1; ++i)
                moveJumpHeadBack();
            check(jumpHistoryRing[jumpHistoryHead].line == 6,
                  "jump history: 39 steps back is the oldest entry that survived");
        }

        // A recorded file deleted behind our back: the walk strikes the entry out
        // and carries on in the same direction until it lands on something that
        // really opens. The hole stays a hole (the accepted simplification).
        {
            clearJumpHistory();
            pushJumpEntry(JumpHistoryEntry{navA, 1});   // oldest of the three
            pushJumpEntry(JumpHistoryEntry{goneFile, 2}); // stale target in the middle
            pushJumpEntry(JumpHistoryEntry{navB, 3});   // head

            const int staleSlot = (jumpHistoryHead + 1) % JumpHistoryCapacity;
            check(!jumpHistoryRing[staleSlot].file.isEmpty(),
                  "jump history: the stale entry is in the ring before the walk");

            goBackJump();
            check(jumpHistoryRing[staleSlot].file.isEmpty(),
                  "jump history: an entry whose file is gone is struck out");
            check(jumpHistoryRing[jumpHistoryHead].file == jumpEntryKey(navA),
                  "jump history: the walk carried on to the next entry that exists");
            check(window->currentEditor()
                      && QFileInfo(window->currentEditor()->getFilePath()).canonicalFilePath()
                              == QFileInfo(navA).canonicalFilePath(),
                  "jump history: the carried-on walk really opened that file");
            check(!jumpBackAction->isEnabled(),
                  "jump history: past the end of the records the arrow goes grey");
        }

        // Back/forward really navigate: they open the recorded file and switch to
        // it (requirement 3), using two real files so the editor switch is
        // observable. The head is the only thing that moves.
        {
            clearJumpHistory();
            check(jumpToProjectSymbol(navA, 1), "jump history: the navigation file opens");
            const QString navACanonical = QFileInfo(navA).canonicalFilePath();
            check(window->currentEditor() && QFileInfo(window->currentEditor()->getFilePath()).canonicalFilePath() == navACanonical,
                  "jump history: the navigation file is the current one");

            recordJump(jumpPositionFor(window->currentEditor()), JumpHistoryEntry{navB, 1});
            const int landing = jumpHistoryHead;
            goBackJump();
            check(window->currentEditor() && QFileInfo(window->currentEditor()->getFilePath()).canonicalFilePath() == navACanonical,
                  "jump history: back really switches to the recorded file");
            check(jumpHistoryHead == (landing + 1) % JumpHistoryCapacity,
                  "jump history: the walk back moved the head by exactly one slot");
            goForwardJump();
            check(window->currentEditor()
                      && QFileInfo(window->currentEditor()->getFilePath()).canonicalFilePath()
                              == QFileInfo(navB).canonicalFilePath(),
                  "jump history: forward really switches to the recorded file");
            check(jumpHistoryHead == landing,
                  "jump history: the walk back and forward left the head where it was");
        }

        clearJumpHistory();
        check(jumpHistoryHead == 0 && !jumpBackAction->isEnabled() && !jumpForwardAction->isEnabled(),
              "jump history: clearing resets the head and the buttons");
    }

    QDir(base).removeRecursively();

    if (failures.isEmpty()) {
        qInfo("ProjectSelfCheck: ALL PASSED");
    } else {
        qInfo("ProjectSelfCheck: %d FAILURE(S)", failures.size());
    }
    QTimer::singleShot(300, qApp, &QCoreApplication::quit);
}

// Language names have to survive the differences in spelling between the two
// projects ("Objective-C" vs "ObjectiveC", "ADA" vs "Ada"), so case and
// separators are removed before comparing. '+' and '#' are deliberately kept:
// without them C, C++ and C# would all collapse into the same key.
static QString normalizeLanguageName(QString name)
{
    name = name.toLower();

    static const QString separators = QStringLiteral(" \t.-_/()[]");
    for (const QChar separator : separators)
        name.remove(separator);

    return name;
}

QString ProjectMainWindow::ctagsLanguageFor(const QString &languageName)
{
    if (languageName.isEmpty())
        return QString();

    const QString wanted = normalizeLanguageName(languageName);
    if (wanted.isEmpty())
        return QString();

    // The mapping is derived from ctags itself rather than being hand written, so
    // it covers every language the installed build understands and stays correct
    // across ctags upgrades.
    //
    // A name without a match returns an empty string, which makes the worker
    // leave the language to ctags' own file name detection. That still covers
    // every language ctags knows, so a missing entry only costs the cross-check
    // (which is what corrects extensions like ".v", taken for V rather than
    // Verilog) and never the feature itself.
    const QStringList known = CtagsSymbolManager::knownLanguages();
    for (const QString &candidate : known) {
        if (normalizeLanguageName(candidate) == wanted)
            return candidate;
    }

    return QString();
}

