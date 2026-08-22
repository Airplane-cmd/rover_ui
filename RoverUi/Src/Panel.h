/*
 * Panel.h — rover_ui panel abstraction.
 * MIT license. Copyright (c) 2026 Airplane-cmd.
 *
 * A Panel is a rectangular head-locked / world-anchored / yaw-locked quad
 * with its own swapchain. Content (color, WebView texture, VirtualDisplay,
 * etc) is written to the swapchain per-frame or once at init.
 */
#pragma once

#if defined(ANDROID)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define XR_USE_PLATFORM_ANDROID 1
#endif
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>
#include <cstdint>

namespace rover {

enum class DofMode {
    HeadLocked,       // in HeadSpace, always follows head fully. 0DoF (rover)
    WorldAnchored,    // in LocalSpace, stays where placed. 6DoF.
    YawLocked,        // in LocalSpace, translates with head + rotates with head yaw only (no pitch/roll)
    BodyLocked,       // in LocalSpace, translates with head, orientation fixed to world. This is what rover calls 3DoF.
};

struct Panel {
    XrPosef pose;              // for HeadLocked: relative to head. For WorldAnchored: world coord. For YawLocked: local-offset from head yaw origin.
    XrExtent2Df size;          // meters (width, height)
    DofMode dofMode;
    XrSwapchain swapchain;
    int32_t width;
    int32_t height;
    float clearColor[4];       // RGBA, used by fill() when content is a solid color
};

class PanelManager {
public:
    void Init(XrSession session, XrSpace headSpace, XrSpace localSpace);
    void Shutdown();

    // Create a panel and add it. Returns index.
    int AddPanel(DofMode mode, const XrPosef& pose, const XrExtent2Df& size,
                 float r, float g, float b, float a,
                 int32_t width = 256, int32_t height = 256);

    // Fill a panel's swapchain image with its clearColor (one-time).
    void FillSolidColor(int panelIdx);

    // Build XrCompositionLayerQuad structs for all panels, appended into layersOut.
    // headPoseInLocal is used to derive YawLocked poses.
    void BuildLayers(XrCompositionLayerQuad* outQuads, int outCap, int* outCount,
                     const XrPosef& headPoseInLocal);

    const std::vector<Panel>& Panels() const { return panels_; }

private:
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace headSpace_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    std::vector<Panel> panels_;
};

} // namespace rover
