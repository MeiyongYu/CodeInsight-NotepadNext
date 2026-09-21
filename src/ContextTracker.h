/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * ContextTracker: the behaviour behind the Context panel. It watches the caret
 * of the active editor and, whenever the caret comes to rest on a word, reads
 * that word with the editor's own word characters (so the language decides what
 * a word is), looks it up in the project symbol table and has the panel show
 * the file the symbol is defined in.
 *
 * The resolved symbol is also announced on symbolAtCaret(), which is what the
 * unlocked relation forms of the relation panel listen to; the panel and the
 * forms follow the caret independently of each other.
 *
 * Nothing happens in these cases, and what the panel already shows stays:
 *
 *   - no project is open: there is no symbol table to ask,
 *   - the caret is not on a word, or the word is no symbol of the project,
 *   - the panel is not on screen at all (the dock is closed, or the search
 *     results tab is the one in front) - reading files for a view nobody can
 *     see would be wasted work. Coming back on screen refreshes it.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef CONTEXTTRACKER_H
#define CONTEXTTRACKER_H

#include <QObject>
#include <QPointer>
#include <QString>

#include "ProjectManager.h"

class ContextPanel;
class QTimer;
class ScintillaNext;

class ContextTracker : public QObject
{
    Q_OBJECT

public:
    ContextTracker(ProjectManager *projectManager, ContextPanel *panel, QObject *parent = nullptr);

public slots:
    // The editor whose caret drives the panel (MainWindow::editorActivated).
    void setCurrentEditor(ScintillaNext *editor);

    // Read the caret and update the panel now. force skips the "that symbol is
    // already on screen" shortcut, which is what a refresh after unlocking
    // needs.
    void refresh(bool force = false);

    // The symbol under the caret right now, resolved the same way the panel
    // sees it (word of the language, looked up in the project symbol table).
    // False when there is nothing to resolve - no editor, no project, the
    // caret off a word, or a word no symbol of the project answers to. This is
    // what the refresh button of a relation form asks for.
    bool currentSymbol(QString *name, QString *filePath, int *line);

signals:
    // The caret came to rest on this symbol of the project. Emitted no matter
    // whether the Context panel follows (its lock) - the unlocked relation
    // forms take it as their root.
    void symbolAtCaret(const QString &name, const QString &filePath, int line);

private:
    // The shared body of refresh() and currentSymbol(): read the caret, look
    // the word up, prefer a definition over a prototype. False when there is
    // nothing to resolve.
    bool resolveAtCaret(ProjectManager::ProjectSymbol *chosen);

    // Wait for the caret to settle before reading it: a caret moves on every
    // keystroke and the panel would otherwise re-read a file per character.
    void scheduleRefresh();

    ProjectManager *project = nullptr;
    ContextPanel *panel = nullptr;

    QPointer<ScintillaNext> editor;
    QMetaObject::Connection editorUiConnection;
    QTimer *timer = nullptr;

    // The word the panel was last asked to show. Moving the caret inside one
    // and the same word must not reload the file over and over.
    QString shownSymbol;
};

#endif // CONTEXTTRACKER_H
