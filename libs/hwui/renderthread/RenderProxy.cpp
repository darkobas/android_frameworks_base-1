/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RenderProxy.h"

#include "DeferredLayerUpdater.h"
#include "DisplayList.h"
#include "Properties.h"
#include "Readback.h"
#include "Rect.h"
#include "renderthread/CanvasContext.h"
#include "renderthread/EglManager.h"
#include "renderthread/RenderTask.h"
#include "renderthread/RenderThread.h"
#include "renderstate/RenderState.h"
#include "utils/Macros.h"
#include "utils/TimeUtils.h"

#include <ui/GraphicBuffer.h>

namespace android {
namespace uirenderer {
namespace renderthread {

#define ARGS(method) method ## Args

#define CREATE_BRIDGE0(name) CREATE_BRIDGE(name,,,,,,,,)
#define CREATE_BRIDGE1(name, a1) CREATE_BRIDGE(name, a1,,,,,,,)
#define CREATE_BRIDGE2(name, a1, a2) CREATE_BRIDGE(name, a1,a2,,,,,,)
#define CREATE_BRIDGE3(name, a1, a2, a3) CREATE_BRIDGE(name, a1,a2,a3,,,,,)
#define CREATE_BRIDGE4(name, a1, a2, a3, a4) CREATE_BRIDGE(name, a1,a2,a3,a4,,,,)
#define CREATE_BRIDGE5(name, a1, a2, a3, a4, a5) CREATE_BRIDGE(name, a1,a2,a3,a4,a5,,,)
#define CREATE_BRIDGE6(name, a1, a2, a3, a4, a5, a6) CREATE_BRIDGE(name, a1,a2,a3,a4,a5,a6,,)
#define CREATE_BRIDGE7(name, a1, a2, a3, a4, a5, a6, a7) CREATE_BRIDGE(name, a1,a2,a3,a4,a5,a6,a7,)
#define CREATE_BRIDGE(name, a1, a2, a3, a4, a5, a6, a7, a8) \
    typedef struct { \
        a1; a2; a3; a4; a5; a6; a7; a8; \
    } ARGS(name); \
    static_assert(std::is_trivially_destructible<ARGS(name)>::value, \
            "Error, ARGS must be trivially destructible!"); \
    static void* Bridge_ ## name(ARGS(name)* args)

#define SETUP_TASK(method) \
    LOG_ALWAYS_FATAL_IF( METHOD_INVOKE_PAYLOAD_SIZE < sizeof(ARGS(method)), \
        "METHOD_INVOKE_PAYLOAD_SIZE %zu is smaller than sizeof(" #method "Args) %zu", \
                METHOD_INVOKE_PAYLOAD_SIZE, sizeof(ARGS(method))); \
    MethodInvokeRenderTask* task = new MethodInvokeRenderTask((RunnableMethod) Bridge_ ## method); \
    ARGS(method) *args = (ARGS(method) *) task->payload()

CREATE_BRIDGE4(createContext, RenderThread* thread, bool translucent,
        RenderNode* rootRenderNode, IContextFactory* contextFactory) {
    return CanvasContext::create(*args->thread, args->translucent,
            args->rootRenderNode, args->contextFactory);
}

RenderProxy::RenderProxy(bool translucent, RenderNode* rootRenderNode, IContextFactory* contextFactory)
        : mRenderThread(RenderThread::getInstance())
        , mContext(nullptr) {
    SETUP_TASK(createContext);
    args->translucent = translucent;
    args->rootRenderNode = rootRenderNode;
    args->thread = &mRenderThread;
    args->contextFactory = contextFactory;
    mContext = (CanvasContext*) postAndWait(task);
    mDrawFrameTask.setContext(&mRenderThread, mContext, rootRenderNode);
}

RenderProxy::~RenderProxy() {
    destroyContext();
}

CREATE_BRIDGE1(destroyContext, CanvasContext* context) {
    delete args->context;
    return nullptr;
}

void RenderProxy::destroyContext() {
    if (mContext) {
        SETUP_TASK(destroyContext);
        args->context = mContext;
        mContext = nullptr;
        mDrawFrameTask.setContext(nullptr, nullptr, nullptr);
        // This is also a fence as we need to be certain that there are no
        // outstanding mDrawFrame tasks posted before it is destroyed
        postAndWait(task);
    }
}

CREATE_BRIDGE2(setSwapBehavior, CanvasContext* context, SwapBehavior swapBehavior) {
    args->context->setSwapBehavior(args->swapBehavior);
    return nullptr;
}

void RenderProxy::setSwapBehavior(SwapBehavior swapBehavior) {
    SETUP_TASK(setSwapBehavior);
    args->context = mContext;
    args->swapBehavior = swapBehavior;
    post(task);
}

CREATE_BRIDGE1(loadSystemProperties, CanvasContext* context) {
    bool needsRedraw = false;
    if (Caches::hasInstance()) {
        needsRedraw = Properties::load();
    }
    if (args->context->profiler().consumeProperties()) {
        needsRedraw = true;
    }
    return (void*) needsRedraw;
}

bool RenderProxy::loadSystemProperties() {
    SETUP_TASK(loadSystemProperties);
    args->context = mContext;
    return (bool) postAndWait(task);
}

CREATE_BRIDGE2(setName, CanvasContext* context, const char* name) {
    args->context->setName(std::string(args->name));
    return nullptr;
}

void RenderProxy::setName(const char* name) {
    SETUP_TASK(setName);
    args->context = mContext;
    args->name = name;
    postAndWait(task); // block since name/value pointers owned by caller
}

CREATE_BRIDGE2(initialize, CanvasContext* context, Surface* surface) {
    args->context->initialize(args->surface);
    return nullptr;
}

void RenderProxy::initialize(const sp<Surface>& surface) {
    SETUP_TASK(initialize);
    args->context = mContext;
    args->surface = surface.get();
    post(task);
}

CREATE_BRIDGE2(updateSurface, CanvasContext* context, Surface* surface) {
    args->context->updateSurface(args->surface);
    return nullptr;
}

void RenderProxy::updateSurface(const sp<Surface>& surface) {
    SETUP_TASK(updateSurface);
    args->context = mContext;
    args->surface = surface.get();
    post(task);
}

CREATE_BRIDGE2(pauseSurface, CanvasContext* context, Surface* surface) {
    return (void*) args->context->pauseSurface(args->surface);
}

bool RenderProxy::pauseSurface(const sp<Surface>& surface) {
    SETUP_TASK(pauseSurface);
    args->context = mContext;
    args->surface = surface.get();
    return (bool) postAndWait(task);
}

CREATE_BRIDGE2(setStopped, CanvasContext* context, bool stopped) {
    args->context->setStopped(args->stopped);
    return nullptr;
}

void RenderProxy::setStopped(bool stopped) {
    SETUP_TASK(setStopped);
    args->context = mContext;
    args->stopped = stopped;
    postAndWait(task);
}

CREATE_BRIDGE4(setup, CanvasContext* context,
        float lightRadius, uint8_t ambientShadowAlpha, uint8_t spotShadowAlpha) {
    args->context->setup(args->lightRadius,
            args->ambientShadowAlpha, args->spotShadowAlpha);
    return nullptr;
}

void RenderProxy::setup(float lightRadius,
        uint8_t ambientShadowAlpha, uint8_t spotShadowAlpha) {
    SETUP_TASK(setup);
    args->context = mContext;
    args->lightRadius = lightRadius;
    args->ambientShadowAlpha = ambientShadowAlpha;
    args->spotShadowAlpha = spotShadowAlpha;
    post(task);
}

CREATE_BRIDGE2(setLightCenter, CanvasContext* context, Vector3 lightCenter) {
    args->context->setLightCenter(args->lightCenter);
    return nullptr;
}

void RenderProxy::setLightCenter(const Vector3& lightCenter) {
    SETUP_TASK(setLightCenter);
    args->context = mContext;
    args->lightCenter = lightCenter;
    post(task);
}

CREATE_BRIDGE2(setOpaque, CanvasContext* context, bool opaque) {
    args->context->setOpaque(args->opaque);
    return nullptr;
}

void RenderProxy::setOpaque(bool opaque) {
    SETUP_TASK(setOpaque);
    args->context = mContext;
    args->opaque = opaque;
    post(task);
}

int64_t* RenderProxy::frameInfo() {
    return mDrawFrameTask.frameInfo();
}

int RenderProxy::syncAndDrawFrame() {
    return mDrawFrameTask.drawFrame();
}

CREATE_BRIDGE1(destroy, CanvasContext* context) {
    args->context->destroy();
    return nullptr;
}

void RenderProxy::destroy() {
    SETUP_TASK(destroy);
    args->context = mContext;
    // destroyCanvasAndSurface() needs a fence as when it returns the
    // underlying BufferQueue is going to be released from under
    // the render thread.
    postAndWait(task);
}

CREATE_BRIDGE2(invokeFunctor, RenderThread* thread, Functor* functor) {
    CanvasContext::invokeFunctor(*args->thread, args->functor);
    return nullptr;
}

void RenderProxy::invokeFunctor(Functor* functor, bool waitForCompletion) {
    ATRACE_CALL();
    RenderThread& thread = RenderThread::getInstance();
    SETUP_TASK(invokeFunctor);
    args->thread = &thread;
    args->functor = functor;
    if (waitForCompletion) {
        // waitForCompletion = true is expected to be fairly rare and only
        // happen in destruction. Thus it should be fine to temporarily
        // create a Mutex
        staticPostAndWait(task);
    } else {
        thread.queue(task);
    }
}

CREATE_BRIDGE1(createTextureLayer, CanvasContext* context) {
    return args->context->createTextureLayer();
}

DeferredLayerUpdater* RenderProxy::createTextureLayer() {
    SETUP_TASK(createTextureLayer);
    args->context = mContext;
    void* retval = postAndWait(task);
    DeferredLayerUpdater* layer = reinterpret_cast<DeferredLayerUpdater*>(retval);
    return layer;
}

CREATE_BRIDGE2(buildLayer, CanvasContext* context, RenderNode* node) {
    args->context->buildLayer(args->node);
    return nullptr;
}

void RenderProxy::buildLayer(RenderNode* node) {
    SETUP_TASK(buildLayer);
    args->context = mContext;
    args->node = node;
    postAndWait(task);
}

CREATE_BRIDGE3(copyLayerInto, CanvasContext* context, DeferredLayerUpdater* layer,
        SkBitmap* bitmap) {
    bool success = args->context->copyLayerInto(args->layer, args->bitmap);
    return (void*) success;
}

bool RenderProxy::copyLayerInto(DeferredLayerUpdater* layer, SkBitmap& bitmap) {
    SETUP_TASK(copyLayerInto);
    args->context = mContext;
    args->layer = layer;
    args->bitmap = &bitmap;
    return (bool) postAndWait(task);
}

void RenderProxy::pushLayerUpdate(DeferredLayerUpdater* layer) {
    mDrawFrameTask.pushLayerUpdate(layer);
}

void RenderProxy::cancelLayerUpdate(DeferredLayerUpdater* layer) {
    mDrawFrameTask.removeLayerUpdate(layer);
}

CREATE_BRIDGE1(detachSurfaceTexture, DeferredLayerUpdater* layer) {
    args->layer->detachSurfaceTexture();
    return nullptr;
}

void RenderProxy::detachSurfaceTexture(DeferredLayerUpdater* layer) {
    SETUP_TASK(detachSurfaceTexture);
    args->layer = layer;
    postAndWait(task);
}

CREATE_BRIDGE1(destroyHardwareResources, CanvasContext* context) {
    args->context->destroyHardwareResources();
    return nullptr;
}

void RenderProxy::destroyHardwareResources() {
    SETUP_TASK(destroyHardwareResources);
    args->context = mContext;
    postAndWait(task);
}

CREATE_BRIDGE2(trimMemory, RenderThread* thread, int level) {
    CanvasContext::trimMemory(*args->thread, args->level);
    return nullptr;
}

void RenderProxy::trimMemory(int level) {
    // Avoid creating a RenderThread to do a trimMemory.
    if (RenderThread::hasInstance()) {
        RenderThread& thread = RenderThread::getInstance();
        SETUP_TASK(trimMemory);
        args->thread = &thread;
        args->level = level;
        thread.queue(task);
    }
}

CREATE_BRIDGE2(overrideProperty, const char* name, const char* value) {
    Properties::overrideProperty(args->name, args->value);
    return nullptr;
}

void RenderProxy::overrideProperty(const char* name, const char* value) {
    SETUP_TASK(overrideProperty);
    args->name = name;
    args->value = value;
    staticPostAndWait(task); // expensive, but block here since name/value pointers owned by caller
}

CREATE_BRIDGE0(fence) {
    // Intentionally empty
    return nullptr;
}

template <typename T>
void UNUSED(T t) {}

void RenderProxy::fence() {
    SETUP_TASK(fence);
    UNUSED(args);
    postAndWait(task);
}

void RenderProxy::staticFence() {
    SETUP_TASK(fence);
    UNUSED(args);
    staticPostAndWait(task);
}

CREATE_BRIDGE1(stopDrawing, CanvasContext* context) {
    args->context->stopDrawing();
    return nullptr;
}

void RenderProxy::stopDrawing() {
    SETUP_TASK(stopDrawing);
    args->context = mContext;
    postAndWait(task);
}

CREATE_BRIDGE1(notifyFramePending, CanvasContext* context) {
    args->context->notifyFramePending();
    return nullptr;
}

void RenderProxy::notifyFramePending() {
    SETUP_TASK(notifyFramePending);
    args->context = mContext;
    mRenderThread.queueAtFront(task);
}

CREATE_BRIDGE4(dumpProfileInfo, CanvasContext* context, RenderThread* thread,
        int fd, int dumpFlags) {
    args->context->profiler().dumpData(args->fd);
    if (args->dumpFlags & DumpFlags::FrameStats) {
        args->context->dumpFrames(args->fd);
    }
    if (args->dumpFlags & DumpFlags::Reset) {
        args->context->resetFrameStats();
    }
    if (args->dumpFlags & DumpFlags::JankStats) {
        args->thread->jankTracker().dump(args->fd);
    }
    return nullptr;
}

void RenderProxy::dumpProfileInfo(int fd, int dumpFlags) {
    SETUP_TASK(dumpProfileInfo);
    args->context = mContext;
    args->thread = &mRenderThread;
    args->fd = fd;
    args->dumpFlags = dumpFlags;
    postAndWait(task);
}

CREATE_BRIDGE1(resetProfileInfo, CanvasContext* context) {
    args->context->resetFrameStats();
    return nullptr;
}

void RenderProxy::resetProfileInfo() {
    SETUP_TASK(resetProfileInfo);
    args->context = mContext;
    postAndWait(task);
}

CREATE_BRIDGE2(frameTimePercentile, RenderThread* thread, int percentile) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(
        args->thread->jankTracker().findPercentile(args->percentile)));
}

uint32_t RenderProxy::frameTimePercentile(int p) {
    SETUP_TASK(frameTimePercentile);
    args->thread = &mRenderThread;
    args->percentile = p;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
        postAndWait(task)));
}

CREATE_BRIDGE2(dumpGraphicsMemory, int fd, RenderThread* thread) {
    args->thread->jankTracker().dump(args->fd);

    FILE *file = fdopen(args->fd, "a");
    if (Caches::hasInstance()) {
        String8 cachesLog;
        Caches::getInstance().dumpMemoryUsage(cachesLog);
        fprintf(file, "\nCaches:\n%s\n", cachesLog.string());
    } else {
        fprintf(file, "\nNo caches instance.\n");
    }
    fprintf(file, "\nPipeline=FrameBuilder\n");
    fflush(file);
    return nullptr;
}

void RenderProxy::dumpGraphicsMemory(int fd) {
    if (!RenderThread::hasInstance()) return;
    SETUP_TASK(dumpGraphicsMemory);
    args->fd = fd;
    args->thread = &RenderThread::getInstance();
    staticPostAndWait(task);
}

CREATE_BRIDGE2(setProcessStatsBuffer, RenderThread* thread, int fd) {
    args->thread->jankTracker().switchStorageToAshmem(args->fd);
    close(args->fd);
    return nullptr;
}

void RenderProxy::setProcessStatsBuffer(int fd) {
    SETUP_TASK(setProcessStatsBuffer);
    auto& rt = RenderThread::getInstance();
    args->thread = &rt;
    args->fd = dup(fd);
    rt.queue(task);
}

CREATE_BRIDGE1(rotateProcessStatsBuffer, RenderThread* thread) {
    args->thread->jankTracker().rotateStorage();
    return nullptr;
}

void RenderProxy::rotateProcessStatsBuffer() {
    SETUP_TASK(rotateProcessStatsBuffer);
    auto& rt = RenderThread::getInstance();
    args->thread = &rt;
    rt.queue(task);
}

int RenderProxy::getRenderThreadTid() {
    return mRenderThread.getTid();
}

CREATE_BRIDGE3(addRenderNode, CanvasContext* context, RenderNode* node, bool placeFront) {
    args->context->addRenderNode(args->node, args->placeFront);
    return nullptr;
}

void RenderProxy::addRenderNode(RenderNode* node, bool placeFront) {
    SETUP_TASK(addRenderNode);
    args->context = mContext;
    args->node = node;
    args->placeFront = placeFront;
    post(task);
}

CREATE_BRIDGE2(removeRenderNode, CanvasContext* context, RenderNode* node) {
    args->context->removeRenderNode(args->node);
    return nullptr;
}

void RenderProxy::removeRenderNode(RenderNode* node) {
    SETUP_TASK(removeRenderNode);
    args->context = mContext;
    args->node = node;
    post(task);
}

CREATE_BRIDGE2(drawRenderNode, CanvasContext* context, RenderNode* node) {
    args->context->prepareAndDraw(args->node);
    return nullptr;
}

void RenderProxy::drawRenderNode(RenderNode* node) {
    SETUP_TASK(drawRenderNode);
    args->context = mContext;
    args->node = node;
    // Be pseudo-thread-safe and don't use any member variables
    staticPostAndWait(task);
}

CREATE_BRIDGE5(setContentDrawBounds, CanvasContext* context, int left, int top,
        int right, int bottom) {
    args->context->setContentDrawBounds(args->left, args->top, args->right, args->bottom);
    return nullptr;
}

void RenderProxy::setContentDrawBounds(int left, int top, int right, int bottom) {
    SETUP_TASK(setContentDrawBounds);
    args->context = mContext;
    args->left = left;
    args->top = top;
    args->right = right;
    args->bottom = bottom;
    staticPostAndWait(task);
}

CREATE_BRIDGE1(serializeDisplayListTree, CanvasContext* context) {
    args->context->serializeDisplayListTree();
    return nullptr;
}

void RenderProxy::serializeDisplayListTree() {
    SETUP_TASK(serializeDisplayListTree);
    args->context = mContext;
    post(task);
}

CREATE_BRIDGE2(addFrameMetricsObserver, CanvasContext* context,
        FrameMetricsObserver* frameStatsObserver) {
   args->context->addFrameMetricsObserver(args->frameStatsObserver);
   if (args->frameStatsObserver != nullptr) {
       args->frameStatsObserver->decStrong(args->context);
   }
   return nullptr;
}

void RenderProxy::addFrameMetricsObserver(FrameMetricsObserver* observer) {
    SETUP_TASK(addFrameMetricsObserver);
    args->context = mContext;
    args->frameStatsObserver = observer;
    if (observer != nullptr) {
        observer->incStrong(mContext);
    }
    post(task);
}

CREATE_BRIDGE2(removeFrameMetricsObserver, CanvasContext* context,
        FrameMetricsObserver* frameStatsObserver) {
   args->context->removeFrameMetricsObserver(args->frameStatsObserver);
   if (args->frameStatsObserver != nullptr) {
       args->frameStatsObserver->decStrong(args->context);
   }
   return nullptr;
}

void RenderProxy::removeFrameMetricsObserver(FrameMetricsObserver* observer) {
    SETUP_TASK(removeFrameMetricsObserver);
    args->context = mContext;
    args->frameStatsObserver = observer;
    if (observer != nullptr) {
        observer->incStrong(mContext);
    }
    post(task);
}

CREATE_BRIDGE4(copySurfaceInto, RenderThread* thread,
        Surface* surface, Rect srcRect, SkBitmap* bitmap) {
    return (void*)args->thread->readback().copySurfaceInto(*args->surface,
            args->srcRect, args->bitmap);
}

int RenderProxy::copySurfaceInto(sp<Surface>& surface, int left, int top,
        int right, int bottom,  SkBitmap* bitmap) {
    SETUP_TASK(copySurfaceInto);
    args->bitmap = bitmap;
    args->surface = surface.get();
    args->thread = &RenderThread::getInstance();
    args->srcRect.set(left, top, right, bottom);
    return static_cast<int>(
            reinterpret_cast<intptr_t>( staticPostAndWait(task) ));
}

CREATE_BRIDGE2(prepareToDraw, RenderThread* thread, Bitmap* bitmap) {
    CanvasContext::prepareToDraw(*args->thread, args->bitmap);
    args->bitmap->unref();
    args->bitmap = nullptr;
    return nullptr;
}

void RenderProxy::prepareToDraw(Bitmap& bitmap) {
    // If we haven't spun up a hardware accelerated window yet, there's no
    // point in precaching these bitmaps as it can't impact jank.
    // We also don't know if we even will spin up a hardware-accelerated
    // window or not.
    if (!RenderThread::hasInstance()) return;
    RenderThread* renderThread = &RenderThread::getInstance();
    SETUP_TASK(prepareToDraw);
    args->thread = renderThread;
    bitmap.ref();
    args->bitmap = &bitmap;
    nsecs_t lastVsync = renderThread->timeLord().latestVsync();
    nsecs_t estimatedNextVsync = lastVsync + renderThread->timeLord().frameIntervalNanos();
    nsecs_t timeToNextVsync = estimatedNextVsync - systemTime(CLOCK_MONOTONIC);
    // We expect the UI thread to take 4ms and for RT to be active from VSYNC+4ms to
    // VSYNC+12ms or so, so aim for the gap during which RT is expected to
    // be idle
    // TODO: Make this concept a first-class supported thing? RT could use
    // knowledge of pending draws to better schedule this task
    if (timeToNextVsync > -6_ms && timeToNextVsync < 1_ms) {
        renderThread->queueAt(task, estimatedNextVsync + 8_ms);
    } else {
        renderThread->queue(task);
    }
}

CREATE_BRIDGE2(allocateHardwareBitmap, RenderThread* thread, SkBitmap* bitmap) {
    sk_sp<Bitmap> hardwareBitmap = Bitmap::allocateHardwareBitmap(*args->thread, *args->bitmap);
    return hardwareBitmap.release();
}

sk_sp<Bitmap> RenderProxy::allocateHardwareBitmap(SkBitmap& bitmap) {
    SETUP_TASK(allocateHardwareBitmap);
    args->bitmap = &bitmap;
    args->thread = &RenderThread::getInstance();
    sk_sp<Bitmap> hardwareBitmap(reinterpret_cast<Bitmap*>(staticPostAndWait(task)));
    return hardwareBitmap;
}

CREATE_BRIDGE3(copyGraphicBufferInto, RenderThread* thread, GraphicBuffer* buffer, SkBitmap* bitmap) {
    return (void*) args->thread->readback().copyGraphicBufferInto(args->buffer, args->bitmap);
}

int RenderProxy::copyGraphicBufferInto(GraphicBuffer* buffer, SkBitmap* bitmap) {
    RenderThread& thread = RenderThread::getInstance();
    if (Properties::isSkiaEnabled() && gettid() == thread.getTid()) {
        //TODO: fix everything that hits this. We should never be triggering a readback ourselves.
        return (int) thread.readback().copyGraphicBufferInto(buffer, bitmap);
    } else {
        SETUP_TASK(copyGraphicBufferInto);
        args->thread = &thread;
        args->bitmap = bitmap;
        args->buffer = buffer;
        return static_cast<int>(reinterpret_cast<intptr_t>(staticPostAndWait(task)));
    }
}

CREATE_BRIDGE2(onBitmapDestroyed, RenderThread* thread, uint32_t pixelRefId) {
    args->thread->renderState().onBitmapDestroyed(args->pixelRefId);
    return nullptr;
}

void RenderProxy::onBitmapDestroyed(uint32_t pixelRefId) {
    if (!RenderThread::hasInstance()) return;
    SETUP_TASK(onBitmapDestroyed);
    RenderThread& thread = RenderThread::getInstance();
    args->thread = &thread;
    args->pixelRefId = pixelRefId;
    thread.queue(task);
}

void RenderProxy::disableVsync() {
    Properties::disableVsync = true;
}

void RenderProxy::post(RenderTask* task) {
    mRenderThread.queue(task);
}

void* RenderProxy::postAndWait(MethodInvokeRenderTask* task) {
    void* retval;
    task->setReturnPtr(&retval);
    SignalingRenderTask syncTask(task, &mSyncMutex, &mSyncCondition);
    AutoMutex _lock(mSyncMutex);
    mRenderThread.queue(&syncTask);
    while (!syncTask.hasRun()) {
        mSyncCondition.wait(mSyncMutex);
    }
    return retval;
}

void* RenderProxy::staticPostAndWait(MethodInvokeRenderTask* task) {
    RenderThread& thread = RenderThread::getInstance();
    LOG_ALWAYS_FATAL_IF(gettid() == thread.getTid());
    void* retval;
    task->setReturnPtr(&retval);
    thread.queueAndWait(task);
    return retval;
}

} /* namespace renderthread */
} /* namespace uirenderer */
} /* namespace android */
