/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * ProjectManager: codeinsight project support.
 *
 * A project is a folder "<name>.codeinsight" stored under a user chosen data
 * path. It holds:
 *
 *   <name>.codeinsightprj  - project configuration (INI, QSettings format)
 *   <name>.filelist        - one absolute file path per line
 *   <name>.ctags           - ctags symbol database for the project files
 *   <name>.cscope          - cscope cross-reference database
 *
 * The list of known projects and the current project are stored in the
 * application wide settings under the "Project" section.
 *
 * Synchronization runs ctags and cscope over the project file list. It can be
 * triggered synchronously (menu action, with a progress dialog owned by the
 * caller) or asynchronously (file saved -> fileChangeList -> background
 * thread). A synchronous run is cancellable: the caller's progress callback
 * doubles as the cancellation hook and synchronize() also polls it while an
 * external tool is running. All shared state is guarded by QMutex instances.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef PROJECTMANAGER_H
#define PROJECTMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMutex>
#include <QHash>
#include <QList>

#include <functional>

class QSettings;
class QThread;

class ProjectManager : public QObject
{
    Q_OBJECT

public:
    struct ProjectSymbol
    {
        QString name;
        QString file; // absolute path, forward slashes
        int line = 0; // 1-based
        QString kind;
    };

    // Result of comparing the desired file list against the last synced state.
    struct SyncDelta
    {
        QStringList added;   // new or modified files -> re-run ctags
        QStringList removed; // files whose symbols must be dropped
        bool empty() const { return added.isEmpty() && removed.isEmpty(); }
    };

    // Progress callback: (current, total, description). It doubles as the
    // cancellation hook: return false to request cancellation. synchronize()
    // then stops at its next cancellation point and also polls the callback
    // while ctags/cscope are running, so even a long tool run can be
    // interrupted.
    using ProgressFn = std::function<bool(int, int, const QString &)>;

    explicit ProjectManager(QSettings *globalSettings, QObject *parent = nullptr);
    ~ProjectManager() override;

    // ---- state -------------------------------------------------------------
    bool hasProject() const { return !projectName.isEmpty(); }
    QString currentProjectName() const { return projectName; }
    QString projectDir() const { return projectDirPath; }
    QString sourceRoot() const { return projectSourceRoot; }

    QString ctagsFilePath() const;
    QString cscopeFilePath() const;
    QString fileListFilePath() const;
    QString projectConfigFilePath() const;

    bool isProjectFile(const QString &filePath) const;
    bool isProjectDir(const QString &dirPath) const;

    // ---- global project registry ------------------------------------------
    QStringList availableProjects() const; // project names, alphabetical
    QString projectPath(const QString &name) const;
    bool setCurrentProjectInConfig(const QString &name);
    QString currentProjectFromConfig() const;

    // Identity of a "<name>.codeinsightprj" file picked by hand: the project
    // name is the file name without that suffix and the project folder is the
    // folder holding it - exactly what openProject() resolves a registered
    // entry back to. Read only, so a caller can judge a file before it commits
    // to anything (see registerProject()).
    static bool projectIdentityFromFile(const QString &projectConfigFile, QString *name, QString *dir,
                                        QString *errorMessage = nullptr);
    // Add a project folder found on disk to the global project list, the way
    // back after the list itself was lost (reinstalled machine, fresh settings
    // file) while the <name>.codeinsight folders survived. Writes
    // name -> folder into the global settings so the project shows up in
    // "Open Project"/"Remove Project" from now on; the project is not opened
    // here (call openProject()). An entry of that name pointing at a different
    // folder is refused instead of silently re-pointed, since that would orphan
    // the other folder.
    bool registerProject(const QString &projectConfigFile, QString *projectName = nullptr,
                         QString *errorMessage = nullptr);

    // ---- project lifecycle -------------------------------------------------
    // name: validated by validateProjectName(); dataPath: where the
    // <name>.codeinsight folder is created; sourceRoot: code root.
    bool createProject(const QString &name, const QString &dataPath, const QString &sourceRoot, QString *errorMessage = nullptr);
    bool openProject(const QString &name, QString *errorMessage = nullptr);
    void closeProject();
    bool removeProject(const QString &name, QString *errorMessage = nullptr);

    static bool validateProjectName(const QString &name, QString *errorMessage = nullptr);

    // True when the file has a source-code extension handled by ctags/cscope.
    // Non-language files (docs, images, build artifacts, ...) are rejected at
    // addFiles() so they never enter the project file list, keeping the file
    // list and the synchronization small and fast.
    static bool isSupportedSourceFile(const QString &filePath);
    // The supported source-code extensions (without the dot).
    static QStringList supportedSourceExtensions();

    // Folders whose name starts with a dot hold tool or build state (".git",
    // ".cmake-cache", ".vscode"), not the project's sources, so nothing inside
    // them belongs to the project - however many .c files they contain.
    // "." and ".." are how a path spells "this folder" and "the folder above",
    // not folder names, so "./src/main.c" stays valid and is not filtered.
    static bool isHiddenFolderName(const QString &name);
    // True when a directory on the way to the file is a hidden folder. Only the
    // directories are judged: a file whose *own* name starts with a dot
    // (".foorc.c") is a file, and isSupportedSourceFile() already has its say.
    static bool isUnderHiddenFolder(const QString &filePath);

    // ---- file list ---------------------------------------------------------
    QStringList projectFiles() const; // thread safe copy
    bool addFiles(const QStringList &paths, bool *changed = nullptr);
    bool removeFiles(const QStringList &paths, bool *changed = nullptr);
    bool removeAllFiles(bool *changed = nullptr);
    bool saveFileList() const;

    // ---- symbols -----------------------------------------------------------
    QVector<ProjectSymbol> symbols() const; // thread safe copy
    int symbolCount() const; // cheap size check, no copy
    QVector<ProjectSymbol> lookupSymbols(const QString &name) const; // exact name match

    // Every place that calls the given symbol, straight from the cscope
    // database (cscope query 3, "functions calling this function"). One entry
    // per call site, sorted by file and line, so a symbol called twice from the
    // same function shows up twice - that is what "go to the call site" needs.
    //
    // name is the caller field, file/line point at the call. Returns an empty
    // list and fills errorMessage when there is no project, the database has
    // not been built yet, cscope is missing or nothing calls the symbol (an
    // empty errorMessage together with an empty result means "no callers").
    QVector<ProjectSymbol> findCallers(const QString &name, QString *errorMessage = nullptr) const;

    // ---- synchronization ---------------------------------------------------
    // Diff the desired list against the last synced state (mtime+size based).
    SyncDelta computeDelta(bool force) const;
    // ctags is started once per batch of files instead of once per file. On
    // Windows a single process launch costs roughly 230 ms, which dominated
    // the whole run on large projects (measured with 300 files: 44 s when
    // started per file versus 1.7 s batched). The batch is capped both by a
    // file count and by the total length of the paths passed on the command
    // line: Windows rejects command lines longer than ~32767 characters, so a
    // project full of very long paths needs a smaller batch.
    static constexpr int CtagsFilesPerBatch = 256;
    static constexpr int CtagsBatchPathBudget = 24000;
    // Split a file list into batches honoured by both caps above. Pure and
    // order-preserving: every file appears exactly once, in order, and no
    // batch exceeds either limit. Both syncStepCount() and synchronize() use
    // this so the progress range and the work agree.
    static QList<QStringList> ctagsBatches(const QStringList &files)
    {
        QList<QStringList> batches;
        QStringList cur;
        int len = 0;
        for (const QString &f : files) {
            const int add = f.length() + 1;
            if (!cur.isEmpty() && (cur.size() >= CtagsFilesPerBatch || len + add > CtagsBatchPathBudget)) {
                batches.append(std::move(cur));
                cur.clear();
                len = 0;
            }
            cur.append(f);
            len += add;
        }
        if (!cur.isEmpty())
            batches.append(std::move(cur));
        return batches;
    }
    // Count the batches a file count would fall into. Used only for logging -
    // the real split (ctagsBatches) also honours the path-length budget.
    static int ctagsBatchCount(int fileCount)
    {
        return fileCount <= 0 ? 0 : (fileCount + CtagsFilesPerBatch - 1) / CtagsFilesPerBatch;
    }
    // Number of progress steps synchronize() reports for a delta (stale tag
    // cleanup + one per ctags batch + cscope + symbol table refresh). Used by
    // the progress dialog for its initial range.
    static int syncStepCount(const SyncDelta &delta) { return ctagsBatches(delta.added).size() + 3; }
    // Run ctags/cscope over the delta, refresh the in-memory symbol table and
    // persist the new synced state. progress may be null. Cancelling via the
    // progress callback rolls the tags file back to its previous state, leaves
    // the synced snapshot untouched and reports *wasCancelled = true, so the
    // next synchronization simply redoes the work. Thread safe: used both from
    // the GUI thread (menu action) and the background thread.
    bool synchronize(const SyncDelta &delta, const ProgressFn &progress = ProgressFn(), QString *errorMessage = nullptr, bool *wasCancelled = nullptr);
    // Convenience: computeDelta + synchronize. Returns false when nothing to do.
    bool synchronizeIncremental(bool force, const ProgressFn &progress = ProgressFn(), QString *errorMessage = nullptr, bool *wasCancelled = nullptr);

    // ---- auto sync (file saved) -------------------------------------------
    // Appends the file to the in-memory fileChangeList and starts the
    // background sync thread if it is not already running.
    void enqueueFileChange(const QString &filePath);

    // Locate the cscope executable the same way ctags is located
    // (env NOTEPADNEXT_CSCOPE -> app dir -> platform locations -> PATH).
    static QString cscopeExecutable();
    static bool validateCscope(const QString &executable);

signals:
    void projectOpened(const QString &name);
    void projectClosed();
    void projectFileListChanged();
    void projectSymbolsUpdated();
    void projectListChanged(); // projects created/removed in the registry

private:
    struct SyncedEntry
    {
        QString path;
        qint64 mtime = 0;
        qint64 size = 0;
    };

    void resetInMemoryState();
    bool loadProjectConfig();  // read <name>.codeinsightprj + .filelist
    bool loadFileList();       // .filelist -> m_files
    QHash<QString, SyncedEntry> readSyncedEntries() const; // from prj file
    bool writeSyncedEntries(const QHash<QString, SyncedEntry> &entries) const;
    bool reloadSymbols(); // parse <name>.ctags into m_symbols (guarded)
    // Both runners take an optional poll: it is called every ~150 ms while the
    // tool runs and a false return kills the process and sets *cancelled (no
    // error message is produced for a cancellation).
    bool runCtags(const QStringList &files, const QString &outputFile, QString *errorMessage,
                  const std::function<bool()> &poll = std::function<bool()>(), bool *cancelled = nullptr);
    bool runCscope(QString *errorMessage,
                   const std::function<bool()> &poll = std::function<bool()>(), bool *cancelled = nullptr);
    bool rewriteTagsWithout(const QStringList &files, QString *errorMessage);

    QSettings *settings = nullptr; // global application settings (not owned)

    // Project identity - only touched on the GUI thread.
    QString projectName;
    QString projectDirPath;
    QString projectSourceRoot;

    // Shared state, guarded by stateMutex.
    mutable QMutex stateMutex;
    QStringList m_files;                  // desired file list (mirrors .filelist)
    QVector<ProjectSymbol> m_symbols;     // parsed from <name>.ctags
    QHash<QString, SyncedEntry> m_synced; // last synced mtime/size snapshot
    QStringList m_changeList;             // fileChangeList for the background thread
    QVector<QThread *> activeThreads;     // background threads to join on destruction
    bool syncThreadRunning = false;

    // Serializes whole-synchronization between the GUI thread and the
    // background auto-sync thread.
    QMutex syncMutex;
};

#endif // PROJECTMANAGER_H
