/*
 * This file is part of Notepad Next.
 * Copyright 2026 Justin Dailey
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


#include "ChangeHistoryDecorator.h"

#include "Scintilla.h"

const int MARGIN = 1;

// The bars of the symbol margin: a line edited since the last save is yellow,
// a line whose edit has been saved is green. Scintilla maintains the marker
// numbers 21-24 for the change history on its own once the feature is on, the
// decorator only picks the two that get a bar and the colours to draw them.
const int COLOR_SAVED = 0x00A800;   // green: changed and saved
const int COLOR_MODIFIED = 0xFFC800; // yellow: changed and not saved

ChangeHistoryDecorator::ChangeHistoryDecorator(ScintillaNext *editor) :
    EditorDecorator(editor)
{
    // Keeps the undo history around to know which lines changed and whether
    // the change has been saved or not. The Markers bit draws the bars in the
    // margin: without it the changes are only recorded and nothing shows up.
    editor->setChangeHistory(SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS);

    editor->markerDefine(SC_MARKNUM_HISTORY_MODIFIED, SC_MARK_BAR);
    editor->markerSetFore(SC_MARKNUM_HISTORY_MODIFIED, COLOR_MODIFIED);
    editor->markerSetBack(SC_MARKNUM_HISTORY_MODIFIED, COLOR_MODIFIED);

    editor->markerDefine(SC_MARKNUM_HISTORY_SAVED, SC_MARK_BAR);
    editor->markerSetFore(SC_MARKNUM_HISTORY_SAVED, COLOR_SAVED);
    editor->markerSetBack(SC_MARKNUM_HISTORY_SAVED, COLOR_SAVED);

    const int mask = editor->marginMaskN(MARGIN);
    editor->setMarginMaskN(MARGIN, (1 << SC_MARKNUM_HISTORY_MODIFIED) | (1 << SC_MARKNUM_HISTORY_SAVED) | mask);
}

void ChangeHistoryDecorator::notify(const Scintilla::NotificationData *pscn)
{
    Q_UNUSED(pscn)
}
