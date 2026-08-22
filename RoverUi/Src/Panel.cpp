/*
 * Panel.cpp — rover_ui panel implementation.
 * MIT license. Copyright (c) 2026 Airplane-cmd.
 */

#include "Panel.h"
#include <cmath>
#include <cstring>

// Reuse OXR() macro / OXR_CheckErrors from RoverUi.h
extern void OXR_CheckErrors(XrResult result, const char* function, bool failOnError);
#define OXR(func) OXR_CheckErrors(func, #func, true)

namespace rover {

void PanelManager::Init(XrSession session, XrSpace headSpace, XrSpace localSpace) {
    session_ = session;
    headSpace_ = headSpace;
    localSpace_ = localSpace;
}

void PanelManager::Shutdown() {
    for (auto& p : panels_) {
        if (p.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(p.swapchain);
            p.swapchain = XR_NULL_HANDLE;
        }
    }
    panels_.clear();
}

int PanelManager::AddPanel(DofMode mode, const XrPosef& pose, const XrExtent2Df& size,
                            float r, float g, float b, float a,
                            int32_t width, int32_t height) {
    Panel p{};
    p.pose = pose;
    p.size = size;
    p.dofMode = mode;
    p.width = width;
    p.height = height;
    p.clearColor[0] = r; p.clearColor[1] = g; p.clearColor[2] = b; p.clearColor[3] = a;

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = GL_SRGB8_ALPHA8;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(width);
    ci.height = static_cast<uint32_t>(height);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    OXR(xrCreateSwapchain(session_, &ci, &p.swapchain));

    panels_.push_back(p);
    int idx = static_cast<int>(panels_.size()) - 1;
    FillSolidColor(idx);
    return idx;
}

void PanelManager::FillSolidColor(int panelIdx) {
    if (panelIdx < 0 || panelIdx >= static_cast<int>(panels_.size())) return;
    Panel& p = panels_[panelIdx];

    uint32_t len = 0;
    OXR(xrEnumerateSwapchainImages(p.swapchain, 0, &len, nullptr));
    auto* imgs = new XrSwapchainImageOpenGLESKHR[len];
    for (uint32_t i = 0; i < len; i++) imgs[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    OXR(xrEnumerateSwapchainImages(p.swapchain, len, &len,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs)));

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    OXR(xrAcquireSwapchainImage(p.swapchain, &ai, &idx));
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    OXR(xrWaitSwapchainImage(p.swapchain, &wi));

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            static_cast<GLuint>(imgs[idx].image), 0);
    glClearColor(p.clearColor[0], p.clearColor[1], p.clearColor[2], p.clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    OXR(xrReleaseSwapchainImage(p.swapchain, &ri));
    delete[] imgs;
}

// Extract yaw (rotation around +Y) from a unit quaternion.
static float ExtractYaw(const XrQuaternionf& q) {
    // yaw = atan2(2*(w*y + z*x), 1 - 2*(y*y + x*x))
    return std::atan2(2.0f * (q.w * q.y + q.z * q.x),
                       1.0f - 2.0f * (q.y * q.y + q.x * q.x));
}

void PanelManager::BuildLayers(XrCompositionLayerQuad* outQuads, int outCap, int* outCount,
                                const XrPosef& headPoseInLocal) {
    int count = 0;
    for (const Panel& p : panels_) {
        if (count >= outCap) break;
        XrCompositionLayerQuad& q = outQuads[count];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = p.swapchain;
        q.subImage.imageRect.offset = {0, 0};
        q.subImage.imageRect.extent = {p.width, p.height};
        q.subImage.imageArrayIndex = 0;
        q.size = p.size;

        switch (p.dofMode) {
            case DofMode::HeadLocked: {
                // In HeadSpace; pose is head-relative.
                q.space = headSpace_;
                q.pose = p.pose;
                break;
            }
            case DofMode::WorldAnchored: {
                // In LocalSpace; pose is world-relative, unchanged.
                q.space = localSpace_;
                q.pose = p.pose;
                break;
            }
            case DofMode::BodyLocked: {
                // In LocalSpace; panel translates with head position + offset, orientation fixed to identity.
                // Turn head -> panel stays at same world-facing direction. Walk -> panel comes with you.
                q.space = localSpace_;
                q.pose.position.x = headPoseInLocal.position.x + p.pose.position.x;
                q.pose.position.y = headPoseInLocal.position.y + p.pose.position.y;
                q.pose.position.z = headPoseInLocal.position.z + p.pose.position.z;
                q.pose.orientation = p.pose.orientation;  // fixed to whatever was baked into the panel (typically identity)
                break;
            }
            case DofMode::YawLocked: {
                // In LocalSpace, but pose derived from head yaw + head position + p.pose offset.
                // Only yaw of head applies; pitch/roll ignored. Position from head, plus p.pose offset rotated by yaw.
                q.space = localSpace_;
                float yaw = ExtractYaw(headPoseInLocal.orientation);
                float cy = std::cos(yaw * 0.5f);
                float sy = std::sin(yaw * 0.5f);
                XrQuaternionf yawQ = {0.0f, sy, 0.0f, cy};

                // Rotate the panel's local offset (p.pose.position) by yaw, add head position
                float ox = p.pose.position.x;
                float oz = p.pose.position.z;
                float cosY = std::cos(yaw);
                float sinY = std::sin(yaw);
                float rx = ox * cosY + oz * sinY;
                float rz = -ox * sinY + oz * cosY;

                q.pose.position.x = headPoseInLocal.position.x + rx;
                q.pose.position.y = headPoseInLocal.position.y + p.pose.position.y;
                q.pose.position.z = headPoseInLocal.position.z + rz;

                // Panel faces the user: orientation = yaw only, plus any pre-baked orientation of p.pose (usually identity)
                q.pose.orientation = yawQ;
                break;
            }
        }
        count++;
    }
    *outCount = count;
}

} // namespace rover
