/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * SymbolIcon implementation: see SymbolIcon.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "SymbolIcon.h"

#include <QColor>
#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QPixmap>

namespace {

QIcon makeKindIcon(const QColor &color, const QString &letter)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(2, 2, 12, 12);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(10);
    p.setFont(f);
    p.drawText(pm.rect(), Qt::AlignCenter, letter);
    return QIcon(pm);
}

} // namespace

QIcon SymbolIcon::icon(const QString &kind)
{
    // The cache is a heap object that is intentionally never deleted: static
    // QIcon/QPixmap objects would be destroyed after QGuiApplication, which is
    // a known crash source on macOS and Linux.
    static QHash<QString, QIcon> *cache = nullptr;
    static QMutex cacheMutex;

    QMutexLocker locker(&cacheMutex);
    if (!cache)
        cache = new QHash<QString, QIcon>();

    const auto it = cache->constFind(kind);
    if (it != cache->constEnd())
        return it.value();

    QIcon icon;
    if (kind == QStringLiteral("f"))
        icon = makeKindIcon(QColor(0x2e9e4f), QStringLiteral("f")); // function
    else if (kind == QStringLiteral("p"))
        icon = makeKindIcon(QColor(0x88b04b), QStringLiteral("p")); // prototype/proto
    else if (kind == QStringLiteral("m"))
        icon = makeKindIcon(QColor(0x1d7f9f), QStringLiteral("m")); // member/module
    else if (kind == QStringLiteral("v") || kind == QStringLiteral("g") || kind == QStringLiteral("e"))
        icon = makeKindIcon(QColor(0xd97706), kind); // variable / global / enumerator
    else if (kind == QStringLiteral("d"))
        icon = makeKindIcon(QColor(0xc2419a), QStringLiteral("d")); // macro
    else if (kind == QStringLiteral("c") || kind == QStringLiteral("s") || kind == QStringLiteral("u"))
        icon = makeKindIcon(QColor(0x2456c7), kind); // class / struct / union
    else if (kind == QStringLiteral("t"))
        icon = makeKindIcon(QColor(0x7c3aed), QStringLiteral("t")); // typedef
    else if (kind == QStringLiteral("n") || kind == QStringLiteral("M"))
        icon = makeKindIcon(QColor(0x0f766e), kind); // namespace / lua module
    else
        icon = makeKindIcon(QColor(0x6b7280), kind.isEmpty() ? QStringLiteral("?") : kind.left(1));

    cache->insert(kind, icon);
    return icon;
}
