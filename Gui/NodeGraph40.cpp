/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NodeGraph.h"
#include "NodeGraphPrivate.h"

#include <sstream>
#include <stdexcept>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QThread>
#include <QFile>
#include <QUrl>
#include <QtCore/QMimeData>

#include "Engine/Node.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/NodeGroup.h"
#include "Engine/RotoLayer.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"

#include "Gui/BackdropGui.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/ScriptEditor.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGui.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/NodeClipBoard.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/SerializationIO.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER;
void
NodeGraph::togglePreviewsForSelectedNodes()
{
    bool empty = false;
    {
        QMutexLocker l(&_imp->_nodesMutex);
        empty = _imp->_selection.empty();
        for (NodesGuiList::iterator it = _imp->_selection.begin();
             it != _imp->_selection.end();
             ++it) {
            (*it)->togglePreview();
        }
    }

    if (empty) {
        Dialogs::warningDialog( tr("Toggle Preview").toStdString(), tr("You must select a node first").toStdString() );
    }
}

void
NodeGraph::showSelectedNodeSettingsPanel()
{
    if (_imp->_selection.size() == 1) {
        showNodePanel(false, false, _imp->_selection.front());
    }
}

void
NodeGraph::switchInputs1and2ForSelectedNodes()
{
    QMutexLocker l(&_imp->_nodesMutex);

    for (NodesGuiList::iterator it = _imp->_selection.begin();
         it != _imp->_selection.end();
         ++it) {
        (*it)->onSwitchInputActionTriggered();
    }
}

void
NodeGraph::centerOnItem(QGraphicsItem* item)
{
    _imp->_refreshOverlays = true;
    centerOn(item);
}

void
NodeGraph::copyNodes(const NodesGuiList& nodes,
                     SERIALIZATION_NAMESPACE::NodeClipBoard& clipboard)
{
    _imp->copyNodesInternal(nodes, clipboard);
}

void
NodeGraph::copySelectedNodes()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Copy").toStdString(), tr("You must select at least a node to copy first.").toStdString() );

        return;
    }

    SERIALIZATION_NAMESPACE::NodeClipBoard& cb = appPTR->getNodeClipBoard();
    _imp->copyNodesInternal(_imp->_selection, cb);

    std::ostringstream ss;

    try {
        SERIALIZATION_NAMESPACE::write(ss, cb);
    } catch (...) {
        qDebug() << "Failed to copy selection to system clipboard";
    }

    QMimeData* mimedata = new QMimeData;
    QByteArray data( ss.str().c_str() );
    mimedata->setData(QLatin1String("text/plain"), data);
    QClipboard* clipboard = QApplication::clipboard();

    //ownership is transferred to the clipboard
    clipboard->setMimeData(mimedata);
}

void
NodeGraph::cutSelectedNodes()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Cut").toStdString(), tr("You must select at least a node to cut first.").toStdString() );

        return;
    }
    copySelectedNodes();
    deleteSelection();
}

void
NodeGraph::pasteCliboard(const SERIALIZATION_NAMESPACE::NodeClipBoard& clipboard,
                         std::list<std::pair<std::string, NodeGuiPtr > >* newNodes)
{
    QPointF position =  mapToScene( mapFromGlobal( QCursor::pos() ) );

    _imp->pasteNodesInternal(clipboard.nodes, position, false, newNodes);
}

bool
NodeGraph::tryReadClipboard(const QPointF& pos, std::istream& ss)
{

    
    // Check if this is a regular clipboard
    // This will also check if this is a single node
    try {
        SERIALIZATION_NAMESPACE::NodeClipBoard& cb = appPTR->getNodeClipBoard();
        SERIALIZATION_NAMESPACE::read(ss, &cb);

        for (SERIALIZATION_NAMESPACE::NodeSerializationList::const_iterator it = cb.nodes.begin(); it!=cb.nodes.end(); ++it) {
            // This is a pyplug, convert it to a group
            if ((*it)->_encodeType == SERIALIZATION_NAMESPACE::NodeSerialization::eNodeSerializationTypePyPlug) {
                (*it)->_pluginID = PLUGINID_NATRON_GROUP;
            }
        }

        _imp->pasteNodesInternal(cb.nodes, pos, true);
    } catch (...) {
        
        // Check if this was copy/pasted from a project directly
        try {
            ss.seekg(0);
            SERIALIZATION_NAMESPACE::ProjectSerialization isProject;
            SERIALIZATION_NAMESPACE::read(ss, &isProject);
            _imp->pasteNodesInternal(isProject._nodes, pos, true);
        } catch (...) {
            
            return false;
        }
    }
    

    return true;

}

bool
NodeGraph::pasteClipboard(const QPointF& pos)
{
    QPointF position;
    if (pos.x() == INT_MIN || pos.y() == INT_MIN) {
        position =  mapToScene( mapFromGlobal( QCursor::pos() ) );
    } else {
        position = pos;
    }

    QClipboard* clipboard = QApplication::clipboard();
    const QMimeData* mimedata = clipboard->mimeData();

    // If this is a list of files, try to open them
    if ( mimedata->hasUrls() ) {
        QList<QUrl> urls = mimedata->urls();
        getGui()->handleOpenFilesFromUrls( urls, QCursor::pos() );
        return true;
    }

    if ( !mimedata->hasFormat( QLatin1String("text/plain") ) ) {
        return false;
    }
    QByteArray data = mimedata->data( QLatin1String("text/plain") );
    std::string s = QString::fromUtf8(data).toStdString();
    std::istringstream ss(s);
    return tryReadClipboard(position, ss);
    
}


void
NodeGraph::duplicateSelectedNodes(const QPointF& pos)
{
    if ( _imp->_selection.empty() && _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Duplicate").toStdString(), tr("You must select at least a node to duplicate first.").toStdString() );

        return;
    }

    ///Don't use the member clipboard as the user might have something copied
    SERIALIZATION_NAMESPACE::NodeClipBoard tmpClipboard;
    _imp->copyNodesInternal(_imp->_selection, tmpClipboard);
    _imp->pasteNodesInternal(tmpClipboard.nodes, pos, true);
}

void
NodeGraph::duplicateSelectedNodes()
{
    QPointF scenePos = mapToScene( mapFromGlobal( QCursor::pos() ) );

    duplicateSelectedNodes(scenePos);
}

void
NodeGraph::cloneSelectedNodes(const QPointF& scenePos)
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Clone").toStdString(), tr("You must select at least a node to clone first.").toStdString() );

        return;
    }

    double xmax = INT_MIN;
    double xmin = INT_MAX;
    double ymin = INT_MAX;
    double ymax = INT_MIN;
    NodesGuiList nodesToCopy = _imp->_selection;
    for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        if ( (*it)->getNode()->getMasterNode() ) {
            Dialogs::errorDialog( tr("Clone").toStdString(), tr("You cannot clone a node which is already a clone.").toStdString() );

            return;
        }
        QRectF bbox = (*it)->mapToScene( (*it)->boundingRect() ).boundingRect();
        if ( ( bbox.x() + bbox.width() ) > xmax ) {
            xmax = ( bbox.x() + bbox.width() );
        }
        if (bbox.x() < xmin) {
            xmin = bbox.x();
        }

        if ( ( bbox.y() + bbox.height() ) > ymax ) {
            ymax = ( bbox.y() + bbox.height() );
        }
        if (bbox.y() < ymin) {
            ymin = bbox.y();
        }

        ///Also copy all nodes within the backdrop
        BackdropGuiPtr isBd =toBackdropGui(*it);
        if (isBd) {
            NodesGuiList nodesWithinBD = getNodesWithinBackdrop(*it);
            for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
                NodesGuiList::iterator found = std::find(nodesToCopy.begin(), nodesToCopy.end(), *it2);
                if ( found == nodesToCopy.end() ) {
                    nodesToCopy.push_back(*it2);
                }
            }
        }
    }

    for (NodesGuiList::iterator it = nodesToCopy.begin(); it != nodesToCopy.end(); ++it) {
        if ( (*it)->getNode()->getEffectInstance()->isSlave() ) {
            Dialogs::errorDialog( tr("Clone").toStdString(), tr("You cannot clone a node which is already a clone.").toStdString() );

            return;
        }
        ViewerInstancePtr isViewer = (*it)->getNode()->isEffectViewerInstance();
        if (isViewer) {
            Dialogs::errorDialog( tr("Clone").toStdString(), tr("Cloning a viewer is not a valid operation.").toStdString() );

            return;
        }

    }

    QPointF offset((xmax + xmin) / 2.,(ymax + ymin) / 2.);

    NodesGuiList newNodesList;
    for (NodesGuiList::iterator it = nodesToCopy.begin(); it != nodesToCopy.end(); ++it) {
        SERIALIZATION_NAMESPACE::NodeSerializationPtr  internalSerialization( new SERIALIZATION_NAMESPACE::NodeSerialization );
        (*it)->getNode()->toSerialization(internalSerialization.get());
        NodeGuiPtr clone = NodeGraphPrivate::pasteNode(internalSerialization, offset, scenePos,
                                           _imp->group.lock(), (*it)->getNode());

        if (clone) {
            newNodesList.push_back(clone);
        }

    }


    pushUndoCommand( new AddMultipleNodesCommand(this, newNodesList) );
} // NodeGraph::cloneSelectedNodes

void
NodeGraph::cloneSelectedNodes()
{
    QPointF scenePos = mapToScene( mapFromGlobal( QCursor::pos() ) );

    cloneSelectedNodes(scenePos);
} // cloneSelectedNodes

void
NodeGraph::decloneSelectedNodes()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Declone").toStdString(), tr("You must select at least a node to declone first.").toStdString() );

        return;
    }
    NodesGuiList nodesToDeclone;


    for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        BackdropGuiPtr isBd = toBackdropGui(*it);
        if (isBd) {
            ///Also copy all nodes within the backdrop
            NodesGuiList nodesWithinBD = getNodesWithinBackdrop(*it);
            for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
                NodesGuiList::iterator found = std::find(nodesToDeclone.begin(), nodesToDeclone.end(), *it2);
                if ( found == nodesToDeclone.end() ) {
                    nodesToDeclone.push_back(*it2);
                }
            }
        }
        if ( (*it)->getNode()->getEffectInstance()->isSlave() ) {
            nodesToDeclone.push_back(*it);
        }
    }

    pushUndoCommand( new DecloneMultipleNodesCommand(this, nodesToDeclone) );
}

void
NodeGraph::setUndoRedoStackLimit(int limit)
{
    _imp->_undoStack->clear();
    _imp->_undoStack->setUndoLimit(limit);
}

void
NodeGraph::deleteNodePermanantly(const NodeGuiPtr& n)
{
    assert(n);
    NodesGuiList::iterator it = std::find(_imp->_nodesTrash.begin(), _imp->_nodesTrash.end(), n);

    if ( it != _imp->_nodesTrash.end() ) {
        _imp->_nodesTrash.erase(it);
    }

    {
        QMutexLocker l(&_imp->_nodesMutex);
        NodesGuiList::iterator it = std::find(_imp->_nodes.begin(), _imp->_nodes.end(), n);
        if ( it != _imp->_nodes.end() ) {
            _imp->_nodes.erase(it);
        }
    }

    NodesGuiList::iterator found = std::find(_imp->_selection.begin(), _imp->_selection.end(), n);
    if ( found != _imp->_selection.end() ) {
        n->setUserSelected(false);
        _imp->_selection.erase(found);
    }
} // deleteNodePermanantly

void
NodeGraph::invalidateAllNodesParenting()
{
    for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        (*it)->setParentItem(NULL);
        if ( (*it)->scene() ) {
            (*it)->scene()->removeItem((*it).get());
        }
    }
    for (NodesGuiList::iterator it = _imp->_nodesTrash.begin(); it != _imp->_nodesTrash.end(); ++it) {
        (*it)->setParentItem(NULL);
        if ( (*it)->scene() ) {
            (*it)->scene()->removeItem((*it).get());
        }
    }
}

void
NodeGraph::centerOnAllNodes()
{
    assert( QThread::currentThread() == qApp->thread() );
    double xmin = INT_MAX;
    double xmax = INT_MIN;
    double ymin = INT_MAX;
    double ymax = INT_MIN;
    //_imp->_root->setPos(0,0);

    if ( _imp->_selection.empty() ) {
        QMutexLocker l(&_imp->_nodesMutex);

        for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
            if ( /*(*it)->isActive() &&*/ (*it)->isVisible() ) {
                QSize size = (*it)->getSize();
                QPointF pos = (*it)->mapToScene( (*it)->mapFromParent( (*it)->pos() ) );
                xmin = std::min( xmin, pos.x() );
                xmax = std::max( xmax, pos.x() + size.width() );
                ymin = std::min( ymin, pos.y() );
                ymax = std::max( ymax, pos.y() + size.height() );
            }
        }
    } else {
        for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
            if ( /*(*it)->isActive() && */ (*it)->isVisible() ) {
                QSize size = (*it)->getSize();
                QPointF pos = (*it)->mapToScene( (*it)->mapFromParent( (*it)->pos() ) );
                xmin = std::min( xmin, pos.x() );
                xmax = std::max( xmax, pos.x() + size.width() );
                ymin = std::min( ymin, pos.y() );
                ymax = std::max( ymax, pos.y() + size.height() );
            }
        }
    }
    QRectF bbox( xmin, ymin, (xmax - xmin), (ymax - ymin) );
    fitInView(bbox, Qt::KeepAspectRatio);

    double currentZoomFactor = transform().mapRect( QRectF(0, 0, 1, 1) ).width();
    assert(currentZoomFactor != 0);
    //we want to fit at scale 1 at most
    if (currentZoomFactor > 1.) {
        double scaleFactor = 1. / currentZoomFactor;
        setTransformationAnchor(QGraphicsView::AnchorViewCenter);
        scale(scaleFactor, scaleFactor);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    }

    _imp->_refreshOverlays = true;
    update();
} // NodeGraph::centerOnAllNodes

NATRON_NAMESPACE_EXIT;
