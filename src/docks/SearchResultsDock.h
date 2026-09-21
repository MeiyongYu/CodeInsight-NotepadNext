/*
 * This file is part of Notepad Next.
 * Copyright 2022 Justin Dailey
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


#ifndef SEARCHRESULTSDOCK_H
#define SEARCHRESULTSDOCK_H

#include <QDockWidget>

#include "ISearchResultsHandler.h"

namespace Ui {
class SearchResultsDock;
}

class QTreeWidgetItem;
class ScintillaNext;
class ContextPanel;
class DockTabTitleBar;
class ProjectManager;
class RelationForm;
class RelationPanel;

// The dock shows two views that share the same panel, picked from the tab
// buttons in its title bar: the search results tree (the original content)
// and the relation panel. See SearchResultsDock.ui for the page stack and
// DockTabTitleBar for the title bar itself.
//
// The relation panel page holds a row of windows - the Context view first,
// then the relation forms - that together fill the page. Everything about
// that row is RelationPanel's business (see RelationForm.h): its geometry,
// its drags, its build-up and the layout it saves and restores. This dock is
// only the shell around it - it hands the page over and relays the requests
// that reach the main window.
class SearchResultsDock : public QDockWidget, public ISearchResultsHandler
{
    Q_OBJECT

public:
    // Pages of the stack, in title bar tab order.
    enum PanelPage {
        SearchResultsPage = 0,
        RelationPanelPage = 1
    };

    explicit SearchResultsDock(QWidget *parent = nullptr);
    ~SearchResultsDock();

    // The Context view of the relation panel page: a window of its own inside
    // that page, the leftmost one of the row the page is filled with.
    ContextPanel *contextPanel() const;

    // Give the relation panel its ProjectManager: the relation forms ask it
    // for the relation trees. Calling this arms the default layout - two
    // forms to the right of the Context view - which is built as soon as the
    // page knows its size.
    void initRelationPanel(ProjectManager *projectManager);

    // The relation forms, left to right after the Context view - the order of
    // the row itself, which a move drag may have rearranged.
    QList<RelationForm *> relationForms() const;

    // Writes the current layout of the relation panel into the settings
    // ([RelationPanel]): how many windows the row has, how wide each one is,
    // the lock of the Context view, and for every form its lock, its mode and
    // the dragged widths of its tree columns. Called on a timer whenever the
    // layout changes, once more when the dock goes away - and by the
    // regression self check.
    void saveRelationPanelLayout();

    // The unlocked relation forms take name as their root symbol (their lock
    // is their own business). This is what ContextTracker::symbolAtCaret
    // feeds.
    void updateUnlockedRelationRoots(const QString &name, const QString &filePath, int line);

    // Bring the relation panel to the front, opening the dock if it was
    // closed. Opening a project does this: the relation panel is where its
    // symbols show up.
    void showRelationPanel();

    void newSearch(const QString searchTerm) override;
    void newFileEntry(ScintillaNext *editor, const QString &filePath = QString()) override;
    void newResultsEntry(const QString line, int lineNumber, int startPositionFromBeginning, int endPositionFromBeginning, int hitCount=1) override;
    void completeSearch() override;

public slots:
    void collapseAll() const;
    void expandAll() const;
    void deleteEntry(QTreeWidgetItem *item);
    void deleteAll();

private slots:
    void itemActivated(QTreeWidgetItem *item, int column);
    void itemExpanded(QTreeWidgetItem *item);
    void copyAllSearchResultsToClipboard();
    void copySelectedSearchResultsToClipboard();


signals:
    // filePath always identifies the file; editor is null when that file was
    // searched without being open in an editor (project wide search), in which
    // case the receiver has to open it before jumping to the hit.
    void searchResultActivated(ScintillaNext *editor, const QString &filePath, int lineNumber, int startPositionFromBeginning, int endPositionFromBeginning);
    // The refresh button of a relation form was clicked: the form wants the
    // symbol under the caret of the active editor as its root. Whoever owns
    // the editor answers (see ProjectMainWindow). Relayed from RelationPanel.
    void relationFormRefreshRequested(RelationForm *form);
    // A symbol row of a relation form was double-clicked: the file is to be
    // opened in the editor with the line in the upper middle of the view.
    // Whoever owns the editor answers (see ProjectMainWindow). Relayed from
    // RelationPanel.
    void relationFormOpenRequested(const QString &filePath, int line);

private:
    void updateSearchStatus();

    Ui::SearchResultsDock *ui;
    DockTabTitleBar *titleBar = Q_NULLPTR;
    // The relation panel page's body: the row of windows and all of its
    // bookkeeping (see RelationForm.h). The dock only delegates to it.
    RelationPanel *relationPanel = Q_NULLPTR;

    QString searchTerm;
    QString currentFilePath;

    int currentFileCount = 0;
    int totalFileHitCount = 0;
    int totalHitCount = 0;

    QTreeWidgetItem *currentSearch = Q_NULLPTR;
    QTreeWidgetItem *currentFile = Q_NULLPTR;
};

#endif // SEARCHRESULTSDOCK_H
