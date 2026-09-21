/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * DockTabTitleBar: a drop in replacement for the title bar of a QDockWidget
 * that turns the panel names into flat tab buttons, so that several views can
 * share one dock (the buttons switch the QStackedWidget that holds them).
 *
 * Install it with QDockWidget::setTitleBarWidget(); the dock keeps drawing the
 * frame around it, and the chrome Qt normally puts in the title bar is
 * recreated here: the float/close buttons on the right, dragging the panel to
 * move or undock it, and double clicking to float it. Those last two work
 * because this widget never accepts the mouse events it does not use - the
 * default QWidget handlers ignore them, so they reach the dock widget, which
 * is where Qt implements dragging (QDockWidgetPrivate::mousePressEvent and
 * friends are driven by QDockWidgetLayout::titleArea(), which is the whole
 * title bar strip, custom widget or not).
 *
 * Qt's own title bar buttons are private (QDockWidgetTitleButton), hence the
 * two buttons here. They use the style's standard pixmaps and the same metrics
 * the default title bar is built from, so the strip keeps its native height.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef DOCKTABTITLEBAR_H
#define DOCKTABTITLEBAR_H

#include <QList>
#include <QWidget>

class QDockWidget;
class QHBoxLayout;
class QToolButton;

class DockTabTitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit DockTabTitleBar(QDockWidget *dockWidget);

    // Append a tab button and return its index. The index is meant to be the
    // QStackedWidget page index of the view the tab shows.
    int addTab(const QString &text);

    int currentIndex() const { return currentIndexValue; }
    void setCurrentIndex(int index);

    QSize sizeHint() const override;

signals:
    // The user picked another tab (also emitted for programmatic changes).
    void currentChanged(int index);

protected:
    void changeEvent(QEvent *event) override;

private:
    void updateFloatButton();
    void updateTabAppearance();

    QDockWidget *dock;
    QHBoxLayout *layout;
    QToolButton *floatButton = nullptr;
    QToolButton *closeButton = nullptr;
    QList<QToolButton *> tabButtons;
    int currentIndexValue = -1;
};

#endif // DOCKTABTITLEBAR_H
