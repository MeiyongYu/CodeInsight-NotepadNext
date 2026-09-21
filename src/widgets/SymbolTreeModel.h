/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * SymbolTreeModel: the lazy row model behind the "Project Symbols" list.
 *
 * The panel used to build one QTreeWidgetItem per match. That works for the
 * couple of thousand rows it was capped at and is hopeless for a real project:
 * the Linux kernel holds ~490k symbols, and building that many widget items
 * froze the window for seconds every time the database changed - including
 * right when a synchronization reached 100%, with the progress dialog unable
 * to repaint.
 *
 * A model is only asked for the rows that are on screen, so the cap can go:
 * every match becomes a row, the scrollbar covers the whole result set, and
 * rebuilding costs one pass over the symbol table plus one int per match. The
 * filter box re-runs that pass on its own, without touching the symbol data.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef SYMBOLTREEMODEL_H
#define SYMBOLTREEMODEL_H

#include "ProjectManager.h"

#include <QAbstractTableModel>
#include <QStringList>
#include <QVector>

class SymbolTreeModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { SymbolColumn = 0, FileColumn = 1 };
    enum Role {
        PathRole = Qt::UserRole,     // absolute path of the symbol's file
        LineRole = Qt::UserRole + 1, // 1-based line
    };

    explicit SymbolTreeModel(QObject *parent = nullptr);

    // New symbol table, plus the source root used to shorten the file column.
    void setSymbols(QVector<ProjectManager::ProjectSymbol> newSymbols, const QString &sourceRoot);
    // Show only the symbols whose name holds every keyword; an empty filter
    // shows all of them.
    void setFilter(const QString &filter);

    // Matches currently listed. The rows behind them are never materialised.
    int matchCount() const { return rows.size(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    // Recompute the row list: the only thing a filter change has to do.
    void refilter();
    QString displayFile(const QString &file) const;
    const ProjectManager::ProjectSymbol &symbolAt(int row) const;

    QVector<ProjectManager::ProjectSymbol> symbols;
    QVector<int> rows; // one index into symbols per listed row
    QStringList keywords;
    QString root;
};

#endif // SYMBOLTREEMODEL_H
