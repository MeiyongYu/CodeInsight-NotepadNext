/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See SymbolJumpFilter.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "SymbolJumpFilter.h"
#include "ScintillaNext.h"

#include <QEvent>
#include <QMouseEvent>

SymbolJumpFilter::SymbolJumpFilter(ScintillaNext *editor, QObject *parent)
    : QObject(parent),
      editor(editor)
{
}

bool SymbolJumpFilter::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton
            && (mouseEvent->modifiers() & Qt::ControlModifier)) {
            // Translate the viewport-relative click point into a document
            // position and consume the event so Scintilla does not also move
            // the caret.
            const QPoint pos = mouseEvent->pos();
            const sptr_t position = editor->positionFromPoint(pos.x(), pos.y());
            if (position >= 0)
                emit symbolClicked(static_cast<int>(position));
            return true;
        }
    }

    return QObject::eventFilter(obj, event);
}
