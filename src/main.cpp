/*
 * This file is part of Notepad Next.
 * Copyright 2019 Justin Dailey
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


#include <QDebug>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QApplication>
#include <QDataStream>

#include "NotepadNextApplication.h"

int main(int argc, char *argv[])
{
    qSetMessagePattern("[%{time process}] %{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}%{if-critical}C%{endif}%{if-fatal}F%{endif}: %{message}");

    // A development / regression run (NOTEPADNEXT_PROJECT_SELFCHECK) executes
    // next to a normal instance without sharing its configuration: the settings
    // file goes to a scratch directory and Qt's test mode keeps the session and
    // the rest of the user's app data untouched.
    const bool selfCheck = qEnvironmentVariableIsSet("NOTEPADNEXT_PROJECT_SELFCHECK");
    if (selfCheck) {
        const QString sandbox = QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("npn_selfcheck"));
        // A killed or crashed run leaves its project registration behind (the
        // settings file survives the process), which would make the next run
        // fail with "a project named ProjCheck already exists": always start
        // from a clean scratch directory.
        QDir(sandbox).removeRecursively();
        QDir().mkpath(sandbox);
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, sandbox);
        qInfo("Self-check mode: settings in %s, app data in %s",
              qUtf8Printable(sandbox),
              qUtf8Printable(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
    }

    // Set these since other parts of the app references these
    QApplication::setOrganizationName("NotepadNext");
    QApplication::setApplicationName("NotepadNext");
    QGuiApplication::setApplicationDisplayName("Notepad Next");
    QGuiApplication::setApplicationVersion(APP_VERSION);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    // Default settings format
    QSettings::setDefaultFormat(QSettings::IniFormat);

    NotepadNextApplication app(argc, argv);

    // Log some debug info
    qInfo("=============================");
    for(const auto &d : app.debugInfo()){
        qInfo("%s", qUtf8Printable(d));
    }
    qInfo("=============================");


    // The self-check runs as its own primary so that a normal instance does not
    // swallow it (SingleApplication identifies instances independently of the
    // executable path).
    if(selfCheck || app.isPrimary()) {
        app.init();

        return app.exec();
    }
    else {
        qInfo() << "Primary instance already running. PID:" << app.primaryPid();

        app.sendInfoToPrimaryInstance();

        qInfo() << "Secondary instance closing...";

        app.exit(0);

        return 0;
    }
}
