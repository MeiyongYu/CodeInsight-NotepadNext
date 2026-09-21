/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See RelationForm.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "RelationForm.h"

#include "ApplicationSettings.h"
#include "ContextPanel.h"
#include "NotepadNextApplication.h"
#include "ProjectManager.h"

#include <QApplication>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

// How wide the rim of the window's own background is. It is the grab area for
// the width drags: the tree fills the window, so the strip the pointer can
// take hold of has to exist as the window's own edge.
constexpr int RimWidth = 4;

// Data roles on a tree item.
constexpr int FileRole = Qt::UserRole;        // absolute path of the row's file
constexpr int LineRole = Qt::UserRole + 1;    // 1-based line the row points at
constexpr int LoadedRole = Qt::UserRole + 2;  // children already queried

// A padlock drawn here rather than taken from the theme, the same way the
// Context view does it: the two states have to differ (an open shackle versus
// a closed one) and a standard pixmap cannot say that.
QIcon makeLockIcon(bool locked, const QColor &color)
{
    const int size = 16;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    QPen pen(color, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(locked ? 5.0 : 7.0, 2.5, 6.0, 8.0), 0, 180 * 16);

    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(3.0, 7.0, 10.0, 6.5), 1.2, 1.2);

    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.drawEllipse(QPointF(8.0, 9.6), 1.1, 1.1);
    painter.drawRect(QRectF(7.4, 10.0, 1.2, 2.4));
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.end();

    return QIcon(pixmap);
}

// One pixel of rule instead of the three pixel bevel a QFrame draws, exactly
// like the Context view: the strips around the tree are chrome.
QWidget *makeSeparator(QWidget *parent)
{
    auto *line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAutoFillBackground(true);

    QPalette palette = line->palette();
    palette.setColor(QPalette::Window, palette.color(QPalette::Mid));
    line->setPalette(palette);

    return line;
}

// One row of the tree. Every row is a symbol occurrence - the root is a
// definition, the children are call sites - and every row can be expanded one
// level further, so the indicator is shown before the children exist.
QTreeWidgetItem *makeRow(const QString &name, const QString &file, int line)
{
    auto *item = new QTreeWidgetItem();
    item->setText(0, name);
    item->setText(1, line > 0 ? QString::number(line) : QString());
    item->setText(2, file);
    item->setData(0, FileRole, file);
    item->setData(0, LineRole, line);
    item->setData(0, LoadedRole, false);
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    return item;
}

} // namespace

RelationForm::RelationForm(ProjectManager *projectManager, QWidget *parent)
    : QWidget(parent),
      project(projectManager)
{
    setObjectName(QStringLiteral("relationForm"));
    // The window is solid: it sits on the page next to the other windows, and
    // none of them may show through it.
    setAutoFillBackground(true);
    setMinimumWidth(140);
    // Hovering the rim has to be noticed without a button held down, otherwise
    // the resize cursor never appears.
    setMouseTracking(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(RimWidth, 0, RimWidth, RimWidth);
    layout->setSpacing(0);

    buildHeader();
    layout->addWidget(header);
    layout->addWidget(makeSeparator(this));

    buildTree();
    layout->addWidget(relationTree, 1);

    layout->addWidget(makeSeparator(this));

    buildFooter();
    layout->addWidget(footer);

    header->installEventFilter(this);

    updateModeButton();
    updateLockButton();
}

void RelationForm::buildHeader()
{
    header = new QWidget(this);
    header->setObjectName(QStringLiteral("relationFormHeader"));
    header->setAutoFillBackground(true);
    // The drag bar the window is moved (reordered) by.
    header->setCursor(Qt::SizeAllCursor);

    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 1, 1, 1);
    headerLayout->setSpacing(4);

    headerIcon = new QLabel(header);
    headerIcon->setPixmap(style()->standardIcon(QStyle::SP_FileDialogDetailedView).pixmap(QSize(12, 12)));
    // Decoration: the press belongs to the header around it, so the labels
    // stay out of the way of the mouse.
    headerIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(headerIcon);

    headerText = new QLabel(tr("Relation References"), header);
    headerText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerText->setMinimumWidth(0);
    headerText->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(headerText, 1);

    closeButton = new QToolButton(header);
    closeButton->setObjectName(QStringLiteral("relationFormCloseButton"));
    closeButton->setAutoRaise(true);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setCursor(Qt::ArrowCursor);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, this));
    closeButton->setToolTip(tr("Remove this relation form"));
    connect(closeButton, &QToolButton::clicked, this, &RelationForm::closeRequested);
    headerLayout->addWidget(closeButton);
}

void RelationForm::buildTree()
{
    relationTree = new QTreeWidget(this);
    relationTree->setObjectName(QStringLiteral("relationFormTree"));
    relationTree->setColumnCount(3);
    relationTree->setHeaderLabels({tr("Name"), tr("Lines of Code"), tr("Title")});
    relationTree->setRootIsDecorated(true);
    relationTree->setUniformRowHeights(true);
    relationTree->setAllColumnsShowFocus(true);
    relationTree->setSelectionMode(QAbstractItemView::NoSelection);
    relationTree->setFrameShape(QFrame::NoFrame);
    // Rows are read-only entries: neither the double click nor any other
    // gesture may turn one into an edit session.
    relationTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // A double click opens the symbol in the editor instead (see the
    // itemDoubleClicked connection below); the next level is only ever
    // expanded through the branch indicator.
    relationTree->setExpandsOnDoubleClick(false);
    // The window gives itself a resize cursor while the pointer rides its
    // rim; a view without a cursor of its own would inherit it and keep the
    // "<->" over the symbol rows long after the rim. The arrow is explicit.
    relationTree->setCursor(Qt::ArrowCursor);
    // Every boundary between two header labels can be grabbed to reshape the
    // columns, so all three are interactive. The columns are spread across
    // the viewport by adjustColumns() - the header itself keeps no stretch,
    // because a stretching column cannot be dragged at all.
    relationTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    relationTree->header()->setStretchLastSection(false);
    connect(relationTree->header(), &QHeaderView::sectionResized, this, [this](int, int, int) {
        // A width change that is not this window's own filling work came
        // from the user dragging a boundary between two headers: the widths
        // are the user's from then on, and the last column takes what is
        // left over right away, so the row of headers keeps ending at the
        // tree's right edge while the drag is going on.
        if (adjustingColumns)
            return;
        userAdjustedColumns = true;
        adjustingColumns = true;
        fillLastColumn();
        adjustingColumns = false;
        // The user's widths are worth keeping: whoever saves the layout of
        // the panel notes them down.
        emit columnWidthsChanged();
    });
    // The viewport changes its size with the form and with the vertical
    // scrollbar's comings and goings; both are the moments the columns have
    // to be spread across it again.
    relationTree->viewport()->installEventFilter(this);

    // A click on a row hands the symbol to the Context view. A click on the
    // expand indicator is not a row click (it only expands), which is what
    // keeps the two gestures apart.
    connect(relationTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
        Q_UNUSED(column);
        const QString file = item->data(0, FileRole).toString();
        if (!file.isEmpty())
            emit symbolActivated(file, item->data(0, LineRole).toInt());
    });

    // A double click on a row asks the editor for the symbol's file, with the
    // line placed in the upper middle of the view - expansion is the branch
    // indicator's business alone.
    connect(relationTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        Q_UNUSED(column);
        const QString file = item->data(0, FileRole).toString();
        if (!file.isEmpty())
            emit symbolOpenRequested(file, item->data(0, LineRole).toInt());
    });

    // Expanding a row asks the database what the next level down is - once;
    // an item that has been filled stays as it is.
    connect(relationTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        populateChildren(item);
    });
}

void RelationForm::adjustColumns()
{
    if (adjustingColumns)
        return;

    adjustingColumns = true;

    if (userAdjustedColumns) {
        // The user's widths stay as they are; the last column takes whatever
        // is left over, so the row of headers still ends at the window's
        // right edge.
        fillLastColumn();
    } else {
        // The line column takes what its own header needs; the other two
        // share the rest evenly - the shape the columns have before the
        // user takes over.
        QHeaderView *headerView = relationTree->header();
        const int viewportWidth = relationTree->viewport()->width();
        const int linesWidth = qBound(60,
                                      headerView->fontMetrics().horizontalAdvance(
                                          headerView->model()->headerData(1, Qt::Horizontal).toString())
                                          + 24,
                                      120);
        if (viewportWidth >= linesWidth + 80) {
            const int nameWidth = (viewportWidth - linesWidth) / 2;
            headerView->resizeSection(0, nameWidth);
            headerView->resizeSection(1, linesWidth);
            headerView->resizeSection(2, viewportWidth - nameWidth - linesWidth);
        }
    }

    adjustingColumns = false;
}

void RelationForm::fillLastColumn()
{
    // The caller owns the adjustingColumns guard.
    QHeaderView *headerView = relationTree->header();
    const int remaining = relationTree->viewport()->width()
        - headerView->sectionSize(0) - headerView->sectionSize(1);
    headerView->resizeSection(2, qMax(60, remaining));
}

void RelationForm::buildFooter()
{
    footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("relationFormFooter"));
    footer->setAutoFillBackground(true);

    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(3, 0, 3, 0);
    footerLayout->setSpacing(2);

    refreshButton = new QToolButton(footer);
    refreshButton->setObjectName(QStringLiteral("relationFormRefreshButton"));
    refreshButton->setAutoRaise(true);
    refreshButton->setFocusPolicy(Qt::NoFocus);
    refreshButton->setCursor(Qt::ArrowCursor);
    refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload, nullptr, this));
    refreshButton->setIconSize(QSize(14, 14));
    refreshButton->setFixedSize(18, 16);
    refreshButton->setToolTip(tr("Refresh from the symbol under the caret"));
    connect(refreshButton, &QToolButton::clicked, this, &RelationForm::refreshRequested);
    footerLayout->addWidget(refreshButton);

    lockButton = new QToolButton(footer);
    lockButton->setObjectName(QStringLiteral("relationFormLockButton"));
    lockButton->setCheckable(true);
    // The relation form is locked by default: it only follows the caret on
    // purpose, after the lock is opened.
    lockButton->setChecked(true);
    lockButton->setAutoRaise(true);
    lockButton->setFocusPolicy(Qt::NoFocus);
    lockButton->setCursor(Qt::ArrowCursor);
    lockButton->setIconSize(QSize(16, 16));
    lockButton->setFixedSize(18, 16);
    connect(lockButton, &QToolButton::toggled, this, [this](bool locked) {
        updateLockButton();
        emit lockChanged(locked);
    });
    footerLayout->addWidget(lockButton);

    footerLayout->addStretch(1);

    // The mode switch: a two-segment capsule. The lit segment (the palette's
    // highlight) is the mode the tree is in, the other one is one click away -
    // state and switchability sit side by side instead of hiding behind a
    // single word. The frames stay the grey of the buttons around here.
    const QString highlight = palette().color(QPalette::Highlight).name();
    const QString frame = palette().color(QPalette::Mid).name();
    const QString face = palette().color(QPalette::Base).name();
    const QString ink = palette().color(QPalette::WindowText).name();
    const QString hover = palette().color(QPalette::AlternateBase).name();
    const QString pressed = palette().color(QPalette::Button).name();

    auto makeSegment = [this](const QString &objectName, const QString &text) {
        auto *segment = new QToolButton(footer);
        segment->setObjectName(objectName);
        segment->setText(text);
        segment->setCheckable(true);
        segment->setFocusPolicy(Qt::NoFocus);
        segment->setCursor(Qt::ArrowCursor);
        segment->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return segment;
    };
    calledButton = makeSegment(QStringLiteral("relationFormModeCalled"), tr("Called"));
    callButton = makeSegment(QStringLiteral("relationFormModeCall"), tr("Call"));
    connect(calledButton, &QToolButton::clicked, this, [this]() { setCalledMode(true); });
    connect(callButton, &QToolButton::clicked, this, [this]() { setCalledMode(false); });
    footerLayout->addWidget(calledButton);
    footerLayout->addWidget(callButton);

    newButton = new QToolButton(footer);
    newButton->setObjectName(QStringLiteral("relationFormNewButton"));
    newButton->setFocusPolicy(Qt::NoFocus);
    newButton->setCursor(Qt::ArrowCursor);
    newButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    newButton->setText(tr("New"));
    newButton->setToolTip(tr("Open a new relation form to the right of this one"));
    connect(newButton, &QToolButton::clicked, this, &RelationForm::newRequested);
    footerLayout->addWidget(newButton);

    // One sheet dresses the capsule and the New button alike: a grey frame on
    // a plain face, dark text, rounded ends - the capsule shares its middle
    // border, and the checked segment is filled with the highlight colour.
    footer->setStyleSheet(QStringLiteral(
        "QToolButton#relationFormModeCalled, QToolButton#relationFormModeCall,"
        " QToolButton#relationFormNewButton {"
        "  border: 1px solid %1; background: %2; color: %3; padding: 1px 10px; }"
        "QToolButton#relationFormModeCalled {"
        "  border-top-left-radius: 9px; border-bottom-left-radius: 9px; }"
        "QToolButton#relationFormModeCall {"
        "  border-top-right-radius: 9px; border-bottom-right-radius: 9px;"
        "  border-left: none; }"
        "QToolButton#relationFormModeCalled:checked, QToolButton#relationFormModeCall:checked {"
        "  background: %4; color: white; }"
        "QToolButton#relationFormModeCalled:hover:!checked,"
        " QToolButton#relationFormModeCall:hover:!checked,"
        " QToolButton#relationFormNewButton:hover { background: %5; }"
        "QToolButton#relationFormModeCalled:pressed, QToolButton#relationFormModeCall:pressed,"
        " QToolButton#relationFormNewButton:pressed { background: %6; }")
        .arg(frame, face, ink, highlight, hover, pressed));
}

bool RelationForm::isLocked() const
{
    return lockButton->isChecked();
}

void RelationForm::setRootSymbol(const QString &name, const QString &filePath, int line, bool force)
{
    // The caret announces the same symbol over and over while it sits on it;
    // unless the root changed or the question about it changed, the tree that
    // is up already is the answer.
    if (!force && rootBuilt && name == rootName && rootBuiltCalled == calledMode)
        return;

    rootName = name;
    rootFile = filePath;
    rootLine = line;
    rootBuiltCalled = calledMode;
    rootBuilt = !name.isEmpty();

    relationTree->clear();

    if (rootName.isEmpty())
        return;

    auto *rootItem = makeRow(rootName, rootFile, rootLine);
    relationTree->addTopLevelItem(rootItem);

    // The next level comes up with the root: one click of the tree is already
    // done when the root is placed.
    populateChildren(rootItem);
    rootItem->setExpanded(true);
}

void RelationForm::populateChildren(QTreeWidgetItem *item)
{
    if (item == Q_NULLPTR || item->data(0, LoadedRole).toBool())
        return;

    item->setData(0, LoadedRole, true);

    if (project == Q_NULLPTR || !project->hasProject())
        return;

    const QString name = item->text(0);
    if (name.isEmpty())
        return;

    // "Called" answers with the symbols that call this one, "Call" with the
    // ones this symbol calls. Either way every entry is a call site: its own
    // file and line, and a name that can be expanded in turn.
    const QVector<ProjectManager::ProjectSymbol> sites =
        calledMode ? project->findCallers(name) : project->findCallees(name);

    for (const ProjectManager::ProjectSymbol &site : sites) {
        auto *row = makeRow(site.name, site.file, site.line);
        // Call sites carry no ctags kind; the rows below them are queried by
        // their function name all the same.
        row->setText(2, QFileInfo(site.file).fileName());
        item->addChild(row);
    }
}

void RelationForm::updateModeButton()
{
    // A click toggles the segment that was clicked on its own; here is where
    // the lit segment is forced back onto the mode the tree is actually in,
    // which also repairs a click on the already lit one.
    calledButton->setChecked(calledMode);
    callButton->setChecked(!calledMode);

    calledButton->setToolTip(tr("Called mode: expanding a symbol lists the symbols that call it."));
    callButton->setToolTip(tr("Call mode: expanding a symbol lists the symbols it calls."));
}

void RelationForm::setCalledMode(bool called)
{
    const bool changed = calledMode != called;
    calledMode = called;
    updateModeButton();

    if (changed) {
        // Whoever keeps the saved layout notes the flip down.
        emit modeChanged(called);

        if (!rootName.isEmpty()) {
            // A flip of the mode is a different question about the same symbol:
            // the first level is rebuilt from the root, the deeper levels the
            // user opened belong to the old question and go away with it.
            setRootSymbol(rootName, rootFile, rootLine);
        }
    }
}

void RelationForm::setLocked(bool locked)
{
    // No announcement: a restoration knows the state it puts back, and the
    // toggled signal of the button would only make the saver write the very
    // values it just read.
    lockButton->setChecked(locked);
    updateLockButton();
}

QList<int> RelationForm::columnWidths() const
{
    const QHeaderView *headerView = relationTree->header();
    return {headerView->sectionSize(0), headerView->sectionSize(1), headerView->sectionSize(2)};
}

void RelationForm::setColumnWidths(const QList<int> &widths)
{
    if (widths.size() != 3)
        return;

    // The widths of a saved layout are the user's own: the filling work the
    // viewport resizes do from here on treats them exactly like the widths a
    // drag left behind. The guard keeps the setting itself out of the
    // sectionResized answer (it is not a user drag).
    userAdjustedColumns = true;
    adjustingColumns = true;
    QHeaderView *headerView = relationTree->header();
    headerView->resizeSection(0, qMax(0, widths.at(0)));
    headerView->resizeSection(1, qMax(0, widths.at(1)));
    headerView->resizeSection(2, qMax(0, widths.at(2)));
    adjustingColumns = false;
}

void RelationForm::updateLockButton()
{
    const bool locked = isLocked();

    QColor color = locked ? palette().color(QPalette::Highlight) : palette().color(QPalette::WindowText);
    lockButton->setIcon(makeLockIcon(locked, color));
    lockButton->setToolTip(locked
                               ? tr("Locked: the tree only changes through the refresh button")
                               : tr("Follows the caret; click to lock it"));
}

void RelationForm::paintEvent(QPaintEvent *)
{
    // The window sits on a page painted in the same colour, so without a line
    // around it the tree would just stop, with nothing to say where the window
    // ends - or where its rims can be grabbed.
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void RelationForm::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int x = event->position().toPoint().x();
    if (x < RimWidth || x >= width() - RimWidth) {
        widthDragLeft = x < RimWidth;
        widthDragStartX = event->globalPosition().toPoint().x();
        emit widthDragStarted(widthDragLeft);
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void RelationForm::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();

    if (event->buttons() & Qt::LeftButton && widthDragStartX >= 0) {
        emit widthDragMoved(event->globalPosition().toPoint().x() - widthDragStartX);
        event->accept();
        return;
    }

    // No button held: the cursor says what a drag from here would do.
    const Qt::CursorShape shape = (position.x() < RimWidth || position.x() >= width() - RimWidth)
                                      ? Qt::SizeHorCursor
                                      : Qt::ArrowCursor;
    if (cursor().shape() != shape)
        setCursor(shape);

    QWidget::mouseMoveEvent(event);
}

void RelationForm::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && widthDragStartX >= 0) {
        widthDragStartX = -1;
        emit dragEnded();
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

bool RelationForm::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == relationTree->viewport() && event->type() == QEvent::Resize) {
        adjustColumns();
        // No interference with the viewport's own handling of it.
        return false;
    }

    if (watched == header) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);

        switch (event->type()) {
        case QEvent::MouseButtonPress:
            if (mouseEvent->button() == Qt::LeftButton) {
                moveDragStartX = mouseEvent->globalPosition().toPoint().x();
                emit moveDragStarted();
                return true;
            }
            break;
        case QEvent::MouseMove:
            if (moveDragStartX >= 0 && (mouseEvent->buttons() & Qt::LeftButton)) {
                emit moveDragMoved(mouseEvent->globalPosition().toPoint().x());
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (moveDragStartX >= 0 && mouseEvent->button() == Qt::LeftButton) {
                moveDragStartX = -1;
                emit dragEnded();
                return true;
            }
            break;
        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

// ---- RelationPanel ----------------------------------------------------------

namespace {

// The gap the row keeps under the title bar of the host dock, so the windows
// do not look glued to the tab names above it.
constexpr int HostTopGap = 2;

} // namespace

RelationPanel::RelationPanel(QWidget *page)
    : QObject(page),
      panelPage(page)
{
    // The page holds a row of windows that fills it: the Context view first,
    // then the relation forms. Nothing here is laid out by a layout: the
    // panel hands out the geometry itself (ensureLayout and the methods
    // around it), because the width of one window is the neighbour's business
    // and the row has to stay as wide as the page however it is dragged.
    context = new ContextPanel(panelPage);

    // The Context view speaks the same chrome-drag protocol the relation
    // forms do: its width drags are compensated by the neighbour, its header
    // drag reorders the row - the panel answers, exactly as it does for a
    // form. Its layout-affecting moments are saved with the rest of the
    // panel.
    connect(context, &ContextPanel::widthDragStarted, this, [this](bool leftEdge) {
        widthDragWindow = context;
        widthDragLeftEdge = leftEdge;
        widthDragStartWidths.clear();
        for (QWidget *w : pageWindows)
            widthDragStartWidths.append(w->width());
    });
    connect(context, &ContextPanel::widthDragMoved, this, [this](int delta) {
        if (widthDragWindow == context)
            applyWidthDrag(context, widthDragLeftEdge, delta);
    });
    connect(context, &ContextPanel::moveDragStarted, this, [this]() {
        moveDragWindow = context;
        moveDragStartX = -1;
        moveDragStartWindowX = context->x();
    });
    connect(context, &ContextPanel::moveDragMoved, this, [this](int globalX) {
        if (moveDragWindow != context)
            return;

        if (moveDragStartX < 0)
            moveDragStartX = globalX;

        const QRect area = relationArea();
        const int x = qBound(area.left(), moveDragStartWindowX + globalX - moveDragStartX,
                             area.right() + 1 - context->width());
        context->move(x, area.top());
    });
    connect(context, &ContextPanel::dragEnded, this, [this]() {
        if (moveDragWindow != nullptr)
            finishMoveDrag();
        else if (widthDragWindow != nullptr) {
            widthDragWindow = nullptr;
            widthDragStartWidths.clear();
        }
        scheduleSave();
    });
    connect(context, &ContextPanel::lockChanged, this, [this](bool) {
        scheduleSave();
    });

    // The layout changes arrive by the dozen while a drag or a resize is
    // going on; the settings get one write a moment after the last of them.
    saveTimer = new QTimer(this);
    saveTimer->setSingleShot(true);
    saveTimer->setInterval(400);
    connect(saveTimer, &QTimer::timeout, this, &RelationPanel::saveLayout);
}

QList<RelationForm *> RelationPanel::forms() const
{
    QList<RelationForm *> rowForms;
    for (QWidget *window : pageWindows) {
        if (auto *form = qobject_cast<RelationForm *>(window))
            rowForms.append(form);
    }
    return rowForms;
}

QRect RelationPanel::relationArea() const
{
    return panelPage->rect().adjusted(0, HostTopGap, 0, 0);
}

void RelationPanel::init(ProjectManager *projectManager)
{
    if (relationProject != nullptr || projectManager == nullptr)
        return;

    relationProject = projectManager;
    // The row is built as soon as the page knows its size, which the page's
    // resizes announce.
    panelPage->installEventFilter(this);
    ensureLayout();
}

void RelationPanel::ensureLayout()
{
    const QRect area = relationArea();

    if (relationLayoutReady || relationProject == nullptr)
        return;

    // Nothing to place the row in yet: the page is not laid out. The size it
    // reports before that is a default, not the size it will have, so the row
    // waits for the first resize after the page was shown.
    if (area.width() <= 0 || area.height() <= 0 || !panelPage->isVisible())
        return;

    relationLayoutReady = true;
    lastPageWidth = area.width();

    pageWindows.append(context);

    // A layout saved earlier comes back instead of the default row: the same
    // number of windows, each at the width it had - the heights are the
    // page's own, as they always are. A page of a different size than the one
    // the layout was saved with gets the row scaled to fit, which is what a
    // plain resize of the window does to the row as well.
    ApplicationSettings *settings = qobject_cast<NotepadNextApplication*>(qApp)->getSettings();
    const int savedCount = settings->value("RelationPanel/windowCount", -1).toInt();
    const QStringList savedWidths =
        settings->value("RelationPanel/widths").toString().split(',', Qt::SkipEmptyParts);
    const bool restore = savedCount >= 0 && savedWidths.size() == savedCount + 1;

    if (restore) {
        for (int i = 0; i < savedCount; ++i)
            createRelationForm(pageWindows.size());
    } else {
        // The default row: two relation forms to the right of the Context view.
        createRelationForm(pageWindows.size());
        createRelationForm(pageWindows.size());
    }

    if (restore) {
        for (int i = 0; i < pageWindows.size(); ++i) {
            QWidget *window = pageWindows.at(i);
            window->resize(qMax(window->minimumWidth(), savedWidths.at(i).toInt()), area.height());
        }
    } else {
        // The Context view takes the share it was built for, as much of it as
        // the page allows; what is left is shared out over the forms.
        const int contextWidth = qMin(ContextPanel::DefaultWidth, area.width() / 2);
        const int rest = area.width() - contextWidth;
        const int each = rest / rowForms.size();
        int x = area.left();

        context->resize(contextWidth, area.height());
        x += contextWidth;
        for (RelationForm *form : rowForms) {
            form->resize(each, area.height());
            x += each;
        }
        // The rounding of the division is handed to the last window, so the
        // row ends exactly at the right edge of the page.
        rowForms.last()->resize(rest - each * (rowForms.size() - 1), area.height());
    }

    relayoutRelationPage();

    if (restore) {
        // The widths are in place, so the columns are restored against the
        // viewport they will live on: the first two columns come back as they
        // were, the third absorbs whatever the page's width differs.
        context->setLocked(settings->value("RelationPanel/contextLocked", true).toBool());
        for (int i = 0; i < rowForms.size(); ++i) {
            RelationForm *form = rowForms.at(i);
            form->setLocked(settings->value(QStringLiteral("RelationPanel/form.%1.locked").arg(i), true).toBool());
            form->setCalledMode(
                settings->value(QStringLiteral("RelationPanel/form.%1.mode").arg(i), QStringLiteral("Called"))
                    .toString()
                == QStringLiteral("Called"));
            QStringList columns = settings
                                      ->value(QStringLiteral("RelationPanel/form.%1.columns").arg(i))
                                      .toString()
                                      .split(',', Qt::SkipEmptyParts);
            if (columns.size() == 3) {
                form->setColumnWidths({columns.at(0).toInt(), columns.at(1).toInt(), columns.at(2).toInt()});
            }
        }
        // The page of this start may be narrower or wider than the one the
        // layout was saved with: the row is evened out to fill it, the shares
        // staying what they were.
        normalizeRowToArea();

        // Putting the layout back is not a change of it: the signals the
        // restoration itself raised must not make the panel write what it
        // just read back into the settings.
        saveTimer->stop();
        layoutDirty = false;
    } else {
        // The default row is a layout of its own: it gets recorded, so the
        // next start restores it as it was.
        scheduleSave();
    }
}

RelationForm *RelationPanel::createRelationForm(int index)
{
    auto *form = new RelationForm(relationProject, panelPage);

    // A form built while the page is mid-show is not dragged along by the
    // page's own show - it has to be shown on its own.
    form->show();

    wireRelationForm(form);
    rowForms.append(form);
    pageWindows.insert(index, form);
    return form;
}

void RelationPanel::wireRelationForm(RelationForm *form)
{
    // A width drag on one window is the neighbour's business: the panel keeps
    // the widths the row started with and hands out the changes.
    connect(form, &RelationForm::widthDragStarted, this, [this, form](bool leftEdge) {
        widthDragWindow = form;
        widthDragLeftEdge = leftEdge;
        widthDragStartWidths.clear();
        for (QWidget *w : pageWindows)
            widthDragStartWidths.append(w->width());
    });
    connect(form, &RelationForm::widthDragMoved, this, [this, form](int delta) {
        if (widthDragWindow == form)
            applyWidthDrag(form, widthDragLeftEdge, delta);
    });

    // A move drag slides the window along the row; where it lands decides
    // which boundary it is inserted at (finishMoveDrag).
    connect(form, &RelationForm::moveDragStarted, this, [this, form]() {
        moveDragWindow = form;
        moveDragStartX = -1; // the first move sets the reference point
        moveDragStartWindowX = form->x();
    });
    connect(form, &RelationForm::moveDragMoved, this, [this, form](int globalX) {
        if (moveDragWindow != form)
            return;

        if (moveDragStartX < 0)
            moveDragStartX = globalX;

        const QRect area = relationArea();
        const int x = qBound(area.left(), moveDragStartWindowX + globalX - moveDragStartX,
                             area.right() + 1 - form->width());
        form->move(x, area.top());
    });

    // One release ends either drag.
    connect(form, &RelationForm::dragEnded, this, [this]() {
        if (moveDragWindow != nullptr) {
            finishMoveDrag();
        } else if (widthDragWindow != nullptr) {
            widthDragWindow = nullptr;
            widthDragStartWidths.clear();
        }
        // The widths the drag left behind are a layout worth keeping.
        scheduleSave();
    });
    // Lock, mode and dragged columns are parts of the saved layout, too.
    connect(form, &RelationForm::lockChanged, this, [this](bool) {
        scheduleSave();
    });
    connect(form, &RelationForm::modeChanged, this, [this](bool) {
        scheduleSave();
    });
    connect(form, &RelationForm::columnWidthsChanged, this, [this]() {
        scheduleSave();
    });

    connect(form, &RelationForm::symbolActivated, this, [this](const QString &filePath, int line) {
        // A click on a symbol shows it in the Context view - that is the whole
        // point of the two windows sitting next to each other.
        context->showSymbol(filePath, line);
    });
    // A double click goes further than the Context view: the editor opens the
    // file. The panel only relays - the editor belongs to the main window.
    connect(form, &RelationForm::symbolOpenRequested, this, &RelationPanel::relationFormOpenRequested);
    connect(form, &RelationForm::newRequested, this, [this, form]() {
        addRelationFormAfter(form);
    });
    connect(form, &RelationForm::closeRequested, this, [this, form]() {
        removeRelationForm(form);
    });
    connect(form, &RelationForm::refreshRequested, this, [this, form]() {
        emit relationFormRefreshRequested(form);
    });
}

void RelationPanel::relayoutRelationPage()
{
    const QRect area = relationArea();
    int x = area.left();

    for (QWidget *window : pageWindows) {
        window->setGeometry(x, area.top(), window->width(), area.height());
        x += window->width();
    }
}

void RelationPanel::applyWidthDrag(QWidget *window, bool leftEdge, int delta)
{
    const int i = pageWindows.indexOf(window);

    if (i < 0 || widthDragStartWidths.size() != pageWindows.size())
        return;

    // The neighbour on the side the drag is on pays for the change, so the row
    // keeps filling the page exactly. Both sides are held at their minimum
    // width; whatever a clamp takes away, the other side does not get.
    const int startWidth = widthDragStartWidths.at(i);
    const int change = leftEdge ? -delta : delta;

    int newWidth = startWidth + change;

    const int j = i + (leftEdge ? -1 : 1);
    if (j >= 0 && j < pageWindows.size()) {
        const int startNeighbour = widthDragStartWidths.at(j);
        const int neighbourMin = pageWindows.at(j)->minimumWidth();

        int newNeighbour = qMax(neighbourMin, startNeighbour - change);
        newWidth = qMax(window->minimumWidth(), startWidth + (startNeighbour - newNeighbour));
        newNeighbour = qMax(neighbourMin, startNeighbour - (newWidth - startWidth));

        pageWindows.at(j)->resize(newNeighbour, pageWindows.at(j)->height());
    } else {
        // No neighbour on that side: the window may take the room the rest of
        // the row leaves it, and not a pixel more.
        int others = 0;
        for (int k = 0; k < pageWindows.size(); ++k)
            if (k != i)
                others += pageWindows.at(k)->minimumWidth();
        newWidth = qBound(window->minimumWidth(), newWidth,
                          qMax(window->minimumWidth(), relationArea().width() - others));
    }

    window->resize(newWidth, window->height());
    relayoutRelationPage();
}

void RelationPanel::finishMoveDrag()
{
    QWidget *dragged = moveDragWindow;
    moveDragWindow = nullptr;
    moveDragStartX = -1;

    const int from = pageWindows.indexOf(dragged);

    if (from < 0)
        return;

    // The window goes where its middle points: between the two windows whose
    // boundary it was dropped on, which is the count of windows whose middle
    // is still to the left of its own.
    const int draggedCenter = dragged->x() + dragged->width() / 2;

    QList<QWidget *> others = pageWindows;
    others.removeAt(from);

    int insertAt = 0;
    for (QWidget *w : others) {
        if (w->x() + w->width() / 2 < draggedCenter)
            ++insertAt;
        else
            break;
    }

    others.insert(insertAt, dragged);
    pageWindows = others;

    relayoutRelationPage();
    scheduleSave();
}

void RelationPanel::addRelationFormAfter(QWidget *window)
{
    const int index = pageWindows.indexOf(window) + 1;

    createRelationForm(index);

    // The room to the right of the window the new form was asked from is
    // shared out equally over everything right of it - the row fills the page
    // again without any window leaving its place. The windows left of it keep
    // their geometry, so the room starts at the requester's right edge.
    const QRect area = relationArea();
    const int roomLeft = area.right() + 1 - (window->x() + window->width());
    const int rightCount = pageWindows.size() - index;
    const int share = roomLeft / qMax(1, rightCount);

    for (int k = index; k < pageWindows.size() - 1; ++k) {
        QWidget *w = pageWindows.at(k);
        w->resize(qMax(w->minimumWidth(), share), w->height());
    }
    // The rounding of the division goes to the last window of the row, so the
    // row ends exactly at the right edge of the page.
    QWidget *last = pageWindows.last();
    last->resize(qMax(last->minimumWidth(),
                      roomLeft - share * (rightCount - 1)),
                 last->height());

    normalizeRowToArea();
    scheduleSave();
}

// The row always has to be exactly as wide as the page - a share that fell
// below a window's minimum width, or plain rounding, has to be evened out.
// Every window is scaled towards the page width, each at least its minimum;
// whatever the minimums still leave over is taken back from the windows that
// have slack, so the last one ends at the right edge.
void RelationPanel::normalizeRowToArea()
{
    const QRect area = relationArea();

    if (area.width() <= 0 || pageWindows.isEmpty())
        return;

    int sum = 0;
    for (QWidget *w : pageWindows)
        sum += w->width();

    if (sum != area.width()) {
        const qreal ratio = static_cast<qreal>(area.width()) / sum;
        for (QWidget *w : pageWindows)
            w->resize(qMax(w->minimumWidth(), static_cast<int>(w->width() * ratio)), area.height());
    }

    // The scaled widths can still miss the page width when minimums bound the
    // scaling. The rest is taken back from the windows that have room left.
    int diff = area.width();
    for (QWidget *w : pageWindows)
        diff -= w->width();

    for (int k = pageWindows.size() - 1; k >= 0 && diff < 0; --k) {
        QWidget *w = pageWindows.at(k);
        const int slack = w->width() - w->minimumWidth();
        const int take = qMin(slack, -diff);
        if (take > 0) {
            w->resize(w->width() - take, w->height());
            diff += take;
        }
    }

    // A positive rest is plain rounding: the last window takes it, it has no
    // upper bound to bump into.
    if (diff > 0) {
        QWidget *last = pageWindows.last();
        last->resize(last->width() + diff, last->height());
    }

    relayoutRelationPage();
}

void RelationPanel::removeRelationForm(RelationForm *form)
{
    // The last relation form of the row stays where it is: with it gone there
    // would be no New button left to build the row up again, so its close
    // click is answered with silence.
    if (rowForms.size() <= 1)
        return;

    rowForms.removeOne(form);
    pageWindows.removeOne(form);
    form->deleteLater();

    // The width the form left behind is shared out over the rest, so the row
    // still fills the page.
    normalizeRowToArea();
    scheduleSave();
}

void RelationPanel::updateUnlockedRoots(const QString &name, const QString &filePath, int line)
{
    for (RelationForm *form : forms()) {
        if (!form->isLocked())
            form->setRootSymbol(name, filePath, line);
    }
}

void RelationPanel::scheduleSave()
{
    if (!relationLayoutReady)
        return;

    layoutDirty = true;
    saveTimer->start();
}

void RelationPanel::saveLayout()
{
    if (!relationLayoutReady || !layoutDirty)
        return;

    // The windows are recorded in the order they stand in the row - a move
    // drag reorders pageWindows, the rowForms list does not follow it, so the
    // row's own order is what gets saved (see forms).
    const QList<RelationForm *> orderedForms = forms();

    QStringList widths;
    for (QWidget *window : pageWindows)
        widths.append(QString::number(window->width()));

    ApplicationSettings *settings = qobject_cast<NotepadNextApplication*>(qApp)->getSettings();

    settings->beginGroup(QStringLiteral("RelationPanel"));
    settings->setValue(QStringLiteral("windowCount"), orderedForms.size());
    settings->setValue(QStringLiteral("widths"), widths.join(QLatin1Char(',')));
    settings->setValue(QStringLiteral("contextLocked"), context->isLocked());
    for (int i = 0; i < orderedForms.size(); ++i) {
        RelationForm *form = orderedForms.at(i);
        settings->setValue(QStringLiteral("form.%1.locked").arg(i), form->isLocked());
        settings->setValue(QStringLiteral("form.%1.mode").arg(i),
                           form->isCalledMode() ? QStringLiteral("Called") : QStringLiteral("Call"));
        QStringList columns;
        for (int width : form->columnWidths())
            columns.append(QString::number(width));
        settings->setValue(QStringLiteral("form.%1.columns").arg(i), columns.join(QLatin1Char(',')));
    }
    settings->endGroup();

    layoutDirty = false;
}

bool RelationPanel::eventFilter(QObject *watched, QEvent *event)
{
    // The page is the world the row lives in: its first resize - or the first
    // time it is shown, the two can arrive in either order and a resize while
    // still hidden does not come with a visible page - builds the row; every
    // later resize keeps the windows' shares and lays the row out again.
    if (watched == panelPage && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        ensureLayout();

        if (event->type() == QEvent::Resize && relationLayoutReady) {
            const QRect area = relationArea();

            if (area.width() > 0 && area.width() != lastPageWidth) {
                lastPageWidth = area.width();

                // Every window keeps its share of the page; the exact ending
                // at the right edge is normalizeRowToArea's business.
                int sum = 0;
                for (QWidget *w : pageWindows)
                    sum += w->width();
                const qreal ratio = static_cast<qreal>(area.width()) / qMax(1, sum);
                for (QWidget *w : pageWindows)
                    w->resize(qMax(w->minimumWidth(), static_cast<int>(w->width() * ratio)), area.height());
            } else {
                // Only the height changed: the row follows it.
                for (QWidget *w : pageWindows)
                    w->resize(w->width(), area.height());
            }

            normalizeRowToArea();
            // The shares the page's new width handed out are the layout the
            // next start is to come back to.
            scheduleSave();
        }

        return QObject::eventFilter(watched, event);
    }

    return QObject::eventFilter(watched, event);
}
