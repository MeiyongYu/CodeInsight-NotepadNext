/*
 * This file is part of Notepad Next.
 * Copyright 2026 Notepad Next contributors
 *
 * See ContextPanel.h.
 *
 * This file is part of a feature licensed under the GNU General Public
 * License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */

#include "ContextPanel.h"

#include "ApplicationSettings.h"
#include "NotepadNextApplication.h"
#include "ScintillaNext.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// How wide the rim of the window's own background is (see the layout). It is
// the grab area for the edges: a width drag needs a strip the pointer can land
// on, and the editor fills the window, so that strip has to exist.
constexpr int RimWidth = 4;

// A padlock drawn here rather than taken from the theme: the two states have to
// differ (an open shackle versus a closed one) and a standard pixmap cannot say
// that. The shackle is the upper half of an ellipse; "open" moves it to the
// right so it only hangs on the right hand side of the body, which is what an
// unlocked padlock looks like.
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

    // Punch the keyhole out of the body so whatever is behind the button shows
    // through it - the button is drawn on top of the panel's own background.
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.drawEllipse(QPointF(8.0, 9.6), 1.1, 1.1);
    painter.drawRect(QRectF(7.4, 10.0, 1.2, 2.4));
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.end();

    return QIcon(pixmap);
}

} // namespace

// One pixel of rule instead of the three pixel bevel a QFrame draws: the strips
// around the editor are chrome, and every pixel of chrome is a pixel the code
// cannot use.
static QWidget *makeSeparator(QWidget *parent)
{
    auto *line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAutoFillBackground(true);

    QPalette palette = line->palette();
    palette.setColor(QPalette::Window, palette.color(QPalette::Mid));
    line->setPalette(palette);

    return line;
}

ContextPanel::ContextPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("contextPanel"));
    // The window is solid: it sits on the page next to the relation forms, and
    // none of them may show through it.
    setAutoFillBackground(true);
    setMinimumWidth(180);
    // Hovering the rim has to be noticed without a button held down, otherwise
    // the resize cursor never appears.
    setMouseTracking(true);

    auto *layout = new QVBoxLayout(this);
    // The rim: a few pixels of the window's own background around the chrome.
    // Nothing is drawn in it, it is the strip the pointer can grab the edges
    // by (see mousePressEvent), and it is why the window reads as a window
    // instead of as a bare editor.
    layout->setContentsMargins(RimWidth, 0, RimWidth, RimWidth);
    layout->setSpacing(0);

    // ---- header: which file is on screen, and the bar to drag by ----
    header = new QWidget(this);
    header->setObjectName(QStringLiteral("contextHeader"));
    header->setAutoFillBackground(true);
    // The drag bar of the window; its cursor and tooltip say so (updateHeader
    // fills the tooltip in, it is the same place the file name lives).
    header->setCursor(Qt::SizeAllCursor);

    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 1, 1, 1);
    headerLayout->setSpacing(4);

    headerIcon = new QLabel(header);
    headerIcon->setPixmap(style()->standardIcon(QStyle::SP_FileIcon).pixmap(QSize(12, 12)));
    // Decoration: the press belongs to the header around it, which is the drag
    // bar (see eventFilter), so the labels stay out of the way of the mouse.
    headerIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(headerIcon);

    headerText = new QLabel(header);
    // The text carries a full path worth of information; letting it decide the
    // window's width would make a resize of it fight the label.
    headerText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerText->setMinimumWidth(0);
    headerText->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(headerText, 1);

    closeButton = new QToolButton(header);
    closeButton->setObjectName(QStringLiteral("contextCloseButton"));
    closeButton->setAutoRaise(true);
    closeButton->setFocusPolicy(Qt::NoFocus);
    // The button takes its own cursor back from the header's drag cursor.
    closeButton->setCursor(Qt::ArrowCursor);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, this));
    closeButton->setToolTip(tr("Close"));
    connect(closeButton, &QToolButton::clicked, this, &ContextPanel::clear);
    headerLayout->addWidget(closeButton);

    layout->addWidget(header);
    layout->addWidget(makeSeparator(this));

    // ---- the editor ----
    view = new ScintillaNext(QString(), this);
    view->setReadOnly(true);
    view->setUndoCollection(false);
    view->usePopUp(SC_POPUP_NEVER);
    view->setCaretLineVisible(true);
    view->setCaretLineVisibleAlways(true);
    view->setScrollWidthTracking(true);
    view->setScrollWidth(1);
    // Scintilla clamps the top line to MaxScrollPos(), which stops the view one
    // screen short of the end while endAtLastLine is on. A short file would then
    // always sit at line 1 and the symbol could not be pushed down below the top
    // edge, so the view is allowed to scroll past the last line.
    view->setEndAtLastLine(false);
    view->setTabWidth(4);
    view->setMarginLeft(4);
    view->setMarginWidthN(2, 0);
    applyEditorFonts();
    layout->addWidget(view, 1);

    layout->addWidget(makeSeparator(this));

    // ---- footer: the lock ----
    footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("contextFooter"));
    footer->setAutoFillBackground(true);

    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(3, 0, 0, 0);
    footerLayout->setSpacing(0);

    lockButton = new QToolButton(footer);
    lockButton->setObjectName(QStringLiteral("contextLockButton"));
    lockButton->setCheckable(true);
    lockButton->setAutoRaise(true);
    lockButton->setFocusPolicy(Qt::NoFocus);
    lockButton->setCursor(Qt::ArrowCursor);
    // Only as tall as the glyph itself: the strip is a control, not a band, so
    // every pixel it does not need is a pixel the code below it cannot use.
    lockButton->setIconSize(QSize(16, 16));
    lockButton->setFixedSize(18, 16);
    connect(lockButton, &QToolButton::toggled, this, [this](bool locked) {
        updateLockButton();
        emit lockChanged(locked);
    });
    footerLayout->addWidget(lockButton);
    footerLayout->addStretch(1);

    layout->addWidget(footer);

    // The header is the move handle: it is watched so a drag on it is the
    // window's business and not something that has to travel up from the
    // labels on it first (see eventFilter).
    header->installEventFilter(this);

    updateLockButton();
    updateHeader();

    // Keep the strip in step with the editor font preferences, the same way
    // every other editor does.
    auto *settings = qobject_cast<NotepadNextApplication *>(qApp)->getSettings();
    connect(settings, &ApplicationSettings::fontNameChanged, this, [this]() {
        applyEditorFonts();
        updateLineNumberMargin();
    });
    connect(settings, &ApplicationSettings::fontSizeChanged, this, [this]() {
        applyEditorFonts();
        updateLineNumberMargin();
    });
}

bool ContextPanel::isLocked() const
{
    return lockButton->isChecked();
}

void ContextPanel::setLocked(bool locked)
{
    // No announcement: a restoration knows the state it puts back.
    lockButton->setChecked(locked);
    updateLockButton();
}

void ContextPanel::clear()
{
    loadedFile.clear();

    // Scintilla drops the deletion while the document is read only, so the flag
    // has to come off around it - otherwise the file that is on screen stays.
    view->setReadOnly(false);
    view->clearAll();
    view->setReadOnly(true);

    view->setILexer(0);
    updateHeader();
}

void ContextPanel::showSymbol(const QString &filePath, int line)
{
    if (filePath.isEmpty())
        return;

    const int targetLine = qMax(1, line);

    // Same file: only the position changes, re-reading it would just throw away
    // the styling and the scroll position for nothing.
    if (filePath != loadedFile) {
        // loadFromFile() empties the buffer before it reads, but that deletion
        // is dropped while the document is read only and the new file would be
        // appended to the one already on screen (loadFromFile lifts the flag
        // only afterwards). The panel is read only for the user, not for this
        // load, so the flag is cleared for it and put back right after.
        view->setReadOnly(false);

        // A file that cannot be read any more has to leave the panel empty
        // instead of half showing the previous file.
        if (!view->loadFromFile(filePath)) {
            clear();
            return;
        }

        loadedFile = filePath;
        // loadFromFile() clears the read-only flag so that a caller can reuse
        // the editor; this one only ever reads.
        view->setReadOnly(true);

        applyEditorFonts();

        auto *app = qobject_cast<NotepadNextApplication *>(qApp);
        if (app) {
            const QString language = app->detectLanguage(view);
            if (!language.isEmpty())
                app->setEditorLanguage(view, language);
        }

        // Every language styles a fold margin onto the editor; this view has
        // nothing to fold, so the strip is dropped again.
        view->setMarginWidthN(2, 0);
        view->styleSetFore(STYLE_LINENUMBER, 0x808080);
        view->styleSetBack(STYLE_LINENUMBER, 0xE4E4E4);
        updateLineNumberMargin();

        updateHeader();
    }

    // Put the caret on the symbol (that is what highlights its line) and then
    // scroll the symbol down a couple of lines - setting the caret may scroll
    // it into view, so the scroll has to come second.
    const int position = static_cast<int>(view->positionFromLine(targetLine - 1));
    view->setEmptySelection(position);
    view->setFirstVisibleLine(qMax(0, targetLine - 1 - SymbolTopMarginLines));
}

void ContextPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    emit shown();
}

void ContextPanel::paintEvent(QPaintEvent *)
{
    // The window sits on a page painted in the same colour, so without a line
    // around it the code view would just stop, with nothing to say where the
    // window ends - or where its rims can be grabbed.
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

bool ContextPanel::eventFilter(QObject *watched, QEvent *event)
{
    // The header is the handle the window is moved (reordered) by: the drag is
    // taken here rather than left to travel up from the header, so it starts
    // the same way wherever on the strip it began and keeps following the
    // pointer once it has left the strip.
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

void ContextPanel::mousePressEvent(QMouseEvent *event)
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

void ContextPanel::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();

    if ((event->buttons() & Qt::LeftButton) && widthDragStartX >= 0) {
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

void ContextPanel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && widthDragStartX >= 0) {
        widthDragStartX = -1;
        emit dragEnded();
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void ContextPanel::updateHeader()
{
    const QFileInfo info(loadedFile);
    // The header is the bar the window is dragged by and the one place the file
    // on screen is named, so its tooltip has to carry both facts.
    const QString dragHint = tr("Drag to move the window");

    if (loadedFile.isEmpty() || !info.exists()) {
        headerText->setText(tr("No symbol"));
        header->setToolTip(dragHint);
        closeButton->setEnabled(false);
        return;
    }

    headerText->setText(QStringLiteral("%1 (%2)   %3 bytes; modified on %4")
                            .arg(info.fileName(), info.dir().dirName())
                            .arg(info.size())
                            .arg(info.lastModified().toString(QStringLiteral("yyyy-MM-dd"))));
    header->setToolTip(QStringLiteral("%1\n%2").arg(dragHint, QDir::toNativeSeparators(loadedFile)));
    closeButton->setEnabled(true);
}

void ContextPanel::updateLockButton()
{
    const bool locked = isLocked();

    // Locked is the state worth shouting about, so it takes the accent colour.
    QColor color = locked ? palette().color(QPalette::Highlight) : palette().color(QPalette::WindowText);
    lockButton->setIcon(makeLockIcon(locked, color));
    lockButton->setToolTip(locked
                               ? tr("Locked: the context no longer follows the caret")
                               : tr("Follows the caret; click to lock it"));
}

void ContextPanel::applyEditorFonts()
{
    auto *settings = qobject_cast<NotepadNextApplication *>(qApp)->getSettings();
    const QByteArray fontName = settings->fontName().toUtf8();
    const int fontSize = settings->fontSize();

    // Like EditorManager::setupEditor: every style carries the font, so the
    // colours a language paints later only have to set colours.
    for (int style = 0; style <= STYLE_MAX; ++style) {
        view->styleSetFont(style, fontName.constData());
        view->styleSetSize(style, fontSize);
    }

    view->styleSetFore(STYLE_LINENUMBER, 0x808080);
    view->styleSetBack(STYLE_LINENUMBER, 0xE4E4E4);
}

void ContextPanel::updateLineNumberMargin()
{
    const int digits = QString::number(qMax(1, view->lineCount())).size();
    const QByteArray sample(digits, '9');
    view->setMarginWidthN(0, view->textWidth(STYLE_LINENUMBER, sample.constData()));
}
