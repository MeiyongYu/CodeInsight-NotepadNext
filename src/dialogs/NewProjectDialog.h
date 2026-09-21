/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * "New Project" dialog: project name + data path + sourcecode root path.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef NEWPROJECTDIALOG_H
#define NEWPROJECTDIALOG_H

#include <QDialog>

#include <functional>

class QLineEdit;
class QPushButton;

class NewProjectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewProjectDialog(QWidget *parent = nullptr);

    QString projectName() const;
    QString dataPath() const;
    QString sourceRoot() const;

    // Runs when OK is pressed, from inside this dialog's own event loop. It has
    // to return an empty string to let the dialog close, or the message to show
    // to the user - in which case the dialog stays exactly as it is, with every
    // field still holding what was typed, so one rejected attempt costs an edit
    // rather than the whole form.
    void setAcceptHandler(std::function<QString()> handler);

public slots:
    void accept() override;

private slots:
    void browseDataPath();
    void browseSourceRoot();
    void validateInput();

private:
    QLineEdit *nameEdit = nullptr;
    QLineEdit *dataPathEdit = nullptr;
    QLineEdit *sourceRootEdit = nullptr;
    QPushButton *okButton = nullptr;
    std::function<QString()> acceptHandler;
};

#endif // NEWPROJECTDIALOG_H
