/*
 * Panel.cpp — rover_ui panel implementation.
 * MIT license. Copyright (c) 2026 Airplane-cmd.
 */

#include "Panel.h"
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <algorithm>

extern void OXR_CheckErrors(XrResult result, const char* function, bool failOnError);
#define OXR(func) OXR_CheckErrors(func, #func, true)

namespace rover {

static XrQuaternionf QNorm(const XrQuaternionf& q) {
    float L2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!(L2 > 1e-12f) || L2 != L2) return {0, 0, 0, 1};  // NaN or zero
    float inv = 1.0f / std::sqrt(L2);
    return { q.x*inv, q.y*inv, q.z*inv, q.w*inv };
}

static bool VecFinite(const XrVector3f& v) {
    return v.x == v.x && v.y == v.y && v.z == v.z
        && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
// forward decls (defined later in this file)
static XrSwapchain CreateColorSwapchain(XrSession session, int32_t w, int32_t h);
static void FillSwapchainSolid(XrSwapchain sc, float r, float g, float b, float a);




// Bar geometry: below the panel, small and thin
static constexpr float BAR_HEIGHT_M = 0.012f;   // 3cm tall
static constexpr float BAR_WIDTH_RATIO = 0.25f; // 40% of panel width
static constexpr float BAR_GAP_M = 0.015f;      // 2cm gap between panel bottom and bar
static constexpr float BAR_HOVER_HEIGHT_M = 0.030f; // v0.8-1b: expanded bar height on hover
static constexpr float BAR_HOVER_WIDTH_RATIO = 1.0f;  // v0.8-1b: full panel width when hovered
static constexpr int32_t BAR_HOVER_TEX_W = 1024;
static constexpr int32_t BAR_HOVER_TEX_H = 64;
static constexpr int32_t BAR_TEX_W = 64;
static constexpr int32_t BAR_TEX_H = 16;
static constexpr float HANDLE_SIZE_M = 0.025f;   // 2.5cm corner handle
static constexpr int32_t HANDLE_TEX = 16;

void PanelManager::Init(XrSession session, XrSpace headSpace, XrSpace localSpace) {
    session_ = session;
    headSpace_ = headSpace;
    localSpace_ = localSpace;
    handleSc_ = CreateColorSwapchain(session, HANDLE_TEX, HANDLE_TEX);
    FillSwapchainSolid(handleSc_, 0.85f, 0.85f, 0.90f, 0.95f);
}

void PanelManager::Shutdown() {
    for (auto& p : panels_) {
        if (p.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(p.swapchain);
        if (p.barSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(p.barSwapchain);
    }
    if (handleSc_ != XR_NULL_HANDLE) { xrDestroySwapchain(handleSc_); handleSc_ = XR_NULL_HANDLE; }
    panels_.clear();
}

static XrSwapchain CreateColorSwapchain(XrSession session, int32_t w, int32_t h) {
    XrSwapchain sc = XR_NULL_HANDLE;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = GL_SRGB8_ALPHA8;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(w);
    ci.height = static_cast<uint32_t>(h);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    OXR(xrCreateSwapchain(session, &ci, &sc));
    return sc;
}

static void FillSwapchainSolid(XrSwapchain sc, float r, float g, float b, float a) {
    uint32_t len = 0;
    OXR(xrEnumerateSwapchainImages(sc, 0, &len, nullptr));
    auto* imgs = new XrSwapchainImageOpenGLESKHR[len];
    for (uint32_t i = 0; i < len; i++) imgs[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    OXR(xrEnumerateSwapchainImages(sc, len, &len,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs)));

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    OXR(xrAcquireSwapchainImage(sc, &ai, &idx));
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    OXR(xrWaitSwapchainImage(sc, &wi));

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            static_cast<GLuint>(imgs[idx].image), 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    OXR(xrReleaseSwapchainImage(sc, &ri));
    delete[] imgs;
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
    // Bar colors: dim gray idle, brighter on hover
    p.barColor[0] = 0.5f; p.barColor[1] = 0.5f; p.barColor[2] = 0.55f; p.barColor[3] = 0.85f;
    p.barHoverColor[0] = 0.85f; p.barHoverColor[1] = 0.85f; p.barHoverColor[2] = 1.0f; p.barHoverColor[3] = 0.95f;
    p.barHovered = false;

    p.swapchain = CreateColorSwapchain(session_, width, height);
    p.barSwapchain = CreateColorSwapchain(session_, BAR_HOVER_TEX_W, BAR_HOVER_TEX_H);  // v0.8-1b: larger so hover UI fits

    panels_.push_back(p);
    int idx = static_cast<int>(panels_.size()) - 1;
    FillSolidColor(idx);
    FillBar(idx, false);
    return idx;
}

void PanelManager::FillSolidColor(int panelIdx) {
    if (panelIdx < 0 || panelIdx >= static_cast<int>(panels_.size())) return;
    const Panel& p = panels_[panelIdx];
    FillSwapchainSolid(p.swapchain, p.clearColor[0], p.clearColor[1], p.clearColor[2], p.clearColor[3]);
}

void PanelManager::FillBar(int panelIdx, bool hovered) {
    if (panelIdx < 0 || panelIdx >= static_cast<int>(panels_.size())) return;
    const Panel& p = panels_[panelIdx];
    const float* c = hovered ? p.barHoverColor : p.barColor;
    FillSwapchainSolid(p.barSwapchain, c[0], c[1], c[2], c[3]);
}


bool PanelManager::ResizePanelSwapchain(int panelIdx, int32_t newW, int32_t newH) {
    if (panelIdx < 0 || panelIdx >= (int)panels_.size()) return false;
    Panel& p = panels_[panelIdx];
    if (p.width == newW && p.height == newH && p.swapchain != XR_NULL_HANDLE) return true;  // v0.8-fix: no-op
    if (p.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(p.swapchain);
        p.swapchain = XR_NULL_HANDLE;
    }
    p.swapchain = CreateColorSwapchain(session_, newW, newH);
    if (p.swapchain == XR_NULL_HANDLE) return false;
    p.width = newW;
    p.height = newH;
    // For oesSourced panels, don't fill solid color — OesBlitter will populate next frame.
    if (!p.oesSourced) FillSolidColor(panelIdx);
    return true;
}

// v0.8.4 #14: free per-panel resources; slot kept (indices stable) with dead=true
void PanelManager::DestroyPanelResources(int idx) {
    if (idx < 0 || idx >= (int)panels_.size()) return;
    Panel& p = panels_[idx];
    if (p.dead) return;
    if (p.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(p.swapchain);
        p.swapchain = XR_NULL_HANDLE;
    }
    if (p.barSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(p.barSwapchain);
        p.barSwapchain = XR_NULL_HANDLE;
    }
    p.dead = true;
    p.visible = false;
    __android_log_print(ANDROID_LOG_ERROR, "PanelManager", "DestroyPanelResources idx=%d done", idx);
}

static float ExtractYaw(const XrQuaternionf& q) {
    return std::atan2(2.0f * (q.w * q.y + q.z * q.x),
                       1.0f - 2.0f * (q.y * q.y + q.x * q.x));
}

// Compose two poses: out = a * b (like matrix mult). Simple version — b applied first, then a.
static XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {
    XrPosef r;
    // rotate b.position by a.orientation
    const XrQuaternionf& q = a.orientation;
    XrVector3f v = b.position;
    // v' = q * v * q_conj  (quaternion-rotate)
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    XrVector3f rot;
    rot.x = v.x * (1 - 2*yy - 2*zz) + v.y * (2*xy - 2*wz)   + v.z * (2*xz + 2*wy);
    rot.y = v.x * (2*xy + 2*wz)     + v.y * (1 - 2*xx - 2*zz) + v.z * (2*yz - 2*wx);
    rot.z = v.x * (2*xz - 2*wy)     + v.y * (2*yz + 2*wx)   + v.z * (1 - 2*xx - 2*yy);
    r.position.x = a.position.x + rot.x;
    r.position.y = a.position.y + rot.y;
    r.position.z = a.position.z + rot.z;
    // orientation = a * b
    r.orientation.w = a.orientation.w * b.orientation.w - a.orientation.x * b.orientation.x
                    - a.orientation.y * b.orientation.y - a.orientation.z * b.orientation.z;
    r.orientation.x = a.orientation.w * b.orientation.x + a.orientation.x * b.orientation.w
                    + a.orientation.y * b.orientation.z - a.orientation.z * b.orientation.y;
    r.orientation.y = a.orientation.w * b.orientation.y - a.orientation.x * b.orientation.z
                    + a.orientation.y * b.orientation.w + a.orientation.z * b.orientation.x;
    r.orientation.z = a.orientation.w * b.orientation.z + a.orientation.x * b.orientation.y
                    - a.orientation.y * b.orientation.x + a.orientation.z * b.orientation.w;
    return r;
}

static XrPosef PoseInvert(const XrPosef& p) {
    XrPosef r;
    // inverse quaternion (conjugate for unit quats)
    r.orientation.w = p.orientation.w;
    r.orientation.x = -p.orientation.x;
    r.orientation.y = -p.orientation.y;
    r.orientation.z = -p.orientation.z;
    // rotate -p.position by inverse orientation
    XrPosef negPos = {r.orientation, {-p.position.x, -p.position.y, -p.position.z}};
    XrPosef zeroP = {r.orientation, {0, 0, 0}};
    XrPosef tmp = PoseMul(zeroP, {{0,0,0,1}, {-p.position.x, -p.position.y, -p.position.z}});
    r.position = tmp.position;
    return r;
}

XrPosef PanelManager::ResolveWorldPose(int panelIdx, const XrPosef& headPoseInLocal) const {
    const Panel& p = panels_[panelIdx];
    switch (p.dofMode) {
        case DofMode::HeadLocked: {
            // p.pose is in head space; world = head * p.pose
            return PoseMul(headPoseInLocal, p.pose);
        }
        case DofMode::WorldAnchored: {
            return p.pose;
        }
        case DofMode::BodyLocked: {
            XrPosef r;
            r.position.x = headPoseInLocal.position.x + p.pose.position.x;
            r.position.y = headPoseInLocal.position.y + p.pose.position.y;
            r.position.z = headPoseInLocal.position.z + p.pose.position.z;
            r.orientation = p.pose.orientation;
            return r;
        }
        case DofMode::YawLocked: {
            float yaw = ExtractYaw(headPoseInLocal.orientation);
            float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
            XrQuaternionf yawQ = {0.0f, sy, 0.0f, cy};
            float ox = p.pose.position.x, oz = p.pose.position.z;
            float cosY = std::cos(yaw), sinY = std::sin(yaw);
            XrPosef r;
            r.position.x = headPoseInLocal.position.x + ox * cosY + oz * sinY;
            r.position.y = headPoseInLocal.position.y + p.pose.position.y;
            r.position.z = headPoseInLocal.position.z + (-ox * sinY + oz * cosY);
            r.orientation = yawQ;
            return r;
        }
    }
    return p.pose;
}

void PanelManager::CommitWorldPose(int panelIdx, const XrPosef& worldPose, const XrPosef& headPoseInLocal) {
    Panel& p = panels_[panelIdx];
    switch (p.dofMode) {
        case DofMode::HeadLocked: {
            // store as head-relative: head^-1 * world
            XrPosef headInv = PoseInvert(headPoseInLocal);
            p.pose = PoseMul(headInv, worldPose);
            break;
        }
        case DofMode::WorldAnchored: {
            p.pose = worldPose;
            break;
        }
        case DofMode::BodyLocked: {
            p.pose.position.x = worldPose.position.x - headPoseInLocal.position.x;
            p.pose.position.y = worldPose.position.y - headPoseInLocal.position.y;
            p.pose.position.z = worldPose.position.z - headPoseInLocal.position.z;
            p.pose.orientation = worldPose.orientation;
            break;
        }
        case DofMode::YawLocked: {
            float yaw = ExtractYaw(headPoseInLocal.orientation);
            float cosY = std::cos(-yaw), sinY = std::sin(-yaw); // un-rotate by head yaw
            float dx = worldPose.position.x - headPoseInLocal.position.x;
            float dz = worldPose.position.z - headPoseInLocal.position.z;
            p.pose.position.x = dx * cosY + dz * sinY;
            p.pose.position.y = worldPose.position.y - headPoseInLocal.position.y;
            p.pose.position.z = -dx * sinY + dz * cosY;
            p.pose.orientation = {0, 0, 0, 1}; // rebased
            break;
        }
    }
}

// Ray-quad intersection. Quad centered at pose, size (w,h), normal = pose.orientation * (0,0,1)
// (i.e., quad's front face points along +Z of its local frame — actually for quads the FRONT is -Z per OpenXR convention)
// Returns t (distance along ray) if hit, negative if miss. Also returns local (u,v) offset from center.
static bool RayQuadHit(const XrVector3f& ro, const XrVector3f& rd,
                       const XrPosef& quadPose, float w, float h,
                       float* outT) {
    // Quad normal in world: rotate (0,0,-1) by quadPose.orientation. Actually XR quads face -Z of their pose.
    // But for hit-test, either face works since we care about intersection.
    const XrQuaternionf& q = quadPose.orientation;
    XrVector3f nz = {0, 0, -1};
    // rotate nz by q
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    XrVector3f n;
    n.x = nz.x * (1 - 2*yy - 2*zz) + nz.y * (2*xy - 2*wz)   + nz.z * (2*xz + 2*wy);
    n.y = nz.x * (2*xy + 2*wz)     + nz.y * (1 - 2*xx - 2*zz) + nz.z * (2*yz - 2*wx);
    n.z = nz.x * (2*xz - 2*wy)     + nz.y * (2*yz + 2*wx)   + nz.z * (1 - 2*xx - 2*yy);

    float denom = n.x * rd.x + n.y * rd.y + n.z * rd.z;
    if (std::fabs(denom) < 1e-6f) return false;
    XrVector3f pc = {quadPose.position.x - ro.x, quadPose.position.y - ro.y, quadPose.position.z - ro.z};
    float t = (pc.x * n.x + pc.y * n.y + pc.z * n.z) / denom;
    if (t <= 0.01f) return false;

    // hit point
    XrVector3f hp = {ro.x + t * rd.x, ro.y + t * rd.y, ro.z + t * rd.z};

    // Local coords: rotate (hp - center) by quad's inverse orientation, project onto XY of quad frame
    XrVector3f d = {hp.x - quadPose.position.x, hp.y - quadPose.position.y, hp.z - quadPose.position.z};
    // Inverse rotation = conjugate
    XrQuaternionf qi = {-q.x, -q.y, -q.z, q.w};
    float ixx = qi.x * qi.x, iyy = qi.y * qi.y, izz = qi.z * qi.z;
    float iwx = qi.w * qi.x, iwy = qi.w * qi.y, iwz = qi.w * qi.z;
    float ixy = qi.x * qi.y, ixz = qi.x * qi.z, iyz = qi.y * qi.z;
    float lx = d.x * (1 - 2*iyy - 2*izz) + d.y * (2*ixy - 2*iwz)   + d.z * (2*ixz + 2*iwy);
    float ly = d.x * (2*ixy + 2*iwz)     + d.y * (1 - 2*ixx - 2*izz) + d.z * (2*iyz - 2*iwx);
    // check bounds
    if (std::fabs(lx) > w * 0.5f || std::fabs(ly) > h * 0.5f) return false;
    *outT = t;
    return true;
}

HitResult PanelManager::Raycast(const XrVector3f& rayOrigin, const XrVector3f& rayDir,
                                 const XrPosef& headPoseInLocal) const {
    HitResult best = {-1, false, -1, 1e9f};
    for (int i = 0; i < static_cast<int>(panels_.size()); i++) {
        const Panel& p = panels_[i];
        if (p.dead) continue;
        if (!p.visible) continue;
        XrPosef world = ResolveWorldPose(i, headPoseInLocal);

        // bar center: below panel — use hover height for hit-test so pointing at expanded region counts
        float barCenterOffsetY = -(p.size.height * 0.5f + BAR_GAP_M + BAR_HOVER_HEIGHT_M * 0.5f);
        XrPosef barLocalOffset = {{0,0,0,1}, {0, barCenterOffsetY, 0}};
        XrPosef barWorld = PoseMul(world, barLocalOffset);
        float barW = p.size.width * BAR_WIDTH_RATIO;

        float t = 0;
        // bar first — v0.8-1b: always use hover-expanded bounds so pointing near activates it
        float barHitH = BAR_HOVER_HEIGHT_M;
        float barHitW = p.size.width * BAR_HOVER_WIDTH_RATIO;
        if (RayQuadHit(rayOrigin, rayDir, barWorld, barHitW, barHitH, &t)) {
            if (t < best.distance) { best = {i, true, -1, t}; }
        }
        // Also test body but only if no closer bar hit
        if (RayQuadHit(rayOrigin, rayDir, world, p.size.width, p.size.height, &t)) {
            if (t < best.distance && (best.panelIdx == -1 || !best.hitBar)) {
                best = {i, false, -1, t};
            }
        }

        // Corner handles (4). Same size, positioned at panel corners in local frame.
        const float hw = p.size.width  * 0.5f;
        const float hh = p.size.height * 0.5f;
        const XrVector3f cornerOffs[4] = {
            {-hw, -hh, 0}, { hw, -hh, 0}, {-hw,  hh, 0}, { hw,  hh, 0},
        };
        for (int c = 0; c < 4; c++) {
            XrPosef cornerLocal = {{0,0,0,1}, cornerOffs[c]};
            XrPosef cornerWorld = PoseMul(world, cornerLocal);
            if (RayQuadHit(rayOrigin, rayDir, cornerWorld, HANDLE_SIZE_M, HANDLE_SIZE_M, &t)) {
                // Corners take priority over body but not over bar
                if (t < best.distance) {
                    best = {i, false, c, t};
                }
            }
        }
    }
    if (best.panelIdx == -1) best.distance = 0;
    return best;
}

void PanelManager::SetHovered(int panelIdx) {
    for (int i = 0; i < static_cast<int>(panels_.size()); i++) {
        bool wantHover = (i == panelIdx);
        if (panels_[i].barHovered != wantHover) {
            panels_[i].barHovered = wantHover;
            FillBar(i, wantHover);
        }
    }
}

void PanelManager::BuildLayers(XrCompositionLayerQuad* outQuads, int outCap, int* outCount,
                                const XrPosef& headPoseInLocal) {
    int count = 0;
    // Panel bodies (launcher deferred to render last so it draws on top)
    for (int i = 0; i < static_cast<int>(panels_.size()); i++) {
        if (count >= outCap) break;
        const Panel& p = panels_[i];
        if (p.dead) continue;
        if (!p.visible) continue;
        if (p.bodyHidden) continue;
        if (p.isLauncher) continue;  // v0.9.3: draw launcher last
        if (p.isKeyboard) continue;  // v0.9.5: draw keyboard last too
        XrPosef world = ResolveWorldPose(i, headPoseInLocal);
        world.orientation = QNorm(world.orientation);
        if (!VecFinite(world.position)) continue;
        XrCompositionLayerQuad& q = outQuads[count++];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space = localSpace_;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = p.swapchain;
        q.subImage.imageRect.offset = {0, 0};
        q.subImage.imageRect.extent = {p.width, p.height};
        q.subImage.imageArrayIndex = 0;
        q.size = p.size;
        q.pose = world;
    }
    // v0.9.3/5: draw launcher + keyboard body last so they z-order above hosted panels
    for (int i = 0; i < static_cast<int>(panels_.size()); i++) {
        if (count >= outCap) break;
        const Panel& p = panels_[i];
        if (!(p.isLauncher || p.isKeyboard) || p.dead || !p.visible || p.bodyHidden) continue;
        XrPosef world = ResolveWorldPose(i, headPoseInLocal);
        world.orientation = QNorm(world.orientation);
        if (!VecFinite(world.position)) continue;
        XrCompositionLayerQuad& q = outQuads[count++];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space = localSpace_;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = p.swapchain;
        q.subImage.imageRect.offset = {0, 0};
        q.subImage.imageRect.extent = {p.width, p.height};
        q.subImage.imageArrayIndex = 0;
        q.size = p.size;
        q.pose = world;
    }

    // Display bars — v0.8-1b: hover-expanded, sourced from OES when available
    for (int i = 0; i < static_cast<int>(panels_.size()); i++) {
        if (count >= outCap) break;
        const Panel& p = panels_[i];
        if (p.dead) continue;         // v0.8.4 #14
        if (!p.visible) continue;
        if (p.isKeyboard || p.isDock || p.isLauncher) continue;  // v0.9.2: kb/dock/launcher no bar
        XrPosef world = ResolveWorldPose(i, headPoseInLocal);
        float barH = p.barHovered ? BAR_HOVER_HEIGHT_M : BAR_HEIGHT_M;
        float barW = p.size.width * (p.barHovered ? BAR_HOVER_WIDTH_RATIO : BAR_WIDTH_RATIO);
        float barCenterOffsetY = -(p.size.height * 0.5f + BAR_GAP_M + barH * 0.5f);
        XrPosef barLocalOffset = {{0,0,0,1}, {0, barCenterOffsetY, 0}};
        XrPosef barWorld = PoseMul(world, barLocalOffset);
        barWorld.orientation = QNorm(barWorld.orientation);
        if (!VecFinite(barWorld.position)) continue;

        XrCompositionLayerQuad& q = outQuads[count++];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space = localSpace_;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = p.barSwapchain;
        q.subImage.imageRect.offset = {0, 0};
        q.subImage.imageRect.extent = {BAR_HOVER_TEX_W, BAR_HOVER_TEX_H};
        q.subImage.imageArrayIndex = 0;
        q.size = {barW, barH};
        q.pose = barWorld;
    }
    // Corner handles: draw only for hovered panel to respect layer budget
    for (int i = 0; i < static_cast<int>(panels_.size()); i++) {
        if (count >= outCap) break;
        const Panel& p = panels_[i];
        if (!p.visible) continue;
        if (!p.barHovered) continue;
        XrPosef world = ResolveWorldPose(i, headPoseInLocal);
        const float hw = p.size.width * 0.5f;
        const float hh = p.size.height * 0.5f;
        const XrVector3f offs[4] = { {-hw,-hh,0}, {hw,-hh,0}, {-hw,hh,0}, {hw,hh,0} };
        for (int c = 0; c < 4 && count < outCap; c++) {
            XrPosef corner = PoseMul(world, {{0,0,0,1}, offs[c]});
            corner.orientation = QNorm(corner.orientation);
            if (!VecFinite(corner.position)) continue;
            XrCompositionLayerQuad& q = outQuads[count++];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = localSpace_;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = handleSc_;
            q.subImage.imageRect.offset = {0, 0};
            q.subImage.imageRect.extent = {HANDLE_TEX, HANDLE_TEX};
            q.subImage.imageArrayIndex = 0;
            q.size = {HANDLE_SIZE_M, HANDLE_SIZE_M};
            q.pose = corner;
        }
    }
    *outCount = count;
}



// ---------------- RayCursor ----------------

void RayCursor::Init(XrSession session) {
    sc_ = CreateColorSwapchain(session, 32, 32);
    active_ = true;
    SetActive(false);
}
void RayCursor::Shutdown() {
    if (sc_ != XR_NULL_HANDLE) { xrDestroySwapchain(sc_); sc_ = XR_NULL_HANDLE; }
}
void RayCursor::SetActive(bool active) {
    if (active_ == active && sc_ != XR_NULL_HANDLE) return;
    active_ = active;
    if (active) FillSwapchainSolid(sc_, 0.95f, 0.20f, 0.20f, 0.95f);
    else        FillSwapchainSolid(sc_, 0.30f, 0.85f, 0.95f, 0.90f);
}
bool RayCursor::BuildLayer(XrCompositionLayerQuad* out, XrSpace localSpace,
                             const XrVector3f& worldPoint, const XrPosef& headPoseInLocal) {
    if (!visible_ || sc_ == XR_NULL_HANDLE) return false;
    // Billboard: rotate +Z (OpenXR quad normal) to face the head from worldPoint.
    float dx = headPoseInLocal.position.x - worldPoint.x;
    float dy = headPoseInLocal.position.y - worldPoint.y;
    float dz = headPoseInLocal.position.z - worldPoint.z;
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 1e-4f) return false;
    dx /= len; dy /= len; dz /= len;
    // Rotation from +Z to normalized(dx,dy,dz). Quad face is +Z per OpenXR spec.
    // dot(+Z, d) = dz. cross(+Z, d) = (-dy, dx, 0).
    XrQuaternionf ori;
    if (dz > 0.9999f) {
        // d ~ +Z: no rotation, face already points to head
        ori = {0, 0, 0, 1};
    } else if (dz < -0.9999f) {
        // d ~ -Z: 180 rotation around Y
        ori = {0, 1, 0, 0};
    } else {
        float cosA = dz;
        float halfA = std::acos(cosA) * 0.5f;
        float sh = std::sin(halfA), ch = std::cos(halfA);
        float axLen = std::sqrt(dy*dy + dx*dx);
        ori = { (-dy/axLen) * sh, (dx/axLen) * sh, 0.0f, ch };
    }
    ori = QNorm(ori);
    if (!VecFinite(worldPoint)) return false;

    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = localSpace;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = sc_;
    out->subImage.imageRect.offset = {0, 0};
    out->subImage.imageRect.extent = {32, 32};
    out->subImage.imageArrayIndex = 0;
    out->size = {0.02f, 0.02f};  // 2cm cursor
    out->pose.orientation = ori;
    out->pose.position = worldPoint;
    return true;
}



// ---------------- RayLine ----------------

void RayLine::Init(XrSession session) {
    sc_ = CreateColorSwapchain(session, 8, 32);
    active_ = true;
    SetActive(false);
}
void RayLine::Shutdown() {
    if (sc_ != XR_NULL_HANDLE) { xrDestroySwapchain(sc_); sc_ = XR_NULL_HANDLE; }
}
void RayLine::SetActive(bool active) {
    if (active_ == active && sc_ != XR_NULL_HANDLE) return;
    active_ = active;
    if (active) FillSwapchainSolid(sc_, 1.0f, 0.75f, 0.30f, 0.85f);   // amber when triggered
    else        FillSwapchainSolid(sc_, 0.85f, 0.90f, 1.00f, 0.65f);  // pale blue idle
}
bool RayLine::BuildLayer(XrCompositionLayerQuad* out, XrSpace localSpace,
                          const XrVector3f& origin, const XrVector3f& dir, float length,
                          const XrPosef& headPoseInLocal) {
    if (!visible_ || sc_ == XR_NULL_HANDLE || length < 0.05f) return false;

    // Midpoint of the ray in world
    XrVector3f mid = {
        origin.x + dir.x * length * 0.5f,
        origin.y + dir.y * length * 0.5f,
        origin.z + dir.z * length * 0.5f,
    };

    // We render a thin quad. The quad's "up" (Y) should align with the ray direction.
    // The quad's normal (-Z) should face the head (billboarded around the ray axis).
    // Build orientation:
    //   forward (quad -Z) = perpendicular to ray AND facing head
    //   up (quad +Y) = ray direction (dir)
    //   right (quad +X) = up × forward
    // First: normalize ray dir
    float rlen = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (rlen < 1e-4f) return false;
    XrVector3f rd = { dir.x / rlen, dir.y / rlen, dir.z / rlen };

    // mid-to-head vector
    XrVector3f m2h = {
        headPoseInLocal.position.x - mid.x,
        headPoseInLocal.position.y - mid.y,
        headPoseInLocal.position.z - mid.z,
    };
    float m2hLen = std::sqrt(m2h.x*m2h.x + m2h.y*m2h.y + m2h.z*m2h.z);
    if (m2hLen < 1e-4f) return false;
    m2h.x /= m2hLen; m2h.y /= m2hLen; m2h.z /= m2hLen;

    // quad normal = m2h projected perpendicular to rd, normalized
    // n = m2h - (m2h . rd) * rd
    float dot = m2h.x*rd.x + m2h.y*rd.y + m2h.z*rd.z;
    XrVector3f n = { m2h.x - dot*rd.x, m2h.y - dot*rd.y, m2h.z - dot*rd.z };
    float nLen = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
    if (nLen < 1e-4f) return false;
    n.x /= nLen; n.y /= nLen; n.z /= nLen;


    // OpenXR quad face normal is +Z. We want +Z to point TOWARD head (= +n direction, since n is m2h projected).
    // Matrix R with columns [right, up, +Z]: R * (0,0,1) = column 2 = +n (points to head).
    // We need rt × rd = +n for right-handed frame. Our rt = n × rd gives rt × rd = -n. So swap to right = rd × n.
    XrVector3f rtc = { rd.y * n.z - rd.z * n.y,
                        rd.z * n.x - rd.x * n.z,
                        rd.x * n.y - rd.y * n.x };
    float m00 = rtc.x, m01 = rd.x, m02 = n.x;
    float m10 = rtc.y, m11 = rd.y, m12 = n.y;
    float m20 = rtc.z, m21 = rd.z, m22 = n.z;

    // Standard matrix-to-quaternion
    float tr = m00 + m11 + m22;
    XrQuaternionf q;
    if (tr > 0.0f) {
        float S = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * S;
        q.x = (m21 - m12) / S;
        q.y = (m02 - m20) / S;
        q.z = (m10 - m01) / S;
    } else if (m00 > m11 && m00 > m22) {
        float S = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / S;
        q.x = 0.25f * S;
        q.y = (m01 + m10) / S;
        q.z = (m02 + m20) / S;
    } else if (m11 > m22) {
        float S = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / S;
        q.x = (m01 + m10) / S;
        q.y = 0.25f * S;
        q.z = (m12 + m21) / S;
    } else {
        float S = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / S;
        q.x = (m02 + m20) / S;
        q.y = (m12 + m21) / S;
        q.z = 0.25f * S;
    }

    q = QNorm(q);
    if (!VecFinite(mid)) return false;
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = localSpace;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = sc_;
    out->subImage.imageRect.offset = {0, 0};
    out->subImage.imageRect.extent = {8, 32};
    out->subImage.imageArrayIndex = 0;
    out->size = {0.004f, length};  // 4mm wide, length meters tall
    out->pose.orientation = q;
    out->pose.position = mid;
    return true;
}



void PanelManager::UpdateDynamic(float time) {
    // For each dynamic panel: shift a base color's brightness with time.
    // MVP: cycle through hues. Later phases replace this with real content (Kotlin bitmap, VirtualDisplay).
    for (size_t i = 0; i < panels_.size(); i++) {
        Panel& p = panels_[i];
        if (!p.dynamic) continue;
        // Simple HSV-like sweep: alternate through R/G/B with sinusoidal weights
        float phase = time * 0.5f;  // slow cycle
        float r = 0.5f + 0.35f * std::sin(phase + 0.0f);
        float g = 0.5f + 0.35f * std::sin(phase + 2.094f);   // + 2*pi/3
        float b = 0.5f + 0.35f * std::sin(phase + 4.188f);   // + 4*pi/3
        p.clearColor[0] = r;
        p.clearColor[1] = g;
        p.clearColor[2] = b;
        p.clearColor[3] = 0.85f;
        FillSwapchainSolid(p.swapchain, r, g, b, 0.85f);
    }
}



// ---------------- OesBlitter ----------------

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

static const char* kOesVertex = R"(#version 300 es
layout(location=0) in vec2 aPos;
uniform mat4 uSTMatrix;
out vec2 vUV;
void main() {
    // Raw UV in [0,1]; SurfaceTexture's transform matrix handles crop + Y-flip.
    vec2 raw = vec2(aPos.x * 0.5 + 0.5, aPos.y * 0.5 + 0.5);
    vUV = (uSTMatrix * vec4(raw, 0.0, 1.0)).xy;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kOesFragment = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTex;
uniform float uForceOpaque;  // 1.0=force alpha=1; 0.0=preserve source alpha
uniform float uPanelAlpha;   // v0.8-1b-ii: extra alpha multiplier per panel (slider)
in vec2 vUV;
out vec4 outColor;
void main() {
    vec4 c = texture(uTex, vUV);
    float a = mix(c.a, 1.0, uForceOpaque) * uPanelAlpha;
    outColor = vec4(c.rgb, a);
}
)";

static unsigned int CompileShader(unsigned int type, const char* src) {
    unsigned int sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        // no ALOGE in Panel.cpp; use __android_log_print directly
        __android_log_print(ANDROID_LOG_ERROR, "OesBlitter", "shader compile failed: %s", log);
        glDeleteShader(sh); return 0;
    }
    return sh;
}

bool OesBlitter::Init() {
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, kOesVertex);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, kOesFragment);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return false; }
    program_ = glCreateProgram();
    glAttachShader(program_, vs); glAttachShader(program_, fs);
    glLinkProgram(program_);
    GLint ok = 0; glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(program_, sizeof log, nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, "OesBlitter", "link failed: %s", log);
        glDeleteProgram(program_); program_ = 0; return false;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    uTexLoc_ = glGetUniformLocation(program_, "uTex");
    uSTMatrixLoc_ = glGetUniformLocation(program_, "uSTMatrix");
    uForceOpaqueLoc_ = glGetUniformLocation(program_, "uForceOpaque");
    uPanelAlphaLoc_ = glGetUniformLocation(program_, "uPanelAlpha");

    // Fullscreen triangle strip (2 triangles)
    static const float verts[] = { -1,-1,  1,-1,  -1,1,  1,1 };
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    ready_ = true;
    __android_log_print(ANDROID_LOG_ERROR, "OesBlitter", "init ok prog=%u vao=%u", program_, vao_);
    return true;
}

void OesBlitter::Shutdown() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    vbo_ = vao_ = program_ = 0; ready_ = false;
}

bool OesBlitter::BlitToPanel(Panel& p, unsigned int oesTexId, const float* stMatrix4x4) {
    if (!ready_ || oesTexId == 0) return false;
    if (p.swapchain == XR_NULL_HANDLE) return false;

    // Acquire panel swapchain image
    unsigned int len = 0;
    OXR(xrEnumerateSwapchainImages(p.swapchain, 0, &len, nullptr));
    auto* imgs = new XrSwapchainImageOpenGLESKHR[len];
    for (unsigned int i = 0; i < len; i++) imgs[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
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
    // v0.4.3g: correct viewport to actual swap dims (was 4096x4096 which made triangles cover only 6.25% of FBO)
    glViewport(0, 0, p.width, p.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    // v0.4.3a: clear to opaque black so unfilled pixels are not transparent
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTexId);
    glUniform1i(uTexLoc_, 0);
    glUniform1f(uForceOpaqueLoc_, p.oesForceOpaque ? 1.0f : 0.0f);
        glUniform1f(uPanelAlphaLoc_, p.panelAlpha);
    static const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float* mat = stMatrix4x4 ? stMatrix4x4 : kIdentity;
    if (uSTMatrixLoc_ >= 0) glUniformMatrix4fv(uSTMatrixLoc_, 1, GL_FALSE, mat);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    OXR(xrReleaseSwapchainImage(p.swapchain, &ri));
    delete[] imgs;
    return true;
}

// v0.8-1b: blit OES tex into p.barSwapchain (uses hover-expanded pixel dims)
bool OesBlitter::BlitToBar(Panel& p, unsigned int oesTexId, const float* stMatrix4x4) {
    if (!ready_ || oesTexId == 0) return false;
    if (p.barSwapchain == XR_NULL_HANDLE) return false;

    unsigned int len = 0;
    OXR(xrEnumerateSwapchainImages(p.barSwapchain, 0, &len, nullptr));
    auto* imgs = new XrSwapchainImageOpenGLESKHR[len];
    for (unsigned int i = 0; i < len; i++) imgs[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    OXR(xrEnumerateSwapchainImages(p.barSwapchain, len, &len,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs)));
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    OXR(xrAcquireSwapchainImage(p.barSwapchain, &ai, &idx));
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    OXR(xrWaitSwapchainImage(p.barSwapchain, &wi));

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            static_cast<GLuint>(imgs[idx].image), 0);

    glViewport(0, 0, BAR_HOVER_TEX_W, BAR_HOVER_TEX_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTexId);
    glUniform1i(uTexLoc_, 0);
    glUniform1f(uForceOpaqueLoc_, 0.0f);
    glUniform1f(uPanelAlphaLoc_, 1.0f);  // bar not affected by panel alpha slider
    if (stMatrix4x4) glUniformMatrix4fv(uSTMatrixLoc_, 1, GL_FALSE, stMatrix4x4);
    else {
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        glUniformMatrix4fv(uSTMatrixLoc_, 1, GL_FALSE, ident);
    }

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    OXR(xrReleaseSwapchainImage(p.barSwapchain, &ri));
    delete[] imgs;
    return true;
}

} // namespace rover
