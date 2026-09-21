/*
 * This file is part of Notepad Next.
 * Copyright 2022 Justin Dailey
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


#include "ApplicationSettings.h"
#include "DockTabTitleBar.h"
#include "NotepadNextApplication.h"
#include "RelationForm.h"
#include "SearchResultHighlighterDelegate.h"
#include "SearchResultData.h"
#include "SearchResultsDock.h"
#include "ScintillaNext.h"
#include "ui_SearchResultsDock.h"

#include <QKeyEvent>
#include <QPointer>
#include <QMenu>
#include <QShortcut>
#include <QClipboard>


SearchResultsDock::SearchResultsDock(QWidget *parent) :
    QDockWidget(parent),
    ui(new Ui::SearchResultsDock)
{
    ui->setupUi(this);

    // The two views share this one panel, so the plain dock title text is
    // replaced by tab buttons in the title bar. Tabs and pages line up by
    // index (see PanelPage).
    titleBar = new DockTabTitleBar(this);
    titleBar->addTab(tr("Search Results"));
    titleBar->addTab(tr("Relation Panel"));
    connect(titleBar, &DockTabTitleBar::currentChanged, ui->pageStack, &QStackedWidget::setCurrentIndex);
    setTitleBarWidget(titleBar);
    // The title bar picked its first tab while nothing was connected yet.
    ui->pageStack->setCurrentIndex(titleBar->currentIndex());

    // ---- relation panel page ----
    // The page's whole body - the row of windows (the Context view first,
    // then the relation forms), their geometry, their drags, the layout the
    // settings keep - is RelationPanel's business (see RelationForm.h). The
    // dock hands the page over and relays the two requests that reach the
    // main window.
    relationPanel = new RelationPanel(ui->relationPanelPage);
    connect(relationPanel, &RelationPanel::relationFormRefreshRequested,
            this, &SearchResultsDock::relationFormRefreshRequested);
    connect(relationPanel, &RelationPanel::relationFormOpenRequested,
            this, &SearchResultsDock::relationFormOpenRequested);

    // Close the results when escape is pressed
    new QShortcut(QKeySequence::Cancel, this, this, &SearchResultsDock::close, Qt::WidgetWithChildrenShortcut);

    connect(ui->treeWidget, &QTreeWidget::itemActivated, this, &SearchResultsDock::itemActivated);
    connect(ui->treeWidget, &QTreeWidget::itemExpanded, this, &SearchResultsDock::itemExpanded);
    connect(ui->btnCopyResults, &QToolButton::released,this, &SearchResultsDock::copyAllSearchResultsToClipboard);

    connect(ui->treeWidget, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = ui->treeWidget->itemAt(pos);

        if (item == Q_NULLPTR) {
            return;
        }

        // Create the menu and show it
        QMenu menu(this);
        menu.addAction(tr("Copy"), this, &SearchResultsDock::copySelectedSearchResultsToClipboard);
        menu.addSeparator();
        menu.addAction(tr("Collapse All"), this, &SearchResultsDock::collapseAll);
        menu.addAction(tr("Expand All"), this, &SearchResultsDock::expandAll);
        menu.addSeparator();
        menu.addAction(tr("Delete Entry"), this, [=, this]() { deleteEntry(item); });
        menu.addSeparator();
        menu.addAction(tr("Delete All"), this, &SearchResultsDock::deleteAll);

        menu.exec(QCursor::pos());
    });

    ui->treeWidget->setItemDelegate(new SearchResultHighlighterDelegate(ui->treeWidget));

    ApplicationSettings *settings = qobject_cast<NotepadNextApplication*>(qApp)->getSettings();
    auto updateTreeWidgetFont = [=, this]() {
        QFont f(settings->fontName(), settings->fontSize());
        ui->treeWidget->setFont(f);
        ui->treeWidget->resizeColumnToContents(0);
    };
    connect(settings, &ApplicationSettings::fontNameChanged, this, updateTreeWidgetFont);
    connect(settings, &ApplicationSettings::fontSizeChanged, this, updateTreeWidgetFont);
    updateTreeWidgetFont();
}

SearchResultsDock::~SearchResultsDock()
{
    // Whatever the last change was, it gets its write before the dock goes.
    saveRelationPanelLayout();

    delete ui;
}

// ---- the relation panel: thin delegation -----------------------------------
// Every call goes straight to RelationPanel, the page's body (see
// RelationForm.h); the dock adds nothing of its own.

ContextPanel *SearchResultsDock::contextPanel() const
{
    return relationPanel->contextPanel();
}

void SearchResultsDock::initRelationPanel(ProjectManager *projectManager)
{
    relationPanel->init(projectManager);
}

QList<RelationForm *> SearchResultsDock::relationForms() const
{
    return relationPanel->forms();
}

void SearchResultsDock::saveRelationPanelLayout()
{
    relationPanel->saveLayout();
}

void SearchResultsDock::updateUnlockedRelationRoots(const QString &name, const QString &filePath, int line)
{
    relationPanel->updateUnlockedRoots(name, filePath, line);
}

void SearchResultsDock::showRelationPanel()
{
    // Bringing the relation panel up means opening the dock - both views share
    // it, so it may well be closed - and picking its tab, because the Context
    // view lives on that page.
    show();
    raise();
    titleBar->setCurrentIndex(RelationPanelPage);
}


void SearchResultsDock::newSearch(const QString searchTerm)
{
    show();

    // A new search always brings its own view to the front: the panel can be
    // sitting on the relation panel tab when the search starts.
    titleBar->setCurrentIndex(SearchResultsPage);

    this->searchTerm = searchTerm;

    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); ++i)
    {
        const QTreeWidgetItem* topLevelItem = ui->treeWidget->topLevelItem(i);
        ui->treeWidget->collapseItem(topLevelItem);
    }

    currentSearch = new QTreeWidgetItem();
    ui->treeWidget->insertTopLevelItem(0, currentSearch);

    currentSearch->setBackground(0, QColor(232, 232, 255));
    currentSearch->setForeground(0, QColor(0, 0, 170));
    currentSearch->setExpanded(true);
    currentSearch->setFirstColumnSpanned(true);

    updateSearchStatus();
}

void SearchResultsDock::newFileEntry(ScintillaNext *editor, const QString &filePath)
{
    // Store a QPointer since there is no guarantee this editor will be around later
    QPointer<ScintillaNext> editor_pointer = editor;

    totalFileHitCount = 0;

    if (!filePath.isEmpty())
        currentFilePath = filePath;
    else if (editor != Q_NULLPTR)
        currentFilePath = editor->isFile() ? editor->getFilePath() : editor->getName();
    else
        currentFilePath = QString(); // unsaved, unnamed buffer: nothing to show

    currentFile = new QTreeWidgetItem(currentSearch);
    currentFile->setData(0, Qt::UserRole, QVariant::fromValue(editor_pointer));
    // The path is what lets a hit group whose file is not open yet be opened
    // when one of its results is activated (project wide search).
    currentFile->setData(0, SearchResultData::FilePath, currentFilePath);

    currentFile->setBackground(0, QColor(213, 255, 213));
    currentFile->setForeground(0, QColor(0, 128, 0));
    currentFile->setExpanded(true);
    currentFile->setFirstColumnSpanned(true);

    currentFileCount++;
    updateSearchStatus();
}

void SearchResultsDock::newResultsEntry(const QString line, int lineNumber, int startPositionFromBeginning, int endPositionFromBeginning, int hitCount)
{
    QTreeWidgetItem *item = new QTreeWidgetItem(currentFile);

    // Scintilla internally references line numbers starting at 0, however it needs displayed starting at 1
    item->setText(0, QString::number(lineNumber + 1));
    item->setBackground(0, QBrush(QColor(220, 220, 220)));
    item->setTextAlignment(0, Qt::AlignRight);

    item->setData(1, SearchResultData::LineNumber, lineNumber);
    item->setData(1, SearchResultData::LinePosStart, startPositionFromBeginning);
    item->setData(1, SearchResultData::LinePosEnd, endPositionFromBeginning);
    item->setData(1, SearchResultData::Highlight, true); // <- Flag to enable highlight
    item->setText(1, line);

    totalFileHitCount += hitCount;
    totalHitCount += hitCount;

    updateSearchStatus();
}

void SearchResultsDock::completeSearch()
{
    currentSearch = Q_NULLPTR;
    currentFile = Q_NULLPTR;
    currentFileCount = 0;
    totalFileHitCount = 0;
    totalHitCount = 0;

    ui->treeWidget->resizeColumnToContents(0);
    ui->treeWidget->resizeColumnToContents(1);
}

void SearchResultsDock::collapseAll() const
{
    ui->treeWidget->collapseAll();
}

void SearchResultsDock::expandAll() const
{
    ui->treeWidget->expandAll();
}

void SearchResultsDock::deleteEntry(QTreeWidgetItem *item)
{
    QTreeWidgetItem *parent = item->parent();

    if (parent != Q_NULLPTR) {
        parent->removeChild(item);
        delete item;
    }
    else {
        const int index = ui->treeWidget->indexOfTopLevelItem(item);
        delete ui->treeWidget->takeTopLevelItem(index);
    }
}

void SearchResultsDock::deleteAll()
{
    ui->treeWidget->clear();
}

void SearchResultsDock::itemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);

    // Result entries have no children
    // Make sure the entry has a parent since search entries can have no children
    if (item->childCount() == 0 && item->parent() != Q_NULLPTR) {
        QTreeWidgetItem *fileItem = item->parent();
        QPointer<ScintillaNext> editor = fileItem->data(0, Qt::UserRole).value<QPointer<ScintillaNext>>();
        const QString filePath = fileItem->data(0, SearchResultData::FilePath).toString();

        // The editor may no longer exist - or was never there, which is how a
        // project file that is not open shows up. Either way the path still
        // lets the window open the file, so the hit stays clickable.
        if (editor || !filePath.isEmpty()) {
            int lineNumber = item->data(1, SearchResultData::LineNumber).toInt();
            int startPositionFromBeginning = item->data(1, SearchResultData::LinePosStart).toInt();
            int endPositionFromBeginning = item->data(1, SearchResultData::LinePosEnd).toInt();

            emit searchResultActivated(editor, filePath, lineNumber, startPositionFromBeginning, endPositionFromBeginning);
        }
    }
}

void SearchResultsDock::itemExpanded(QTreeWidgetItem *)
{
    ui->treeWidget->resizeColumnToContents(1);
}

void SearchResultsDock::updateSearchStatus()
{
    currentSearch->setText(0, QStringLiteral("Search \"%1\" (%L2 hits in %L3 files)").arg(searchTerm).arg(totalHitCount).arg(currentFileCount));

    if (currentFile)
        currentFile->setText(0, QStringLiteral("%1 (%L2 hits)").arg(currentFilePath).arg(totalFileHitCount));
}

void SearchResultsDock::copyAllSearchResultsToClipboard()
{
    QStringList results;
    QTreeWidgetItemIterator it(ui->treeWidget);

    while (*it) {
        const QTreeWidgetItem *item = *it;
        results.append(QStringLiteral("%1 %2").arg(item->text(0), item->text(1)));
        ++it;
    }

    QGuiApplication::clipboard()->setText(results.join('\n'));
}

void SearchResultsDock::copySelectedSearchResultsToClipboard()
{
    QStringList results;
    QTreeWidgetItemIterator it(ui->treeWidget);

    while (*it) {
        const QTreeWidgetItem *item = *it;

        if (item->isSelected()) {
            results.append(QStringLiteral("%1 %2").arg(item->text(0), item->text(1)));
        }

        ++it;
    }

    QGuiApplication::clipboard()->setText(results.join('\n'));
}
