/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * ProjectManager implementation. See ProjectManager.h for the overview.
 *
 * Threading model:
 *  - Project lifecycle (create/open/close/remove) and the file list editing
 *    happen on the GUI thread.
 *  - Synchronization is serialized by syncMutex and can run on the GUI thread
 *    (menu action, modal progress) or on the auto-sync background thread
 *    (file saved). stateMutex guards the shared containers.
 *  - QSettings objects for the project config file are created fresh inside
 *    the function that uses them, so a QSettings instance never crosses a
 *    thread boundary.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ProjectManager.h"
#include "CtagsSymbolManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QThread>

#include <algorithm>

namespace {

const QLatin1String CurrentProjectKey("Project/CurrentProject");
const QLatin1String ProjectListPrefix("Project/List/");

QString normalizePath(const QString &path)
{
    QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    // ctags writes the file field exactly as the path was passed on the
    // command line; keep the case of the drive letter consistent.
    if (p.size() >= 2 && p.at(1) == QLatin1Char(':'))
        p[0] = p.at(0).toUpper();
#endif
    return p;
}

QSettings *openProjectConfig(const QString &projectDir, const QString &projectName)
{
    // A fresh QSettings instance per use: instances must never be shared
    // across threads (Qt6 INI files are UTF-8 by default).
    const QString file = QDir(projectDir).absoluteFilePath(projectName + QStringLiteral(".codeinsightprj"));
    return new QSettings(file, QSettings::IniFormat);
}

// Parse a ctags tags file into the project symbol list. Tolerates both the
// classic exuberant output ("name file /pat/;" f line:12, bare kind letter)
// and the u-ctags output ("kind:function" style extension fields).
QVector<ProjectManager::ProjectSymbol> parseTagsFile(const QString &tagsFile)
{
    QVector<ProjectManager::ProjectSymbol> parsed;
    QFile f(tagsFile);
    if (!f.open(QIODevice::ReadOnly))
        return parsed;

    // Work on raw bytes: building a QString plus a QStringList of every field
    // plus two QRegularExpression matches per line took 24.7 s for the 5.09 M
    // symbols of the linux kernel database. Per line we now locate the columns
    // we need with simple scans and translate only what the caller wants, which
    // brings the same file down to ~4 s.
    //
    // Two ctags format traps are honoured: the pattern field may contain real
    // tab characters (e.g. "/^\t\tuint64_t reserved_59_63:5;$/;\""), so the
    // "line:" / "kind:" extensions are matched as whole tab-delimited fields,
    // and the pattern field itself ends with the ";\" terminator.
    QByteArray line;
    while (!f.atEnd()) {
        line = f.readLine();
        int end = line.size();
        if (end == 0)
            continue;
        // strip trailing CR/LF (read without QIODevice::Text)
        if (line[end - 1] == '\n')
            --end;
        if (end > 0 && line[end - 1] == '\r')
            --end;
        if (end <= 0)
            continue;
        if (line[0] == '!') // ctags metadata header
            continue;

        const int t1 = line.indexOf('\t', 0);
        if (t1 <= 0)
            continue;
        const int t2 = line.indexOf('\t', t1 + 1);
        if (t2 <= t1)
            continue;

        ProjectManager::ProjectSymbol sym;
        sym.name = QString::fromUtf8(line.constData(), t1);
        sym.file = QString::fromUtf8(line.constData() + t1 + 1, t2 - t1 - 1);

        // Scan the remaining fields for the line: / kind: extensions and the
        // classic bare kind letter that follows the pattern field.
        bool kindSet = false;
        bool sawPattern = false;
        int p = t2 + 1;
        while (p < end) {
            const int nt = line.indexOf('\t', p);
            const int fe = (nt == -1 || nt > end) ? end : nt;
            const char *base = line.constData() + p;
            const int flen = fe - p;
            if (flen >= 2 && base[flen - 1] == '"' && base[flen - 2] == ';') {
                sawPattern = true; // the pattern field ends with ;"
            } else if (flen >= 5 && base[0] == 'l' && base[1] == 'i' && base[2] == 'n'
                       && base[3] == 'e' && base[4] == ':') {
                sym.line = QByteArray(base + 5, flen - 5).toInt();
            } else if (flen >= 5 && base[0] == 'k' && base[1] == 'i' && base[2] == 'n'
                       && base[3] == 'd' && base[4] == ':') {
                sym.kind = QString::fromUtf8(base + 5, flen - 5);
                kindSet = true;
            } else if (!kindSet && sawPattern && flen > 0) {
                // classic bare kind letter, the field right after the pattern
                bool hasColon = false;
                for (int i = p; i < fe; ++i) {
                    if (line[i] == ':') {
                        hasColon = true;
                        break;
                    }
                }
                if (!hasColon) {
                    sym.kind = QString::fromUtf8(base, flen);
                    kindSet = true;
                }
            }
            p = (nt == -1) ? end : nt + 1;
        }
        parsed.append(sym);
    }
    return parsed;
}

// Pick the first candidate binary that answers --version correctly.
QString probeCandidates(const QStringList &candidates, const QByteArray &requiredToken)
{
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty() || !QFileInfo::exists(candidate))
            continue;

        QProcess proc;
        proc.setStandardInputFile(QProcess::nullDevice());
        proc.start(candidate, {QStringLiteral("--version")});
        if (proc.waitForStarted(3000) && proc.waitForFinished(5000)) {
            const QByteArray out = proc.readAllStandardOutput() + proc.readAllStandardError();
            if (out.contains(requiredToken))
                return candidate;
        }
    }
    return QString();
}

// Waits for a running ctags/cscope to finish while giving the caller a chance
// to cancel: "poll" is called every 150 ms and a false return kills the
// process. Returns true when the process stopped on its own (the caller still
// has to look at exitCode()), false when it could not be started or was killed
// on request - *cancelled tells those two apart.
bool waitForProcess(QProcess &proc, const std::function<bool()> &poll, bool *cancelled)
{
    if (!proc.waitForStarted(5000))
        return false;

    while (true) {
        if (proc.waitForFinished(150))
            return true;
        if (proc.state() == QProcess::NotRunning)
            return true; // finished or failed, exitCode()/error() reports it

        if (poll && !poll()) {
            proc.kill();
            proc.waitForFinished(5000);
            if (cancelled)
                *cancelled = true;
            return false;
        }
    }
}

QStringList pathDirectories()
{
    QStringList dirs;
    const QString pathEnv = qEnvironmentVariable("PATH");
#if defined(Q_OS_WIN)
    const QLatin1Char sep(';');
#else
    const QLatin1Char sep(':');
#endif
    const QStringList raw = pathEnv.split(sep, Qt::SkipEmptyParts);
    for (const QString &d : raw) {
        const QString clean = QDir::cleanPath(d);
        if (!clean.isEmpty())
            dirs.append(clean);
    }
    return dirs;
}

} // namespace

// ---------------------------------------------------------------------------
// construction / destruction
// ---------------------------------------------------------------------------

ProjectManager::ProjectManager(QSettings *globalSettings, QObject *parent)
    : QObject(parent),
      settings(globalSettings)
{
}

ProjectManager::~ProjectManager()
{
    // Wait for any background work (auto-sync thread, symbol loader) to exit
    // before the members guarding them are destroyed. Threads signal "no new
    // work" through syncThreadRunning and drain the change list first.
    QVector<QThread *> threads;
    {
        QMutexLocker locker(&stateMutex);
        syncThreadRunning = false;
        threads = activeThreads;
    }
    for (QThread *thread : threads) {
        if (thread && thread->isRunning())
            thread->wait();
    }
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

QString ProjectManager::ctagsFilePath() const
{
    return hasProject() ? QDir(projectDirPath).absoluteFilePath(projectName + QStringLiteral(".ctags")) : QString();
}

QString ProjectManager::cscopeFilePath() const
{
    return hasProject() ? QDir(projectDirPath).absoluteFilePath(projectName + QStringLiteral(".cscope")) : QString();
}

QString ProjectManager::fileListFilePath() const
{
    return hasProject() ? QDir(projectDirPath).absoluteFilePath(projectName + QStringLiteral(".filelist")) : QString();
}

QString ProjectManager::projectConfigFilePath() const
{
    return hasProject() ? QDir(projectDirPath).absoluteFilePath(projectName + QStringLiteral(".codeinsightprj")) : QString();
}

bool ProjectManager::isProjectFile(const QString &filePath) const
{
    QMutexLocker locker(&stateMutex);
    return m_files.contains(normalizePath(filePath));
}

bool ProjectManager::isProjectDir(const QString &dirPath) const
{
    const QString dir = normalizePath(dirPath);
    if (!hasProject() || dir.isEmpty())
        return false;
    const QString root = normalizePath(projectSourceRoot);
    if (dir == root)
        return true;
    // The directory is part of the project when it contains at least one
    // project file.
    const QString prefix = dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
    QMutexLocker locker(&stateMutex);
    for (const QString &f : m_files) {
        if (f.startsWith(prefix))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// global registry
// ---------------------------------------------------------------------------

QStringList ProjectManager::availableProjects() const
{
    if (!settings)
        return {};

    QStringList names;
    settings->beginGroup(QStringLiteral("Project"));
    settings->beginGroup(QStringLiteral("List"));
    names = settings->childKeys();
    settings->endGroup();
    settings->endGroup();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString ProjectManager::projectPath(const QString &name) const
{
    if (!settings || name.isEmpty())
        return {};
    return settings->value(ProjectListPrefix + name).toString();
}

bool ProjectManager::setCurrentProjectInConfig(const QString &name)
{
    if (!settings)
        return false;
    settings->setValue(CurrentProjectKey, name);
    return true;
}

QString ProjectManager::currentProjectFromConfig() const
{
    if (!settings)
        return {};
    return settings->value(CurrentProjectKey).toString();
}

bool ProjectManager::projectIdentityFromFile(const QString &projectConfigFile, QString *name, QString *dir,
                                             QString *errorMessage)
{
    const QFileInfo info(normalizePath(projectConfigFile));
    if (!info.exists() || !info.isFile()) {
        if (errorMessage)
            *errorMessage = tr("Project file was not found: %1").arg(QDir::toNativeSeparators(projectConfigFile));
        return false;
    }
    if (info.suffix().compare(QLatin1String("codeinsightprj"), Qt::CaseInsensitive) != 0) {
        if (errorMessage)
            *errorMessage = tr("\"%1\" is not a project file (*.codeinsightprj).").arg(info.fileName());
        return false;
    }

    const QString projectName = info.completeBaseName();
    if (!validateProjectName(projectName, errorMessage))
        return false;

    const QString projectDir = normalizePath(info.absolutePath());
    // A project is opened as "<dir>/<name>.codeinsightprj" with the name taken
    // from the registry entry, so the file name has to be usable as that name.
    // A file that only differs in case - possible on a case sensitive file
    // system - would register fine and then fail to load: refuse it now.
    const QString expected = QDir(projectDir).absoluteFilePath(projectName + QStringLiteral(".codeinsightprj"));
    if (!QFileInfo::exists(expected)) {
        if (errorMessage)
            *errorMessage = tr("Expected the project file to be named \"%1\" in %2.")
                                .arg(QFileInfo(expected).fileName(), QDir::toNativeSeparators(projectDir));
        return false;
    }

    if (name)
        *name = projectName;
    if (dir)
        *dir = projectDir;
    return true;
}

bool ProjectManager::registerProject(const QString &projectConfigFile, QString *projectName, QString *errorMessage)
{
    QString name;
    QString dir;
    if (!projectIdentityFromFile(projectConfigFile, &name, &dir, errorMessage))
        return false;

    if (!settings) {
        if (errorMessage)
            *errorMessage = tr("The global project list is not available.");
        return false;
    }

    // Same name, another folder: keep the entry that is already there.
    // Re-pointing it would hide the other folder without ever saying so.
    const QString registered = projectPath(name);
    if (!registered.isEmpty() && normalizePath(registered) != dir) {
        if (errorMessage)
            *errorMessage = tr("A project named \"%1\" is already registered for %2.\n"
                               "Remove that entry first (Project -> Remove Project...) if it is stale.")
                                .arg(name, QDir::toNativeSeparators(registered));
        return false;
    }

    settings->setValue(ProjectListPrefix + name, dir);
    settings->sync(); // the list has to survive a crash right after this point
    if (projectName)
        *projectName = name;
    emit projectListChanged();
    return true;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool ProjectManager::validateProjectName(const QString &name, QString *errorMessage)
{
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9._-]+$"));
    if (!valid.match(name).hasMatch()) {
        if (errorMessage)
            *errorMessage = QObject::tr("Project name may only contain English letters, digits, '.', '-' and '_'.");
        return false;
    }
    return true;
}

bool ProjectManager::createProject(const QString &name, const QString &dataPath, const QString &sourceRoot, QString *errorMessage)
{
    if (!validateProjectName(name, errorMessage))
        return false;

    if (availableProjects().contains(name, Qt::CaseInsensitive)) {
        if (errorMessage)
            *errorMessage = tr("A project named \"%1\" already exists.").arg(name);
        return false;
    }

    QDir dataDir(normalizePath(dataPath));
    if (!dataDir.exists()) {
        // The default data path (~/.codeinsight) usually does not exist yet:
        // create it (and any missing parents) instead of failing.
        if (!QDir().mkpath(dataDir.absolutePath())) {
            if (errorMessage)
                *errorMessage = tr("Project data path does not exist: %1").arg(dataPath);
            return false;
        }
    }
    if (!QFileInfo::exists(normalizePath(sourceRoot))) {
        if (errorMessage)
            *errorMessage = tr("Project sourcecode root path does not exist: %1").arg(sourceRoot);
        return false;
    }

    const QString dir = dataDir.absoluteFilePath(name + QStringLiteral(".codeinsight"));
    if (!QDir().mkpath(dir)) {
        if (errorMessage)
            *errorMessage = tr("Cannot create project folder: %1").arg(dir);
        return false;
    }

    // Project configuration file: write the source root for later use.
    {
        QSettings *cfg = openProjectConfig(dir, name);
        cfg->beginGroup(QStringLiteral("Project"));
        cfg->setValue(QStringLiteral("Name"), name);
        cfg->setValue(QStringLiteral("SourceRoot"), normalizePath(sourceRoot));
        cfg->endGroup();
        cfg->sync();
        delete cfg;
    }

    // Empty file list.
    QFile fileListFile(QDir(dir).absoluteFilePath(name + QStringLiteral(".filelist")));
    fileListFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    fileListFile.close();

    // Register globally and make it the current project.
    settings->setValue(ProjectListPrefix + name, dir);
    setCurrentProjectInConfig(name);
    emit projectListChanged();

    // Adopt it as the open project (empty symbols / file list).
    resetInMemoryState();
    projectName = name;
    projectDirPath = dir;
    projectSourceRoot = normalizePath(sourceRoot);
    emit projectOpened(name);
    return true;
}

void ProjectManager::resetInMemoryState()
{
    QMutexLocker locker(&stateMutex);
    m_files.clear();
    m_symbols.clear();
    m_synced.clear();
    m_changeList.clear();
}

bool ProjectManager::loadProjectConfig()
{
    if (!QFileInfo::exists(projectConfigFilePath()))
        return false;

    QSettings *cfg = openProjectConfig(projectDirPath, projectName);
    cfg->beginGroup(QStringLiteral("Project"));
    projectSourceRoot = normalizePath(cfg->value(QStringLiteral("SourceRoot")).toString());
    cfg->endGroup();
    delete cfg;

    {
        QMutexLocker locker(&stateMutex);
        m_synced = readSyncedEntries();
    }
    loadFileList();
    return true;
}

bool ProjectManager::loadFileList()
{
    QFile file(fileListFilePath());
    QStringList files;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.isEmpty())
                files.append(normalizePath(QString::fromUtf8(line)));
        }
    }
    files.removeDuplicates();
    files.sort(Qt::CaseInsensitive);

    QMutexLocker locker(&stateMutex);
    m_files = files;
    return true;
}

bool ProjectManager::openProject(const QString &name, QString *errorMessage)
{
    const QString dir = projectPath(name);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        if (errorMessage)
            *errorMessage = tr("Project \"%1\" was not found at %2").arg(name, dir);
        return false;
    }

    if (hasProject() && projectName == name)
        return true; // already open

    if (hasProject())
        closeProject();

    projectName = name;
    projectDirPath = dir;
    if (!loadProjectConfig()) {
        // Broken project: still open it, but with empty content.
        projectSourceRoot.clear();
        resetInMemoryState();
    }

    setCurrentProjectInConfig(name);

    // Parse the existing ctags database in the background so opening a large
    // project does not block the UI.
    const QString tagsFile = ctagsFilePath();
    QThread *loader = QThread::create([this, tagsFile]() {
        const QVector<ProjectSymbol> parsed = parseTagsFile(tagsFile);
        {
            QMutexLocker locker(&stateMutex);
            m_symbols = parsed;
        }
        emit projectSymbolsUpdated();
    });
    connect(loader, &QThread::finished, loader, &QObject::deleteLater);
    {
        QMutexLocker locker(&stateMutex);
        activeThreads.append(loader);
    }
    connect(loader, &QThread::finished, this, [this, loader]() {
        QMutexLocker locker(&stateMutex);
        activeThreads.removeAll(loader);
    }, Qt::DirectConnection);
    loader->start();

    emit projectOpened(name);
    return true;
}

void ProjectManager::closeProject()
{
    if (!hasProject())
        return;

    saveFileList();
    resetInMemoryState();
    projectName.clear();
    projectDirPath.clear();
    projectSourceRoot.clear();
    setCurrentProjectInConfig(QString());
    emit projectClosed();
}

bool ProjectManager::removeProject(const QString &name, QString *errorMessage)
{
    const QString dir = projectPath(name);
    if (dir.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("Project \"%1\" is not registered.").arg(name);
        return false;
    }

    if (hasProject() && projectName == name)
        closeProject();

    settings->remove(ProjectListPrefix + name);
    emit projectListChanged();

    if (QDir(dir).exists() && !QDir(dir).removeRecursively()) {
        if (errorMessage)
            *errorMessage = tr("Cannot delete project folder: %1").arg(dir);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// file list
// ---------------------------------------------------------------------------

QStringList ProjectManager::projectFiles() const
{
    QMutexLocker locker(&stateMutex);
    return m_files;
}

QStringList ProjectManager::supportedSourceExtensions()
{
    static const QStringList exts = {
        // C / C++
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("cxx"), QStringLiteral("c++"),
        QStringLiteral("h"), QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx"), QStringLiteral("h++"), QStringLiteral("inl"),
        // others commonly understood by universal-ctags
        QStringLiteral("cs"), QStringLiteral("java"), QStringLiteral("py"), QStringLiteral("js"), QStringLiteral("ts"),
        QStringLiteral("jsx"), QStringLiteral("tsx"), QStringLiteral("go"), QStringLiteral("rs"), QStringLiteral("rb"),
        QStringLiteral("php"), QStringLiteral("swift"), QStringLiteral("kt"), QStringLiteral("kts"), QStringLiteral("scala"),
        QStringLiteral("m"), QStringLiteral("mm"), QStringLiteral("pl"), QStringLiteral("pm"), QStringLiteral("lua"),
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("sql"), QStringLiteral("tcl"),
        QStringLiteral("asm"), QStringLiteral("s"), QStringLiteral("f"), QStringLiteral("for"), QStringLiteral("f90"),
        QStringLiteral("pas"), QStringLiteral("vb"), QStringLiteral("erl"), QStringLiteral("hs"), QStringLiteral("clj"),
    };
    return exts;
}

bool ProjectManager::isSupportedSourceFile(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    return !suffix.isEmpty() && supportedSourceExtensions().contains(suffix);
}

bool ProjectManager::isHiddenFolderName(const QString &name)
{
    if (!name.startsWith(QLatin1Char('.')))
        return false;
    // "." is the folder itself and ".." the one above it: a path written as
    // "./src/main.c" has to keep working, so neither counts as a hidden folder.
    return name != QLatin1String(".") && name != QLatin1String("..");
}

bool ProjectManager::isUnderHiddenFolder(const QString &filePath)
{
    const QString dir = QDir::fromNativeSeparators(QFileInfo(filePath).path());
    const QStringList parts = dir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (isHiddenFolderName(part))
            return true;
    }
    return false;
}

bool ProjectManager::addFiles(const QStringList &paths, bool *changed)
{
    if (changed)
        *changed = false;
    if (!hasProject())
        return false;

    QMutexLocker locker(&stateMutex);
    bool any = false;
    for (const QString &p : paths) {
        const QString norm = normalizePath(p);
        // Non-language files (readme, images, build outputs, ...) are filtered
        // out right away: they carry no symbols and would only slow the
        // ctags/cscope synchronization down.
        if (!isSupportedSourceFile(norm))
            continue;
        // And so is anything a build or a tool parked in a hidden folder:
        // adding a tree used to drag in ".cmake-cache" and friends, which the
        // user then saw sitting in the project file list as if it were source.
        if (isUnderHiddenFolder(norm))
            continue;
        if (!m_files.contains(norm) && QFileInfo::exists(norm)) {
            m_files.append(norm);
            any = true;
        }
    }
    if (any) {
        m_files.sort(Qt::CaseInsensitive);
        locker.unlock();
        saveFileList();
        emit projectFileListChanged();
        if (changed)
            *changed = true;
    }
    return true;
}

bool ProjectManager::removeFiles(const QStringList &paths, bool *changed)
{
    if (changed)
        *changed = false;
    if (!hasProject())
        return false;

    QMutexLocker locker(&stateMutex);
    bool any = false;
    for (const QString &p : paths) {
        const QString norm = normalizePath(p);
        if (m_files.removeOne(norm))
            any = true;
    }
    if (any) {
        locker.unlock();
        saveFileList();
        emit projectFileListChanged();
        if (changed)
            *changed = true;
    }
    return true;
}

bool ProjectManager::removeAllFiles(bool *changed)
{
    if (changed)
        *changed = false;
    if (!hasProject())
        return false;

    // Removing every file is the one removal with nothing left to rebuild the
    // databases from, so it does not have to wait for the synchronization the
    // dialog triggers on close: the symbols of a file list that is empty are
    // gone at the press. The synced snapshot has to go with them - if it
    // survived, adding the same files back would look like "nothing changed on
    // disk" (their mtime is unchanged) and computeDelta() would never list them,
    // so they would never be parsed again.
    const auto dropDatabases = [this]() {
        {
            QMutexLocker locker(&stateMutex);
            m_synced.clear();
            m_symbols.clear();
        }
        QFile::remove(ctagsFilePath());
        QFile::remove(cscopeFilePath());
        writeSyncedEntries(QHash<QString, SyncedEntry>());
        emit projectSymbolsUpdated();
    };

    bool hadFiles = false;
    {
        QMutexLocker locker(&stateMutex);
        hadFiles = !m_files.isEmpty();
        m_files.clear();
    }

    if (!hadFiles) {
        // The list was already empty, but a database of an earlier run may still
        // be on disk: a project whose list had been emptied before this cleanup
        // existed kept a 10 MB *.cscope and 1503 "!_TAG_*" lines of a file set it
        // no longer had. Pressing the button again is the way to clean that up.
        dropDatabases();
        return true;
    }

    saveFileList();
    dropDatabases();
    emit projectFileListChanged();
    if (changed)
        *changed = true;
    return true;
}

bool ProjectManager::saveFileList() const
{
    if (!hasProject())
        return false;

    QStringList copy;
    {
        QMutexLocker locker(&stateMutex);
        copy = m_files;
    }

    QFile file(fileListFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    for (const QString &f : copy)
        file.write((f + QLatin1Char('\n')).toUtf8());
    return true;
}

// ---------------------------------------------------------------------------
// symbols
// ---------------------------------------------------------------------------

QVector<ProjectManager::ProjectSymbol> ProjectManager::symbols() const
{
    QMutexLocker locker(&stateMutex);
    return m_symbols;
}

int ProjectManager::symbolCount() const
{
    QMutexLocker locker(&stateMutex);
    return m_symbols.size();
}

QVector<ProjectManager::ProjectSymbol> ProjectManager::lookupSymbols(const QString &name) const
{
    QMutexLocker locker(&stateMutex);
    QVector<ProjectSymbol> out;
    for (const ProjectSymbol &s : m_symbols) {
        if (s.name == name)
            out.append(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// synchronization
// ---------------------------------------------------------------------------

QHash<QString, ProjectManager::SyncedEntry> ProjectManager::readSyncedEntries() const
{
    QHash<QString, SyncedEntry> out;
    if (!hasProject())
        return out;

    // Read through a fresh QSettings instance: this may run on the background
    // thread and QSettings instances must not be shared across threads.
    QSettings *cfg = openProjectConfig(projectDirPath, projectName);
    const QStringList raw = cfg->value(QStringLiteral("Sync/SyncedFiles")).toStringList();
    delete cfg;

    for (const QString &entry : raw) {
        const QStringList parts = entry.split(QLatin1Char('|'));
        if (parts.size() != 3)
            continue;
        SyncedEntry e;
        e.path = parts.at(0);
        e.mtime = parts.at(1).toLongLong();
        e.size = parts.at(2).toLongLong();
        out.insert(e.path, e);
    }
    return out;
}

bool ProjectManager::writeSyncedEntries(const QHash<QString, SyncedEntry> &entries) const
{
    if (!hasProject())
        return false;

    QStringList raw;
    raw.reserve(entries.size());
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        raw.append(QStringLiteral("%1|%2|%3").arg(it->path, QString::number(it->mtime), QString::number(it->size)));
    }
    raw.sort();

    QSettings *cfg = openProjectConfig(projectDirPath, projectName);
    cfg->setValue(QStringLiteral("Sync/SyncedFiles"), raw);
    cfg->sync();
    delete cfg;
    return true;
}

ProjectManager::SyncDelta ProjectManager::computeDelta(bool force) const
{
    SyncDelta delta;

    QStringList desired;
    QHash<QString, SyncedEntry> synced;
    {
        QMutexLocker locker(&stateMutex);
        desired = m_files;
        synced = m_synced;
    }

    if (force) {
        delta.added = desired;
        delta.removed = synced.keys();
        return delta;
    }

    for (const QString &path : desired) {
        const QFileInfo info(path);
        if (!info.exists())
            continue; // vanished from disk; treat as removed below via synced diff
        const auto it = synced.constFind(path);
        const bool changedOnDisk = it == synced.constEnd()
            || it->mtime != info.lastModified().toMSecsSinceEpoch()
            || it->size != info.size();
        if (changedOnDisk) {
            delta.added.append(path);
            // A modified file also has to drop its previous entries first:
            // ctags --append only de-duplicates byte-identical lines, so the
            // stale entries (old line numbers) would survive otherwise.
            if (it != synced.constEnd())
                delta.removed.append(path);
        }
    }

    QSet<QString> desiredSet(desired.cbegin(), desired.cend());
    for (auto it = synced.constBegin(); it != synced.constEnd(); ++it) {
        if (!desiredSet.contains(it->path) || !QFileInfo::exists(it->path))
            delta.removed.append(it->path);
    }
    return delta;
}

// The tags file is only read back by this program (parseTagsFile accepts either
// ending), so it is written with the platform's own line ending: CRLF on
// Windows, LF on Unix/macOS. Normalising to one style keeps a file that ctags
// wrote on one platform consistent once this program rewrites it.
static QByteArray nativeLineEnding()
{
#if defined(Q_OS_WIN)
    return QByteArray("\r\n");
#else
    return QByteArray("\n");
#endif
}

bool ProjectManager::rewriteTagsWithout(const QStringList &files, QString *errorMessage)
{
    const QString tagsFile = ctagsFilePath();
    QFileInfo info(tagsFile);
    if (!info.exists() || files.isEmpty())
        return true;

    QSet<QString> doomed;
    for (const QString &f : files)
        doomed.insert(normalizePath(f));

    QFile in(tagsFile);
    if (!in.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = tr("Cannot open %1").arg(tagsFile);
        return false;
    }

    // Stream the file through a scratch file and replace it atomically: the old
    // code loaded the whole tags file into a QByteArray (892 MB for the kernel
    // project), which is both a memory risk and the source of the pipe failures
    // seen when the app moves that much data around. The rewrite drops the stale
    // "!_TAG_FILE_SORTED" claim (ctags used to spin forever when it was asked to
    // --append to a file that carried it) and the lines of the doomed files;
    // every other line (including the rest of the metadata) is kept. Line endings
    // stay whatever the file already used, so the result is native to the
    // platform (CRLF on Windows, LF on Unix) rather than a fixed style.
    const QString scratch = tagsFile + QStringLiteral(".rewrite");
    QFile out(scratch);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage)
            *errorMessage = tr("Cannot write %1").arg(scratch);
        return false;
    }

    int keptTagLines = 0;
    const QByteArray eol = nativeLineEnding();
    while (!in.atEnd()) {
        QByteArray rawLine = in.readLine();
        // strip trailing CR/LF (read without QIODevice::Text); the line is
        // written back with the platform's native ending (see nativeLineEnding)
        if (rawLine.endsWith("\r\n"))
            rawLine.chop(2);
        else if (rawLine.endsWith('\n'))
            rawLine.chop(1);

        // tags line: "<name>\t<file>\t<pattern>;\"..."
        const int firstTab = rawLine.indexOf('\t');
        const int secondTab = rawLine.indexOf('\t', firstTab + 1);
        if (firstTab > 0 && secondTab > firstTab) {
            const QString fileField = QString::fromUtf8(rawLine.constData() + firstTab + 1, secondTab - firstTab - 1);
            if (doomed.contains(normalizePath(fileField)))
                continue;
        }
        if (rawLine.startsWith("!_TAG_FILE_SORTED"))
            continue; // ctags must not see this line again
        if (rawLine.startsWith('!')) {
            // keep other metadata
        } else {
            ++keptTagLines;
        }
        out.write(rawLine);
        out.write(eol);
    }
    in.close();
    out.close();

    // Nothing but metadata left: leave a genuinely empty file behind. ctags
    // appends to it again on the next run; the symbol tables of a project
    // without files may not hold anything.
    if (keptTagLines == 0) {
        if (QFileInfo::exists(tagsFile) && !QFile::remove(tagsFile)) {
            if (errorMessage)
                *errorMessage = tr("Cannot remove %1").arg(tagsFile);
            QFile::remove(scratch);
            return false;
        }
        QFile::remove(scratch);
        return true;
    }

    // Atomic replace: remove the old file then move the scratch over it.
    if (QFileInfo::exists(tagsFile) && !QFile::remove(tagsFile)) {
        if (errorMessage)
            *errorMessage = tr("Cannot remove %1").arg(tagsFile);
        QFile::remove(scratch);
        return false;
    }
    if (!QFile::rename(scratch, tagsFile)) {
        if (errorMessage)
            *errorMessage = tr("Cannot replace %1").arg(tagsFile);
        return false;
    }
    return true;
}

// Append the tag entries from one ctags batch output file into the project
// tags file. ctags writes its "!_TAG_*" metadata header at the top of every
// run; only the first copy in the concatenated file would be meaningful, but
// every later one is dropped so the file never carries a stale
// "!_TAG_FILE_SORTED" line (that line makes a later ctags --append loop
// forever). The lines are written with the platform's native ending (see
// nativeLineEnding()): CRLF on Windows, LF on Unix/macOS.
static bool appendBatchToTags(const QString &tagsFile, const QString &batchFile)
{
    QFile in(batchFile);
    if (!in.open(QIODevice::ReadOnly))
        return false;
    QFile out(tagsFile);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    const QByteArray eol = nativeLineEnding();
    while (!in.atEnd()) {
        QByteArray line = in.readLine();
        if (line.startsWith("!_TAG_"))
            continue;
        // Strip whatever ending ctags wrote and re-apply the platform's native
        // one, so the concatenated file stays internally consistent.
        if (line.endsWith("\r\n"))
            line.chop(2);
        else if (line.endsWith('\n'))
            line.chop(1);
        out.write(line);
        out.write(eol);
    }
    return true;
}

bool ProjectManager::runCtags(const QStringList &files, const QString &outputFile, QString *errorMessage,
                              const std::function<bool()> &poll, bool *cancelled)
{
    if (files.isEmpty())
        return true;

    const QString ctags = CtagsSymbolManager::ctagsExecutable();
    if (ctags.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("ctags executable was not found. Install Universal Ctags or set NOTEPADNEXT_CTAGS.");
        return false;
    }

    QStringList args;
    // +n adds the "line:N" extension field: without it the panel cannot jump
    // to the symbol's line.
    args << QStringLiteral("--fields=+n");
    // The tags file only feeds this program and is read line by line, so the
    // order does not matter. Not sorting matters a lot: with its default
    // (--sort=yes) ctags merges and rewrites the whole file on every run, so a
    // synchronization would get slower with every batch.
    args << QStringLiteral("--sort=no");
    // ctags writes this batch into its own file; the caller appends it (dropping
    // the "!_TAG_*" header) to the project tags file. Writing to a file instead
    // of --append avoids re-reading and rewriting the whole growing tags file on
    // every batch - that re-read is what made a kernel-sized project take an
    // hour. Writing to a file (rather than to stdout) also keeps a multi-hundred
    // MB stream off the process pipe.
    args << QStringLiteral("-f") << QDir::toNativeSeparators(outputFile);
    // Absolute forward-slash paths keep the tags file "file" field stable so
    // incremental rewrites can match it.
    for (const QString &f : files)
        args << QDir::fromNativeSeparators(f);

    QFile::remove(outputFile); // start clean in case a previous batch left it
    QProcess proc;
    proc.setWorkingDirectory(projectDirPath);
    // ctags never reads stdin; handing it the null device avoids the anonymous
    // pipe Qt would otherwise create, which fails to open on Windows after the
    // process has already moved hundreds of MB around ("CreateFile failed").
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(ctags, args);
    if (!waitForProcess(proc, poll, cancelled)) {
        if (cancelled && *cancelled)
            return false; // cancellation: the caller reports it, no error text
        QFile::remove(outputFile);
        if (errorMessage)
            *errorMessage = tr("Failed to run ctags: %1").arg(ctags);
        return false;
    }
    if (proc.exitCode() != 0) {
        QFile::remove(outputFile);
        if (errorMessage)
            *errorMessage = tr("ctags failed (%1): %2").arg(proc.exitCode()).arg(QString::fromUtf8(proc.readAllStandardError().left(500)));
        return false;
    }
    return true;
}

bool ProjectManager::runCscope(QString *errorMessage, const std::function<bool()> &poll, bool *cancelled)
{
    const QStringList files = projectFiles();
    if (files.isEmpty()) {
        // The database follows the file list, so it may not outlive it. cscope
        // cannot build an empty database, and this early return used to leave the
        // previous one untouched: after "Remove All" the project had no files at
        // all while a 10 MB "*.cscope" of the old file set stayed behind and kept
        // answering queries about files that are no longer in the project. ctags
        // has the same rule for the tags file, which the caller empties for this
        // case (see rewriteTagsWithout()).
        const QString database = cscopeFilePath();
        if (QFileInfo::exists(database) && !QFile::remove(database)) {
            if (errorMessage)
                *errorMessage = tr("Cannot remove %1").arg(database);
            return false;
        }
        return true;
    }

    const QString cscope = cscopeExecutable();
    if (cscope.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("cscope executable was not found. Install cscope or set NOTEPADNEXT_CSCOPE.");
        return false;
    }

    // This is a rebuild, not an update: cscope keeps the cross references of the
    // files it is not asked to rescan, so the entries of a file that just left the
    // project survived every "full rebuild" - measured with this build: remove a
    // file from the list, synchronize, and Find Callers still reported call sites
    // inside the removed file. The database is therefore always built into a
    // scratch name and moved over the real one once cscope is done: the rebuild
    // starts from nothing, and a run that is cancelled in the middle leaves the
    // previous database in place instead of leaving the project without one.
    const QString database = cscopeFilePath();
    const QString scratchDatabase = database + QStringLiteral(".new");
    QFile::remove(scratchDatabase);
    QStringList args;
    args << QStringLiteral("-b")               // build-only (no TUI)
         << QStringLiteral("-i") << QFileInfo(fileListFilePath()).fileName()
         << QStringLiteral("-f") << QFileInfo(scratchDatabase).fileName();

    QProcess proc;
    // The commonly bundled msys2-based Windows cscope resolves POSIX paths
    // against its own runtime root and rejects Windows-style ones. Run it
    // from the project directory with relative file names and a relative
    // TMPDIR pointing at a scratch folder inside the project. Native cscope
    // builds simply ignore the extra variable and use their system temp dir.
    proc.setWorkingDirectory(projectDirPath);
#if defined(Q_OS_WIN)
    {
        const QString scratch = QDir(projectDirPath).absoluteFilePath(QStringLiteral("tmp"));
        QDir().mkpath(scratch);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("TMPDIR"), QStringLiteral("tmp"));
        proc.setProcessEnvironment(env);
    }
#endif
    // cscope never reads stdin; the null device avoids the anonymous pipe Qt
    // would otherwise open, which can fail to create on Windows after the
    // process has moved a lot of data around.
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(cscope, args);
    if (!waitForProcess(proc, poll, cancelled)) {
        QFile::remove(scratchDatabase); // nothing half-written is kept around
        if (cancelled && *cancelled)
            return false; // cancellation: the caller reports it, no error text
        if (errorMessage)
            *errorMessage = tr("Failed to run cscope: %1").arg(cscope);
        return false;
    }
    // cscope returns 1 when the file list is empty or has unsupported items;
    // only treat real failures as errors.
    if (proc.exitCode() != 0 && proc.exitCode() != 1) {
        QFile::remove(scratchDatabase);
        if (errorMessage)
            *errorMessage = tr("cscope failed (%1): %2").arg(proc.exitCode()).arg(QString::fromUtf8(proc.readAllStandardError().left(500)));
        return false;
    }
    if (!QFileInfo::exists(scratchDatabase)) {
        // It refused the file list and built nothing. What is on disk then is the
        // database of an earlier, larger file set, which would answer every query
        // with files the project no longer has - the same reason a project
        // without files gets its database removed for.
        if (QFileInfo::exists(database) && !QFile::remove(database)) {
            if (errorMessage)
                *errorMessage = tr("Cannot remove %1").arg(database);
            return false;
        }
        return true;
    }
    if ((QFileInfo::exists(database) && !QFile::remove(database))
        || !QFile::rename(scratchDatabase, database)) {
        QFile::remove(scratchDatabase);
        if (errorMessage)
            *errorMessage = tr("Cannot replace %1").arg(database);
        return false;
    }
    return true;
}

QVector<ProjectManager::ProjectSymbol> ProjectManager::findCallers(const QString &name, QString *errorMessage) const
{
    return queryRelations(3, QStringLiteral("callers"), name, errorMessage);
}

QVector<ProjectManager::ProjectSymbol> ProjectManager::findCallees(const QString &name, QString *errorMessage) const
{
    return queryRelations(2, QStringLiteral("callees"), name, errorMessage);
}

// The one cscope lookup behind findCallers() and findCallees(): the two only
// differ in the query number, -3 for "functions calling this symbol" and -2
// for "functions this symbol calls", and in what the relations are called in
// the error texts. Both come back as one entry per call site, sorted by file
// and line.
QVector<ProjectManager::ProjectSymbol> ProjectManager::queryRelations(int query, const QString &what, const QString &name, QString *errorMessage) const
{
    QVector<ProjectSymbol> sites;

    if (name.isEmpty())
        return sites;

    if (!hasProject()) {
        if (errorMessage)
            *errorMessage = tr("Finding %1 needs an open project: the call sites come from its cscope database.").arg(what);
        return sites;
    }

    const QString database = cscopeFilePath();
    if (!QFileInfo::exists(database)) {
        if (errorMessage)
            *errorMessage = tr("The project has no cscope database yet. Synchronize the project files once.");
        return sites;
    }

    const QString cscope = cscopeExecutable();
    if (cscope.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("cscope executable was not found. Install cscope or set NOTEPADNEXT_CSCOPE.");
        return sites;
    }

    // -d: answer from the existing database instead of rebuilding it (a query
    // must never touch the file); -L: plain text on stdout, no TUI; then the
    // query number. The lookup is an exact symbol match, case sensitive, and a
    // name inside a string literal is not a call, so no extra filtering is
    // needed.
    QStringList args;
    args << QStringLiteral("-d")
         << QStringLiteral("-f") << QFileInfo(database).fileName()
         << QStringLiteral("-L")
         << QStringLiteral("-%1").arg(query)
         << name;

    QProcess proc;
    // Same launch conditions as the build run: the msys2-based Windows cscope
    // resolves POSIX paths against its own runtime root, so it is started from
    // the project directory with a relative database name and TMPDIR.
    proc.setWorkingDirectory(projectDirPath);
#if defined(Q_OS_WIN)
    {
        const QString scratch = QDir(projectDirPath).absoluteFilePath(QStringLiteral("tmp"));
        QDir().mkpath(scratch);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("TMPDIR"), QStringLiteral("tmp"));
        proc.setProcessEnvironment(env);
    }
#endif
    // cscope never reads stdin; the null device avoids the anonymous pipe Qt
    // would otherwise open, which can fail to create on Windows after the
    // process has moved a lot of data around.
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(cscope, args);
    if (!proc.waitForStarted(5000) || !proc.waitForFinished(20000)) {
        proc.kill();
        proc.waitForFinished(1000);
        if (errorMessage)
            *errorMessage = tr("Failed to query the cscope database: %1").arg(cscope);
        return sites;
    }

    // One line per call site:  <file> <name> <line> <source text>
    // The file name may contain spaces, so the leading fields are matched
    // lazily: the engine grows "file" until the two fields that follow are a
    // name and a number, which is exactly the file/name/line split. Anything
    // after that number is the source text and is kept as is. The name field
    // is the caller for query 3 and the callee for query 2.
    static const QRegularExpression linePattern(QStringLiteral("^(.*?)\\s+(\\S+)\\s+(\\d+)\\s+(.*)$"));

    QSet<QString> seen;
    const QList<QByteArray> lines = proc.readAllStandardOutput().split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty())
            continue;

        const QRegularExpressionMatch match = linePattern.match(line);
        if (!match.hasMatch())
            continue;

        ProjectSymbol site;
        site.file = normalizePath(match.captured(1));
        site.name = match.captured(2);
        site.line = match.captured(3).toInt();
        site.kind = QStringLiteral("call");
        if (site.file.isEmpty() || site.line <= 0)
            continue;

        // One entry per line: a statement that calls the symbol twice (nested
        // calls) must not show up twice in the chooser.
        const QString key = site.file + QLatin1Char(':') + QString::number(site.line);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        sites.append(site);
    }

    std::sort(sites.begin(), sites.end(), [](const ProjectSymbol &a, const ProjectSymbol &b) {
        const int byFile = QString::compare(a.file, b.file, Qt::CaseInsensitive);
        return byFile != 0 ? byFile < 0 : a.line < b.line;
    });
    return sites;
}

bool ProjectManager::synchronize(const SyncDelta &delta, const ProgressFn &progress, QString *errorMessage, bool *wasCancelled)
{
    if (wasCancelled)
        *wasCancelled = false;

    if (!hasProject())
        return false;

    QMutexLocker syncLocker(&syncMutex);

    if (delta.empty()) {
        if (progress)
            progress(1, 1, tr("Project is up to date"));
        return true;
    }

    const int total = syncStepCount(delta);
    int step = 0;

    // Reports a step and, at the same time, evaluates the cancellation
    // request: a false return aborts the run. The same callback is polled
    // while ctags/cscope are running (see poll()).
    const auto report = [&progress, &step, total](const QString &what) {
        return progress ? progress(step, total, what) : true;
    };
    const auto poll = [&report](const QString &what) {
        return [&report, what]() { return report(what); };
    };

    // Size the main tags file had before this run started appending batches.
    // Cancellation truncates back to it instead of rewriting the whole file.
    qint64 appendStart = 0;
    bool step1Done = false;

    // Set when this run throws the tags file away and rebuilds it from nothing
    // (see step 1): cancelling then just drops the file again.
    bool rebuiltFromScratch = false;

    // Did this run already drop entries of files that are up to date on disk?
    // Only a forced synchronization does that: the incremental pass would not
    // notice that those symbols are missing from the tags file again.
    const auto leftHoles = [this, &delta]() {
        if (delta.removed.isEmpty())
            return false;
        const SyncDelta incremental = computeDelta(/*force=*/false);
        const QSet<QString> reparsed(incremental.added.cbegin(), incremental.added.cend());
        for (const QString &file : delta.removed) {
            if (reparsed.contains(file) || !isProjectFile(file))
                continue;
            if (QFileInfo::exists(file))
                return true;
        }
        return false;
    };

    // Cancellation must leave nothing half-done: the entries appended by this
    // run are dropped again and the synced snapshot is left untouched, so the
    // next synchronization redoes the work. When the run had already wiped
    // entries it cannot restore (forced re-parse), the tags file and the
    // snapshot are discarded instead and the next synchronization rebuilds
    // both from scratch.
    const auto cancel = [&]() {
        const QString tagsFile = ctagsFilePath();
        if (rebuiltFromScratch) {
            // The run started from an empty tags file, so there is nothing
            // worth keeping in it.
            QFile::remove(tagsFile);
        } else if (step1Done && QFileInfo::exists(tagsFile)) {
            // Drop exactly what this run appended: a killed ctags wrote nothing
            // (its output only lands after a clean exit), so the finished
            // batches are all there is to undo. Truncating is O(1) where a
            // rewrite would touch the whole, possibly huge, file again.
            const qint64 currentSize = QFileInfo(tagsFile).size();
            if (currentSize > appendStart)
                QFile::resize(tagsFile, appendStart);
        }
        if (leftHoles()) {
            {
                QMutexLocker locker(&stateMutex);
                m_synced.clear();
            }
            QFile::remove(ctagsFilePath());
            writeSyncedEntries(QHash<QString, SyncedEntry>());
        }
        if (wasCancelled)
            *wasCancelled = true;
        return false;
    };

    // 1. Drop the tags of removed/changed files (they get re-added below).
    ++step;
    if (!report(tr("Removing stale symbols")))
        return cancel();
    if (!delta.removed.isEmpty()) {
        // The tags file may only be thrown away when this run parses *every*
        // file that has entries in it. An ordinary "file saved" run removes one
        // file and re-adds that same one, which looks covered from the delta
        // alone but must keep the entries of all the other files - so the
        // synced snapshot has to be checked as well. A forced synchronization
        // removes exactly that snapshot, which is the case this is for: it is
        // much cheaper for a big project than rewriting the whole tags file on
        // the way to writing the very same entries again.
        QStringList syncedFiles;
        {
            QMutexLocker locker(&stateMutex);
            syncedFiles = m_synced.keys();
        }
        const QSet<QString> removed(delta.removed.cbegin(), delta.removed.cend());
        const QSet<QString> reparsed(delta.added.cbegin(), delta.added.cend());

        bool wholeDatabase = !syncedFiles.isEmpty();
        for (const QString &file : syncedFiles) {
            if (!removed.contains(file)) {
                wholeDatabase = false;
                break;
            }
        }
        if (wholeDatabase) {
            for (const QString &file : delta.removed) {
                if (!reparsed.contains(file) || !isProjectFile(file)) {
                    wholeDatabase = false;
                    break;
                }
            }
        }

        if (wholeDatabase) {
            rebuiltFromScratch = true;
            QFile::remove(ctagsFilePath());
        } else if (!rewriteTagsWithout(delta.removed, errorMessage)) {
            return false;
        }
    }

    // The tags file is now in its pre-ctags state: the file that existed before
    // this run (incremental) or no file at all (rebuilt from scratch). Remember
    // its size so a cancellation can truncate back to it.
    appendStart = QFileInfo::exists(ctagsFilePath()) ? QFileInfo(ctagsFilePath()).size() : 0;
    step1Done = true;

    // 2. ctags, one process per batch of files: cancellable between batches and
    //    while ctags runs. Launching ctags once per file would spend nearly all
    //    of the synchronization time in process creation (see CtagsFilesPerBatch).
    //    Each batch is written to its own temp file and appended to the project
    //    tags file, so ctags never re-reads the whole database.
    const QString batchFile = ctagsFilePath() + QStringLiteral(".batch");
    const QList<QStringList> batches = ctagsBatches(delta.added);
    for (int bi = 0; bi < batches.size(); ++bi) {
        const QStringList &batch = batches.at(bi);
        ++step;
        const QString what = batch.size() == 1
            ? QFileInfo(batch.first()).fileName()
            : tr("Parsing %1 files").arg(batch.size());
        if (!report(what))
            return cancel();

        bool killed = false;
        if (!runCtags(batch, batchFile, errorMessage, poll(what), &killed)) {
            if (killed) {
                // A killed ctags wrote nothing (its output only lands after a
                // clean exit), so the batches already appended are all there is
                // to undo; cancel() truncates the file back to appendStart.
                return cancel();
            }
            return false;
        }
        if (!appendBatchToTags(ctagsFilePath(), batchFile)) {
            if (errorMessage)
                *errorMessage = tr("Cannot append ctags output to %1").arg(ctagsFilePath());
            return false;
        }
    }

    // 3. cscope database (full rebuild; cscope has no incremental mode).
    ++step;
    if (!report(tr("Building cscope database")))
        return cancel();
    {
        bool killed = false;
        if (!runCscope(errorMessage, poll(tr("Building cscope database")), &killed)) {
            if (killed)
                return cancel();
            return false;
        }
    }

    // 4. Refresh the synced snapshot and the in-memory symbol table. The step
    //    is only reported once all of this is done, so the bar reaches 100%
    //    when the run is really over: reparsing the tags file of a big project
    //    takes seconds, and a bar that already said 100% while the window did
    //    not react is exactly what made a finished run look like a hung one.
    ++step;

    QHash<QString, SyncedEntry> synced = readSyncedEntries();
    for (const QString &removed : delta.removed)
        synced.remove(normalizePath(removed));
    for (const QString &added : delta.added) {
        QFileInfo info(added);
        if (!info.exists())
            continue;
        SyncedEntry e;
        e.path = normalizePath(added);
        e.mtime = info.lastModified().toMSecsSinceEpoch();
        e.size = info.size();
        synced.insert(e.path, e);
    }

    if (!reloadSymbols()) {
        if (errorMessage)
            *errorMessage = tr("Cannot parse %1").arg(ctagsFilePath());
        return false;
    }

    {
        QMutexLocker locker(&stateMutex);
        m_synced = synced;
    }
    writeSyncedEntries(synced);

    // Same as the other steps: a refused report cancels, and cancelling here
    // throws away what this run rebuilt (see cancel()).
    if (!report(tr("Updating project symbols")))
        return cancel();

    emit projectSymbolsUpdated();
    return true;
}

bool ProjectManager::synchronizeIncremental(bool force, const ProgressFn &progress, QString *errorMessage, bool *wasCancelled)
{
    if (wasCancelled)
        *wasCancelled = false;

    if (!hasProject())
        return false;
    saveFileList(); // the dialog may have been closed without a save
    const SyncDelta delta = computeDelta(force);
    if (delta.empty()) {
        if (progress)
            progress(1, 1, tr("Project is up to date"));
        return true;
    }
    return synchronize(delta, progress, errorMessage, wasCancelled);
}

bool ProjectManager::reloadSymbols()
{
    const QVector<ProjectSymbol> parsed = parseTagsFile(ctagsFilePath());
    QMutexLocker locker(&stateMutex);
    m_symbols = parsed;
    return true;
}

// ---------------------------------------------------------------------------
// auto sync (background thread)
// ---------------------------------------------------------------------------

void ProjectManager::enqueueFileChange(const QString &filePath)
{
    if (!hasProject())
        return;

    bool startThread = false;
    {
        QMutexLocker locker(&stateMutex);
        const QString norm = normalizePath(filePath);
        if (!m_changeList.contains(norm))
            m_changeList.append(norm);

        if (!syncThreadRunning) {
            syncThreadRunning = true;
            startThread = true;
        }
    }

    if (startThread) {
        QThread *thread = QThread::create([this]() {
            while (true) {
                QStringList todo;
                {
                    QMutexLocker locker(&stateMutex);
                    todo = m_changeList;
                    m_changeList.clear();
                    if (todo.isEmpty() || !syncThreadRunning) {
                        syncThreadRunning = false;
                        break; // drained: exit quickly, as specified
                    }
                }
                const QHash<QString, SyncedEntry> syncedNow = readSyncedEntries();
                // One synchronization for the whole burst. synchronize() always
                // rebuilds the cscope database (cscope has no incremental mode),
                // so calling it per file rebuilt the database once per file:
                // adding or saving 300 files meant 300 full rebuilds.
                SyncDelta changed;
                for (const QString &file : todo) {
                    // Skip files that left the project or the disk meanwhile.
                    if (!isProjectFile(file) || !QFileInfo::exists(file))
                        continue;
                    const QString norm = normalizePath(file);
                    changed.added.append(norm);
                    // A changed file has to drop its previous entries first:
                    // ctags --append only de-duplicates byte-identical lines,
                    // so the stale (old line number) entries would survive.
                    if (syncedNow.contains(norm))
                        changed.removed.append(norm);
                }
                if (!changed.empty())
                    synchronize(changed); // synchronize() takes syncMutex itself
                // Also drop files that disappeared from disk while synced.
                SyncDelta vanished;
                const auto synced = readSyncedEntries();
                for (auto it = synced.constBegin(); it != synced.constEnd(); ++it) {
                    if (!QFileInfo::exists(it->path))
                        vanished.removed.append(it->path);
                }
                if (!vanished.empty())
                    synchronize(vanished);
            }
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        {
            QMutexLocker locker(&stateMutex);
            activeThreads.append(thread);
        }
        connect(thread, &QThread::finished, this, [this, thread]() {
            QMutexLocker locker(&stateMutex);
            activeThreads.removeAll(thread);
        }, Qt::DirectConnection);
        thread->start(QThread::LowPriority);
    }
}

// ---------------------------------------------------------------------------
// cscope executable discovery (mirrors the ctags lookup)
// ---------------------------------------------------------------------------

QString ProjectManager::cscopeExecutable()
{
    const QString envOverride = qEnvironmentVariable("NOTEPADNEXT_CSCOPE");
    QStringList candidates;
    if (!envOverride.isEmpty())
        candidates.append(envOverride);

    const QString appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    candidates << QDir::toNativeSeparators(appDir + QStringLiteral("/cscope.exe"));
#elif defined(Q_OS_MACOS)
    candidates << appDir + QStringLiteral("/cscope")
               << appDir + QStringLiteral("/../Resources/cscope")
               << QStringLiteral("/opt/homebrew/bin/cscope")
               << QStringLiteral("/usr/local/bin/cscope")
               << QStringLiteral("/opt/local/bin/cscope")
               << QStringLiteral("/usr/bin/cscope");
#else
    candidates << appDir + QStringLiteral("/cscope")
               << QStringLiteral("/usr/local/bin/cscope")
               << QStringLiteral("/usr/bin/cscope")
               << QStringLiteral("/snap/bin/cscope");
#endif
    for (const QString &dir : pathDirectories())
        candidates << QDir(dir).absoluteFilePath(QStringLiteral("cscope"));

    QString found = probeCandidates(candidates, "cscope");
    if (found.isEmpty())
        found = probeCandidates(candidates, "scope"); // fallback: any cscope build
    return normalizePath(found);
}

bool ProjectManager::validateCscope(const QString &executable)
{
    if (executable.isEmpty() || !QFileInfo::exists(executable))
        return false;
    QProcess proc;
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(executable, {QStringLiteral("--version")});
    return proc.waitForStarted(3000) && proc.waitForFinished(5000)
        && (proc.readAllStandardOutput() + proc.readAllStandardError()).contains("cscope");
}
