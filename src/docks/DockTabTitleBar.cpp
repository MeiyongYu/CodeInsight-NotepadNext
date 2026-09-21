/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See DockTabTitleBar.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "DockTabTitleBar.h"

#include <QButtonGroup>
#include <QDockWidget>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QStyle>
#include <QToolButton>

namespace {

QString rgb(const QColor &color)
{
    return QStringLiteral("rgb(%1,%2,%3)").arg(color.red()).arg(color.green()).arg(color.blue());
}

QString rgba(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

} // namespace

DockTabTitleBar::DockTabTitleBar(QDockWidget *dockWidget)
    : QWidget(dockWidget),
      dock(dockWidget)
{
    setObjectName(QStringLiteral("dockTabTitleBar"));

    layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 1, 1, 1);
    layout->setSpacing(0);
    layout->addStretch(1);

    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    const int buttonMargin = style()->pixelMetric(QStyle::PM_DockWidgetTitleBarButtonMargin, nullptr, this);

    floatButton = new QToolButton(this);
    floatButton->setObjectName(QStringLiteral("dockFloatButton"));
    floatButton->setAutoRaise(true);
    floatButton->setFocusPolicy(Qt::NoFocus);
    floatButton->setIconSize(QSize(iconSize, iconSize));
    floatButton->setFixedSize(iconSize + 2 * buttonMargin, iconSize + 2 * buttonMargin);
    floatButton->setToolTip(tr("Float"));
    layout->addWidget(floatButton);

    closeButton = new QToolButton(this);
    closeButton->setObjectName(QStringLiteral("dockCloseButton"));
    closeButton->setAutoRaise(true);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setIconSize(QSize(iconSize, iconSize));
    closeButton->setFixedSize(iconSize + 2 * buttonMargin, iconSize + 2 * buttonMargin);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, this));
    closeButton->setToolTip(tr("Close"));
    layout->addWidget(closeButton);

    // Same two connections QDockWidgetPrivate::init() makes for its own
    // buttons (_q_toggleTopLevel and close), the float one only honored
    // while the dock is actually floatable.
    connect(floatButton, &QToolButton::clicked, this, [this]() {
        if (dock->features() & QDockWidget::DockWidgetFloatable)
            dock->setFloating(!dock->isFloating());
    });
    connect(closeButton, &QToolButton::clicked, dockWidget, &QDockWidget::close);
    connect(dock, &QDockWidget::topLevelChanged, this, [this]() { updateFloatButton(); });
    connect(dock, &QDockWidget::featuresChanged, this, [this]() { updateFloatButton(); });

    updateFloatButton();
}

int DockTabTitleBar::addTab(const QString &text)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("dockTabButton"));
    button->setText(text);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);

    const int index = tabButtons.size();
    tabButtons.append(button);
    layout->insertWidget(index, button);

    // Exclusive so that clicking the current tab cannot uncheck every tab.
    auto *group = findChild<QButtonGroup *>(QStringLiteral("dockTabGroup"));
    if (group == nullptr) {
        group = new QButtonGroup(this);
        group->setObjectName(QStringLiteral("dockTabGroup"));
        group->setExclusive(true);
    }
    group->addButton(button, index);

    connect(button, &QToolButton::clicked, this, [this, index]() { setCurrentIndex(index); });

    updateTabAppearance();

    if (currentIndexValue < 0)
        setCurrentIndex(0);

    return index;
}

void DockTabTitleBar::setCurrentIndex(int index)
{
    if (index < 0 || index >= tabButtons.size() || index == currentIndexValue)
        return;

    currentIndexValue = index;
    for (int i = 0; i < tabButtons.size(); ++i)
        tabButtons.at(i)->setChecked(i == index);

    updateTabAppearance();
    emit currentChanged(index);
}

QSize DockTabTitleBar::sizeHint() const
{
    // QDockWidgetLayout::titleHeight() asks the title bar widget for its
    // height when a custom one is installed, so this is what sizes the strip.
    // Keep the height the default title bar would have had (its buttons plus
    // the frame margin, never less than the caption height).
    const int buttonHeight = floatButton->sizeHint().height();
    const int titleMargin = style()->pixelMetric(QStyle::PM_DockWidgetTitleMargin, nullptr, this);
    const int captionHeight = style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, this);

    int height = qMax(buttonHeight + 2, QFontMetrics(font()).height() + 2 * titleMargin);
    height = qMax(height, layout->sizeHint().height());
    height = qMax(height, captionHeight);

    return QSize(QWidget::sizeHint().width(), height);
}

void DockTabTitleBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);

    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange)
        updateTabAppearance();
}

void DockTabTitleBar::updateFloatButton()
{
    // A dock that may not leave its area (the search results dock owns the
    // full bottom of the window) gets no float button at all.
    if (!(dock->features() & QDockWidget::DockWidgetFloatable)) {
        floatButton->setVisible(false);
        return;
    }

    floatButton->setVisible(true);
    floatButton->setIcon(style()->standardIcon(
        dock->isFloating() ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton,
        nullptr, this));
    floatButton->setToolTip(dock->isFloating() ? tr("Dock") : tr("Float"));
}

void DockTabTitleBar::updateTabAppearance()
{
    // The tab strip only says which view is in front, so the inactive tabs are
    // dimmed instead of bold: that keeps every button the same width, and the
    // strip therefore does not reflow when the user switches tabs.
    const QColor text = palette().color(QPalette::WindowText);

    QColor idle = text;
    idle.setAlpha(140);
    QColor hover = text;
    hover.setAlpha(24);

    const QString style = QStringLiteral(
                              "QToolButton {"
                              "  border: none;"
                              "  border-bottom: 2px solid transparent;"
                              "  background: transparent;"
                              "  padding: 1px 10px 0px 10px;"
                              "  color: %1;"
                              "}"
                              "QToolButton:hover { background: %2; }"
                              "QToolButton:checked {"
                              "  border-bottom-color: %3;"
                              "  color: %4;"
                              "}")
                              .arg(rgba(idle), rgba(hover), rgb(palette().color(QPalette::Highlight)), rgb(text));

    for (QToolButton *button : tabButtons)
        button->setStyleSheet(style);
}
