/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * Joint keyword matching shared by the symbol filter boxes ("Project Symbols"
 * panel and function list panel).
 *
 * A filter string is a list of whitespace separated keywords and a symbol
 * matches only when *every* keyword occurs in its name (case insensitive), so
 * "rec skb" finds "recv_vlan_skb" while "skb rec" finds the same entry. A
 * single keyword keeps the plain substring behaviour both panels had before.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef SYMBOLFILTER_H
#define SYMBOLFILTER_H

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace SymbolFilter {

// Keywords of a filter string. Splitting on runs of whitespace (rather than on
// a single space) makes "  rec   skb  " and tab separated keywords work too.
inline QStringList keywords(const QString &filter)
{
    return filter.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// True when every keyword occurs in name. An empty keyword list matches
// everything, i.e. an empty filter box shows the whole symbol list.
inline bool matches(const QString &name, const QStringList &keywords)
{
    for (const QString &keyword : keywords) {
        if (!name.contains(keyword, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

} // namespace SymbolFilter

#endif // SYMBOLFILTER_H
