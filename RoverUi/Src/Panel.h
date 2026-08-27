/*
 * Panel.h — rover_ui panel abstraction.
 * MIT license. Copyright (c) 2026 Airplane-cmd.
 *
 * A Panel is a rectangular quad with its own swapchain, plus a small display bar
 * rendered below it that acts as the grab handle (Meta-style UX).
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
    XrPosef pose;              // for HeadLocked: relative to head. For WorldAnchored: world coord. For BodyLocked/YawLocked: offset from head.
    XrExtent2Df size;          // meters (width, height)
    DofMode dofMode;
    XrSwapchain swapchain;
    int32_t width;
    int32_t height;
    float clearColor[4];       // RGBA, used by FillSolidColor
    XrSwapchain barSwapchain;  // display bar (grab handle) — separate small swapchain
    float barColor[4];         // bar color (idle)
    float barHoverColor[4];    // bar color (hovered)
    bool barHovered;           // per-frame flag, set by input logic
    bool dynamic = false;      // if true, PanelManager::UpdateDynamic re-fills swapchain each frame
    bool oesSourced = false;   // if true, sourced from a GL_TEXTURE_EXTERNAL_OES via OesBlitter
};

// Ray-cast hit result
struct HitResult {
    int panelIdx;              // -1 if none
    bool hitBar;               // true if hit was on display bar
    int cornerIdx;             // 0=BL, 1=BR, 2=TL, 3=TR; -1 if not corner
    float distance;            // meters along ray
};

class PanelManager {
public:
    void Init(XrSession session, XrSpace headSpace, XrSpace localSpace);
    void Shutdown();

    int AddPanel(DofMode mode, const XrPosef& pose, const XrExtent2Df& size,
                 float r, float g, float b, float a,
                 int32_t width = 256, int32_t height = 256);

    void FillSolidColor(int panelIdx);
    void FillBar(int panelIdx, bool hovered);   // fills bar swapchain with idle or hover color
    void UpdateDynamic(float time);

    // Compute the panel's current world pose (in localSpace) given current head pose.
    XrPosef ResolveWorldPose(int panelIdx, const XrPosef& headPoseInLocal) const;

    // Ray-cast against all panel bars and bodies.
    // rayOrigin/rayDir in localSpace. Returns nearest hit, or {-1, false, 0} if miss.
    HitResult Raycast(const XrVector3f& rayOrigin, const XrVector3f& rayDir,
                      const XrPosef& headPoseInLocal) const;

    // Reset barHovered on all panels, then set on the given panel (or none if -1).
    void SetHovered(int panelIdx);

    // Convert a world pose into the panel's own DoF reference (called on grab-release
    // to store the new pose in the appropriate space).
    void CommitWorldPose(int panelIdx, const XrPosef& worldPose, const XrPosef& headPoseInLocal);

    // Build XrCompositionLayerQuad structs for all panels + bars, appended into outQuads.
    // Panel body layers first, then bar layers (bars render on top).
    void BuildLayers(XrCompositionLayerQuad* outQuads, int outCap, int* outCount,
                     const XrPosef& headPoseInLocal);

    const std::vector<Panel>& Panels() const { return panels_; }
    Panel& PanelAt(int idx) { return panels_[idx]; }

    // Destroy + recreate a panel's swapchain at new pixel dimensions.
    // Returns true on success. Updates p.width/p.height.
    bool ResizePanelSwapchain(int panelIdx, int32_t newW, int32_t newH);

private:
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace headSpace_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    std::vector<Panel> panels_;
    XrSwapchain handleSc_ = XR_NULL_HANDLE;  // shared corner-handle texture
};



// Small aim-point cursor: a floating quad at the ray-hit point, billboard-oriented to the head.
class RayCursor {
public:
    void Init(XrSession session);
    void Shutdown();
    // Fill the cursor with idle (cyan) or active (red) color
    void SetActive(bool active);
    // Build a quad layer at worldPoint facing the head. Returns true if enabled.
    bool BuildLayer(XrCompositionLayerQuad* out, XrSpace localSpace,
                    const XrVector3f& worldPoint, const XrPosef& headPoseInLocal);
    void SetVisible(bool v) { visible_ = v; }

private:
    XrSwapchain sc_ = XR_NULL_HANDLE;
    bool visible_ = false;
    bool active_ = false;
};



// Ray-line: a thin quad rendered from controller origin along the ray direction, up to hit distance
// (or a fixed far distance if no hit). Billboard-oriented so it's visible from the head.
class RayLine {
public:
    void Init(XrSession session);
    void Shutdown();
    void SetActive(bool active);  // idle color vs pressed color
    // Build a quad layer representing the ray. Returns true if enabled.
    bool BuildLayer(XrCompositionLayerQuad* out, XrSpace localSpace,
                    const XrVector3f& origin, const XrVector3f& dir, float length,
                    const XrPosef& headPoseInLocal);
    void SetVisible(bool v) { visible_ = v; }

private:
    XrSwapchain sc_ = XR_NULL_HANDLE;
    bool visible_ = false;
    bool active_ = false;
};



// Blits a GL_TEXTURE_EXTERNAL_OES texture into a panel's swapchain image via a shader + FBO.
class OesBlitter {
public:
    bool Init();
    void Shutdown();
    // Acquire panel swapchain, sample OES texture into it, release.
    // Returns false if setup failed (no-op).
    bool BlitToPanel(Panel& p, unsigned int oesTexId, const float* stMatrix4x4 /*column-major, may be null*/);
private:
    unsigned int program_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    int uTexLoc_ = -1;
    int uSTMatrixLoc_ = -1;
    bool ready_ = false;
};

} // namespace rover
