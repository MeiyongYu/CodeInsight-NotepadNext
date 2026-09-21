/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * ContextPanel: the Context view, a window of its own inside the relation
 * panel page. It is an editor (read only) that shows the file a symbol is
 * defined in, with the symbol placed a little below the top of the view.
 *
 *   [ file name (folder)  size; modified on date            X ]
 *   [ editor                                                  ]
 *   [ lock                                                    ]
 *
 * The lock button decides whether the panel follows the caret of the main
 * editor: locked (the default) means the content is only replaced on purpose,
 * unlocked the content follows the caret. Whoever drives it is ContextTracker.
 *
 * It is not laid out by the page it lives in: RelationPanel lines it up
 * with the relation forms side by side, filling the page, and owns the
 * geometry. The panel only reports what its chrome is doing - a width drag on
 * its left or right rim (the neighbouring window compensates), a move drag on
 * its header (RelationPanel inserts the window where it was dropped) - the
 * same protocol the relation forms speak.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef CONTEXTPANEL_H
#define CONTEXTPANEL_H

#include <QWidget>

class QLabel;
class QMouseEvent;
class QPaintEvent;
class QToolButton;
class ScintillaNext;

class ContextPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ContextPanel(QWidget *parent = nullptr);

    // Lines of the file that stay visible above the symbol line, so the symbol
    // reads as "near the top" instead of being glued to the first row.
    static constexpr int SymbolTopMarginLines = 2;

    // The share of the relation panel page the panel starts with, taken from
    // the reference screenshot of the window (746 px). The dock only uses as
    // much of it as the page allows.
    static constexpr int DefaultWidth = 746;

    // Show filePath with its symbol on line (1 based) a little below the top.
    // The file is only re-read when it is not the one already on screen, so a
    // second symbol in the same file just moves the view.
    void showSymbol(const QString &filePath, int line);

    // Drop the content. The panel itself stays, it only goes back to showing
    // nothing (nothing here closes the panel - there would be no way back).
    void clear();

    // The panel does not follow the caret while this is true.
    bool isLocked() const;

    // Puts the lock into the given state without announcing it: whoever
    // restores a saved layout knows the state it puts back.
    void setLocked(bool locked);

    // The file currently on screen (empty when the panel shows nothing).
    QString currentFilePath() const { return loadedFile; }

    // The embedded editor. Exposed for the regression self check.
    ScintillaNext *editor() const { return view; }

signals:
    // The panel became visible, either because the dock was opened or because
    // the relation panel tab was picked: whoever drives the panel can use it
    // to catch the view up with the caret right away.
    void shown();
    // The lock was toggled. Unlocking asks for a refresh again.
    void lockChanged(bool locked);

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
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    // The header is watched so its drag starts the same way wherever on the
    // strip it began (the labels on it stay out of the way of the mouse).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateHeader();
    void updateLockButton();
    void applyEditorFonts();
    void updateLineNumberMargin();

    QWidget *header = nullptr;
    QLabel *headerIcon = nullptr;
    QLabel *headerText = nullptr;
    QToolButton *closeButton = nullptr;

    ScintillaNext *view = nullptr;

    QWidget *footer = nullptr;
    QToolButton *lockButton = nullptr;

    // Chrome drag state: which rim a width drag took (and where it began), or
    // where the header drag began. -1 means no drag is going on.
    bool widthDragLeft = false;
    int widthDragStartX = -1;
    int moveDragStartX = -1;

    QString loadedFile;
};

#endif // CONTEXTPANEL_H
