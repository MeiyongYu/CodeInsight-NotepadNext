/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * Function list panel implementation.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "FunctionListWidget.h"
#include "SymbolFilter.h"
#include "SymbolIcon.h"

#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHeaderView>

FunctionListWidget::FunctionListWidget(QWidget *parent)
    : QWidget(parent)
{
    titleLabel = new QLabel(this);
    titleLabel->setObjectName(QStringLiteral("functionListTitle"));
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    filterEdit = new QLineEdit(this);
    filterEdit->setObjectName(QStringLiteral("functionListFilter"));
    filterEdit->setPlaceholderText(tr("Symbol Name"));
    filterEdit->setClearButtonEnabled(true);

    // Hint shown while a parse runs, or when a file simply has no symbols.
    statusLabel = new QLabel(this);
    statusLabel->setObjectName(QStringLiteral("functionListStatus"));
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    statusLabel->setVisible(false);
    {
        QFont statusFont = statusLabel->font();
        statusFont.setItalic(true);
        statusLabel->setFont(statusFont);
    }

    tree = new QTreeWidget(this);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setColumnCount(1);
    tree->setFrameShape(QFrame::NoFrame);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(titleLabel);
    layout->addWidget(filterEdit);
    layout->addWidget(statusLabel);
    layout->addWidget(tree, 1);

    connect(filterEdit, &QLineEdit::textChanged, this, &FunctionListWidget::onFilterChanged);
    connect(tree, &QTreeWidget::itemClicked, this, &FunctionListWidget::onItemClicked);
}

void FunctionListWidget::setFileLabel(const QString &fileName)
{
    titleLabel->setText(fileName);
    titleLabel->setToolTip(fileName);
}

void FunctionListWidget::setSymbols(const QVector<FunctionSymbol> &symbols)
{
    QMutexLocker locker(&symbolsMutex);
    this->symbols = symbols;
    rebuildTree();
}

void FunctionListWidget::clearSymbols()
{
    QMutexLocker locker(&symbolsMutex);
    symbols.clear();
    rebuildTree();
}

void FunctionListWidget::setBusy(bool busy)
{
    QMutexLocker locker(&symbolsMutex);
    if (this->busy == busy)
        return;
    this->busy = busy;
    rebuildTree();
}

int FunctionListWidget::symbolCount() const
{
    QMutexLocker locker(&symbolsMutex);
    return symbols.size();
}

QVector<FunctionSymbol> FunctionListWidget::findSymbols(const QString &name) const
{
    QMutexLocker locker(&symbolsMutex);
    QVector<FunctionSymbol> out;
    for (const FunctionSymbol &sym : symbols) {
        if (sym.name == name)
            out.append(sym);
    }
    return out;
}

QString FunctionListWidget::currentFilterText() const
{
    return filterEdit->text();
}

void FunctionListWidget::onFilterChanged(const QString &text)
{
    QMutexLocker locker(&symbolsMutex);
    currentFilter = text;
    rebuildTree();
}

void FunctionListWidget::onItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column)
    if (!item)
        return;
    const int line = item->data(0, Qt::UserRole).toInt();
    if (line > 0)
        emit jumpToLineRequested(line);
}

void FunctionListWidget::rebuildTree()
{
    // NOTE: must be called with symbolsMutex held
    tree->clear();

    const QString filter = currentFilter.trimmed();
    // Joint search: every whitespace separated keyword must occur in the
    // symbol name, so "rec skb" narrows the list down to recv_vlan_skb().
    const QStringList keywords = SymbolFilter::keywords(filter);

    for (const FunctionSymbol &sym : symbols) {
        if (!SymbolFilter::matches(sym.name, keywords))
            continue;

        QTreeWidgetItem *item = new QTreeWidgetItem(tree);
        item->setText(0, sym.name);
        item->setIcon(0, SymbolIcon::icon(sym.kind));
        item->setData(0, Qt::UserRole, sym.line);
        item->setToolTip(0, QStringLiteral("%1  [line %2]").arg(sym.kind, QString::number(sym.line)));
    }

    // Explain an empty list instead of leaving a blank box behind: the panel is
    // opened as soon as ctags claims the file, so "symbols are still being
    // collected" and "this file has none" are two very different states.
    if (tree->topLevelItemCount() > 0) {
        statusLabel->setVisible(false);
        return;
    }

    if (busy)
        statusLabel->setText(tr("Analyzing..."));
    else if (!keywords.isEmpty())
        statusLabel->setText(tr("No symbol matches \"%1\"").arg(filter));
    else
        statusLabel->setText(tr("No symbols found"));

    statusLabel->setVisible(true);
}
