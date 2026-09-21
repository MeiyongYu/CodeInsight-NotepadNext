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


#include "SmartFindReplaceDialog.h"

#include "CtagsSymbolManager.h"
#include "FindReplaceDialog.h"
#include "ui_FindReplaceDialog.h"

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

#include "MainWindow.h"
#include "ProjectManager.h"
#include "ProjectSyncProgressDialog.h"
#include "ScintillaNext.h"
#include "UndoAction.h"


namespace {

// The complete identifier holding a position, extracted with the word rules of
// the document: Scintilla's word characters (letters, digits, underscore) are
// the identifier syntax of the common source languages, C/C++ included, so the
// extraction follows the language for free. Empty when the position does not
// sit inside a word.
QString identifierAround(ScintillaNext *source, int position)
{
    const int wordStart = source->wordStartPosition(position, true);
    const int wordEnd = source->wordEndPosition(position, true);

    if (wordEnd <= wordStart)
        return QString();

    return QString::fromUtf8(source->get_text_range(wordStart, wordEnd));
}

// The rule behind "Match symbol": a hit only counts when the complete
// identifier holding it is exactly the search term, case sensitively. This is
// what keeps "ip_rcv" from matching "ip_rcv_finish". The project symbol table is
// deliberately not consulted - being a symbol adds nothing to the rule, and the
// rule has to work without a project as well. A hit that does not sit inside a
// word (a term of non-word characters, e.g. Chinese text) passes: the rule then
// falls back to the plain text behaviour.
bool identifierEquals(ScintillaNext *source, int matchStart, const QString &term)
{
    const QString word = identifierAround(source, matchStart);

    return word.isEmpty() || word == term;
}

// The one spelling of a file path every lookup in here uses as its key.
QString canonicalPath(const QString &filePath)
{
    const QString canonical = QFileInfo(filePath).canonicalFilePath();

    if (canonical.isEmpty())
        return QString();

    return QDir::cleanPath(QDir::fromNativeSeparators(canonical));
}

// Maps every open file to its editor by canonical path, which is how a project
// file is recognised as "already open".
QHash<QString, ScintillaNext *> openFileEditors(MainWindow *window)
{
    QHash<QString, ScintillaNext *> openFiles;

    if (window == Q_NULLPTR)
        return openFiles;

    for (ScintillaNext *open : window->editors()) {
        if (open->isFile())
            openFiles.insert(canonicalPath(open->getFilePath()), open);
    }

    return openFiles;
}

// One replacement run: what to look for, what to put in its place, and how far
// the run may reach.
struct ReplaceRequest
{
    QString term;
    QString replacement;
    Scintilla::FindOption flags = Scintilla::FindOption::None; // the find flags of the search
    SymbolRenameScope scope; // project wide unless a rename narrowed it down
};

// True when a hit may be replaced: it has to be the complete identifier equal to
// the term (the "Match symbol" rule) and to lie inside the scope of the run -
// a local variable only inside its function, a file scope "static" only inside
// its file. filePath is the canonical path of the document the hit is in.
bool hitSurvivesRequest(ScintillaNext *source, const QString &filePath, const ReplaceRequest &request, int matchStart)
{
    if (!identifierEquals(source, matchStart, request.term))
        return false;

    if (!request.scope.reaches(filePath))
        return false;

    if (request.scope.kind == SymbolRenameScope::Kind::Function) {
        const int line = source->lineFromPosition(matchStart) + 1;

        if (line < request.scope.firstLine)
            return false;

        if (request.scope.lastLine > 0 && line > request.scope.lastLine)
            return false;
    }

    return true;
}

// Replaces every hit of one editor that the request allows; a hit that is
// rejected is skipped untouched and the search resumes behind it. Returns the
// replacement count. The whole run is a single undo step.
int replaceMatchingHits(ScintillaNext *source, const QString &filePath, const ReplaceRequest &request)
{
    if (request.term.isEmpty())
        return 0;

    const QByteArray searchText = request.term.toUtf8();
    const QByteArray replacementText = request.replacement.toUtf8();

    Sci_TextToFind findText{
        {0, static_cast<Sci_PositionCR>(source->length())},
        searchText.constData(),
        {-1, -1}
    };

    const int flags = static_cast<int>(request.flags);

    source->setSearchFlags(flags);

    UndoAction undoAction(source);

    int total = 0;

    // The same loop Finder::replaceAll() uses, with the rule of the run between
    // the search and the replacement.
    while (source->send(SCI_FINDTEXT, flags, reinterpret_cast<sptr_t>(&findText)) != INVALID_POSITION) {
        const Sci_Position start = findText.chrgText.cpMin;
        const Sci_Position end = findText.chrgText.cpMax;

        if (!hitSurvivesRequest(source, filePath, request, static_cast<int>(start))) {
            findText.chrg.cpMin = end;
            findText.chrg.cpMax = source->length();
            continue;
        }

        source->setTargetRange(start, end);

        Sci_Position replacementLength;
        if (Scintilla::FlagSet(request.flags, Scintilla::FindOption::RegExp))
            replacementLength = source->replaceTargetRE(replacementText.length(), replacementText.constData());
        else
            replacementLength = source->replaceTarget(replacementText.length(), replacementText.constData());

        // Continue searching after the replacement.
        findText.chrg.cpMin = start + replacementLength;
        findText.chrg.cpMax = source->length();

        ++total;
    }

    return total;
}

// Walks the project files and replaces what the request allows. Open files are
// replaced in their buffer, the rest is probed in a hidden scratch buffer first
// and - when it holds replacements - opened as a real editor and replaced
// there; nothing is ever written to disk, all of it is left unsaved for the
// user to review and save by hand. Files the scope does not reach are skipped
// before they cost a read. guard is greyed out for the duration of the run (a
// second click can arrive through the event loop the progress dialog pumps) and
// the caller puts it back afterwards.
int replaceHitsInProjectFiles(MainWindow *window, const QStringList &files, const ReplaceRequest &request,
                              const QString &progressTitle, QWidget *guard, bool *cancelled)
{
    QHash<QString, ScintillaNext *> openFiles = openFileEditors(window);

    std::unique_ptr<ScintillaNext> scratch;

    ProjectSyncProgressDialog progress(window, progressTitle);
    progress.setProgress(0, files.size());
    progress.show();
    QCoreApplication::processEvents();

    if (guard != Q_NULLPTR)
        guard->setEnabled(false);

    int count = 0;

    for (int i = 0; i < files.size(); ++i) {
        if (progress.wasCancelled())
            break;

        const QString &filePath = files.at(i);
        const QString canonical = canonicalPath(filePath);

        if (!request.scope.reaches(canonical)) {
            progress.setProgress(i + 1, files.size());
            continue;
        }

        ScintillaNext *source = openFiles.value(canonical, Q_NULLPTR);

        if (source != Q_NULLPTR) {
            // Already open in the UI: replace in its buffer, which stays
            // unsaved for the user to review.
            count += replaceMatchingHits(source, canonical, request);
        }
        else {
            // Not open: probe the file in a hidden scratch buffer first. Only a
            // file with actual replacements is opened as a real editor, and the
            // replacement happens in that editor's buffer.
            if (!scratch)
                scratch.reset(new ScintillaNext(QStringLiteral("project_replace")));

            if (scratch->loadFromFile(filePath)) {
                const int replaced = replaceMatchingHits(scratch.get(), canonical, request);

                if (replaced > 0) {
                    window->openFile(filePath);

                    // The editor openFile() just created; matching by canonical
                    // path, the same way openFiles was built above.
                    ScintillaNext *opened = Q_NULLPTR;
                    const QFileInfo targetInfo(filePath);
                    for (ScintillaNext *editor : window->editors()) {
                        if (editor->isFile() && QFileInfo(editor->getFilePath()).canonicalFilePath() == targetInfo.canonicalFilePath()) {
                            opened = editor;
                            break;
                        }
                    }

                    if (opened != Q_NULLPTR)
                        count += replaceMatchingHits(opened, canonical, request);
                }
            }
        }

        progress.setProgress(i + 1, files.size());
    }

    if (cancelled != Q_NULLPTR)
        *cancelled = progress.wasCancelled();

    progress.accept(); // done: close the dialog

    return count;
}

// ---- the reach of a symbol ("smart" rename) ----

// One tag of the ctags run over the file the caret sits in.
struct CtagsTag
{
    QString name;
    QString kind;      // the one letter ctags kind: f function, v variable, l local, z parameter, ...
    int line = 0;      // 1-based
    QString scopeKind; // "function", "class", "struct", ...; empty when the tag has no scope
    QString scopeName; // the owning function/class name, spelled as ctags spells it
    bool fileScoped = false; // ctags' "file:" field: the name is not visible outside its file
};

// Reads the "name<TAB>file<TAB>/^pattern$/;"<TAB>kind<TAB>line:N[<TAB>field:value]"
// lines of a ctags run. Everything the classification below does not need is
// ignored, so a ctags that writes a few extra fields changes nothing here.
//
// The pattern is the source line itself, and a source line indented with a tab
// carries that tab unescaped inside the pattern - ctags does not encode it - so
// the fields cannot be told apart by splitting the whole line on tabs from the
// start. The pattern is located by its /.../;" delimiters instead, and only
// what follows the terminator - kind and the extension fields - is tab
// separated.
QVector<CtagsTag> parseCtagsTags(const QByteArray &output)
{
    QVector<CtagsTag> tags;

    const QList<QByteArray> lines = output.split('\n');

    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.endsWith('\r') ? raw.left(raw.size() - 1) : raw;

        if (line.isEmpty() || line.startsWith("!_TAG_"))
            continue;

        const int fileTab = line.indexOf('\t');          // after the name
        const int patternStart = line.indexOf('\t', fileTab + 1); // after the file

        if (fileTab <= 0 || patternStart < 0)
            continue;

        // The pattern ends with ";" - the closing quote of the regex followed
        // by the semicolon that ends the pattern section. The first ";" that is
        // followed by a tab (or ends the line) is the real terminator: a ";"
        // inside the pattern itself would be followed by more pattern text.
        int patternEnd = patternStart;

        while (true) {
            patternEnd = line.indexOf(";\"", patternEnd + 1);

            if (patternEnd < 0)
                break;

            const int after = patternEnd + 2;

            if (after >= line.size() || line.at(after) == '\t')
                break;
        }

        if (patternEnd < 0)
            continue;

        // The extension fields start after the ";" terminator and the tab that
        // separates them from the pattern section.
        QByteArray extension = line.mid(patternEnd + 2);

        if (extension.startsWith('\t'))
            extension.remove(0, 1);

        const QList<QByteArray> fields = extension.split('\t');

        if (fields.isEmpty() || fields.first().isEmpty())
            continue;

        CtagsTag tag;
        tag.name = QString::fromUtf8(line.left(fileTab));
        tag.kind = QString::fromUtf8(fields.first());

        for (int i = 1; i < fields.size(); ++i) {
            const QByteArray &field = fields.at(i);
            const int colon = field.indexOf(':');

            if (colon <= 0)
                continue;

            const QByteArray key = field.left(colon);
            const QByteArray value = field.mid(colon + 1);

            if (key == "line")
                tag.line = value.toInt();
            else if (key == "file")
                tag.fileScoped = true; // ctags writes "file:" for a tag that is local to one file
            else if (key == "function" || key == "class" || key == "struct" || key == "enum"
                     || key == "namespace" || key == "union") {
                tag.scopeKind = QString::fromUtf8(key);
                tag.scopeName = QString::fromUtf8(value);
            }
        }

        if (!tag.name.isEmpty() && tag.line > 0)
            tags.append(tag);
    }

    return tags;
}

// Runs ctags over one file and returns its tags. The extra kinds l (local
// variable) and z (function parameter) are what makes locals visible at all:
// the project wide database leaves them out, but they are exactly what a rename
// has to know about. A ctags that refuses the extra kinds (an old Exuberant
// build, say) is retried without them - the rename then still sees the file
// scoped symbols, only the local ones are lost.
QVector<CtagsTag> ctagsTagsForFile(const QString &filePath)
{
    const QString ctags = CtagsSymbolManager::ctagsExecutable();

    if (ctags.isEmpty() || !QFileInfo::exists(filePath))
        return QVector<CtagsTag>();

    QStringList args;
    args << QStringLiteral("-f") << QStringLiteral("-")   // tags to stdout
         << QStringLiteral("--sort=no")                    // keep file order
         << QStringLiteral("--fields=+n")                  // add line numbers
         << QStringLiteral("--kinds-C=+l+z")
         << QStringLiteral("--kinds-C++=+l+z");

    const QString target = QDir::toNativeSeparators(filePath);

    const auto run = [&ctags, &filePath](const QStringList &arguments, QVector<CtagsTag> *tags) {
        QProcess proc;
        proc.setWorkingDirectory(QFileInfo(filePath).absolutePath());
        // ctags never reads stdin: the null device keeps Qt from creating an
        // anonymous pipe for it (see ProjectManager::runCtags).
        proc.setStandardInputFile(QProcess::nullDevice());
        proc.start(ctags, arguments);

        // A single file: a slow disk is the only thing that can stall this, and
        // the answer is optional (no tags means "project wide").
        if (!proc.waitForFinished(5000)) {
            proc.kill();
            proc.waitForFinished(1000);
            return false;
        }

        if (proc.exitCode() != 0)
            return false;

        *tags = parseCtagsTags(proc.readAllStandardOutput());
        return true;
    };

    QVector<CtagsTag> tags;

    if (run(args << target, &tags))
        return tags;

    QStringList withoutKinds;
    withoutKinds << QStringLiteral("-f") << QStringLiteral("-")
                 << QStringLiteral("--sort=no")
                 << QStringLiteral("--fields=+n")
                 << target;

    if (run(withoutKinds, &tags))
        return tags;

    return QVector<CtagsTag>();
}

// The name ctags uses for a function tag as it appears in the scope field of
// the locals it holds: a free function is reported plain ("function:add_v" ->
// "add_v"), a method fully qualified ("function:W::n" -> "W::n", the class
// prepended to the function's own name).
QString qualifiedTagName(const CtagsTag &tag)
{
    if (tag.scopeKind.isEmpty())
        return tag.name;

    return tag.scopeName + QStringLiteral("::") + tag.name;
}

} // namespace


SmartFindReplaceDialog::SmartFindReplaceDialog(FindReplaceDialog *dialog) :
    QObject(dialog),
    dialog(dialog)
{
}

void SmartFindReplaceDialog::install()
{
    Ui::FindReplaceDialog *ui = dialog->ui;

    connect(ui->buttonFindAllInProject, &QPushButton::clicked, this, [this]() {
        // The button is greyed out while there is no project; a project can also
        // have been closed while the dialog sat open, so ask once more.
        ProjectManager *project = this->project();

        if (project == Q_NULLPTR || !project->hasProject())
            return;

        dialog->prepareToPerformSearch();

        const QString term = dialog->findString();

        dialog->searchResultsHandler->newSearch(term);

        // "Match symbol" is applied per hit inside collectMatches() - the
        // one place every hit list is built - so nothing has to be armed here.
        const bool searched = findAllInProject();

        dialog->searchResultsHandler->completeSearch();

        // A cancelled run keeps the dialog open: its status bar says so, and
        // closing it would hide the fact that the results are only partial.
        if (searched)
            dialog->close();
    });

    // "Replace All in All Opened Documents" honours "Match symbol" as well,
    // so it is wired up here instead of in the dialog itself.
    connect(ui->buttonReplaceAllInDocuments, &QPushButton::clicked, this, &SmartFindReplaceDialog::replaceAllInOpenedDocuments);
    connect(ui->buttonReplaceAllInProject, &QPushButton::clicked, this, &SmartFindReplaceDialog::replaceAllInProject);

    // "Find All in Project Files" follows the project lifecycle: without a project
    // there is nothing to search, so it stays greyed out and clicking it does
    // nothing. Opening or closing a project while the dialog sits open moves it
    // in and out of that state.
    if (ProjectManager *project = this->project()) {
        connect(project, &ProjectManager::projectOpened, this, &SmartFindReplaceDialog::updateProjectSearchButton);
        connect(project, &ProjectManager::projectClosed, this, &SmartFindReplaceDialog::updateProjectSearchButton);
    }

    updateProjectSearchButton();
}

void SmartFindReplaceDialog::collectMatches(ScintillaNext *source, const QString &filePath)
{
    bool firstMatch = true;

    // The search rules are the ones the dialog already prepared for the current
    // editor (mode, whole word, case, wrap around); only the document the rules
    // are applied to changes.
    Finder sourceFinder(source);
    sourceFinder.options() = dialog->finder->options();
    sourceFinder.options().text = dialog->findString();
    const QString term = dialog->findString();

    sourceFinder.forEachMatch([&](int start, int end) {
        // "Match symbol": drop hits that are not the complete identifier
        // equal to the term. The rule is a pure text rule and the same for
        // every hit list, so it is read straight off the checkbox here instead
        // of through a flag each caller would have to remember to set - current
        // document, opened documents and project all come through this function.
        if (symbolFilterEngaged() && !identifierEquals(source, start, term))
            return end;

        // Only add the file entry if there was a valid search result
        if (firstMatch) {
            dialog->searchResultsHandler->newFileEntry(source, filePath);
            firstMatch = false;
        }

        const int line = source->lineFromPosition(start);
        const int lineStartPosition = source->positionFromLine(line);
        const int lineEndPosition = source->lineEndPosition(line);
        const int startPositionFromBeginning = start - lineStartPosition;
        const int endPositionFromBeginning = end - lineStartPosition;
        QString lineText = source->get_text_range(lineStartPosition, lineEndPosition);

        dialog->searchResultsHandler->newResultsEntry(lineText, line, startPositionFromBeginning, endPositionFromBeginning);

        return end;
    });
}

bool SmartFindReplaceDialog::findAllInProject()
{
    qInfo(Q_FUNC_INFO);

    MainWindow *window = mainWindow();
    ProjectManager *project = this->project();

    if (project == Q_NULLPTR || !project->hasProject())
        return false;

    const QStringList files = project->projectFiles();
    if (files.isEmpty()) {
        dialog->showMessage(dialog->tr("The project does not contain any files."), "red");
        return false;
    }

    // Files that are already open are searched in their buffer - that is the
    // text the user is looking at, unsaved changes included - so collect them
    // up front. Everything else is read from disk into one reused scratch
    // editor: building an editor per file would cost far more than the search
    // itself on a project holding thousands of source files.
    QHash<QString, ScintillaNext *> openFiles = openFileEditors(window);

    std::unique_ptr<ScintillaNext> scratch;

    // Searching reads every project file, so on a real project it takes long
    // enough to need a progress bar and a way out - the same dialog the file
    // synchronization uses.
    ProjectSyncProgressDialog progress(window, dialog->tr("Searching project"));
    progress.setProgress(0, files.size());
    progress.show();
    QCoreApplication::processEvents();

    // Guard against a second click arriving through the event loop the progress
    // dialog pumps; the button is restored to its proper state afterwards.
    dialog->ui->buttonFindAllInProject->setEnabled(false);

    for (int i = 0; i < files.size(); ++i) {
        if (progress.wasCancelled())
            break;

        const QString &filePath = files.at(i);

        ScintillaNext *source = openFiles.value(canonicalPath(filePath), Q_NULLPTR);

        if (source == Q_NULLPTR) {
            if (!scratch)
                scratch.reset(new ScintillaNext(QStringLiteral("project_search")));

            // One editor is reused for every file that is not open, so it has to
            // be reset to that file's contents first.
            if (scratch->loadFromFile(filePath))
                source = scratch.get();
        }

        if (source != Q_NULLPTR)
            collectMatches(source, filePath);

        progress.setProgress(i + 1, files.size());
    }

    const bool cancelled = progress.wasCancelled();
    progress.accept(); // done: close the dialog

    updateProjectSearchButton();

    if (cancelled) {
        dialog->showMessage(dialog->tr("Search cancelled."), "red");
        return false;
    }

    return true;
}

void SmartFindReplaceDialog::replaceAllInProject()
{
    qInfo(Q_FUNC_INFO);

    MainWindow *window = mainWindow();
    ProjectManager *project = this->project();

    if (project == Q_NULLPTR || !project->hasProject())
        return;

    dialog->prepareToPerformSearch(true);

    QString replaceText = dialog->replaceString();

    if (dialog->ui->radioExtendedSearch->isChecked()) {
        dialog->convertToExtended(replaceText);
    }

    // "Match symbol" is handled inside the rule of the run: with the box
    // unchecked the identifier rule passes every hit, which is the plain
    // behaviour, and the project wide scope leaves every file in play.
    const QStringList files = project->projectFiles();
    if (files.isEmpty()) {
        dialog->showMessage(dialog->tr("The project does not contain any files."), "red");
        return;
    }

    ReplaceRequest request;
    request.term = dialog->findString();
    request.replacement = replaceText;
    request.flags = dialog->finder->options().flags;
    request.scope = SymbolRenameScope();

    bool cancelled = false;

    const int count = replaceHitsInProjectFiles(window, files, request,
                                                dialog->tr("Replacing project files"),
                                                dialog->ui->buttonReplaceAllInProject, &cancelled);

    updateProjectSearchButton();

    dialog->showMessage(dialog->tr("Replaced %Ln matches", "", count), "green");

    // The cancellation note comes last: it is the more important status.
    if (cancelled)
        dialog->showMessage(dialog->tr("Search cancelled."), "red");
}

void SmartFindReplaceDialog::replaceAllInOpenedDocuments()
{
    dialog->prepareToPerformSearch(true);

    QString replaceText = dialog->replaceString();

    if (dialog->ui->radioExtendedSearch->isChecked()) {
        dialog->convertToExtended(replaceText);
    }

    int count = 0;
    ScintillaNext *current_editor = dialog->editor;
    MainWindow *window = mainWindow();

    for (ScintillaNext *editor : window->editors()) {
        dialog->setEditor(editor);
        count += replaceAllInEditor(editor, replaceText);
    }

    dialog->setEditor(current_editor);

    dialog->showMessage(dialog->tr("Replaced %Ln matches", "", count), "green");
}

void SmartFindReplaceDialog::updateProjectSearchButton()
{
    ProjectManager *project = this->project();
    const bool hasProject = project != Q_NULLPTR && project->hasProject();

    Ui::FindReplaceDialog *ui = dialog->ui;

    ui->buttonFindAllInProject->setEnabled(hasProject);
    ui->buttonFindAllInProject->setToolTip(hasProject
        ? dialog->tr("Search all files of the current project")
        : dialog->tr("No project is open"));

    ui->buttonReplaceAllInProject->setEnabled(hasProject);
    ui->buttonReplaceAllInProject->setToolTip(hasProject
        ? dialog->tr("Replace in all files of the current project")
        : dialog->tr("No project is open"));

    // "Match symbol" is deliberately left alone here: it is a pure text
    // rule - it does not look up the project symbol table - so it stays live
    // with or without a project. Only the two project buttons follow the
    // project lifecycle.
}

int SmartFindReplaceDialog::replaceAllInEditor(ScintillaNext *source, const QString &replaceText)
{
    // With "Match symbol" checked only the hits that pass the rule are
    // replaced; unchecked every hit is, which is what Finder::replaceAll()
    // does and what the button did before the option existed.
    return symbolFilterEngaged()
        ? replaceMatchesInEditor(source, replaceText)
        : dialog->finder->replaceAll(replaceText);
}

int SmartFindReplaceDialog::replaceMatchesInEditor(ScintillaNext *source, const QString &replaceText)
{
    ReplaceRequest request;
    request.term = dialog->findString();
    request.replacement = replaceText;
    request.flags = dialog->finder->options().flags;
    request.scope = SymbolRenameScope();

    return replaceMatchingHits(source, canonicalPath(source->getFilePath()), request);
}

bool SmartFindReplaceDialog::passesSymbolFilter(ScintillaNext *source, int matchStart, const QString &term) const
{
    return identifierEquals(source, matchStart, term);
}

bool SmartFindReplaceDialog::symbolFilterEngaged() const
{
    return dialog->ui->checkBoxMatchSymbols->isChecked();
}

FindResult SmartFindReplaceDialog::skipFilteredHits(FindResult result, bool forwards)
{
    if (!symbolFilterEngaged() || !result)
        return result;

    ScintillaNext *editor = dialog->editor;
    const QString term = dialog->findString();
    const int firstAttempt = result.range.cpMin;
    const Sci_Position anchorStart = editor->selectionStart();
    const Sci_Position anchorEnd = editor->selectionEnd();

    // A hit only counts when it is the complete identifier equal to the term.
    // Skip the others; coming full circle back to the first attempted hit means
    // nothing matches.
    while (!passesSymbolFilter(editor, result.range.cpMin, term)) {
        // The next search starts from the selection, so it has to be moved past
        // the rejected hit first - otherwise the very same hit would be
        // reported again and the check below would mistake it for a full circle
        // and give up.
        const Sci_Position anchor = forwards ? result.range.cpMax : result.range.cpMin;
        editor->setSelection(static_cast<int>(anchor), static_cast<int>(anchor));
        result = forwards ? dialog->finder->findNext() : dialog->finder->findPrev();

        if (!result || result.range.cpMin == firstAttempt) {
            result = FindResult{};
            editor->setSelection(static_cast<int>(anchorStart), static_cast<int>(anchorEnd));
            break;
        }
    }

    return result;
}

bool SmartFindReplaceDialog::replaceFilteredSelection(const QString &replaceText)
{
    ScintillaNext *editor = dialog->editor;
    const QString term = dialog->findString();

    // The same hit replaceSelectionIfMatch() would take: the first match inside
    // the current selection. It has to lie fully within the selection and be
    // the complete identifier equal to the term, otherwise the selection stays
    // untouched - that is what keeps "alpha" from being replaced inside the
    // longer identifier "alpha_suffix".
    const Sci_Position selectionStart = editor->selectionStart();
    const Sci_Position selectionEnd = editor->selectionEnd();
    const FindResult selection = dialog->finder->findNextFrom(selectionStart);

    if (!selection || selection.range.cpMin < selectionStart || selection.range.cpMax > selectionEnd
        || !passesSymbolFilter(editor, static_cast<int>(selection.range.cpMin), term))
        return false;

    return static_cast<bool>(dialog->finder->replaceSelectionIfMatch(replaceText));
}

MainWindow *SmartFindReplaceDialog::mainWindow() const
{
    return qobject_cast<MainWindow *>(dialog->parent());
}

ProjectManager *SmartFindReplaceDialog::project() const
{
    MainWindow *window = mainWindow();

    return window ? window->getProjectManager() : Q_NULLPTR;
}


SmartRenameDialog::SmartRenameDialog(MainWindow *window) :
    QDialog(window, Qt::Dialog),
    window(window)
{
    // Turn off the help button on the dialog
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    // The strings are deliberately the ones of the find/replace dialog - the
    // same labels, the same "Replace All in &Project Files" button, the same
    // "Replaced %Ln matches" line - so a language that knows that dialog knows
    // this one too, and both read the same in every language.
    setWindowTitle(FindReplaceDialog::tr("Smart Rename"));

    comboFind = new QComboBox(this);
    comboFind->setEditable(true);
    comboFind->setCompleter(Q_NULLPTR); // auto completion would fight with the prefilled word
    comboFind->setInsertPolicy(QComboBox::NoInsert);

    comboReplace = new QComboBox(this);
    comboReplace->setEditable(true);
    comboReplace->setCompleter(Q_NULLPTR);
    comboReplace->setInsertPolicy(QComboBox::NoInsert);

    auto *labelFind = new QLabel(FindReplaceDialog::tr("&Find:"), this);
    labelFind->setBuddy(comboFind);

    auto *labelReplace = new QLabel(FindReplaceDialog::tr("Replace:"), this);
    labelReplace->setBuddy(comboReplace);

    buttonReplaceAllInProject = new QPushButton(FindReplaceDialog::tr("Replace All in &Project Files"), this);
    connect(buttonReplaceAllInProject, &QPushButton::clicked, this, &SmartRenameDialog::replaceAllInProject);

    // The status line at the bottom left is where the count of the run lands,
    // exactly like the status line of the find/replace dialog.
    statusBar = new QStatusBar(this);
    statusBar->setSizeGripEnabled(true);

    auto *grid = new QGridLayout;
    grid->addWidget(labelFind, 0, 0);
    grid->addWidget(comboFind, 0, 1);
    grid->addWidget(buttonReplaceAllInProject, 0, 2);
    grid->addWidget(labelReplace, 1, 0);
    grid->addWidget(comboReplace, 1, 1);
    grid->setColumnStretch(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addWidget(statusBar);

    resize(480, 150);

    // The one button follows the project lifecycle: without a project there is
    // nothing to replace in.
    if (ProjectManager *project = window->getProjectManager()) {
        connect(project, &ProjectManager::projectOpened, this, &SmartRenameDialog::updateProjectButton);
        connect(project, &ProjectManager::projectClosed, this, &SmartRenameDialog::updateProjectButton);
    }

    updateProjectButton();

    if (comboFind->lineEdit() != Q_NULLPTR)
        comboFind->lineEdit()->selectAll();
}

void SmartRenameDialog::installEditorContextMenu(MainWindow *window)
{
    if (window == Q_NULLPTR)
        return;

    // A direct child of the window: that is how the editor context menu collects
    // the entries it shows (MainWindow::buildMenu looks the actions up by name
    // among the window's children), and the menu lives longer than any single
    // run of the dialog, so the entry is created here and the dialog only when
    // it is really used.
    QAction *action = new QAction(FindReplaceDialog::tr("Smart Rename..."), window);
    action->setObjectName(QStringLiteral("actionSmartRename"));

    connect(action, &QAction::triggered, window, [window]() {
        SmartRenameDialog *dialog = window->findChild<SmartRenameDialog *>(QString(), Qt::FindDirectChildrenOnly);

        if (dialog == Q_NULLPTR)
            dialog = new SmartRenameDialog(window);

        dialog->startForEditor(window->currentEditor());
    });

    const auto updateEnabled = [window, action]() {
        ProjectManager *project = window->getProjectManager();
        const bool hasProject = project != Q_NULLPTR && project->hasProject();

        // Greyed out without a project: the menu shows the entry as unavailable
        // and clicking it does nothing.
        action->setEnabled(hasProject);
        action->setToolTip(hasProject ? QString() : FindReplaceDialog::tr("No project is open"));
    };

    if (ProjectManager *project = window->getProjectManager()) {
        connect(project, &ProjectManager::projectOpened, action, updateEnabled);
        connect(project, &ProjectManager::projectClosed, action, updateEnabled);
    }

    updateEnabled();
}

void SmartRenameDialog::startForEditor(ScintillaNext *editor)
{
    if (editor == Q_NULLPTR)
        return;

    // Both the word and the place it was taken from are remembered: the word
    // goes into "Fi&nd:", the place is what the reach of the rename is worked
    // out from later. The word is taken with the word rules of the document, so
    // an identifier of the language is taken whole.
    const int position = static_cast<int>(editor->currentPos());

    sourceFile = editor->isFile() ? canonicalPath(editor->getFilePath()) : QString();
    sourceLine = editor->lineFromPosition(position) + 1;

    const QString word = identifierAround(editor, position);

    // Right clicking not on a word (whitespace, punctuation) leaves the field
    // alone: the user can type the name instead.
    if (!word.isEmpty())
        setFindString(word);

    statusBar->clearMessage();

    show();
    raise();
    activateWindow();

    comboFind->setFocus();
    comboFind->lineEdit()->selectAll();
}

void SmartRenameDialog::setFindString(const QString &text)
{
    comboFind->setCurrentText(text);
    comboFind->lineEdit()->selectAll();
}

void SmartRenameDialog::setReplaceString(const QString &text)
{
    comboReplace->setCurrentText(text);
}

void SmartRenameDialog::updateProjectButton()
{
    ProjectManager *project = window ? window->getProjectManager() : Q_NULLPTR;
    const bool hasProject = project != Q_NULLPTR && project->hasProject();

    buttonReplaceAllInProject->setEnabled(hasProject);
    buttonReplaceAllInProject->setToolTip(hasProject
        ? FindReplaceDialog::tr("Replace in all files of the current project")
        : FindReplaceDialog::tr("No project is open"));
}

void SmartRenameDialog::replaceAllInProject()
{
    qInfo(Q_FUNC_INFO);

    ProjectManager *project = window ? window->getProjectManager() : Q_NULLPTR;

    if (project == Q_NULLPTR || !project->hasProject()) {
        updateProjectButton();
        showMessage(FindReplaceDialog::tr("No project is open"), "red");
        return;
    }

    const QString term = comboFind->currentText();

    if (term.isEmpty()) {
        showMessage(FindReplaceDialog::tr("No matches found."), "red");
        comboFind->setFocus();
        return;
    }

    const QStringList files = project->projectFiles();
    if (files.isEmpty()) {
        showMessage(FindReplaceDialog::tr("The project does not contain any files."), "red");
        return;
    }

    // How far the name reaches: the whole project for a global symbol, one file
    // for a file scope one, one function for a local variable.
    const SymbolRenameScope scope = symbolScopeFor(term, sourceFile, sourceLine);

    ReplaceRequest request;
    request.term = term;
    request.replacement = comboReplace->currentText();
    // An identifier is what is being replaced, so the search is case sensitive.
    // Combined with the identifier rule of the run that makes the replacement an
    // exact name match, which is what "Match symbol" stands for.
    request.flags = Scintilla::FindOption::MatchCase;
    request.scope = scope;

    bool cancelled = false;

    const int count = replaceHitsInProjectFiles(window, files, request,
                                                FindReplaceDialog::tr("Replacing project files"),
                                                buttonReplaceAllInProject, &cancelled);

    // The button is back in its proper state, and the dialog stays open: the
    // count in the status line is what it has to say, and the run may well be
    // followed by another one.
    updateProjectButton();

    showMessage(FindReplaceDialog::tr("Replaced %Ln matches", "", count), "green");

    if (cancelled)
        showMessage(FindReplaceDialog::tr("Search cancelled."), "red");
}

void SmartRenameDialog::showMessage(const QString &message, const QString &color) const
{
    statusBar->setStyleSheet(QStringLiteral("color: %1").arg(color));
    statusBar->showMessage(message);
}

SymbolRenameScope SmartRenameDialog::symbolScopeFor(const QString &term, const QString &filePath, int line)
{
    // A plain project wide rename until the tags say otherwise: that is the
    // behaviour of "Replace All in Project Files".
    SymbolRenameScope scope;

    if (term.isEmpty() || filePath.isEmpty() || line <= 0)
        return scope;

    const QVector<CtagsTag> tags = ctagsTagsForFile(filePath);

    if (tags.isEmpty())
        return scope;

    // Every function of the file with the line its body starts on: the end of
    // one is where the next one starts (or the end of the file).
    QList<int> functionStarts;

    for (const CtagsTag &tag : tags) {
        if (tag.kind == QLatin1String("f"))
            functionStarts.append(tag.line);
    }

    std::sort(functionStarts.begin(), functionStarts.end());

    const auto functionEnd = [&functionStarts](int startLine) {
        for (const int start : functionStarts) {
            if (start > startLine)
                return start - 1;
        }

        return -1; // the last function of the file runs to its end
    };

    const auto insideFunction = [&functionEnd](int startLine, int line) {
        const int endLine = functionEnd(startLine);

        return line >= startLine && (endLine < 0 || line <= endLine);
    };

    // The function of the given name that holds the line: the name is the one a
    // local carries in its "function:<name>" scope field, spelled the way ctags
    // spells it there - plain for a free function, fully qualified for a method
    // ("function:W::n") - and the line decides between functions of the same
    // name. Returns the start line of the owning function, 0 when none fits.
    const auto owningFunctionLine = [&tags, &insideFunction](const QString &name, int line) {
        for (const CtagsTag &tag : tags) {
            if (tag.kind == QLatin1String("f")
                && (tag.name == name || qualifiedTagName(tag) == name)
                && insideFunction(tag.line, line))
                return tag.line;
        }

        return 0;
    };

    QVector<const CtagsTag *> matches;
    for (const CtagsTag &tag : tags) {
        if (tag.name == term)
            matches.append(&tag);
    }

    if (matches.isEmpty())
        return scope;

    // 1. A local variable or a parameter: it only exists inside its function.
    // The function holding the caret wins when the file declares the same name
    // in more than one of them.
    const CtagsTag *local = Q_NULLPTR;
    int localFunctionLine = 0;

    for (const CtagsTag *tag : matches) {
        if (tag->kind != QLatin1String("l") && tag->kind != QLatin1String("z"))
            continue;

        const int startLine = tag->scopeName.isEmpty() ? 0 : owningFunctionLine(tag->scopeName, line);

        if (startLine > 0) {
            local = tag;
            localFunctionLine = startLine;
            break;
        }

        if (local == Q_NULLPTR)
            local = tag; // a local whose function cannot be placed: remembered for the file scope fallback
    }

    if (local != Q_NULLPTR && localFunctionLine > 0) {
        scope.kind = SymbolRenameScope::Kind::Function;
        scope.file = filePath;
        scope.firstLine = localFunctionLine;
        scope.lastLine = functionEnd(localFunctionLine);
        return scope;
    }

    if (local != Q_NULLPTR) {
        // The function itself is not among the tags (a lambda, a language ctags
        // spells differently): keep the rename inside the file rather than let
        // it run over the whole project.
        scope.kind = SymbolRenameScope::Kind::File;
        scope.file = filePath;
        return scope;
    }

    // 2. A file scope variable - "static" in C - is a different variable in
    // every file that declares one under that name, so only this file is the
    // one the user can mean.
    for (const CtagsTag *tag : matches) {
        if (tag->fileScoped && (tag->kind == QLatin1String("v") || tag->kind == QLatin1String("x"))) {
            scope.kind = SymbolRenameScope::Kind::File;
            scope.file = filePath;
            return scope;
        }
    }

    // 3. A file scope function without a class or namespace of its own ("static"
    // in C) is not visible outside the file either.
    for (const CtagsTag *tag : matches) {
        if (tag->fileScoped && tag->kind == QLatin1String("f") && tag->scopeKind.isEmpty()) {
            scope.kind = SymbolRenameScope::Kind::File;
            scope.file = filePath;
            return scope;
        }
    }

    return scope;
}
