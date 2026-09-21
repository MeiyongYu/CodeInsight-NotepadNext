/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * Project list dialog shared by "Open Project" and "Remove Project".
 * Lists the registered projects as "name (path)"; double click acts as OK.
 *
 * "Open Project" also carries a "browse.." button in its lower left corner: it
 * picks a <name>.codeinsightprj straight from the file system, for the case
 * where the global project list lost its entries (a reinstalled machine, a
 * fresh settings file) while the project folders are still on disk.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef PROJECTLISTDIALOG_H
#define PROJECTLISTDIALOG_H

#include <QDialog>
#include <QHash>

#include <functional>

class QLineEdit;
class QListWidget;
class QPushButton;

class ProjectListDialog : public QDialog
{
    Q_OBJECT

public:
    // What the "browse.." handler did with the chosen project file.
    // Opened:    the project is registered and open now, so the dialog can go.
    // Cancelled: the user backed out (no warning, the dialog stays as it is).
    // Failed:    the file could not be used; errorMessage says why.
    enum class BrowseOutcome { Opened, Cancelled, Failed };

    // mode "open": OK button reads "OK"; mode "remove": OK button reads "Remove"
    explicit ProjectListDialog(const QStringList &projectNames, const QHash<QString, QString> &projectPaths,
                               bool removeMode, QWidget *parent = nullptr);

    QString selectedProject() const;

    // Registers and opens a project file found by hand - the whole "browse.."
    // flow except for the file chooser. Runs from inside this dialog's event
    // loop, and only an Opened outcome closes the window: a cancelled or
    // refused attempt leaves it exactly as it is, so one bad pick costs
    // another pick rather than the whole window. Without a handler "browse.."
    // is not offered at all (the Remove mode does not set one).
    void setBrowseHandler(std::function<BrowseOutcome(const QString &projectFile, QString *errorMessage)> handler);
    // Where the file chooser starts. Ignored when it is empty or gone.
    void setStartDirectory(const QString &dir);
    // Replaces QFileDialog::getOpenFileName for the regression run, which
    // cannot drive a file dialog offscreen.
    using ProjectFileChooser = std::function<QString(const QString &startDir)>;
    void setProjectFileChooser(ProjectFileChooser chooser);
    // The "browse.." button, so the regression run can press it like a user.
    QPushButton *browseProjectFileButton() const { return browseButton; }

private:
    void browseForProjectFile();

    QLineEdit *nameEdit = nullptr;
    QListWidget *listWidget = nullptr;
    QPushButton *okButton = nullptr;
    QPushButton *browseButton = nullptr;
    QString startDirectory;
    std::function<BrowseOutcome(const QString &, QString *)> browseHandler;
    ProjectFileChooser projectFileChooser;
    bool removeMode = false;
};

#endif // PROJECTLISTDIALOG_H
