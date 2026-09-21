/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * SymbolIcon: shared symbol-kind icon provider.
 *
 * Every symbol kind gets a small colored circle with the ctags kind letter,
 * e.g. green "f" for functions, blue "c"/"s" for class/struct, orange "v" for
 * variables. The function list panel and the Project Symbols panel both pull
 * their icons from here so the two lists always look identical.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef SYMBOLICON_H
#define SYMBOLICON_H

#include <QIcon>
#include <QString>

class SymbolIcon
{
public:
    // Icon for a ctags kind letter ("f", "p", "m", "v", "c", ...). Unknown or
    // empty kinds fall back to a gray circle with the first letter or "?".
    static QIcon icon(const QString &kind);
};

#endif // SYMBOLICON_H
