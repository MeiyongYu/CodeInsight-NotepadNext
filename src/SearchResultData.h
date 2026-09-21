/*
 * This file is part of Notepad Next.
 * Copyright 2025 Justin Dailey
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

#pragma once

#include <Qt>

namespace SearchResultData {
    enum Role
    {
        // Result item (column 1): the line the hit sits on plus where inside
        // that line it starts and ends.
        LineNumber = Qt::UserRole,
        LinePosStart,
        LinePosEnd,
        // Result item (column 1): set when the line text should be highlighted.
        Highlight,
        // File item (column 0): the path of the file the hits belong to. It is
        // kept next to the editor pointer (Qt::UserRole of the same column) so
        // that a hit group can still be opened when its file is not in an
        // editor at all - which is the normal case for a project wide search.
        FilePath
    };
}
