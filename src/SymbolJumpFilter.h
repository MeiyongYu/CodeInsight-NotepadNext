/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * Ctrl+LeftClick on a symbol in the editor triggers "jump to symbol".
 * Installed as an event filter on the editor viewport, one instance per
 * editor (mirrors ShiftMiddleClickBlocker / ZoomEventWatcher).
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef SYMBOLJUMPFILTER_H
#define SYMBOLJUMPFILTER_H

#include <QObject>

class ScintillaNext;

class SymbolJumpFilter : public QObject
{
    Q_OBJECT

public:
    explicit SymbolJumpFilter(ScintillaNext *editor, QObject *parent = nullptr);

signals:
    // 0-based character position inside the editor document
    void symbolClicked(int position);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    ScintillaNext *editor;
};

#endif // SYMBOLJUMPFILTER_H
