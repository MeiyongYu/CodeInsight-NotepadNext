/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See SymbolTreeModel.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "SymbolTreeModel.h"
#include "SymbolFilter.h"
#include "SymbolIcon.h"

#include <QDir>

namespace {
// Returned for a row that is not there (model teardown, a stale index): a
// reference, so the caller does not have to copy a symbol it only reads.
const ProjectManager::ProjectSymbol &noSymbol()
{
    static const ProjectManager::ProjectSymbol none;
    return none;
}
}

SymbolTreeModel::SymbolTreeModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void SymbolTreeModel::setSymbols(QVector<ProjectManager::ProjectSymbol> newSymbols, const QString &sourceRoot)
{
    symbols = std::move(newSymbols);
    root = sourceRoot;
    refilter();
}

void SymbolTreeModel::setFilter(const QString &filter)
{
    const QStringList next = SymbolFilter::keywords(filter);
    if (next == keywords)
        return; // same filter: keep the rows, no reset, no flicker
    keywords = next;
    refilter();
}

void SymbolTreeModel::refilter()
{
    beginResetModel();
    rows.clear();
    rows.reserve(symbols.size());
    for (int i = 0; i < symbols.size(); ++i) {
        if (SymbolFilter::matches(symbols.at(i).name, keywords))
            rows.append(i);
    }
    endResetModel();
}

int SymbolTreeModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : rows.size();
}

int SymbolTreeModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 2;
}

const ProjectManager::ProjectSymbol &SymbolTreeModel::symbolAt(int row) const
{
    if (row < 0 || row >= rows.size())
        return noSymbol();
    return symbols.at(rows.at(row));
}

// "name.h (include/net/...)" style relative path for readability.
QString SymbolTreeModel::displayFile(const QString &file) const
{
    if (!root.isEmpty() && file.startsWith(root + QLatin1Char('/')))
        return file.mid(root.size() + 1);
    return file;
}

QVariant SymbolTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rows.size())
        return {};
    const ProjectManager::ProjectSymbol &symbol = symbolAt(index.row());
    const bool onSymbolColumn = index.column() == SymbolColumn;
    switch (role) {
    case Qt::DisplayRole:
        return onSymbolColumn ? symbol.name : displayFile(symbol.file);
    case Qt::DecorationRole:
        return onSymbolColumn ? SymbolIcon::icon(symbol.kind) : QVariant();
    case Qt::ToolTipRole:
        return onSymbolColumn ? QVariant() : QDir::toNativeSeparators(symbol.file);
    case PathRole:
        return onSymbolColumn ? symbol.file : QVariant();
    case LineRole:
        return onSymbolColumn ? symbol.line : QVariant();
    default:
        return {};
    }
}

QVariant SymbolTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return section == SymbolColumn ? tr("Symbol") : tr("File Name");
}
