/*
 * This file is part of Notepad Next.
 * Copyright 2019 Justin Dailey
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


#ifndef SMARTFINDREPLACEDIALOG_H
#define SMARTFINDREPLACEDIALOG_H

#include <QDialog>
#include <QHash>
#include <QObject>
#include <QString>

#include "Finder.h"


class FindReplaceDialog;
class MainWindow;
class ProjectManager;
class ScintillaNext;

class QComboBox;
class QPushButton;
class QStatusBar;


// How far a rename reaches. Running "Replace All in Project Files" touches every
// file of the project; the smart rename first works out what the name under the
// caret really covers and narrows that down: a local variable (or a parameter)
// only exists inside its function, a file scope "static" only inside its file -
// the very same name in another function or file is a different variable and
// must be left alone.
struct SymbolRenameScope
{
    enum class Kind
    {
        Project,  // every file: a global symbol
        File,     // one file: a file scope "static"
        Function  // one function of one file: a local variable or a parameter
    };

    Kind kind = Kind::Project;
    QString file;      // canonical path; only used by File and Function
    int firstLine = 1; // 1-based, inclusive; Function only
    int lastLine = -1; // 1-based, inclusive; -1 means "up to the end of the file"

    // True when a file belongs to the scope at all.
    bool reaches(const QString &canonicalFile) const
    {
        return kind == Kind::Project || file == canonicalFile;
    }
};

// The project aware half of the find/replace dialog: searching and replacing
// across the files of the current project, the hit lists that walk those
// files, and the "Match symbol" hit rule behind the checkbox of the
// dialog. The smart rename keeps its scope classifier and its own dialog here
// as well, so the FindReplaceDialog itself stays the plain find/replace dialog
// it always was.
//
// It works on behalf of the FindReplaceDialog that owns it (and of which it is
// a friend), which leaves the dialog itself with the plain find/replace
// behaviour it always had plus a one line call into here wherever the smart
// behaviour has to kick in. Everything bulky lives in this pair of files.
class SmartFindReplaceDialog : public QObject
{
public:
    explicit SmartFindReplaceDialog(FindReplaceDialog *dialog);

    // Connects the project buttons and the project lifecycle signals, and puts
    // the buttons in their initial state. Called once from the dialog's
    // constructor, after its own widgets are up.
    void install();

    // Reports every match of the prepared search in one editor to the results
    // panel. filePath is the path the panel shows for that hit group; an empty
    // one means "the editor's own file". The single place a hit list is built:
    // find all in current document, in all opened documents and in the project
    // all end up here, so all three report the same thing for the same
    // contents.
    void collectMatches(ScintillaNext *source, const QString &filePath);
    // Same as "Find All in All Opened Documents", but over every file of the
    // current project instead of the open editors. Returns false when there was
    // nothing to search or the user cancelled the run; the caller then leaves
    // the dialog where it is instead of closing it on the results panel.
    bool findAllInProject();
    // "Replace All in Project Files": walks every project file (open buffers
    // are replaced in place, the rest is probed in a hidden scratch buffer and
    // opened as a real editor when it holds replacements), all left unsaved for
    // the user to review and save by hand.
    void replaceAllInProject();
    // "Replace All in All Opened Documents", honouring "Match symbol".
    void replaceAllInOpenedDocuments();
    // Greys the two project buttons out while there is no project to work on,
    // and keeps their tooltips in step with that.
    void updateProjectSearchButton();

    // Replaces every hit of one editor: with "Match symbol" checked only
    // the hits that pass the rule, otherwise every hit - which is exactly what
    // Finder::replaceAll() does. Returns the replacement count.
    int replaceAllInEditor(ScintillaNext *source, const QString &replaceText);
    // Replaces every hit in one editor that passes the symbol filter; hits
    // that fail it are left untouched. Returns the replacement count. The whole
    // run is a single undo step.
    int replaceMatchesInEditor(ScintillaNext *source, const QString &replaceText);
    // The per-hit rule behind "Match symbol": the complete identifier
    // holding the hit - extracted with the word rules of the document's
    // language - has to be exactly the search term. The project symbol table is
    // deliberately not consulted: the rule is a pure text rule, so it works the
    // same with or without a project. True when the hit survives; true as well
    // when the hit does not sit inside a word (a term of non-word characters
    // falls back to the plain text behaviour).
    bool passesSymbolFilter(ScintillaNext *source, int matchStart, const QString &term) const;
    // True while "Match symbol" is checked. The option is a pure text rule
    // and needs no project, so the box is always live.
    bool symbolFilterEngaged() const;
    // Walks the search on until it lands on a hit that passes the symbol
    // filter, or comes full circle back to the first attempted hit - which
    // means nothing matches and an empty result is returned. A search that is
    // not filtered is returned untouched.
    FindResult skipFilteredHits(FindResult result, bool forwards);
    // The single Replace under "Match symbol": the hit inside the current
    // selection is replaced only when it lies fully within the selection and
    // passes the filter. Returns whether a replacement happened.
    bool replaceFilteredSelection(const QString &replaceText);

private:
    MainWindow *mainWindow() const;
    ProjectManager *project() const;

    FindReplaceDialog *dialog;
};

// "Smart Rename...": the editor context menu entry and the small dialog it
// opens. The dialog is the project wide replacement of the find/replace dialog
// over one name - the name the caret stood on when it was opened - with the
// "Match symbol" rule always in force and with the reach of that name worked
// out first (see SymbolRenameScope). It has no project of its own to walk and
// leaves what it replaced unsaved, exactly like the project replacement it is
// built on.
class SmartRenameDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SmartRenameDialog(MainWindow *window);

    // The "Smart Rename..." entry of the editor context menu. The action is a
    // direct child of the window - that is how MainWindow's editor context menu
    // finds its entries, by object name - and it follows the project lifecycle:
    // greyed out while no project is open. Called once, at start up.
    static void installEditorContextMenu(MainWindow *window);

    // Fills "Fi&nd:" with the word the caret sits in, taken with the word rules
    // of the document, and remembers where that caret was: the reach of the
    // rename is worked out from it when the button is pressed.
    void startForEditor(ScintillaNext *editor);

    void setFindString(const QString &text);
    void setReplaceString(const QString &text);

    // The reach of a symbol, decided by running ctags over the file it was found
    // in: a local variable or a parameter is only visible inside its function,
    // a file scope "static" only inside its file, and everything else is
    // project wide. filePath has to be a file on disk (the classification reads
    // it through ctags); line is 1-based. Without ctags, or for a symbol ctags
    // does not report, the answer is the project wide scope - the behaviour of
    // the plain project replacement.
    static SymbolRenameScope symbolScopeFor(const QString &term, const QString &filePath, int line);

public slots:
    // "Replace All in &Project Files": the whole replacement run, on the project
    // and inside the detected scope. The dialog stays open and reports what it
    // replaced in its own status line.
    void replaceAllInProject();
    void updateProjectButton();

private:
    void showMessage(const QString &message, const QString &color) const;

    MainWindow *window = Q_NULLPTR;

    QComboBox *comboFind = Q_NULLPTR;
    QComboBox *comboReplace = Q_NULLPTR;
    QPushButton *buttonReplaceAllInProject = Q_NULLPTR;
    QStatusBar *statusBar = Q_NULLPTR;

    // Where the dialog was opened: the file the caret stood in and the 1-based
    // line it stood on. The scope of the rename is derived from these, not from
    // the caret - which may have moved on while the dialog sat open.
    QString sourceFile;
    int sourceLine = 0;
};

#endif // SMARTFINDREPLACEDIALOG_H
