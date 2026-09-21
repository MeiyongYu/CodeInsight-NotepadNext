/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See ProjectFilesDialog.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ProjectFilesDialog.h"
#include "ProjectManager.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int DirRole = Qt::UserRole;      // full path of a directory tree item
constexpr int FileRole = Qt::UserRole + 1; // full path of a file list item
constexpr int IsDirRole = Qt::UserRole + 2;

// The browser only offers what can actually become part of the project. Hidden
// folders cannot - addFiles() refuses them - so listing them would invite the
// user to descend into a folder whose files are then silently rejected.
QFileInfoList visibleDirs(const QString &dir)
{
    QFileInfoList out;
    const QFileInfoList entries =
        QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : entries) {
        if (!ProjectManager::isHiddenFolderName(fi.fileName()))
            out.append(fi);
    }
    return out;
}

// Collects the tree without descending into hidden folders: they are skipped at
// addFiles() anyway, and walking ".git" or ".cmake-cache" first only to throw
// the results away would make "Add Tree" read thousands of entries for nothing.
void collectTreeFiles(const QString &dir, QStringList *out)
{
    const QFileInfoList entries =
        QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : entries) {
        if (fi.isDir()) {
            if (ProjectManager::isHiddenFolderName(fi.fileName()))
                continue;
            collectTreeFiles(fi.absoluteFilePath(), out);
        } else {
            out->append(QDir::cleanPath(fi.absoluteFilePath()));
        }
    }
}
}

ProjectFilesDialog::ProjectFilesDialog(ProjectManager *projectManager, QWidget *parent)
    : QDialog(parent),
      manager(projectManager)
{
    setWindowTitle(tr("Add and Remove Project Files"));
    resize(900, 640);

    auto *layout = new QVBoxLayout(this);

    // --- "File Name" path input ---
    layout->addWidget(new QLabel(tr("File Name:"), this));
    pathEdit = new QLineEdit(this);
    layout->addWidget(pathEdit);

    // --- Directory tree + File Name list ---
    auto *browserRow = new QHBoxLayout;

    dirTree = new QTreeWidget(this);
    dirTree->setHeaderLabel(tr("Directory"));
    dirTree->setUniformRowHeights(true);
    browserRow->addWidget(dirTree, 1);

    fileList = new QTreeWidget(this);
    fileList->setHeaderLabel(tr("File Name"));
    fileList->setRootIsDecorated(false);
    fileList->setUniformRowHeights(true);
    browserRow->addWidget(fileList, 1);

    layout->addLayout(browserRow, 2);

    // --- Project Files ---
    projectList = new QListWidget(this);
    projectList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(new QLabel(tr("Project Files"), this));
    layout->addWidget(projectList, 3);

    // --- right button column ---
    auto *buttonColumn = new QVBoxLayout;
    auto *closeButton = new QPushButton(tr("Close"), this);
    addButton = new QPushButton(tr("Add"), this);
    addAllButton = new QPushButton(tr("Add All"), this);
    addTreeButton = new QPushButton(tr("Add Tree"), this);
    removeTreeButton = new QPushButton(tr("Remove Tree"), this);
    removeFileButton = new QPushButton(tr("Remove File"), this);
    removeAllButton = new QPushButton(tr("Remove All"), this);
    buttonColumn->addWidget(closeButton);
    buttonColumn->addWidget(addButton);
    buttonColumn->addWidget(addAllButton);
    buttonColumn->addWidget(addTreeButton);
    buttonColumn->addWidget(removeTreeButton);
    buttonColumn->addWidget(removeFileButton);
    buttonColumn->addWidget(removeAllButton);
    buttonColumn->addStretch(1);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(projectList, 1);
    bottomRow->addLayout(buttonColumn);
    layout->addLayout(bottomRow, 3);

    // --- wiring ---
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(pathEdit, &QLineEdit::returnPressed, this, &ProjectFilesDialog::onPathEdited);
    connect(dirTree, &QTreeWidget::itemClicked, this, &ProjectFilesDialog::onDirectoryItemClicked);
    connect(dirTree, &QTreeWidget::itemExpanded, this, &ProjectFilesDialog::onDirectoryExpanded);
    connect(fileList, &QTreeWidget::itemDoubleClicked, this, &ProjectFilesDialog::onFileEntryActivated);
    connect(fileList, &QTreeWidget::itemSelectionChanged, this, &ProjectFilesDialog::onFileListSelectionChanged);
    connect(addButton, &QPushButton::clicked, this, &ProjectFilesDialog::addSelected);
    connect(addAllButton, &QPushButton::clicked, this, &ProjectFilesDialog::addAllInCurrentDir);
    connect(addTreeButton, &QPushButton::clicked, this, &ProjectFilesDialog::addTreeFromCurrentDir);
    connect(removeTreeButton, &QPushButton::clicked, this, &ProjectFilesDialog::removeSelectedFromTree);
    connect(removeFileButton, &QPushButton::clicked, this, &ProjectFilesDialog::removeSelectedFromProject);
    connect(removeAllButton, &QPushButton::clicked, this, &ProjectFilesDialog::removeAllFromProject);

    // --- initial content: source root, expanded one level ---
    const QString root = manager->sourceRoot();
    setCurrentDir(root.isEmpty() ? QDir::homePath() : root);

    refreshProjectList();
    onFileListSelectionChanged();
}

void ProjectFilesDialog::setCurrentDir(const QString &dir)
{
    currentDir = QDir::cleanPath(dir);
    pathEdit->setText(QDir::toNativeSeparators(currentDir));

    // Populate the Directory tree root and expand the first level.
    const QFileInfo info(currentDir);
    const QString rootDir = info.isDir() ? currentDir : info.absolutePath();

    dirTree->clear();
    auto *rootItem = new QTreeWidgetItem(dirTree);
    rootItem->setText(0, rootDir);
    rootItem->setData(0, DirRole, rootDir);
    rootItem->setData(0, IsDirRole, true);

    const QFileInfoList dirs = visibleDirs(rootDir);
    for (const QFileInfo &fi : dirs) {
        auto *child = new QTreeWidgetItem(rootItem);
        child->setText(0, fi.fileName());
        child->setData(0, DirRole, QDir::cleanPath(fi.absoluteFilePath()));
        child->setData(0, IsDirRole, true);
        child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator); // lazily filled on expand
    }
    rootItem->setExpanded(true);

    populateFileList(info.isDir() ? currentDir : info.absolutePath());
}

void ProjectFilesDialog::populateFileList(const QString &dir)
{
    fileList->clear();
    QDir d(dir);

    auto *up = new QTreeWidgetItem(fileList);
    up->setText(0, QStringLiteral(".."));
    up->setData(0, FileRole, QDir::cleanPath(d.absoluteFilePath(QStringLiteral(".."))));
    up->setData(0, IsDirRole, true);

    const QFileInfoList dirs = visibleDirs(dir);
    for (const QFileInfo &fi : dirs) {
        auto *item = new QTreeWidgetItem(fileList);
        item->setText(0, fi.fileName());
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setData(0, FileRole, QDir::cleanPath(fi.absoluteFilePath()));
        item->setData(0, IsDirRole, true);
    }
    const QFileInfoList files = d.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : files) {
        auto *item = new QTreeWidgetItem(fileList);
        item->setText(0, fi.fileName());
        item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        item->setData(0, FileRole, QDir::cleanPath(fi.absoluteFilePath()));
        item->setData(0, IsDirRole, false);
    }
}

void ProjectFilesDialog::onPathEdited()
{
    const QString path = QDir::fromNativeSeparators(pathEdit->text().trimmed());
    const QFileInfo info(path);
    if (info.isDir())
        setCurrentDir(path);
    else if (info.exists())
        setCurrentDir(info.absolutePath());
}

void ProjectFilesDialog::onDirectoryExpanded(QTreeWidgetItem *item)
{
    // Lazily fill the children of an expanded directory item.
    if (!item || item->childCount() > 0)
        return;

    const QString dir = item->data(0, DirRole).toString();
    const QFileInfoList subDirs = visibleDirs(dir);
    for (const QFileInfo &fi : subDirs) {
        auto *child = new QTreeWidgetItem(item);
        child->setText(0, fi.fileName());
        child->setData(0, DirRole, QDir::cleanPath(fi.absoluteFilePath()));
        child->setData(0, IsDirRole, true);
        child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

void ProjectFilesDialog::onDirectoryItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    const QString dir = item->data(0, DirRole).toString();
    if (dir.isEmpty())
        return;
    currentDir = dir;
    pathEdit->setText(QDir::toNativeSeparators(dir));
    populateFileList(dir);
}

void ProjectFilesDialog::onFileEntryActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item)
        return;
    const QString path = item->data(0, FileRole).toString();
    const bool isDir = item->data(0, IsDirRole).toBool();
    if (isDir) {
        setCurrentDir(path);
    } else {
        bool changed = false;
        manager->addFiles({path}, &changed);
        refreshProjectList();
    }
}

void ProjectFilesDialog::onFileListSelectionChanged()
{
    // "Add" is enabled when exactly one file (not a directory) is selected.
    const QList<QTreeWidgetItem *> selected = fileList->selectedItems();
    bool hasFile = selected.size() == 1 && !selected.first()->data(0, IsDirRole).toBool();
    addButton->setEnabled(hasFile);
}

QStringList ProjectFilesDialog::selectedProjectFiles() const
{
    QStringList out;
    for (QListWidgetItem *item : projectList->selectedItems())
        out.append(item->text());
    return out;
}

QString ProjectFilesDialog::currentFileListDir() const
{
    return currentDir;
}

void ProjectFilesDialog::addSelected()
{
    const QList<QTreeWidgetItem *> selected = fileList->selectedItems();
    if (selected.size() != 1)
        return;
    const QString path = selected.first()->data(0, FileRole).toString();
    if (selected.first()->data(0, IsDirRole).toBool())
        return;
    manager->addFiles({path});
    refreshProjectList();
}

void ProjectFilesDialog::addAllInCurrentDir()
{
    QDir d(currentDir);
    QStringList paths;
    const QFileInfoList files = d.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : files)
        paths.append(QDir::cleanPath(fi.absoluteFilePath()));
    if (!paths.isEmpty())
        manager->addFiles(paths);
    refreshProjectList();
}

void ProjectFilesDialog::addTreeFromCurrentDir()
{
    QStringList paths;
    collectTreeFiles(currentDir, &paths);
    if (!paths.isEmpty())
        manager->addFiles(paths);
    refreshProjectList();
}

void ProjectFilesDialog::removeSelectedFromTree()
{
    // "Remove Tree": remove every project file located under the current
    // directory from the project.
    const QString prefix = currentDir.endsWith(QLatin1Char('/')) ? currentDir : currentDir + QLatin1Char('/');
    QStringList doomed;
    for (const QString &f : manager->projectFiles()) {
        if (f.startsWith(prefix))
            doomed.append(f);
    }
    if (!doomed.isEmpty())
        manager->removeFiles(doomed);
    refreshProjectList();
}

void ProjectFilesDialog::removeSelectedFromProject()
{
    const QStringList selected = selectedProjectFiles();
    if (!selected.isEmpty())
        manager->removeFiles(selected);
    refreshProjectList();
}

void ProjectFilesDialog::removeAllFromProject()
{
    manager->removeAllFiles();
    refreshProjectList();
}

void ProjectFilesDialog::refreshProjectList()
{
    const QStringList files = manager->projectFiles();
    projectList->clear();
    for (const QString &f : files)
        projectList->addItem(f);
    removeFileButton->setEnabled(!files.isEmpty());
    removeAllButton->setEnabled(!files.isEmpty());
}
