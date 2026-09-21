/*
 * This file is part of Notepad Next.
 * Copyright 2023 Justin Dailey
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

#include "ScintillaNext.h"

#include <QString>

class ISearchResultsHandler {
public:
    virtual void newSearch(const QString searchTerm) = 0;
    // Opens a new hit group. editor is the buffer the hits were found in; it may
    // be null when the file is not open in an editor at all (a project wide
    // search reads most files straight from disk). filePath then names that file
    // so the handler can open it when one of the hits is activated; an empty
    // filePath means "the editor's own file".
    virtual void newFileEntry(ScintillaNext *editor, const QString &filePath = QString()) = 0;
    virtual void newResultsEntry(const QString line, int lineNumber, int startPositionFromBeginning, int endPositionFromBeginning, int hitCount=1) = 0;
    virtual void completeSearch() = 0;
};
