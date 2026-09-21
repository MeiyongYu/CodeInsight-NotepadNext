/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See ProjectPanelDock.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ProjectPanelDock.h"
#include "ProjectManager.h"
#include "SymbolTreeModel.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int PathRole = Qt::UserRole;     // absolute path
constexpr int LineRole = Qt::UserRole + 1; // 1-based line (symbols)
constexpr int IsDirRole = Qt::UserRole + 2;

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString::number(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}
}

ProjectPanelDock::ProjectPanelDock(ProjectManager *projectManager, QWidget *parent)
    : QDockWidget(parent),
      manager(projectManager)
{
    setObjectName(QStringLiteral("projectPanelDock"));
    setWindowTitle(tr("Project"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("projectTabs"));

    // ------------------------------------------------------------------
    // 1. Project Symbols (leftmost tab, per spec)
    // ------------------------------------------------------------------
    auto *symbolPage = new QWidget(this);
    symbolFilter = new QLineEdit(symbolPage);
    symbolFilter->setObjectName(QStringLiteral("projectSymbolFilter"));
    symbolFilter->setPlaceholderText(tr("Symbol Name"));
    symbolFilter->setClearButtonEnabled(true);
    symbolModel = new SymbolTreeModel(this);
    symbolTree = new QTreeView(symbolPage);
    symbolTree->setObjectName(QStringLiteral("projectSymbolTree"));
    symbolTree->setModel(symbolModel);
    symbolTree->setRootIsDecorated(false);
    symbolTree->setUniformRowHeights(true);
    symbolTree->setAllColumnsShowFocus(true);
    symbolTree->header()->setSectionResizeMode(SymbolTreeModel::SymbolColumn, QHeaderView::ResizeToContents);
    auto *symbolLayout = new QVBoxLayout(symbolPage);
    symbolLayout->setContentsMargins(2, 2, 2, 2);
    symbolLayout->addWidget(symbolFilter);
    symbolLayout->addWidget(symbolTree, 1);
    tabs->addTab(symbolPage, tr("Project Symbols"));

    connect(symbolFilter, &QLineEdit::textChanged, this, [this](const QString &text) { applySymbolFilter(text); });
    // Single click jumps, like the function list panel. (Double click alone
    // breaks after a jump: the focus has moved to the editor, so the first
    // click back into the panel is swallowed as a focus click and no
    // double-click sequence is detected.)
    const auto jumpToRow = [this](const QModelIndex &index) {
        const QString file = index.data(SymbolTreeModel::PathRole).toString();
        if (!file.isEmpty())
            emit jumpToSymbolRequested(file, index.data(SymbolTreeModel::LineRole).toInt());
    };
    connect(symbolTree, &QTreeView::clicked, this, jumpToRow);
    connect(symbolTree, &QTreeView::doubleClicked, this, jumpToRow);

    // ------------------------------------------------------------------
    // 2. Project Files (middle tab)
    // ------------------------------------------------------------------
    auto *filePage = new QWidget(this);
    fileFilter = new QLineEdit(filePage);
    fileFilter->setPlaceholderText(tr("File Name"));
    fileTree = new QTreeWidget(filePage);
    fileTree->setObjectName(QStringLiteral("projectFileTree"));
    fileTree->setColumnCount(4);
    fileTree->setHeaderLabels({tr("File Name"), tr("Directory"), tr("Size"), tr("Modified")});
    fileTree->setRootIsDecorated(false);
    fileTree->setUniformRowHeights(true);
    fileTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    auto *fileLayout = new QVBoxLayout(filePage);
    fileLayout->setContentsMargins(2, 2, 2, 2);
    fileLayout->addWidget(fileFilter);
    fileLayout->addWidget(fileTree, 1);
    tabs->addTab(filePage, tr("Project Files"));

    connect(fileFilter, &QLineEdit::textChanged, this, [this](const QString &) { refreshFiles(); });
    // Single click opens, matching the Project Symbols panel (see above).
    connect(fileTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item)
            emit openFileRequested(item->data(0, PathRole).toString());
    });
    connect(fileTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item)
            emit openFileRequested(item->data(0, PathRole).toString());
    });

    // ------------------------------------------------------------------
    // 3. Folders (rightmost tab)
    // ------------------------------------------------------------------
    auto *folderPage = new QWidget(this);
    folderPathEdit = new QLineEdit(folderPage);
    folderPathEdit->setReadOnly(true);

    folderTree = new QTreeWidget(folderPage);
    folderTree->setObjectName(QStringLiteral("projectFolderTree"));
    folderTree->setHeaderLabel(tr("Directory"));
    folderTree->setUniformRowHeights(true);

    folderFileList = new QTreeWidget(folderPage);
    folderFileList->setObjectName(QStringLiteral("projectFolderFileList"));
    folderFileList->setHeaderLabel(tr("File Name"));
    folderFileList->setRootIsDecorated(false);
    folderFileList->setUniformRowHeights(true);

    auto *folderSplitter = new QSplitter(Qt::Horizontal, folderPage);
    folderSplitter->addWidget(folderTree);
    folderSplitter->addWidget(folderFileList);
    folderSplitter->setStretchFactor(0, 1);
    folderSplitter->setStretchFactor(1, 1);

    auto *folderLayout = new QVBoxLayout(folderPage);
    folderLayout->setContentsMargins(2, 2, 2, 2);
    folderLayout->addWidget(folderPathEdit);
    folderLayout->addWidget(folderSplitter, 1);
    foldersTabIndex = tabs->addTab(folderPage, tr("Folders"));

    connect(folderTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        const QString dir = item->data(0, PathRole).toString();
        folderPathEdit->setText(QDir::toNativeSeparators(dir));
        // First level listing of the clicked directory.
        populateFolderFileList(dir);
    });

    // Level-by-level expansion: a directory's subdirectories are only read
    // from disk when the item is expanded the first time.
    connect(folderTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        fillFolderChildren(item);
    });

    connect(folderFileList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        if (item->data(0, IsDirRole).toBool())
            return; // navigating happens through the Directory tree
        emit openFileRequested(item->data(0, PathRole).toString());
    });

    // Coming back to the Folders tab re-applies the last editor file.
    connect(tabs, &QTabWidget::currentChanged, this, [this](int) {
        syncFoldersToEditorFile();
    });

    setWidget(tabs);
}

void ProjectPanelDock::applySymbolFilter(const QString &filter)
{
    symbolModel->setFilter(filter);
    // Back to the first match, the way the list behaved while it still cleared
    // itself on every filter change.
    symbolTree->scrollToTop();
}

void ProjectPanelDock::refreshSymbols()
{
    // The whole table goes to the model, not a capped slice: a real project
    // holds hundreds of thousands of symbols (Linux kernel: ~470k) and every
    // one of them has to be reachable through the scrollbar. Nothing is built
    // here - the model hands out rows on demand (see SymbolTreeModel).
    symbolModel->setSymbols(manager->symbols(), manager->sourceRoot());
    symbolTree->scrollToTop();
}

void ProjectPanelDock::refreshFiles()
{
    fileTree->clear();
    const QString needle = fileFilter->text().trimmed();
    const QStringList files = manager->projectFiles();

    for (const QString &f : files) {
        const QFileInfo info(f);
        if (!needle.isEmpty() && !info.fileName().contains(needle, Qt::CaseInsensitive))
            continue;
        auto *item = new QTreeWidgetItem(fileTree);
        item->setText(0, info.fileName());
        item->setText(1, QDir::toNativeSeparators(info.absolutePath()));
        item->setText(2, humanSize(info.size()));
        item->setText(3, info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setData(0, PathRole, f);
    }
}

void ProjectPanelDock::fillFolderChildren(QTreeWidgetItem *dirItem)
{
    if (!dirItem || dirItem->childCount() > 0)
        return;

    const QString dir = dirItem->data(0, PathRole).toString();
    const QFileInfoList dirs = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : dirs) {
        auto *child = new QTreeWidgetItem(dirItem);
        child->setText(0, fi.fileName());
        child->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        child->setData(0, PathRole, QDir::cleanPath(fi.absoluteFilePath()));
        child->setData(0, IsDirRole, true);
        child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator); // filled when expanded
    }
}

void ProjectPanelDock::populateFolderFileList(const QString &dir)
{
    folderFileList->clear();
    QDir d(dir);

    auto *up = new QTreeWidgetItem(folderFileList);
    up->setText(0, QStringLiteral(".."));
    up->setData(0, PathRole, QDir::cleanPath(d.absoluteFilePath(QStringLiteral(".."))));
    up->setData(0, IsDirRole, true);

    const QFileInfoList entries = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : entries) {
        auto *item = new QTreeWidgetItem(folderFileList);
        item->setText(0, fi.fileName());
        item->setIcon(0, style()->standardIcon(fi.isDir() ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        item->setData(0, PathRole, QDir::cleanPath(fi.absoluteFilePath()));
        item->setData(0, IsDirRole, fi.isDir());
    }
}

void ProjectPanelDock::revealDirectoryInFolderTree(const QString &dir)
{
    if (folderRoot.isEmpty() || dir.isEmpty() || folderTree->topLevelItemCount() == 0)
        return;

    const QString root = QDir::cleanPath(folderRoot);
    const QString target = QDir::cleanPath(dir);
    if (target != root && !target.startsWith(root + QLatin1Char('/')))
        return; // outside of the project source root

    // Walk down from the root item, expanding and lazily filling one level
    // at a time until the target directory is reached.
    QTreeWidgetItem *item = folderTree->topLevelItem(0);
    QString acc = root;
    const QStringList parts = target == root
        ? QStringList()
        : target.mid(root.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);

    for (const QString &part : parts) {
        if (!item)
            return;
        fillFolderChildren(item);

        acc += QLatin1Char('/') + part;
        QTreeWidgetItem *next = nullptr;
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem *child = item->child(i);
            if (child->data(0, PathRole).toString() == acc) {
                next = child;
                break;
            }
        }
        if (!next)
            return; // directory missing on disk meanwhile: stop here
        item->setExpanded(true);
        item = next;
    }

    if (item) {
        folderTree->setCurrentItem(item);
        folderTree->scrollToItem(item);
    }
}

void ProjectPanelDock::syncFoldersToEditorFile()
{
    if (!isVisible() || pendingEditorFile.isEmpty() || folderRoot.isEmpty())
        return;
    if (tabs->currentIndex() != foldersTabIndex)
        return;

    const QFileInfo info(pendingEditorFile);
    if (!info.exists())
        return;

    const QString dir = QDir::cleanPath(info.absolutePath());
    revealDirectoryInFolderTree(dir);
    folderPathEdit->setText(QDir::toNativeSeparators(dir));
    populateFolderFileList(dir);
}

void ProjectPanelDock::resetFolders()
{
    folderRoot = manager->sourceRoot();
    if (folderRoot.isEmpty())
        return;

    folderTree->clear();
    auto *rootItem = new QTreeWidgetItem(folderTree);
    rootItem->setText(0, folderRoot);
    rootItem->setData(0, PathRole, folderRoot);
    rootItem->setData(0, IsDirRole, true);
    rootItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));

    fillFolderChildren(rootItem);
    rootItem->setExpanded(true);

    folderPathEdit->setText(QDir::toNativeSeparators(folderRoot));

    // First level file list of the root.
    populateFolderFileList(folderRoot);
}

void ProjectPanelDock::trackEditorFile(const QString &filePath)
{
    // Remember the file even while the dock is hidden or another tab is
    // shown; the Folders page is brought up to date as soon as it becomes
    // visible (spec: the Directory tree always points at the directory of
    // the file open in the editor).
    pendingEditorFile = filePath;
    syncFoldersToEditorFile();
}
