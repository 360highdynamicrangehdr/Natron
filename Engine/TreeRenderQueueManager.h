/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
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

#ifndef NATRON_ENGINE_TREERENDERQUEUEMANAGER_H
#define NATRON_ENGINE_TREERENDERQUEUEMANAGER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/EngineFwd.h"

#include <QThread>

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
#include <boost/scoped_ptr.hpp>
#endif

#include "Engine/TreeRenderQueueProvider.h"


NATRON_NAMESPACE_ENTER;

/**
 * @brief An object global to the application that schedules multiple TreeRender objects and maximize CPU utilization.
 * When the manager process a TreeRender, it first tries to launch as many parallel tasks as possible for that tree.
 * When reaching a synchronization point, instead of waiting for results to be available and if resources allow it, we start
 * processing tasks on TreeRender that are further up in the queue.
 **/
class TreeRenderQueueManager
: public QThread
, public TreeRenderLauncherI
{
    struct Implementation;

public:

    TreeRenderQueueManager();

    virtual ~TreeRenderQueueManager();

    /**
     * @brief Stops the thread
     **/
    void quitThread();


    /**
     * @brief Returns the index of the given render and the number of renders currently in the execution queue
     **/
    void getRenderIndex(const TreeRenderPtr& render, int* index, int* numRenders) const;

private:


    /// The methods below are to be called exclusively from TreeRenderQueueProvider
    friend class TreeRenderQueueProvider;

    /// Overriden from TreeRenderLauncherI
    virtual void launchRender(const TreeRenderPtr& render) OVERRIDE FINAL;
    virtual TreeRenderExecutionDataPtr launchSubRender(const EffectInstancePtr& treeRoot,
                                                       TimeValue time,
                                                       ViewIdx view,
                                                       const RenderScale& proxyScale,
                                                       unsigned int mipMapLevel,
                                                       const ImagePlaneDesc* planeParam,
                                                       const RectD* canonicalRoIParam,
                                                       const TreeRenderPtr& render,
                                                       int concatenationFlags) OVERRIDE FINAL;
    virtual ActionRetCodeEnum waitForRenderFinished(const TreeRenderPtr& render) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ///


    /**
     * @brief Returns true if there's any ongoing render execution for this provider.
     **/
    bool hasTreeRendersLaunched(const TreeRenderQueueProviderConstPtr& provider) const;

    /**
     * @brief Returns true if there's any render execution finished for this provider.
     **/
    bool hasTreeRendersFinished(const TreeRenderQueueProviderConstPtr& provider) const;
    
    /**
     * @brief Returns a tree render execution that has finished for this provider.
     * Note that if multiple renders are finished, this will return just one of them,
     * you need to iterate until hasTreeRendersFinished() returns false to retrieve them all.
     * Note that if no render is in the queue, this function returns a NULL pointer
     **/
    TreeRenderPtr waitForAnyTreeRenderFinished(const TreeRenderQueueProviderConstPtr& provider);


private:

    virtual void run() OVERRIDE FINAL;


    /**
     * @brief Executed on a thread-pool thread when a FrameViewRenderRunnable is finished
     **/
    void notifyTaskInRenderFinished(const TreeRenderExecutionDataPtr& render, bool isExecutionFinished, bool isRunningInThreadPoolThread);

    friend class TreeRenderExecutionData;
    friend struct TreeRenderExecutionDataPrivate;
    boost::scoped_ptr<Implementation> _imp;
};
NATRON_NAMESPACE_EXIT;

#endif // NATRON_ENGINE_TREERENDERQUEUEMANAGER_H
