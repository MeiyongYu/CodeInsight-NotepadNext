/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See ContextTracker.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ContextTracker.h"

#include "ContextPanel.h"
#include "ProjectManager.h"
#include "ScintillaNext.h"

#include <QTimer>

namespace {

// Caret movements arrive in bursts (a keystroke, a selection drag, a jump), so
// the caret has to stand still for this long before the panel reacts.
constexpr int CaretSettleMs = 180;

// ctags kind letters that only announce a name without defining it. When the
// same name is both declared and defined, the definition is what a reader
// wants, so prototypes are only used when nothing else matched.
bool isPrototypeKind(const QString &kind)
{
    return kind.size() == 1 && (kind.at(0) == QLatin1Char('p') || kind.at(0) == QLatin1Char('P'));
}

} // namespace

ContextTracker::ContextTracker(ProjectManager *projectManager, ContextPanel *panel, QObject *parent)
    : QObject(parent),
      project(projectManager),
      panel(panel)
{
    timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(CaretSettleMs);
    connect(timer, &QTimer::timeout, this, [this]() { refresh(); });

    if (panel != Q_NULLPTR) {
        // The panel came back on screen: whatever it shows may be from before
        // it was hidden, so catch it up right away.
        connect(panel, &ContextPanel::shown, this, [this]() { refresh(true); });

        // Unlocking means "follow the caret again", so do it now instead of
        // waiting for the next caret movement.
        connect(panel, &ContextPanel::lockChanged, this, [this](bool locked) {
            if (!locked)
                refresh(true);
        });
    }
}

void ContextTracker::setCurrentEditor(ScintillaNext *editor)
{
    if (this->editor.data() == editor)
        return;

    disconnect(editorUiConnection);

    this->editor = editor;
    shownSymbol.clear();

    if (editor != Q_NULLPTR) {
        editorUiConnection = connect(editor, &ScintillaNext::updateUi, this, [this](Scintilla::Update updated) {
            if (Scintilla::FlagSet(updated, Scintilla::Update::Selection))
                scheduleRefresh();
        });
    }

    // The caret of the editor just switched to is somewhere else entirely.
    refresh(true);
}

void ContextTracker::scheduleRefresh()
{
    // The panel is not on screen (dock closed, or the search results tab is in
    // front): reading files for a view nobody can see is wasted work. It is
    // refreshed when it becomes visible again.
    if (panel == Q_NULLPTR || !panel->isVisible())
        return;

    timer->start();
}

bool ContextTracker::resolveAtCaret(ProjectManager::ProjectSymbol *chosen)
{
    ScintillaNext *active = editor.data();
    if (active == Q_NULLPTR)
        return false;

    // Without a project there is no symbol table to ask, so nothing is done at
    // all - the panel keeps whatever it shows.
    if (project == Q_NULLPTR || !project->hasProject())
        return false;

    // The word under the caret, read with the editor's word characters, which
    // is what makes a word match the file's language.
    const Sci_CharacterRange range = active->wordAtPosition(static_cast<int>(active->currentPos()));
    if (range.cpMin == INVALID_POSITION || range.cpMax <= range.cpMin)
        return false;

    const QString word = QString::fromUtf8(active->get_text_range(range.cpMin, range.cpMax));
    if (word.isEmpty())
        return false;

    const QVector<ProjectManager::ProjectSymbol> matches = project->lookupSymbols(word);
    if (matches.isEmpty())
        return false; // no such symbol in the project

    // The same name can be known from several places (a prototype next to the
    // definition, or the same name in several files), and only one of them can
    // be shown. A definition says more than a prototype; among equals the first
    // entry ctags reported is taken, which is the order the project symbol
    // panel lists them in as well.
    const ProjectManager::ProjectSymbol *picked = Q_NULLPTR;
    for (const ProjectManager::ProjectSymbol &match : matches) {
        if (isPrototypeKind(match.kind))
            continue;
        picked = &match;
        break;
    }
    if (picked == Q_NULLPTR)
        picked = &matches.first();

    *chosen = *picked;
    return true;
}

bool ContextTracker::currentSymbol(QString *name, QString *filePath, int *line)
{
    ProjectManager::ProjectSymbol chosen;

    if (!resolveAtCaret(&chosen))
        return false;

    if (name != Q_NULLPTR)
        *name = chosen.name;
    if (filePath != Q_NULLPTR)
        *filePath = chosen.file;
    if (line != Q_NULLPTR)
        *line = chosen.line;
    return true;
}

void ContextTracker::refresh(bool force)
{
    ProjectManager::ProjectSymbol chosen;

    if (!resolveAtCaret(&chosen))
        return;

    // The relation forms driven by the caret hear about every symbol the caret
    // rests on, whether the Context panel follows or not (each form has its
    // own lock, and it sorts out repeats itself).
    emit symbolAtCaret(chosen.name, chosen.file, chosen.line);

    if (panel == Q_NULLPTR || panel->isLocked())
        return;

    // The caret moved, but onto the same word the panel is already showing.
    if (!force && chosen.name == shownSymbol)
        return;

    shownSymbol = chosen.name;
    panel->showSymbol(chosen.file, chosen.line);
}
