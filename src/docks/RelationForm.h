/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * RelationForm: one "relation form" of the relation panel, a window of its own
 * next to the Context view. It shows the relation tree of one root symbol:
 *
 *   [ Relation References                                    X ]
 *   [ Name            Lines of Code   Title                    ]
 *   [ v root symbol   12              file.c (src)             ]
 *     [ > caller       34             other.c (src)            ]
 *   [ refresh  lock                           (Called|Call)  New  ]
 *
 * The root row is the symbol the view was pointed at; expanding a row lists
 * the next level of the relation tree - in "Called" mode the symbols that
 * call it, in "Call" mode the symbols it calls (the capsule in the footer
 * switches between the two). Clicking a row hands the symbol to the Context view.
 *
 * Like the Context view, the form is not laid out by the page it lives in:
 * RelationPanel lines the forms up side by side and owns their geometry.
 * The form only reports what its chrome is doing - a width drag on its left
 * or right rim, a move drag on its header - and the panel does the
 * bookkeeping (a width change is compensated by the neighbouring form, a
 * dropped window is inserted at the boundary it was dropped on).
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef RELATIONFORM_H
#define RELATIONFORM_H

#include <QWidget>

class QLabel;
class QMouseEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QToolButton;
class ProjectManager;
class ContextPanel;
class QTimer;

class RelationForm : public QWidget
{
    Q_OBJECT

public:
    explicit RelationForm(ProjectManager *projectManager, QWidget *parent = nullptr);

    // The width the first forms are laid out with when the page knows its size.
    static constexpr int DefaultWidth = 420;

    // Make name the root row (row 1) and refresh its next level right away:
    // that is what the refresh flow needs - the tree is one click deep without
    // the user asking for it. filePath/line belong to the symbol itself and
    // are what a click on the root row hands to the Context view. The same
    // root in the same mode is a no-op (the caret fires this on every settle),
    // which is what force skips - the refresh button means it.
    void setRootSymbol(const QString &name, const QString &filePath = QString(), int line = 0, bool force = false);

    // While locked (the default) the form does not follow the caret of the
    // main editor; the refresh button works either way.
    bool isLocked() const;

    // "Called" (who calls the symbol) is the default; "Call" is the flip side.
    bool isCalledMode() const { return calledMode; }

    // ---- restoring a saved layout ----
    // The lock is set without announcing it: whoever restores a saved layout
    // knows the state already, and the caret need not chase it.
    void setLocked(bool locked);
    // Puts the tree into the given mode: flipping rebuilds the first level
    // from the root (a form without a root only flips the switch).
    void setCalledMode(bool called);
    // The widths of the three tree columns, and their restoration: the set
    // widths are treated as the user's own, so the last column goes on
    // absorbing what the tree's width leaves over, exactly as after a drag.
    QList<int> columnWidths() const;
    void setColumnWidths(const QList<int> &widths);

    // The tree, for the regression self check.
    QTreeWidget *tree() const { return relationTree; }

signals:
    // A row was clicked: the symbol it stands for is to be shown in the
    // Context view.
    void symbolActivated(const QString &filePath, int line);
    // A row was double-clicked: the symbol's file is to be opened in the
    // editor and the line placed in the upper middle of the view. A double
    // click never expands the row.
    void symbolOpenRequested(const QString &filePath, int line);
    // The X in the header: the form is to be removed from the panel.
    void closeRequested();
    // The New button: a new form is to be inserted to the right of this one.
    void newRequested();
    // The refresh button: the root is to be taken from the editor's caret.
    void refreshRequested();
    void lockChanged(bool locked);
    // The mode capsule was switched (the tree was rebuilt from the root when
    // there was one): whoever keeps the saved layout notes it down.
    void modeChanged(bool calledMode);
    // A boundary between two headers was dragged by the user: the column
    // widths are the user's from then on.
    void columnWidthsChanged();

    // ---- chrome drags, answered by RelationPanel ----
    // A width drag took hold of the left or the right rim of the window.
    void widthDragStarted(bool leftEdge);
    // The pointer moved: total horizontal distance from where the drag began.
    void widthDragMoved(int delta);
    // The header is dragged the window around by.
    void moveDragStarted();
    // The pointer moved: its global x, which decides the insertion boundary.
    void moveDragMoved(int globalX);
    // The pointer came back up: whatever the drag was, it is over.
    void dragEnded();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    // The header is watched so its drag starts the same way wherever on the
    // strip it began (the labels on it stay out of the way of the mouse).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildHeader();
    void buildTree();
    void buildFooter();
    // The rows that expand the given item one level down, from the cscope
    // database and by the mode the form is in. An item that has been filled
    // once is never queried again.
    void populateChildren(QTreeWidgetItem *item);
    // What the mode segments are lit by, and what their tooltips say.
    void updateModeButton();
    void updateLockButton();
    // Spreads the three columns across the tree's viewport: an even share for
    // Name and Title with the line column taking what its header says, until
    // the user drags a boundary between two headers - from then on the user's
    // widths are kept and only the last column absorbs what is left.
    void adjustColumns();
    // The last column is resized to whatever the viewport has left over from
    // the first two. The caller owns the adjustingColumns guard.
    void fillLastColumn();

    ProjectManager *project = nullptr;

    QWidget *header = nullptr;
    QLabel *headerIcon = nullptr;
    QLabel *headerText = nullptr;
    QToolButton *closeButton = nullptr;

    QTreeWidget *relationTree = nullptr;

    QWidget *footer = nullptr;
    QToolButton *refreshButton = nullptr;
    QToolButton *lockButton = nullptr;
    // The two segments of the mode capsule: the checked one is the mode the
    // tree is in, clicking the other one switches.
    QToolButton *calledButton = nullptr;
    QToolButton *callButton = nullptr;
    QToolButton *newButton = nullptr;

    bool calledMode = true;

    // Chrome drag state: which rim a width drag took (and where it began), or
    // where the header drag began. -1 means no drag is going on.
    bool widthDragLeft = false;
    int widthDragStartX = -1;
    int moveDragStartX = -1;

    // What row 1 stands for, so a mode flip can rebuild the tree from it.
    QString rootName;
    QString rootFile;
    int rootLine = 0;
    // The mode the current tree was built in: the same root only needs a
    // rebuild when the question about it changed.
    bool rootBuiltCalled = true;
    bool rootBuilt = false;

    // Column state: false while the columns follow the tree's width on their
    // own, true from the first boundary the user dragged. adjustingColumns
    // marks the widths this file sets itself, so they are not mistaken for a
    // user drag.
    bool userAdjustedColumns = false;
    bool adjustingColumns = false;
};

// RelationPanel: the relation panel page itself - the row of windows (the
// Context view first, then the relation forms) that together fill the page,
// and all the bookkeeping around that row. This is the body the host dock
// hands the page over to; the dock stays a thin shell around it.
//
// The row is not laid out by a layout: the panel hands out the geometry
// itself, because the width of one window is the neighbour's business and the
// row has to stay exactly as wide as the page however it is dragged. It
// answers the chrome drags of every window (the neighbour compensates a width
// drag, a dropped window is inserted at the boundary it was dropped on), it
// builds the default row the first time the page has a size to place it in,
// and it writes the layout into the settings ([RelationPanel], on a debounce)
// whenever it changes - a saved layout comes back instead of the default row
// on the next start.
//
// The windows remain children of the page (they sit on it directly), so the
// page is still what a window's parentWidget() says - the panel is the
// controller behind them, not a widget in between.
class RelationPanel : public QObject
{
    Q_OBJECT

public:
    // The panel manages the page it is given; the windows it builds are
    // children of that page.
    explicit RelationPanel(QWidget *page);

    // The Context view of the row: the leftmost window of it.
    ContextPanel *contextPanel() const { return context; }

    // Give the row its ProjectManager: the relation forms ask it for the
    // relation trees. Calling this arms the default layout - two forms to the
    // right of the Context view - which is built as soon as the page knows
    // its size.
    void init(ProjectManager *projectManager);

    // The relation forms, left to right after the Context view - the order of
    // the row itself, which a move drag may have rearranged.
    QList<RelationForm *> forms() const;

    // Writes the current layout of the row into the settings ([RelationPanel]):
    // how many windows the row has, how wide each one is, the lock of the
    // Context view, and for every form its lock, its mode and the dragged
    // widths of its tree columns. Called on the debounce timer whenever the
    // layout changes, once more when the host goes away - and by the
    // regression self check.
    void saveLayout();

    // The unlocked relation forms take name as their root symbol (their lock
    // is their own business). This is what ContextTracker::symbolAtCaret
    // feeds.
    void updateUnlockedRoots(const QString &name, const QString &filePath, int line);

signals:
    // The refresh button of a relation form was clicked: the form wants the
    // symbol under the caret of the active editor as its root. Whoever owns
    // the editor answers (see ProjectMainWindow).
    void relationFormRefreshRequested(RelationForm *form);
    // A symbol row of a relation form was double-clicked: the file is to be
    // opened in the editor with the line in the upper middle of the view.
    // Whoever owns the editor answers (see ProjectMainWindow).
    void relationFormOpenRequested(const QString &filePath, int line);

protected:
    // The page is watched: its first resize builds the row, every later one
    // lays it out again.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // The room the page gives the row, in page coordinates (a small gap at
    // the top keeps the windows off the tab names above them).
    QRect relationArea() const;

    // Build the default row - the Context view plus two relation forms - the
    // first time the page has a size to place it in. A layout saved earlier
    // ([RelationPanel] in the settings) replaces the default: the saved
    // window count, widths, column widths, locks and modes come back.
    void ensureLayout();
    // One more relation form, wired up, inserted at the given page index.
    RelationForm *createRelationForm(int index);
    // The drag and button answers of one form, all of them bookkeeping of the
    // row.
    void wireRelationForm(RelationForm *form);
    // Put every window of the row at its x position: x is the running sum of
    // the widths, the height is always the full page.
    void relayoutRelationPage();
    // Even the row out to exactly the page width: shares that fell below a
    // minimum width, or plain rounding, are squeezed or stretched away.
    void normalizeRowToArea();
    // A width drag on window: the neighbour on the dragged side compensates,
    // so the row stays exactly as wide as the page.
    void applyWidthDrag(QWidget *window, bool leftEdge, int delta);
    // A dropped window: the row is reordered so the window sits between the
    // windows whose boundary it was dropped on, and the row is laid out again.
    void finishMoveDrag();
    // New form to the right of the given one, the space to the right shared
    // equally among the windows right of it.
    void addRelationFormAfter(QWidget *window);
    // Take a form out of the row, its width shared out over the rest.
    void removeRelationForm(RelationForm *form);
    // Notes the layout down a moment after it changed: drags and resizes
    // change it dozens of times a second, the settings get one write.
    void scheduleSave();

    // The page the row lives on (and the host of every window of the row).
    QWidget *panelPage = nullptr;
    ContextPanel *context = nullptr;

    // The relation panel row: the windows left to right, the relation forms
    // among them, and whether the row has been built yet.
    QList<QWidget *> pageWindows;
    QList<RelationForm *> rowForms;
    ProjectManager *relationProject = nullptr;
    bool relationLayoutReady = false;
    int lastPageWidth = 0;
    // The one-shot timer between a layout change and its write into the
    // settings (see scheduleSave).
    QTimer *saveTimer = nullptr;
    // Whether the layout changed since the settings were last written: the
    // write on the host's way out happens only for a layout that actually
    // changed - a freshly restored one is what the settings already carry.
    bool layoutDirty = false;

    // The width drag in flight: which window is dragged, over which of its
    // rims, and the widths the row had when the drag began.
    QWidget *widthDragWindow = nullptr;
    bool widthDragLeftEdge = false;
    QList<int> widthDragStartWidths;
    // The move drag in flight: which window is dragged, where it started, and
    // the pointer x the movement is measured from (-1 until the first move).
    QWidget *moveDragWindow = nullptr;
    int moveDragStartX = -1;
    int moveDragStartWindowX = 0;
};

#endif // RELATIONFORM_H
