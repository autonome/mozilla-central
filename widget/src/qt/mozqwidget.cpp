/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=4 et sw=4 tw=80: */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Nokia.
 *
 * The Initial Developer of the Original Code is Nokia Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <QtGui/QApplication>
#include <QtGui/QCursor>
#include <QtGui/QInputContext>
#include <QtGui/QGraphicsSceneContextMenuEvent>
#include <QtGui/QGraphicsSceneDragDropEvent>
#include <QtGui/QGraphicsSceneMouseEvent>
#include <QtGui/QGraphicsSceneHoverEvent>
#include <QtGui/QGraphicsSceneWheelEvent>

#include <QtCore/QEvent>
#include <QtCore/QVariant>
#include <QtCore/QTimer>

#include "mozqwidget.h"
#include "nsWindow.h"

#ifdef MOZ_ENABLE_QTMOBILITY
#ifdef MOZ_X11
#include <QX11Info>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
# undef KeyPress
# undef KeyRelease
# undef CursorShape
#endif //MOZ_X11
#endif //MOZ_ENABLE_QTMOBILITY

/*
  Pure Qt is lacking a clear API to get the current state of the VKB (opened
  or closed). So this global is used to track that state for 
  nsWindow::GetIMEEnabled().
*/
static bool gKeyboardOpen = false;

/*
  In case we could not open the keyboard, we will try again when the focus
  event is sent.  This can happen if the keyboard is asked for before the
  window is focused. This global is used to track that case.
*/
static bool gFailedOpenKeyboard = false;
 
/*
  For websites that focus editable elements during other operations for a very
  short time, we add some decoupling to prevent the VKB from appearing and 
  reappearing for a very short time. This global is set when the keyboard should
  be opened and if it is still set when a timer runs out, the VKB is really
  shown.
*/
static bool gPendingVKBOpen = false;

/*
  Contains the last preedit String, this is needed in order to generate KeyEvents
*/
static QString gLastPreeditString;

MozQWidget::MozQWidget(nsWindow* aReceiver, QGraphicsItem* aParent)
    : QGraphicsWidget(aParent),
      mReceiver(aReceiver)
{
#if (QT_VERSION >= QT_VERSION_CHECK(4, 6, 0))
     setFlag(QGraphicsItem::ItemAcceptsInputMethod);
     setAcceptTouchEvents(true);
#endif
     setAcceptHoverEvents(true);
}

MozQWidget::~MozQWidget()
{
    if (mReceiver)
        mReceiver->QWidgetDestroyed();
}

void MozQWidget::paint(QPainter* aPainter, const QStyleOptionGraphicsItem* aOption, QWidget* aWidget /*= 0*/)
{
    mReceiver->DoPaint(aPainter, aOption, aWidget);
}

void MozQWidget::activate()
{
    // ensure that the keyboard is hidden when we activate the window
    hideVKB();
    mReceiver->DispatchActivateEventOnTopLevelWindow();
}

void MozQWidget::deactivate()
{
    // ensure that the keyboard is hidden when we deactivate the window
    hideVKB();
    mReceiver->DispatchDeactivateEventOnTopLevelWindow();
}

void MozQWidget::resizeEvent(QGraphicsSceneResizeEvent* aEvent)
{
    mReceiver->OnResizeEvent(aEvent);
}

void MozQWidget::contextMenuEvent(QGraphicsSceneContextMenuEvent* aEvent)
{
    mReceiver->contextMenuEvent(aEvent);
}

void MozQWidget::dragEnterEvent(QGraphicsSceneDragDropEvent* aEvent)
{
    mReceiver->OnDragEnter(aEvent);
}

void MozQWidget::dragLeaveEvent(QGraphicsSceneDragDropEvent* aEvent)
{
    mReceiver->OnDragLeaveEvent(aEvent);
}

void MozQWidget::dragMoveEvent(QGraphicsSceneDragDropEvent* aEvent)
{
    mReceiver->OnDragMotionEvent(aEvent);
}

void MozQWidget::dropEvent(QGraphicsSceneDragDropEvent* aEvent)
{
    mReceiver->OnDragDropEvent(aEvent);
}

void MozQWidget::focusInEvent(QFocusEvent* aEvent)
{
    mReceiver->OnFocusInEvent(aEvent);

    // The application requested the VKB during startup but did not manage
    // to open it, because there was no focused window yet so we do it now by
    // requesting the VKB without any timeout.
    if (gFailedOpenKeyboard)
        requestVKB(0);
}

#ifdef MOZ_ENABLE_QTMOBILITY
void MozQWidget::orientationChanged()
{
    if (!scene() || !scene()->views().size()) {
        return;
    }

    NS_ASSERTION(scene()->views().size() == 1, "Not exactly one view for our scene!");
    QTransform& transform = MozQOrientationSensorFilter::GetRotationTransform();
    QRect scrTrRect = transform.mapRect(scene()->views()[0]->rect());

    setTransformOriginPoint(scene()->views()[0]->size().width() / 2, scene()->views()[0]->size().height() / 2);
    scene()->views()[0]->setTransform(transform);
    int orientation = MozQOrientationSensorFilter::GetWindowRotationAngle();
    if (orientation == 0 || orientation ==  180) {
        setPos(0,0);
    } else {
        setPos(-(scrTrRect.size().width() - scrTrRect.size().height()) / 2,
               (scrTrRect.size().width() - scrTrRect.size().height()) / 2);
    }
    resize(scrTrRect.size());
    scene()->setSceneRect(QRectF(QPointF(0, 0), scrTrRect.size()));
#ifdef MOZ_X11
    Display* display = QX11Info::display();
    if (!display) {
        return;
    }

    Atom orientationAngleAtom = XInternAtom(display, "_MEEGOTOUCH_ORIENTATION_ANGLE", False);
    XChangeProperty(display, scene()->views()[0]->effectiveWinId(),
                    orientationAngleAtom, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)&orientation, 1);
#endif
}
#endif

void MozQWidget::focusOutEvent(QFocusEvent* aEvent)
{
    mReceiver->OnFocusOutEvent(aEvent);
    //OtherFocusReason most like means VKB was closed manual (done button)
    if (aEvent->reason() == Qt::OtherFocusReason && gKeyboardOpen) {
        hideVKB();
    }
}

void MozQWidget::hoverEnterEvent(QGraphicsSceneHoverEvent* aEvent)
{
    mReceiver->OnEnterNotifyEvent(aEvent);
}

void MozQWidget::hoverLeaveEvent(QGraphicsSceneHoverEvent* aEvent)
{
    mReceiver->OnLeaveNotifyEvent(aEvent);
}

void MozQWidget::hoverMoveEvent(QGraphicsSceneHoverEvent* aEvent)
{
    mReceiver->OnMotionNotifyEvent(aEvent->pos(), aEvent->modifiers());
}

void MozQWidget::keyPressEvent(QKeyEvent* aEvent)
{
#if (MOZ_PLATFORM_MAEMO == 6)
    if (!gKeyboardOpen ||
       //those might get sended as KeyEvents, even in 'NormalMode'
       aEvent->key() == Qt::Key_Space ||
       aEvent->key() == Qt::Key_Return ||
       aEvent->key() == Qt::Key_Backspace) {
        mReceiver->OnKeyPressEvent(aEvent);
    }
#elif (MOZ_PLATFORM_MAEMO == 5)
    // Below removed to prevent invertion of upper and lower case
    // See bug 561234
    // mReceiver->OnKeyPressEvent(aEvent);
#else
    mReceiver->OnKeyPressEvent(aEvent);
#endif
}

void MozQWidget::keyReleaseEvent(QKeyEvent* aEvent)
{
#if (MOZ_PLATFORM_MAEMO == 6)
    if (!gKeyboardOpen ||
       //those might get sended as KeyEvents, even in 'NormalMode'
       aEvent->key() == Qt::Key_Space ||
       aEvent->key() == Qt::Key_Return ||
       aEvent->key() == Qt::Key_Backspace) {
        mReceiver->OnKeyReleaseEvent(aEvent);
    }
    return;
#elif (MOZ_PLATFORM_MAEMO == 5)
    // Below line should be removed when bug 561234 is fixed
    mReceiver->OnKeyPressEvent(aEvent);
#endif
    mReceiver->OnKeyReleaseEvent(aEvent);
}

void MozQWidget::inputMethodEvent(QInputMethodEvent* aEvent)
{
    QString currentPreeditString = aEvent->preeditString();
    QString currentCommitString = aEvent->commitString();

    //first check for some controllkeys send as text...
    if (currentCommitString == " ") {
        sendPressReleaseKeyEvent(Qt::Key_Space, currentCommitString.unicode());
    } else if (currentCommitString == "\n") {
        sendPressReleaseKeyEvent(Qt::Key_Return, currentCommitString.unicode());
    } else if (currentCommitString.isEmpty()) {
        //if its no controllkey than check if current Commit is empty
        //if yes than we have some preedit text here
        if (currentPreeditString.length() == 1 && gLastPreeditString.isEmpty()) {
            //Preedit text can change its entire look'a'like
            //check if length of new compared to the old is 1,
            //means that its a new startup
            sendPressReleaseKeyEvent(0, currentPreeditString.unicode());
        } else if (currentPreeditString.startsWith(gLastPreeditString)) {
            //Length was not 1 or not a new startup
            //check if the current preedit starts with the last one,
            //if so: Add new letters (note: this can be more then one new letter)
            const QChar * text = currentPreeditString.unicode();
            for (int i = gLastPreeditString.length(); i < currentPreeditString.length(); i++) {
                sendPressReleaseKeyEvent(0, &text[i]);
            }
        } else {
            //last possible case, we had a PreeditString which was now completely changed.
            //first, check if just one letter was removed (normal Backspace case!)
            //if so: just send the backspace
            QString tempLastPre = gLastPreeditString;
            tempLastPre.truncate(gLastPreeditString.length()-1);
            if (currentPreeditString == tempLastPre) {
                sendPressReleaseKeyEvent(Qt::Key_Backspace);
            } else if (currentPreeditString != tempLastPre) {
                //more than one character changed, so just renew everything
                //delete all preedit
                for (int i = 0; i < gLastPreeditString.length(); i++) {
                    sendPressReleaseKeyEvent(Qt::Key_Backspace);
                }
                //send new Preedit
                const QChar * text = currentPreeditString.unicode();
                for (int i = 0; i < currentPreeditString.length(); i++) {
                    sendPressReleaseKeyEvent(0, &text[i]);
                }
            }
        }
    } else if (gLastPreeditString != currentCommitString) {
        //User commited something
        if (currentCommitString.length() == 1 && gLastPreeditString.isEmpty()) {
            //if commit string ist one and there is no Preedit String
            //case i.e. when no error correction is enabled in the system (default meego.com)
            sendPressReleaseKeyEvent(0, currentCommitString.unicode());
        } else {
            //There is a Preedit, first remove it
            for (int i = 0; i < gLastPreeditString.length(); i++) {
                sendPressReleaseKeyEvent(Qt::Key_Backspace);
            }
            //Now push commited String into
            const QChar * text = currentCommitString.unicode();
            for (int i = 0; i < currentCommitString.length(); i++) {
                sendPressReleaseKeyEvent(0, &text[i]);
            }
        }
    }

    //save preedit for next round.
    gLastPreeditString = currentPreeditString;

    //pre edit is continues string of new chars pressed by the user.
    //if pre edit is changing rapidly without commit string first then user choose some overed text
    //if commitstring comes directly after, forget about it
    QGraphicsWidget::inputMethodEvent(aEvent);
}

void MozQWidget::sendPressReleaseKeyEvent(int key,
                                          const QChar* letter,
                                          bool autorep,
                                          ushort count)
{
     Qt::KeyboardModifiers modifiers  = Qt::NoModifier;
     if (letter && letter->isUpper()) {
         modifiers = Qt::ShiftModifier;
     }

     QString text = letter ? QString(*letter) : QString();

     QKeyEvent press(QEvent::KeyPress, key, modifiers, text, autorep, count);
     mReceiver->OnKeyPressEvent(&press);
     QKeyEvent release(QEvent::KeyRelease, key, modifiers, text, autorep, count);
     mReceiver->OnKeyReleaseEvent(&release);
}

void MozQWidget::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* aEvent)
{
    // Qt sends double click event, but not second press event.
    mReceiver->OnButtonPressEvent(aEvent);
    mReceiver->OnMouseDoubleClickEvent(aEvent);
}

void MozQWidget::mouseMoveEvent(QGraphicsSceneMouseEvent* aEvent)
{
    mReceiver->OnMotionNotifyEvent(aEvent->pos(), aEvent->modifiers());
}

void MozQWidget::mousePressEvent(QGraphicsSceneMouseEvent* aEvent)
{
    mReceiver->OnButtonPressEvent(aEvent);
}

void MozQWidget::mouseReleaseEvent(QGraphicsSceneMouseEvent* aEvent)
{
    mReceiver->OnButtonReleaseEvent(aEvent);
}

bool MozQWidget::event ( QEvent * event )
{
    // check receiver, since due to deleteLater() call it's possible, that
    // events pass loop after receiver's destroy and while widget is still alive
    if (!mReceiver)
        return QGraphicsWidget::event(event);

#if (QT_VERSION >= QT_VERSION_CHECK(4, 6, 0))
    switch (event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchEnd:
    case QEvent::TouchUpdate:
    {
        // Do not send this event to other handlers, this is needed
        // to be able to receive the gesture events
        PRBool handled = PR_FALSE;
        mReceiver->OnTouchEvent(static_cast<QTouchEvent *>(event),handled);
        return handled;
    }
    case (QEvent::Gesture):
    {
        PRBool handled = PR_FALSE;
        mReceiver->OnGestureEvent(static_cast<QGestureEvent*>(event),handled);
        return handled;
    }
#if (MOZ_PLATFORM_MAEMO != 6)
    // This does not work for maemo6, due to partially implemented IM framework
    case QEvent::InputMethod:
    {
        PRBool handled = PR_FALSE;
        mReceiver->imComposeEvent(static_cast<QInputMethodEvent*>(event),handled);
        return handled;
    }
#endif

    default:
        break;
    }
#endif
    return QGraphicsWidget::event(event);
}

void MozQWidget::wheelEvent(QGraphicsSceneWheelEvent* aEvent)
{
    mReceiver->OnScrollEvent(aEvent);
}

void MozQWidget::closeEvent(QCloseEvent* aEvent)
{
    mReceiver->OnCloseEvent(aEvent);
}

void MozQWidget::hideEvent(QHideEvent* aEvent)
{
    mReceiver->hideEvent(aEvent);
    QGraphicsWidget::hideEvent(aEvent);
}

void MozQWidget::showEvent(QShowEvent* aEvent)
{
    mReceiver->showEvent(aEvent);
    QGraphicsWidget::showEvent(aEvent);
}

bool MozQWidget::SetCursor(nsCursor aCursor)
{
    Qt::CursorShape cursor = Qt::ArrowCursor;
    switch(aCursor) {
    case eCursor_standard:
        cursor = Qt::ArrowCursor;
        break;
    case eCursor_wait:
        cursor = Qt::WaitCursor;
        break;
    case eCursor_select:
        cursor = Qt::IBeamCursor;
        break;
    case eCursor_hyperlink:
        cursor = Qt::PointingHandCursor;
        break;
    case eCursor_ew_resize:
        cursor = Qt::SplitHCursor;
        break;
    case eCursor_ns_resize:
        cursor = Qt::SplitVCursor;
        break;
    case eCursor_nw_resize:
    case eCursor_se_resize:
        cursor = Qt::SizeBDiagCursor;
        break;
    case eCursor_ne_resize:
    case eCursor_sw_resize:
        cursor = Qt::SizeFDiagCursor;
        break;
    case eCursor_crosshair:
    case eCursor_move:
        cursor = Qt::SizeAllCursor;
        break;
    case eCursor_help:
        cursor = Qt::WhatsThisCursor;
        break;
    case eCursor_copy:
    case eCursor_alias:
        break;
    case eCursor_context_menu:
    case eCursor_cell:
    case eCursor_grab:
    case eCursor_grabbing:
    case eCursor_spinning:
    case eCursor_zoom_in:
    case eCursor_zoom_out:

    default:
        break;
    }

    setCursor(cursor);

    return NS_OK;
}

bool MozQWidget::SetCursor(const QPixmap& aCursor, int aHotX, int aHotY)
{
    QCursor bitmapCursor(aCursor, aHotX, aHotY);
    setCursor(bitmapCursor);

    return NS_OK;
}

void MozQWidget::setModal(bool modal)
{
#if QT_VERSION >= 0x040600
    setPanelModality(modal ? QGraphicsItem::SceneModal : QGraphicsItem::NonModal);
#else
    LOG(("Modal QGraphicsWidgets not supported in Qt < 4.6\n"));
#endif
}

QVariant MozQWidget::inputMethodQuery(Qt::InputMethodQuery aQuery) const
{
    // The following query uses enums for the values, which are defined in
    // MeegoTouch headers, because this should also work in the pure Qt case
    // we use the values directly here. The original values are in the comments.
    if (static_cast<Qt::InputMethodQuery>(/*M::ImModeQuery*/ 10004 ) == aQuery)
    {
        return QVariant(/*M::InputMethodModeNormal*/ 0 );
    }

    return QGraphicsWidget::inputMethodQuery(aQuery);
}

/**
  Request the VKB and starts a timer with the given timeout in milliseconds.
  If the request is not canceled when the timer runs out, the VKB is actually
  shown.
*/
void MozQWidget::requestVKB(int aTimeout)
{
    if (!gPendingVKBOpen) {
        gPendingVKBOpen = true;

        if (aTimeout == 0)
            showVKB();
        else
            QTimer::singleShot(aTimeout, this, SLOT(showVKB()));
    }
}



void MozQWidget::showVKB()
{
    // skip showing of keyboard if not pending
    if (!gPendingVKBOpen)
        return;

    gPendingVKBOpen = false;

#if (QT_VERSION >= QT_VERSION_CHECK(4, 6, 0))
    QWidget* focusWidget = qApp->focusWidget();

    if (focusWidget) {
        QInputContext *inputContext = qApp->inputContext();
        if (!inputContext) {
            NS_WARNING("Requesting SIP: but no input context");
            return;
        }

        QEvent request(QEvent::RequestSoftwareInputPanel);
        inputContext->filterEvent(&request);
        focusWidget->setAttribute(Qt::WA_InputMethodEnabled, true);
        inputContext->setFocusWidget(focusWidget);
        gKeyboardOpen = true;
        gFailedOpenKeyboard = false;
    }
    else
    {
        // No focused widget yet, so we have to open the VKB later on.
        gFailedOpenKeyboard = true;
    }
#else
    LOG(("VKB not supported in Qt < 4.6\n"));
#endif
}

void MozQWidget::hideVKB()
{
    if (gPendingVKBOpen) {
        // do not really open
        gPendingVKBOpen = false;
    }

    if (!gKeyboardOpen) {
        return;
    }

#if (QT_VERSION >= QT_VERSION_CHECK(4, 6, 0))
    QInputContext *inputContext = qApp->inputContext();
    if (!inputContext) {
        NS_WARNING("Closing SIP: but no input context");
        return;
    }

    QEvent request(QEvent::CloseSoftwareInputPanel);
    inputContext->filterEvent(&request);
    inputContext->reset();
    gKeyboardOpen = false;
#else
    LOG(("VKB not supported in Qt < 4.6\n"));
#endif
}

bool MozQWidget::isVKBOpen()
{
    return gKeyboardOpen;
}
