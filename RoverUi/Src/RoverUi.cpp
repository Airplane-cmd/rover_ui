/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * Licensed under the Oculus SDK License Agreement (the "License");
 * you may not use the Oculus SDK except in compliance with the License,
 * which is provided at the time of installation or download, or which
 * otherwise accompanies this software in either electronic or hard copy form.
 *
 * You may obtain a copy of the License at
 * https://developer.oculus.com/licenses/oculussdk/
 *
 * Unless required by applicable law or agreed to in writing, the Oculus SDK
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/************************************************************************************

Filename  : XrPassthrough.cpp
Content   : This sample uses the Android NativeActivity class.
Created   :
Authors   :

*************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h> // for memset
#include <math.h>
#include <time.h>

#include <vector>

#if defined(ANDROID)
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h> // for prctl( PR_SET_NAME )
#include <android/log.h>
#include <android/native_window_jni.h> // for native window JNI
#include <android_native_app_glue.h>
#else
#include <thread>
#include <chrono>
#endif // defined(ANDROID)

#include <assert.h>

#include "RoverUi.h"
#include "Panel.h"

// v0.4.2e: forward decls (definitions later in this file)
static jclass g_bridgeCls = nullptr;
static void CacheBridgeClass(struct android_app*, JNIEnv*);

#include "RoverUiInput.h"
#include "RoverUiGl.h"

using namespace OVR;

#if !defined(EGL_OPENGL_ES3_BIT_KHR)
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#define OVR_LOG_TAG "XrPassthrough"

#if !defined(XR_USE_GRAPHICS_API_OPENGL_ES) && !defined(XR_USE_GRAPHICS_API_OPENGL)
#error A graphics backend must be defined!
#elif defined(XR_USE_GRAPHICS_API_OPENGL_ES) && defined(XR_USE_GRAPHICS_API_OPENGL)
#error Only one graphics backend shall be defined!
#endif

#if defined(ANDROID)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, OVR_LOG_TAG, __VA_ARGS__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, OVR_LOG_TAG, __VA_ARGS__)
#else
#define ALOGE(...)       \
    printf("ERROR: ");   \
    printf(__VA_ARGS__); \
    printf("\n")
#define ALOGV(...)       \
    printf("VERBOSE: "); \
    printf(__VA_ARGS__); \
    printf("\n")
#endif

static const int CPU_LEVEL = 2;
static const int GPU_LEVEL = 3;
static const int NUM_MULTI_SAMPLES = 4;

/*
================================================================================

OpenXR Utility Functions

================================================================================
*/

XrInstance instance;
void OXR_CheckErrors(XrResult result, const char* function, bool failOnError) {
    if (XR_FAILED(result)) {
        char errorBuffer[XR_MAX_RESULT_STRING_SIZE];
        xrResultToString(instance, result, errorBuffer);
        if (failOnError) {
            ALOGE("OpenXR error: %s: %s\n", function, errorBuffer);
        } else {
            ALOGV("OpenXR error: %s: %s\n", function, errorBuffer);
        }
    }
}

#define DECL_PFN(pfn) PFN_##pfn pfn = nullptr
#define INIT_PFN(pfn) OXR(xrGetInstanceProcAddr(instance, #pfn, (PFN_xrVoidFunction*)(&pfn)))

// FB_passthrough sample begin
DECL_PFN(xrCreatePassthroughFB);
DECL_PFN(xrDestroyPassthroughFB);
DECL_PFN(xrPassthroughStartFB);
DECL_PFN(xrPassthroughPauseFB);
DECL_PFN(xrCreatePassthroughLayerFB);
DECL_PFN(xrDestroyPassthroughLayerFB);
DECL_PFN(xrPassthroughLayerSetStyleFB);
DECL_PFN(xrPassthroughLayerPauseFB);
DECL_PFN(xrPassthroughLayerResumeFB);
DECL_PFN(xrCreateTriangleMeshFB);
DECL_PFN(xrDestroyTriangleMeshFB);
DECL_PFN(xrTriangleMeshGetVertexBufferFB);
DECL_PFN(xrTriangleMeshGetIndexBufferFB);
DECL_PFN(xrTriangleMeshBeginUpdateFB);
DECL_PFN(xrTriangleMeshEndUpdateFB);
DECL_PFN(xrCreateGeometryInstanceFB);
DECL_PFN(xrDestroyGeometryInstanceFB);
DECL_PFN(xrGeometryInstanceSetTransformFB);
// FB_passthrough sample end

/*
================================================================================

Egl Utility Functions

================================================================================
*/

#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
static const char* EglErrorString(const EGLint error) {
    switch (error) {
        case EGL_SUCCESS:
            return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:
            return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:
            return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:
            return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:
            return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT:
            return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG:
            return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE:
            return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:
            return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE:
            return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH:
            return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER:
            return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP:
            return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW:
            return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST:
            return "EGL_CONTEXT_LOST";
        default:
            return "unknown";
    }
}

void Egl::Clear() {
    MajorVersion = 0;
    MinorVersion = 0;
    Display = 0;
    Config = 0;
    TinySurface = EGL_NO_SURFACE;
    MainSurface = EGL_NO_SURFACE;
    Context = EGL_NO_CONTEXT;
}

void Egl::CreateContext(const Egl* shareEgl) {
    if (Display != 0) {
        return;
    }

    Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    ALOGV("        eglInitialize( Display, &MajorVersion, &MinorVersion )");
    eglInitialize(Display, &MajorVersion, &MinorVersion);
    // Do NOT use eglChooseConfig, because the Android EGL code pushes in multisample
    // flags in eglChooseConfig if the user has selected the "force 4x MSAA" option in
    // settings, and that is completely wasted for our warp target.
    const int MAX_CONFIGS = 1024;
    EGLConfig configs[MAX_CONFIGS];
    EGLint numConfigs = 0;
    if (eglGetConfigs(Display, configs, MAX_CONFIGS, &numConfigs) == EGL_FALSE) {
        ALOGE("        eglGetConfigs() failed: %s", EglErrorString(eglGetError()));
        return;
    }
    const EGLint configAttribs[] = {
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8, // need alpha for the multi-pass timewarp compositor
        EGL_DEPTH_SIZE,
        0,
        EGL_STENCIL_SIZE,
        0,
        EGL_SAMPLES,
        0,
        EGL_NONE};
    Config = 0;
    for (int i = 0; i < numConfigs; i++) {
        EGLint value = 0;

        eglGetConfigAttrib(Display, configs[i], EGL_RENDERABLE_TYPE, &value);
        if ((value & EGL_OPENGL_ES3_BIT_KHR) != EGL_OPENGL_ES3_BIT_KHR) {
            continue;
        }

        // The pbuffer config also needs to be compatible with normal window rendering
        // so it can share textures with the window context.
        eglGetConfigAttrib(Display, configs[i], EGL_SURFACE_TYPE, &value);
        if ((value & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) != (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) {
            continue;
        }

        int j = 0;
        for (; configAttribs[j] != EGL_NONE; j += 2) {
            eglGetConfigAttrib(Display, configs[i], configAttribs[j], &value);
            if (value != configAttribs[j + 1]) {
                break;
            }
        }
        if (configAttribs[j] == EGL_NONE) {
            Config = configs[i];
            break;
        }
    }
    if (Config == 0) {
        ALOGE("        eglChooseConfig() failed: %s", EglErrorString(eglGetError()));
        return;
    }
    EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    ALOGV("        Context = eglCreateContext( Display, Config, EGL_NO_CONTEXT, contextAttribs )");
    Context = eglCreateContext(
        Display,
        Config,
        (shareEgl != nullptr) ? shareEgl->Context : EGL_NO_CONTEXT,
        contextAttribs);
    if (Context == EGL_NO_CONTEXT) {
        ALOGE("        eglCreateContext() failed: %s", EglErrorString(eglGetError()));
        return;
    }
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    ALOGV("        TinySurface = eglCreatePbufferSurface( Display, Config, surfaceAttribs )");
    TinySurface = eglCreatePbufferSurface(Display, Config, surfaceAttribs);
    if (TinySurface == EGL_NO_SURFACE) {
        ALOGE("        eglCreatePbufferSurface() failed: %s", EglErrorString(eglGetError()));
        eglDestroyContext(Display, Context);
        Context = EGL_NO_CONTEXT;
        return;
    }
    ALOGV("        eglMakeCurrent( Display, TinySurface, TinySurface, Context )");
    if (eglMakeCurrent(Display, TinySurface, TinySurface, Context) == EGL_FALSE) {
        ALOGE("        eglMakeCurrent() failed: %s", EglErrorString(eglGetError()));
        eglDestroySurface(Display, TinySurface);
        eglDestroyContext(Display, Context);
        Context = EGL_NO_CONTEXT;
        return;
    }
}

void Egl::DestroyContext() {
    if (Display != 0) {
        ALOGE("        eglMakeCurrent( Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT )");
        if (eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_FALSE) {
            ALOGE("        eglMakeCurrent() failed: %s", EglErrorString(eglGetError()));
        }
    }
    if (Context != EGL_NO_CONTEXT) {
        ALOGE("        eglDestroyContext( Display, Context )");
        if (eglDestroyContext(Display, Context) == EGL_FALSE) {
            ALOGE("        eglDestroyContext() failed: %s", EglErrorString(eglGetError()));
        }
        Context = EGL_NO_CONTEXT;
    }
    if (TinySurface != EGL_NO_SURFACE) {
        ALOGE("        eglDestroySurface( Display, TinySurface )");
        if (eglDestroySurface(Display, TinySurface) == EGL_FALSE) {
            ALOGE("        eglDestroySurface() failed: %s", EglErrorString(eglGetError()));
        }
        TinySurface = EGL_NO_SURFACE;
    }
    if (Display != 0) {
        ALOGE("        eglTerminate( Display )");
        if (eglTerminate(Display) == EGL_FALSE) {
            ALOGE("        eglTerminate() failed: %s", EglErrorString(eglGetError()));
        }
        Display = 0;
    }
}

#elif defined(XR_USE_GRAPHICS_API_OPENGL)

#if defined(WIN32)
// Favor the high performance NVIDIA or AMD GPUs
extern "C" {
// http://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#endif //  defined(WIN32)

void Egl::Clear() {
    hDC = 0;
    hGLRC = 0;
}

void Egl::CreateContext(const Egl*) {
    ovrGl_CreateContext_Windows(&hDC, &hGLRC);
}

void Egl::DestroyContext() {
    ovrGl_DestroyContext_Windows();
}

#endif

void App::Clear() {
#if defined(XR_USE_PLATFORM_ANDROID)
    Resumed = false;
#endif // defined(XR_USE_PLATFORM_ANDROID)
    ShouldExit = false;
    Focused = false;
    Instance = XR_NULL_HANDLE;
    Session = XR_NULL_HANDLE;
    ViewportConfig = {};
    for (int i = 0; i < NUM_EYES; i++) {
        ViewConfigurationView[i] = {};
    }
    SystemId = XR_NULL_SYSTEM_ID;
    HeadSpace = XR_NULL_HANDLE;
    LocalSpace = XR_NULL_HANDLE;
    StageSpace = XR_NULL_HANDLE;
    SessionActive = false;
    SwapInterval = 1;
    for (int i = 0; i < MaxLayerCount; i++) {
        Layers[i] = {};
    }
    LayerCount = 0;
    CpuLevel = 2;
    GpuLevel = 2;
    MainThreadTid = 0;
    RenderThreadTid = 0;
    TouchPadDownLastFrame = false;

    egl.Clear();
    appRenderer.Clear();
}

void App::HandleSessionStateChanges(XrSessionState state) {
    if (state == XR_SESSION_STATE_READY) {
#if defined(XR_USE_PLATFORM_ANDROID)
        static bool pendingBegin = false;
        if (!Resumed) {
            ALOGE("HandleSessionStateChanges: READY while not Resumed — deferring");
            pendingBegin = true;
            return;
        }
        pendingBegin = false;
#endif
        if (SessionActive) return;

        XrSessionBeginInfo sessionBeginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
        sessionBeginInfo.primaryViewConfigurationType = ViewportConfig.viewConfigurationType;

        XrResult result;
        OXR(result = xrBeginSession(Session, &sessionBeginInfo));

        SessionActive = (result == XR_SUCCESS);

#if defined(XR_USE_PLATFORM_ANDROID)
        // Set session state once we have entered VR mode and have a valid session object.
        if (SessionActive) {
            XrPerfSettingsLevelEXT cpuPerfLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
            switch (CpuLevel) {
                case 0:
                    cpuPerfLevel = XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
                    break;
                case 1:
                    cpuPerfLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
                    break;
                case 2:
                    cpuPerfLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
                    break;
                case 3:
                    cpuPerfLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
                    break;
                default:
                    ALOGE("Invalid CPU level %d", CpuLevel);
                    break;
            }

            XrPerfSettingsLevelEXT gpuPerfLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
            switch (GpuLevel) {
                case 0:
                    gpuPerfLevel = XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
                    break;
                case 1:
                    gpuPerfLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
                    break;
                case 2:
                    gpuPerfLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
                    break;
                case 3:
                    gpuPerfLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
                    break;
                default:
                    ALOGE("Invalid GPU level %d", GpuLevel);
                    break;
            }

            PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerfSettingsSetPerformanceLevelEXT = NULL;
            OXR(xrGetInstanceProcAddr(
                Instance,
                "xrPerfSettingsSetPerformanceLevelEXT",
                (PFN_xrVoidFunction*)(&pfnPerfSettingsSetPerformanceLevelEXT)));

            OXR(pfnPerfSettingsSetPerformanceLevelEXT(
                Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, cpuPerfLevel));
            OXR(pfnPerfSettingsSetPerformanceLevelEXT(
                Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpuPerfLevel));

            PFN_xrSetAndroidApplicationThreadKHR pfnSetAndroidApplicationThreadKHR = NULL;
            OXR(xrGetInstanceProcAddr(
                Instance,
                "xrSetAndroidApplicationThreadKHR",
                (PFN_xrVoidFunction*)(&pfnSetAndroidApplicationThreadKHR)));

            OXR(pfnSetAndroidApplicationThreadKHR(
                Session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, MainThreadTid));
            OXR(pfnSetAndroidApplicationThreadKHR(
                Session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, RenderThreadTid));
        }
#endif // defined(XR_USE_PLATFORM_ANDROID)
    } else if (state == XR_SESSION_STATE_STOPPING) {
        // v0.4.2b: MediaProjection dialog can put us into STOPPING before Resumed flips.
        // Skip the pause assert and just end the session cleanly if it is active.
        if (SessionActive) {
            OXR(xrEndSession(Session));
            SessionActive = false;
        }
    }
}

void App::HandleXrEvents() {
    XrEventDataBuffer eventDataBuffer = {};

    // Poll for events
    for (;;) {
        XrEventDataBaseHeader* baseEventHeader = (XrEventDataBaseHeader*)(&eventDataBuffer);
        baseEventHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
        baseEventHeader->next = NULL;
        XrResult r;
        OXR(r = xrPollEvent(Instance, &eventDataBuffer));
        if (r != XR_SUCCESS) {
            break;
        }

        switch (baseEventHeader->type) {
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                ALOGV("xrPollEvent: received XR_TYPE_EVENT_DATA_EVENTS_LOST event");
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                ALOGV("xrPollEvent: received XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING event");
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                ALOGV("xrPollEvent: received XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event");
                break;
            case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
#if defined(XR_USE_PLATFORM_ANDROID)
                const XrEventDataPerfSettingsEXT* perf_settings_event =
                    (XrEventDataPerfSettingsEXT*)(baseEventHeader);
                ALOGV(
                    "xrPollEvent: received XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT event: type %d subdomain %d : level %d -> level %d",
                    perf_settings_event->type,
                    perf_settings_event->subDomain,
                    perf_settings_event->fromLevel,
                    perf_settings_event->toLevel);
#endif // defined(XR_USE_PLATFORM_ANDROID)
            } break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                ALOGV(
                    "xrPollEvent: received XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING event");
                break;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const XrEventDataSessionStateChanged* session_state_changed_event =
                    (XrEventDataSessionStateChanged*)(baseEventHeader);
                ALOGV(
                    "xrPollEvent: received XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: %d for session %p at time %f",
                    session_state_changed_event->state,
                    (void*)session_state_changed_event->session,
                    FromXrTime(session_state_changed_event->time));

                switch (session_state_changed_event->state) {
                    case XR_SESSION_STATE_FOCUSED:
                        Focused = true;
                        break;
                    case XR_SESSION_STATE_VISIBLE:
                        Focused = false;
                        break;
                    case XR_SESSION_STATE_READY:
                    case XR_SESSION_STATE_STOPPING:
                        HandleSessionStateChanges(session_state_changed_event->state);
                        break;
                    case XR_SESSION_STATE_EXITING:
                        ShouldExit = true;
                        break;
                    default:
                        break;
                }
            } break;
            default:
                ALOGV("xrPollEvent: Unknown event");
                break;
        }
    }
}

#if defined(XR_USE_PLATFORM_ANDROID)
/*
================================================================================

Native Activity

================================================================================
*/

/**
 * Process the next main command.
 */
static void app_handle_cmd(struct android_app* androidApp, int32_t cmd) {
    App& app = *(App*)androidApp->userData;

    switch (cmd) {
        // There is no APP_CMD_CREATE. The ANativeActivity creates the
        // application thread from onCreate(). The application thread
        // then calls android_main().
        case APP_CMD_START: {
            ALOGV("onStart()");
            ALOGV("    APP_CMD_START");
            break;
        }
        case APP_CMD_RESUME: {
            ALOGV("onResume()");
            ALOGV("    APP_CMD_RESUME");
            app.Resumed = true;
            // v0.4.2c-fix: if we deferred a READY handling while paused, retry now
            if (!app.SessionActive) {
                app.HandleSessionStateChanges(XR_SESSION_STATE_READY);
            }
            break;
        }
        case APP_CMD_PAUSE: {
            ALOGV("onPause()");
            ALOGV("    APP_CMD_PAUSE");
            app.Resumed = false;
            break;
        }
        case APP_CMD_STOP: {
            ALOGV("onStop()");
            ALOGV("    APP_CMD_STOP");
            break;
        }
        case APP_CMD_DESTROY: {
            ALOGV("onDestroy()");
            ALOGV("    APP_CMD_DESTROY");
            app.Clear();
            break;
        }
        case APP_CMD_INIT_WINDOW: {
            ALOGV("surfaceCreated()");
            ALOGV("    APP_CMD_INIT_WINDOW");
            break;
        }
        case APP_CMD_TERM_WINDOW: {
            ALOGV("surfaceDestroyed()");
            ALOGV("    APP_CMD_TERM_WINDOW");
            break;
        }
    }
}
#endif // defined(XR_USE_PLATFORM_ANDROID)

void UpdateStageBounds(App& app) {
    XrExtent2Df stageBounds = {};

    XrResult result;
    OXR(result = xrGetReferenceSpaceBoundsRect(
            app.Session, XR_REFERENCE_SPACE_TYPE_STAGE, &stageBounds));
    if (result != XR_SUCCESS) {
        ALOGV("Stage bounds query failed: using small defaults");
        stageBounds.width = 1.0f;
        stageBounds.height = 1.0f;
    }

    app.StageBounds = Vector3f(stageBounds.width * 0.5f, 1.0f, stageBounds.height * 0.5f);
}

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
#if defined(XR_USE_PLATFORM_ANDROID)

// v0.4.1: Kotlin bridge — call RoverBridge.helloFromKotlin() and log the result

// GL_TEXTURE_EXTERNAL_OES constant (comes from GL_OES_EGL_image_external)
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

static GLuint g_oesTextureId = 0;
static GLuint g_kbOesTextureId = 0;
static int g_kbPanelIdx = -1;
static int g_sliderPanelIdx = -1;
static int g_sliderHand = 0;  // v0.8-1b: bar-slider drag state  // v0.7.2: tracked so buttons can toggle visibility

static void CallSetupOesTexture(struct android_app* androidApp) {
    // Create OES texture in the current EGL context (called from android_main after Egl init)
    glGenTextures(1, &g_oesTextureId);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_oesTextureId);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    ALOGE("[rover] OES texture created id=%u", g_oesTextureId);

    // Push id to Kotlin
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    jobject activity = androidApp->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getCl = env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject clsLoader = env->CallObjectMethod(activity, getCl);
    jclass clsLoaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(clsLoaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.gantrping.rover.RoverBridge");
    jclass cls = (jclass)env->CallObjectMethod(clsLoader, loadClass, name);
    env->DeleteLocalRef(name); env->DeleteLocalRef(clsLoaderCls);
    env->DeleteLocalRef(clsLoader); env->DeleteLocalRef(actCls);
    if (!cls || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    jmethodID method = env->GetStaticMethodID(cls, "setExternalOesTextureId", "(I)V");
    if (method) env->CallStaticVoidMethod(cls, method, (jint)g_oesTextureId);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(cls);
}


static void CallSetupKeyboardOesTexture(struct android_app* androidApp) {
    glGenTextures(1, &g_kbOesTextureId);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_kbOesTextureId);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    ALOGE("[rover] keyboard OES texture created id=%u", g_kbOesTextureId);
    ALOGE("[rover-kb] about to JNI setKeyboardOesTextureId");
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    ALOGE("[rover-kb] bridgeCls=%p", g_bridgeCls);
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "setKeyboardOesTextureId", "(I)V");
    ALOGE("[rover-kb] method resolved=%p", m);
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)g_kbOesTextureId);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

static bool CallUpdateKbSurfaceTexImage(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return false;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return false;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "updateKbSurfaceTexImage", "()Z");
    if (!m) { env->ExceptionClear(); return false; }
    jboolean r = env->CallStaticBooleanMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return false; }
    return r == JNI_TRUE;
}

static void CallHandleKeyboardHit(struct android_app* androidApp, float u, float v) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "handleKeyboardHit", "(FF)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jfloat)u, (jfloat)v);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

static void CallHandleKeyboardHold(struct android_app* androidApp, float u, float v) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "handleKeyboardHold", "(FF)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jfloat)u, (jfloat)v);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

static int CallPollKbVisRequest(struct android_app* androidApp) {
    // -1=no change, 0=hide, 1=show
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return -1;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return -1;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "pollKbVisRequest", "()I");
    if (!m) { env->ExceptionClear(); return -1; }
    jint r = env->CallStaticIntMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return -1; }
    return (int)r;
}

// v0.8-1a: multi-VD JNI helpers
static std::string CallPollSpawnRequest(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return "";
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return "";
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "pollSpawnRequest", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); return ""; }
    jstring js = (jstring)env->CallStaticObjectMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return ""; }
    if (!js) return "";
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    env->DeleteLocalRef(js);
    return out;
}

static int CallPollCloseRequest(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return -1;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return -1;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "pollCloseRequest", "()I");
    if (!m) { env->ExceptionClear(); return -1; }
    jint r = env->CallStaticIntMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return -1; }
    return (int)r;
}

static void CallOnPanelSpawnedNative(struct android_app* androidApp, int panelIdx, int oesTexId,
                                     const char* pkgActivity, int w, int h) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "onPanelSpawnedNative",
        "(IILjava/lang/String;II)V");
    if (!m) { env->ExceptionClear(); return; }
    jstring js = env->NewStringUTF(pkgActivity);
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)panelIdx, (jint)oesTexId, js, (jint)w, (jint)h);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(js);
}

static void CallOnPanelClosedNative(struct android_app* androidApp, int panelIdx) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "onPanelClosedNative", "(I)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)panelIdx);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

static void CallUpdateAllHostedTexImages(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "updateAllHostedTexImages", "()V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

// v0.8-1b: bar rendering + hover state
static void CallSetBarOesTextureId(struct android_app* androidApp, int panelIdx, int texId,
                                    const char* pkg, int w, int h) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "setBarOesTextureId",
        "(IILjava/lang/String;II)V");
    if (!m) { env->ExceptionClear(); return; }
    jstring jp = env->NewStringUTF(pkg ? pkg : "");
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)panelIdx, (jint)texId, jp, (jint)w, (jint)h);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(jp);
}

static void CallNotifyBarHover(struct android_app* androidApp, int panelIdx, bool hovered) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "notifyBarHover", "(IZ)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)panelIdx, (jboolean)(hovered?1:0));
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

static void CallUpdateAllBarTexImages(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "updateAllBarTexImages", "()V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

// Returns action code: -1=none, 0=drag (default), 1=close, 2=hide, 3=dof, 4=slider
static int CallHandleBarHit(struct android_app* androidApp, int panelIdx, float u, float v) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return -1;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return -1;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "handleBarHit", "(IFF)I");
    if (!m) { env->ExceptionClear(); return -1; }
    jint r = env->CallStaticIntMethod(g_bridgeCls, m, (jint)panelIdx, (jfloat)u, (jfloat)v);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return -1; }
    return (int)r;
}

// Called by native while a bar slider is being dragged: passes new value 0..1
static void CallUpdateBarSlider(struct android_app* androidApp, int panelIdx, float value) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "updateBarSlider", "(IF)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)panelIdx, (jfloat)value);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}



static void CallLaunchAppOnDisplay(struct android_app* androidApp, const char* pkg, const char* act) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    // Read displayId via static field (or getter)
    jfieldID fdId = env->GetStaticFieldID(g_bridgeCls, "virtualDisplayId", "I");
    jint displayId = -1;
    if (fdId) displayId = env->GetStaticIntField(g_bridgeCls, fdId);
    if (displayId < 0) { ALOGE("[rover] launchApp: no display yet"); return; }
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "launchAppOnDisplay",
        "(Ljava/lang/String;Ljava/lang/String;I)Z");
    if (!m) { env->ExceptionClear(); return; }
    jstring jp = env->NewStringUTF(pkg);
    jstring ja = env->NewStringUTF(act);
    env->CallStaticBooleanMethod(g_bridgeCls, m, jp, ja, displayId);
    env->DeleteLocalRef(jp); env->DeleteLocalRef(ja);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    ALOGE("[rover] launchAppOnDisplay(%s/%s, display=%d) called", pkg, act, displayId);
}

static bool IsMediaProjectionGranted(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return false;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return false;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "isMediaProjectionGranted", "()Z");
    if (!m) { env->ExceptionClear(); return false; }
    return env->CallStaticBooleanMethod(g_bridgeCls, m) == JNI_TRUE;
}

// v0.4.2d2: per-frame pump — call RoverBridge.updateSurfaceTexImage() via JNI
// (definition provided via forward decl above)
static jmethodID g_updateTexMethod = nullptr;

static void CacheBridgeClass(struct android_app* androidApp, JNIEnv* env) {
    if (g_bridgeCls != nullptr) return;
    jobject activity = androidApp->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getCl = env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject clsLoader = env->CallObjectMethod(activity, getCl);
    jclass clsLoaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(clsLoaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.gantrping.rover.RoverBridge");
    jclass cls = (jclass)env->CallObjectMethod(clsLoader, loadClass, name);
    env->DeleteLocalRef(name); env->DeleteLocalRef(clsLoaderCls);
    env->DeleteLocalRef(clsLoader); env->DeleteLocalRef(actCls);
    if (!cls) { env->ExceptionClear(); return; }
    g_bridgeCls = (jclass)env->NewGlobalRef(cls);
    env->DeleteLocalRef(cls);
    g_updateTexMethod = env->GetStaticMethodID(g_bridgeCls, "updateSurfaceTexImage", "()Z");
}

static bool CallUpdateSurfaceTexImage(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return false;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls || !g_updateTexMethod) return false;
    jboolean ok = env->CallStaticBooleanMethod(g_bridgeCls, g_updateTexMethod);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return false; }
    return ok == JNI_TRUE;
}

// v0.4.2f: fetch SurfaceTexture transform matrix from Kotlin (column-major 4x4)
static bool CallGetSTMatrix(struct android_app* androidApp, float outMat[16]) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env || !g_bridgeCls) return false;
    jmethodID mid = env->GetStaticMethodID(g_bridgeCls, "getSTMatrix", "()[F");
    if (!mid) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    jfloatArray arr = (jfloatArray)env->CallStaticObjectMethod(g_bridgeCls, mid);
    if (!arr) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    jsize n = env->GetArrayLength(arr);
    if (n < 16) { env->DeleteLocalRef(arr); return false; }
    env->GetFloatArrayRegion(arr, 0, 16, outMat);
    env->DeleteLocalRef(arr);
    return true;
}

// v0.4.3d: pull runtime panel world size from Kotlin (set via broadcast receiver)
static bool CallGetPanelWorld(struct android_app* androidApp, float* outW, float* outH) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return false;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return false;
    jmethodID mW = env->GetStaticMethodID(g_bridgeCls, "getPanelWorldW", "()F");
    jmethodID mH = env->GetStaticMethodID(g_bridgeCls, "getPanelWorldH", "()F");
    if (!mW || !mH) { env->ExceptionClear(); return false; }
    *outW = env->CallStaticFloatMethod(g_bridgeCls, mW);
    *outH = env->CallStaticFloatMethod(g_bridgeCls, mH);
    return true;
}

// v0.4.3e: pull runtime pixels-per-meter density factor from Kotlin
static float CallGetPixelsPerMeter(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return 1000.f;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return 1000.f;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "getPixelsPerMeter", "()F");
    if (!m) { env->ExceptionClear(); return 1000.f; }
    float v = env->CallStaticFloatMethod(g_bridgeCls, m);
    return v > 0.f ? v : 1000.f;
}

// v0.4.3e: pull runtime DPI (currentDpi) from Kotlin
static int CallGetCurrentDpi(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return 200;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return 200;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "getCurrentDpi", "()I");
    if (!m) { env->ExceptionClear(); return 200; }
    return env->CallStaticIntMethod(g_bridgeCls, m);
}

// v0.4.3a: no MP consent — Kotlin creates VD directly via DisplayManager
static void CallEnsureVirtualDisplay(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) { ALOGE("[rover] CallEnsureVirtualDisplay: no env"); return; }
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) { ALOGE("[rover] CallEnsureVirtualDisplay: no bridgeCls"); return; }
    jmethodID mid = env->GetStaticMethodID(g_bridgeCls, "ensureVirtualDisplay", "()V");
    if (mid) env->CallStaticVoidMethod(g_bridgeCls, mid);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

// v0.4.2e: JNI helpers for VirtualDisplay resize + app launch

static void CallResizeVirtualDisplay(struct android_app* androidApp, int w, int h, int dpi) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "resizeVirtualDisplay", "(III)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)w, (jint)h, (jint)dpi);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

// v0.8-fix-reflow: per-panel VD resize
static void CallResizeHostedApp(struct android_app* androidApp, int panelIdx, int w, int h, int dpi) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "resizeHostedApp", "(IIII)V");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_bridgeCls, m, (jint)panelIdx, (jint)w, (jint)h, (jint)dpi);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}


// v0.5.1: input injection helpers — shell out to "input" via su
static int CallGetPanelDisplayId(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return -1;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return -1;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "getPanelDisplayId", "()I");
    if (!m) { env->ExceptionClear(); return -1; }
    jint r = env->CallStaticIntMethod(g_bridgeCls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return -1; }
    return (int)r;
}

// v0.8-1b-fix: per-panel display id lookup
static int CallPanelIdxToDisplayId(struct android_app* androidApp, int panelIdx) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return -1;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return -1;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "panelIdxToDisplayId", "(I)I");
    if (!m) { env->ExceptionClear(); return -1; }
    jint r = env->CallStaticIntMethod(g_bridgeCls, m, (jint)panelIdx);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return -1; }
    return (int)r;
}

// v0.8-1b-fix: per-panel STMatrix lookup
static bool CallGetSTMatrixForPanel(struct android_app* androidApp, int panelIdx, float out[16]) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return false;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return false;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "getStMatrixForPanel", "(I[F)Z");
    if (!m) { env->ExceptionClear(); return false; }
    jfloatArray arr = env->NewFloatArray(16);
    jboolean r = env->CallStaticBooleanMethod(g_bridgeCls, m, (jint)panelIdx, arr);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); env->DeleteLocalRef(arr); return false; }
    if (r) env->GetFloatArrayRegion(arr, 0, 16, out);
    env->DeleteLocalRef(arr);
    return r == JNI_TRUE;
}

static bool CallGetBarSTMatrix(struct android_app* androidApp, int panelIdx, float out[16]) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return false;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return false;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "getBarStMatrix", "(I[F)Z");
    if (!m) { env->ExceptionClear(); return false; }
    jfloatArray arr = env->NewFloatArray(16);
    jboolean r = env->CallStaticBooleanMethod(g_bridgeCls, m, (jint)panelIdx, arr);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); env->DeleteLocalRef(arr); return false; }
    if (r) env->GetFloatArrayRegion(arr, 0, 16, out);
    env->DeleteLocalRef(arr);
    return r == JNI_TRUE;
}

static void CallInjectTap(struct android_app* androidApp, int displayId, int x, int y) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "injectTap", "(III)Z");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticBooleanMethod(g_bridgeCls, m, (jint)displayId, (jint)x, (jint)y);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

static void CallInjectSwipe(struct android_app* androidApp, int displayId, int x1, int y1, int x2, int y2, int durationMs) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    CacheBridgeClass(androidApp, env);
    if (!g_bridgeCls) return;
    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "injectSwipe", "(IIIIII)Z");
    if (!m) { env->ExceptionClear(); return; }
    env->CallStaticBooleanMethod(g_bridgeCls, m, (jint)displayId, (jint)x1, (jint)y1, (jint)x2, (jint)y2, (jint)durationMs);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}


// v0.4.2b: kick off MediaProjection consent dialog once at startup
static void CallRequestMediaProjection(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) return;
    jobject activity = androidApp->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getCl = env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject clsLoader = env->CallObjectMethod(activity, getCl);
    jclass clsLoaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(clsLoaderCls, "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.gantrping.rover.RoverBridge");
    jclass cls = (jclass)env->CallObjectMethod(clsLoader, loadClass, name);
    env->DeleteLocalRef(name); env->DeleteLocalRef(clsLoaderCls);
    env->DeleteLocalRef(clsLoader); env->DeleteLocalRef(actCls);
    if (!cls || env->ExceptionCheck()) {
        ALOGE("CallRequestMediaProjection: loadClass failed");
        env->ExceptionDescribe(); env->ExceptionClear();
        return;
    }
    jmethodID method = env->GetStaticMethodID(cls, "requestMediaProjection", "()V");
    if (!method) { ALOGE("requestMediaProjection method not found"); env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(cls, method);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(cls);
    ALOGE("[rover] requestMediaProjection dispatched");
}

static void CallHelloFromKotlin(struct android_app* androidApp) {
    JavaVM* jvm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (!env) { ALOGE("CallHelloFromKotlin: no JNIEnv"); return; }

    // FindClass on native thread uses bootstrap classloader; use activity ClassLoader instead.
    jobject activity = androidApp->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getCl = env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject clsLoader = env->CallObjectMethod(activity, getCl);
    jclass clsLoaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(clsLoaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring clsName = env->NewStringUTF("com.gantrping.rover.RoverBridge");
    jclass cls = (jclass)env->CallObjectMethod(clsLoader, loadClass, clsName);
    env->DeleteLocalRef(clsName);
    env->DeleteLocalRef(clsLoaderCls);
    env->DeleteLocalRef(clsLoader);
    env->DeleteLocalRef(actCls);
    if (!cls || env->ExceptionCheck()) {
        ALOGE("CallHelloFromKotlin: loadClass failed");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }
    jmethodID method = env->GetStaticMethodID(cls, "helloFromKotlin", "()Ljava/lang/String;");
    if (!method) {
        ALOGE("CallHelloFromKotlin: GetStaticMethodID failed");
        env->ExceptionClear();
        return;
    }
    jstring result = (jstring)env->CallStaticObjectMethod(cls, method);
    if (!result) { ALOGE("CallHelloFromKotlin: null result"); return; }
    const char* utf = env->GetStringUTFChars(result, nullptr);
    ALOGE("[rover] Kotlin says: %s", utf);
    env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
    env->DeleteLocalRef(cls);
}

void android_main(struct android_app* androidApp) {
#else
int main() {
#endif
#if defined(XR_USE_PLATFORM_ANDROID)
    ALOGV("----------------------------------------------------------------");
    ALOGV("android_app_entry()");
    ALOGV("    android_main()");
    CallHelloFromKotlin(androidApp);
    // v0.4.2b3: MediaProjection request deferred (blows up OpenXR lifecycle if called at startup).
    // Wire it to a controller button in v0.4.2c instead.

    JNIEnv* Env;
    (*androidApp->activity->vm).AttachCurrentThread(&Env, nullptr);

    // Note that AttachCurrentThread will reset the thread name.
    prctl(PR_SET_NAME, (long)"OVR::Main", 0, 0, 0);
#endif // defined(XR_USE_PLATFORM_ANDROID)

    App app;
    app.Clear();

#if defined(XR_USE_PLATFORM_ANDROID)
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
    xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
    if (xrInitializeLoaderKHR != NULL) {
        XrLoaderInitInfoAndroidKHR loaderInitializeInfoAndroid = {
            XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInitializeInfoAndroid.applicationVM = androidApp->activity->vm;
        loaderInitializeInfoAndroid.applicationContext = androidApp->activity->clazz;
        xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR*)&loaderInitializeInfoAndroid);
    }
#endif // defined(XR_USE_PLATFORM_ANDROID)

    // Log available layers.
    {
        XrResult result;

        PFN_xrEnumerateApiLayerProperties xrEnumerateApiLayerProperties;
        OXR(result = xrGetInstanceProcAddr(
                XR_NULL_HANDLE,
                "xrEnumerateApiLayerProperties",
                (PFN_xrVoidFunction*)&xrEnumerateApiLayerProperties));
        if (result != XR_SUCCESS) {
            ALOGE("Failed to get xrEnumerateApiLayerProperties function pointer.");
            exit(1);
        }

        uint32_t layerCount = 0;
        OXR(xrEnumerateApiLayerProperties(0, &layerCount, NULL));
        std::vector<XrApiLayerProperties> layerProperties(
            layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
        OXR(xrEnumerateApiLayerProperties(layerCount, &layerCount, layerProperties.data()));

        for (const auto& layer : layerProperties) {
            ALOGV("Found layer %s", layer.layerName);
        }
    }

    // Check that the extensions required are present.
    const char* const requiredExtensionNames[] = {
#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
#elif defined(XR_USE_GRAPHICS_API_OPENGL)
        XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
#endif
#if defined(XR_USE_PLATFORM_ANDROID)
        XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
        XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME,
#endif // defined(XR_USE_PLATFORM_ANDROID)
        XR_FB_PASSTHROUGH_EXTENSION_NAME,
        XR_FB_TRIANGLE_MESH_EXTENSION_NAME};
    const uint32_t numRequiredExtensions =
        sizeof(requiredExtensionNames) / sizeof(requiredExtensionNames[0]);

    // Check the list of required extensions against what is supported by the runtime.
    {
        uint32_t numOutputExtensions = 0;
        OXR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &numOutputExtensions, nullptr));
        ALOGV("xrEnumerateInstanceExtensionProperties found %u extension(s).", numOutputExtensions);

        auto extensionProperties =
            std::vector<XrExtensionProperties>(numOutputExtensions, {XR_TYPE_EXTENSION_PROPERTIES});

        OXR(xrEnumerateInstanceExtensionProperties(
            NULL, numOutputExtensions, &numOutputExtensions, extensionProperties.data()));
        for (uint32_t i = 0; i < numOutputExtensions; i++) {
            ALOGV("Extension #%d = '%s'.", i, extensionProperties[i].extensionName);
        }

        for (uint32_t i = 0; i < numRequiredExtensions; i++) {
            bool found = false;
            for (uint32_t j = 0; j < numOutputExtensions; j++) {
                if (!strcmp(requiredExtensionNames[i], extensionProperties[j].extensionName)) {
                    ALOGV("Found required extension %s", requiredExtensionNames[i]);
                    found = true;
                    break;
                }
            }
            if (!found) {
                ALOGE("Failed to find required extension %s", requiredExtensionNames[i]);
                exit(1);
            }
        }
    }

    // Create the OpenXR instance.
    XrApplicationInfo appInfo = {};
    strcpy(appInfo.applicationName, "XrPassthrough");
    appInfo.applicationVersion = 0;
    strcpy(appInfo.engineName, "Oculus Mobile Sample");
    appInfo.engineVersion = 0;
    appInfo.apiVersion = XR_API_VERSION_1_0;

    XrInstanceCreateInfo instanceCreateInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    instanceCreateInfo.createFlags = 0;
    instanceCreateInfo.applicationInfo = appInfo;
    instanceCreateInfo.enabledApiLayerCount = 0;
    instanceCreateInfo.enabledApiLayerNames = NULL;
    instanceCreateInfo.enabledExtensionCount = numRequiredExtensions;
    instanceCreateInfo.enabledExtensionNames = requiredExtensionNames;

    XrResult initResult;
    OXR(initResult = xrCreateInstance(&instanceCreateInfo, &app.Instance));
    if (initResult != XR_SUCCESS) {
        ALOGE("Failed to create XR app.Instance: %d.", initResult);
        exit(1);
    }
    // Set the global used in macros
    instance = app.Instance;

    XrInstanceProperties instanceInfo = {XR_TYPE_INSTANCE_PROPERTIES};
    OXR(xrGetInstanceProperties(app.Instance, &instanceInfo));
    ALOGV(
        "Runtime %s: Version : %u.%u.%u",
        instanceInfo.runtimeName,
        XR_VERSION_MAJOR(instanceInfo.runtimeVersion),
        XR_VERSION_MINOR(instanceInfo.runtimeVersion),
        XR_VERSION_PATCH(instanceInfo.runtimeVersion));

    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId;
    OXR(initResult = xrGetSystem(app.Instance, &systemGetInfo, &systemId));
    if (initResult != XR_SUCCESS) {
        if (initResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            ALOGE(
                "Failed to get system; the specified form factor is not available. Is your headset connected?");
        } else {
            ALOGE("xrGetSystem failed, error %d", initResult);
        }
        exit(1);
    }

    XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    OXR(xrGetSystemProperties(app.Instance, systemId, &systemProperties));

    ALOGV(
        "System Properties: Name=%s VendorId=%x",
        systemProperties.systemName,
        systemProperties.vendorId);
    ALOGV(
        "System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d",
        systemProperties.graphicsProperties.maxSwapchainImageWidth,
        systemProperties.graphicsProperties.maxSwapchainImageHeight,
        systemProperties.graphicsProperties.maxLayerCount);
    ALOGV(
        "System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
        systemProperties.trackingProperties.orientationTracking ? "True" : "False",
        systemProperties.trackingProperties.positionTracking ? "True" : "False");

    assert(MaxLayerCount <= systemProperties.graphicsProperties.maxLayerCount);

    // Get the graphics requirements.
#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetOpenGLESGraphicsRequirementsKHR = NULL;
    OXR(xrGetInstanceProcAddr(
        app.Instance,
        "xrGetOpenGLESGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)(&pfnGetOpenGLESGraphicsRequirementsKHR)));

    XrGraphicsRequirementsOpenGLESKHR graphicsRequirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    OXR(pfnGetOpenGLESGraphicsRequirementsKHR(app.Instance, systemId, &graphicsRequirements));
#elif defined(XR_USE_GRAPHICS_API_OPENGL)
    PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetOpenGLGraphicsRequirementsKHR = NULL;
    OXR(xrGetInstanceProcAddr(
        app.Instance,
        "xrGetOpenGLGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)(&pfnGetOpenGLGraphicsRequirementsKHR)));

    XrGraphicsRequirementsOpenGLKHR graphicsRequirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    OXR(pfnGetOpenGLGraphicsRequirementsKHR(app.Instance, systemId, &graphicsRequirements));
#endif

    // Create the EGL Context
    app.egl.CreateContext(nullptr);

    // Check the graphics requirements.
    int eglMajor = 0;
    int eglMinor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &eglMajor);
    glGetIntegerv(GL_MINOR_VERSION, &eglMinor);
    const XrVersion eglVersion = XR_MAKE_VERSION(eglMajor, eglMinor, 0);
    if (eglVersion < graphicsRequirements.minApiVersionSupported ||
        eglVersion > graphicsRequirements.maxApiVersionSupported) {
        ALOGE("GLES version %d.%d not supported", eglMajor, eglMinor);
        exit(0);
    }

    app.CpuLevel = CPU_LEVEL;
    app.GpuLevel = GPU_LEVEL;
#if defined(ANDROID)
    app.MainThreadTid = gettid();
#else
    app.MainThreadTid = (int)std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif

    app.SystemId = systemId;

    // FB_passthrough sample begin
    INIT_PFN(xrCreatePassthroughFB);
    INIT_PFN(xrDestroyPassthroughFB);
    INIT_PFN(xrPassthroughStartFB);
    INIT_PFN(xrPassthroughPauseFB);
    INIT_PFN(xrCreatePassthroughLayerFB);
    INIT_PFN(xrDestroyPassthroughLayerFB);
    INIT_PFN(xrPassthroughLayerSetStyleFB);
    INIT_PFN(xrPassthroughLayerPauseFB);
    INIT_PFN(xrPassthroughLayerResumeFB);
    INIT_PFN(xrCreateTriangleMeshFB);
    INIT_PFN(xrDestroyTriangleMeshFB);
    INIT_PFN(xrTriangleMeshGetVertexBufferFB);
    INIT_PFN(xrTriangleMeshGetIndexBufferFB);
    INIT_PFN(xrTriangleMeshBeginUpdateFB);
    INIT_PFN(xrTriangleMeshEndUpdateFB);
    INIT_PFN(xrCreateGeometryInstanceFB);
    INIT_PFN(xrDestroyGeometryInstanceFB);
    INIT_PFN(xrGeometryInstanceSetTransformFB);
    // FB_passthrough sample end

    // Create the OpenXR Session.
#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = app.egl.Display;
    graphicsBinding.config = app.egl.Config;
    graphicsBinding.context = app.egl.Context;
#elif defined(XR_USE_GRAPHICS_API_OPENGL)
    XrGraphicsBindingOpenGLWin32KHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    graphicsBinding.hDC = app.egl.hDC;
    graphicsBinding.hGLRC = app.egl.hGLRC;
#endif

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.createFlags = 0;
    sessionCreateInfo.systemId = app.SystemId;

    OXR(initResult = xrCreateSession(app.Instance, &sessionCreateInfo, &app.Session));
    if (initResult != XR_SUCCESS) {
        ALOGE("Failed to create XR session: %d.", initResult);
        exit(1);
    }

    // App only supports the primary stereo view config.
    const XrViewConfigurationType supportedViewConfigType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    // Enumerate the viewport configurations.
    uint32_t viewportConfigTypeCount = 0;
    OXR(xrEnumerateViewConfigurations(
        app.Instance, app.SystemId, 0, &viewportConfigTypeCount, NULL));

    auto viewportConfigurationTypes = new XrViewConfigurationType[viewportConfigTypeCount];

    OXR(xrEnumerateViewConfigurations(
        app.Instance,
        app.SystemId,
        viewportConfigTypeCount,
        &viewportConfigTypeCount,
        viewportConfigurationTypes));

    ALOGV("Available Viewport Configuration Types: %d", viewportConfigTypeCount);

    for (uint32_t i = 0; i < viewportConfigTypeCount; i++) {
        const XrViewConfigurationType viewportConfigType = viewportConfigurationTypes[i];

        ALOGV(
            "Viewport configuration type %d : %s",
            viewportConfigType,
            viewportConfigType == supportedViewConfigType ? "Selected" : "");

        XrViewConfigurationProperties viewportConfig = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
        OXR(xrGetViewConfigurationProperties(
            app.Instance, app.SystemId, viewportConfigType, &viewportConfig));
        ALOGV(
            "FovMutable=%s ConfigurationType %d",
            viewportConfig.fovMutable ? "true" : "false",
            viewportConfig.viewConfigurationType);

        uint32_t viewCount;
        OXR(xrEnumerateViewConfigurationViews(
            app.Instance, app.SystemId, viewportConfigType, 0, &viewCount, NULL));

        if (viewCount > 0) {
            auto elements = new XrViewConfigurationView[viewCount];

            for (uint32_t e = 0; e < viewCount; e++) {
                elements[e].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
                elements[e].next = NULL;
            }

            OXR(xrEnumerateViewConfigurationViews(
                app.Instance, app.SystemId, viewportConfigType, viewCount, &viewCount, elements));

            // Log the view config info for each view type for debugging purposes.
            for (uint32_t e = 0; e < viewCount; e++) {
                const XrViewConfigurationView* element = &elements[e];
                (void)element;

                ALOGV(
                    "Viewport [%d]: Recommended Width=%d Height=%d SampleCount=%d",
                    e,
                    element->recommendedImageRectWidth,
                    element->recommendedImageRectHeight,
                    element->recommendedSwapchainSampleCount);

                ALOGV(
                    "Viewport [%d]: Max Width=%d Height=%d SampleCount=%d",
                    e,
                    element->maxImageRectWidth,
                    element->maxImageRectHeight,
                    element->maxSwapchainSampleCount);
            }

            // Cache the view config properties for the selected config type.
            if (viewportConfigType == supportedViewConfigType) {
                assert(viewCount == NUM_EYES);
                for (uint32_t e = 0; e < viewCount; e++) {
                    app.ViewConfigurationView[e] = elements[e];
                }
            }

            delete[] elements;
        } else {
            ALOGE("Empty viewport configuration type: %d", viewCount);
        }
    }

    delete[] viewportConfigurationTypes;

    // Get the viewport configuration info for the chosen viewport configuration type.
    app.ViewportConfig.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;

    OXR(xrGetViewConfigurationProperties(
        app.Instance, app.SystemId, supportedViewConfigType, &app.ViewportConfig));

    bool stageSupported = false;

    uint32_t numOutputSpaces = 0;
    OXR(xrEnumerateReferenceSpaces(app.Session, 0, &numOutputSpaces, NULL));

    auto referenceSpaces = new XrReferenceSpaceType[numOutputSpaces];

    OXR(xrEnumerateReferenceSpaces(
        app.Session, numOutputSpaces, &numOutputSpaces, referenceSpaces));

    for (uint32_t i = 0; i < numOutputSpaces; i++) {
        if (referenceSpaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE) {
            stageSupported = true;
            break;
        }
    }

    delete[] referenceSpaces;

    // Create a space to the first path
    XrReferenceSpaceCreateInfo spaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceCreateInfo.poseInReferenceSpace.orientation.w = 1.0f;
    OXR(xrCreateReferenceSpace(app.Session, &spaceCreateInfo, &app.HeadSpace));

    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    OXR(xrCreateReferenceSpace(app.Session, &spaceCreateInfo, &app.LocalSpace));

    if (stageSupported) {
        spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        spaceCreateInfo.poseInReferenceSpace.position.y = 0.0f;
        OXR(xrCreateReferenceSpace(app.Session, &spaceCreateInfo, &app.StageSpace));
        ALOGV("Created stage space");
    }

    XrView projections[NUM_EYES];
    for (int eye = 0; eye < NUM_EYES; eye++) {
        projections[eye] = XrView{XR_TYPE_VIEW};
    }

    GLenum format = GL_SRGB8_ALPHA8;
    int width = app.ViewConfigurationView[0].recommendedImageRectWidth;
    int height = app.ViewConfigurationView[0].recommendedImageRectHeight;

    XrSwapchainCreateInfo swapChainCreateInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapChainCreateInfo.usageFlags =
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapChainCreateInfo.format = format;
    swapChainCreateInfo.sampleCount = 1;
    swapChainCreateInfo.width = width;
    swapChainCreateInfo.height = height;
    swapChainCreateInfo.faceCount = 1;
    swapChainCreateInfo.arraySize = 2;
    swapChainCreateInfo.mipCount = 1;

    // Create the swapchain.
    OXR(xrCreateSwapchain(app.Session, &swapChainCreateInfo, &app.ColorSwapChain));
    OXR(xrEnumerateSwapchainImages(app.ColorSwapChain, 0, &app.SwapChainLength, nullptr));
#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
    auto images = new XrSwapchainImageOpenGLESKHR[app.SwapChainLength];
    // Populate the swapchain image array.
    for (uint32_t i = 0; i < app.SwapChainLength; i++) {
        images[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    }
#elif defined(XR_USE_GRAPHICS_API_OPENGL)
    auto images = new XrSwapchainImageOpenGLKHR[app.SwapChainLength];
    // Populate the swapchain image array.
    for (uint32_t i = 0; i < app.SwapChainLength; i++) {
        images[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR};
    }
#endif

    OXR(xrEnumerateSwapchainImages(
        app.ColorSwapChain,
        app.SwapChainLength,
        &app.SwapChainLength,
        (XrSwapchainImageBaseHeader*)images));

    auto colorTextures = new GLuint[app.SwapChainLength];
    for (uint32_t i = 0; i < app.SwapChainLength; i++) {
        colorTextures[i] = GLuint(images[i].image);
    }

    app.appRenderer.Create(
        format, width, height, NUM_MULTI_SAMPLES, app.SwapChainLength, colorTextures);

    delete[] images;
    delete[] colorTextures;

    AppInput_init(app);
    // v0.2: PanelManager owns per-panel swapchains, replaces the v0.1 hardcoded QuadSwapChain
    static rover::PanelManager panelMgr;
    panelMgr.Init(app.Session, app.HeadSpace, app.LocalSpace);
    static rover::RayCursor cursor;      // right
    cursor.Init(app.Session);
    static rover::RayLine rayLine;       // right
    rayLine.Init(app.Session);
    static rover::RayCursor leftCursor;
    leftCursor.Init(app.Session);
    static rover::RayLine leftRayLine;
    leftRayLine.Init(app.Session);
    // v0.4.2d1: create OES texture; v0.4.3a: also kick off VirtualDisplay creation
    // v0.8-fix: primary OES tex not needed (no default panel)
    CallSetupKeyboardOesTexture(androidApp);
    // v0.8-fix: primary VD not needed (no default panel)
    static rover::OesBlitter oesBlitter;
    oesBlitter.Init();
    {
        // v0.8-fix: no default panel — rover starts empty, user spawns apps via Right-B/Left-X (later: launcher)

        // Left panel: 6DoF (world-anchored). Green. 40x30 cm at 1.5 m ahead + 0.6 m left.
        XrPosef pose1 = {{0,0,0,1}, {-0.6f, 0.0f, -1.5f}};
        // v0.8-fixes-2: dropped side placeholder panel (layer budget)
        // panelMgr.AddPanel(rover::DofMode::WorldAnchored, pose1, {0.40f, 0.30f}, 0.20f, 0.55f, 0.25f, 0.85f);

        // Right panel: 3DoF (yaw-locked, rotates with head yaw but stays put on pitch/roll/translation-drift).
        // Red. Pose is offset from head-yaw origin: 0.6 m right, 1.5 m ahead.
        XrPosef pose2 = {{0,0,0,1}, {0.6f, 0.0f, -1.5f}};
        // v0.8-fixes-2: dropped side placeholder panel (layer budget)
        // panelMgr.AddPanel(rover::DofMode::BodyLocked, pose2, {0.40f, 0.30f}, 0.65f, 0.25f, 0.25f, 0.85f);

        // v0.7: keyboard panel — head-locked below main content. 1.2m x 0.5m at 1.5m ahead.
        XrPosef pose3 = {{0,0,0,1}, {0.0f, -0.55f, -1.5f}};
        int kbIdx = panelMgr.AddPanel(rover::DofMode::HeadLocked, pose3, {1.2f, 0.5f}, 0.10f, 0.10f, 0.10f, 0.95f, 1200, 500);
        panelMgr.PanelAt(kbIdx).oesSourced = true;
        panelMgr.PanelAt(kbIdx).oesTextureId = g_kbOesTextureId;
        panelMgr.PanelAt(kbIdx).isKeyboard = true;
        panelMgr.PanelAt(kbIdx).visible = false;         // hidden until Right-A press
        panelMgr.PanelAt(kbIdx).oesForceOpaque = false;  // v0.7.2: preserve alpha so key-gap bg is transparent
        g_kbPanelIdx = kbIdx;
        {
            // v0.8-fixes: tell Kotlin the keyboard's panelIdx so it can return its STMatrix
            JavaVM* jvm = androidApp->activity->vm; JNIEnv* env = nullptr;
            jvm->AttachCurrentThread(&env, nullptr);
            if (env) {
                CacheBridgeClass(androidApp, env);
                if (g_bridgeCls) {
                    jmethodID m = env->GetStaticMethodID(g_bridgeCls, "setKeyboardPanelIdx", "(I)V");
                    if (m) {
                        env->CallStaticVoidMethod(g_bridgeCls, m, (jint)kbIdx);
                        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                    } else env->ExceptionClear();
                }
            }
        }
    }


    // FB_passthrough sample begin
    // Create passthrough objects
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
    XrPassthroughLayerFB reconPassthroughLayer = XR_NULL_HANDLE;
    XrPassthroughLayerFB geomPassthroughLayer = XR_NULL_HANDLE;
    XrGeometryInstanceFB geomInstance = XR_NULL_HANDLE;
    {
        XrPassthroughCreateInfoFB ptci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        XrResult result;
        OXR(result = xrCreatePassthroughFB(app.Session, &ptci, &passthrough));

        if (XR_SUCCEEDED(result)) {
            XrPassthroughLayerCreateInfoFB plci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            plci.passthrough = passthrough;
            plci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            OXR(xrCreatePassthroughLayerFB(app.Session, &plci, &reconPassthroughLayer));
        }

        if (XR_SUCCEEDED(result)) {
            XrPassthroughLayerCreateInfoFB plci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            plci.passthrough = passthrough;
            plci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB;
            OXR(xrCreatePassthroughLayerFB(app.Session, &plci, &geomPassthroughLayer));

            const XrVector3f verts[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
            const uint32_t indexes[] = {0, 1, 2, 2, 1, 3};
            XrTriangleMeshCreateInfoFB tmci = {XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB};
            tmci.vertexCount = 4;
            tmci.vertexBuffer = &verts[0];
            tmci.triangleCount = 2;
            tmci.indexBuffer = &indexes[0];

            XrTriangleMeshFB mesh = XR_NULL_HANDLE;
            OXR(xrCreateTriangleMeshFB(app.Session, &tmci, &mesh));

            XrGeometryInstanceCreateInfoFB gici = {XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB};
            gici.layer = geomPassthroughLayer;
            gici.mesh = mesh;
            gici.baseSpace = app.LocalSpace;
            gici.pose.orientation.w = 1.0f;
            gici.scale = {1.0f, 1.0f, 1.0f};
            OXR(xrCreateGeometryInstanceFB(app.Session, &gici, &geomInstance));
        }
    }
    // FB_passthrough sample end

#if defined(XR_USE_PLATFORM_ANDROID)
    androidApp->userData = &app;
    androidApp->onAppCmd = app_handle_cmd;
#endif // defined(XR_USE_PLATFORM_ANDROID)

    bool stageBoundsDirty = true;

    int frameCount = -1;
    int framesCyclePaused = 0;
    bool cyclePaused = false;

    constexpr int framesPerMode = 400;

    enum Mode {
        Mode_Passthrough_Basic = 0,
        Mode_Passthrough_DynamicRamp = 1,
        Mode_Passthrough_GreenRampYellowEdges = 2,
        Mode_Passthrough_Masked = 3,
        Mode_Passthrough_ProjQuad = 4,
        Mode_Passthrough_Stopped = 5,
        Mode_NumModes = 6
    };

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.2f};

#if defined(XR_USE_PLATFORM_ANDROID)
    while (androidApp->destroyRequested == 0)
#else
    while (true)
#endif
    {
        frameCount++;

#if defined(XR_USE_PLATFORM_ANDROID)
        // Read all pending events.
        for (;;) {
            int events;
            struct android_poll_source* source;
            // If the timeout is zero, returns immediately without blocking.
            // If the timeout is negative, waits indefinitely until an event appears.
            const int timeoutMilliseconds = (app.Resumed == false && app.SessionActive == false &&
                                             androidApp->destroyRequested == 0)
                ? -1
                : 0;
            if (ALooper_pollOnce(timeoutMilliseconds, NULL, &events, (void**)&source) < 0) {
                break;
            }

            // Process this event.
            if (source != NULL) {
                source->process(androidApp, source);
            }
        }
#elif defined(XR_USE_PLATFORM_WIN32)
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) > 0) {
            if (msg.message == WM_QUIT) {
                app.ShouldExit = true;
            } else {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
            }
        }
#endif // defined(XR_USE_PLATFORM_ANDROID)

        app.HandleXrEvents();

        if (app.ShouldExit) {
            break;
        }

        if (app.SessionActive == false) {
            frameCount = -1;
            framesCyclePaused = 0;
            continue;
        }

        AppInput_syncActions(app);
        if (boolState.type != 0 && boolState.changedSinceLastSync == XR_TRUE &&
            boolState.currentState != XR_FALSE) {
            cyclePaused = !cyclePaused;
        }

        if (cyclePaused) {
            clearColor[0] = 0.3f;
            framesCyclePaused++;
        } else {
            clearColor[0] = 0.0f;
        }
        app.appRenderer.scene.SetClearColor(clearColor);

        // Create the scene if not yet created.
        // The scene is created here to be able to show a loading icon.
        if (!app.appRenderer.scene.IsCreated()) {
            // Create the scene.
            app.appRenderer.scene.Create();
        }

        if (stageBoundsDirty) {
            UpdateStageBounds(app);
            stageBoundsDirty = false;
        }

        // NOTE: OpenXR does not use the concept of frame indices. Instead,
        // XrWaitFrame returns the predicted display time.
        XrFrameWaitInfo waitFrameInfo = {XR_TYPE_FRAME_WAIT_INFO};

        XrFrameState frameState = {XR_TYPE_FRAME_STATE};

        OXR(xrWaitFrame(app.Session, &waitFrameInfo, &frameState));

        // Get the HMD pose, predicted for the middle of the time period during which
        // the new eye images will be displayed. The number of frames predicted ahead
        // depends on the pipeline depth of the engine and the synthesis rate.
        // The better the prediction, the less black will be pulled in at the edges.
        XrFrameBeginInfo beginFrameDesc = {XR_TYPE_FRAME_BEGIN_INFO};
        OXR(xrBeginFrame(app.Session, &beginFrameDesc));

        XrPosef xfLocalFromHead;
        {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            OXR(xrLocateSpace(
                app.HeadSpace, app.LocalSpace, frameState.predictedDisplayTime, &loc));
            xfLocalFromHead = loc.pose;
        }

        XrViewState viewState = {XR_TYPE_VIEW_STATE};

        XrViewLocateInfo projectionInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        projectionInfo.viewConfigurationType = app.ViewportConfig.viewConfigurationType;
        projectionInfo.displayTime = frameState.predictedDisplayTime;
        projectionInfo.space = app.HeadSpace;

        uint32_t projectionCapacityInput = NUM_EYES;
        uint32_t projectionCountOutput = projectionCapacityInput;

        OXR(xrLocateViews(
            app.Session,
            &projectionInfo,
            &viewState,
            projectionCapacityInput,
            &projectionCountOutput,
            projections));

        // update input information
        XrSpace controllerSpace[] = {
            leftControllerAimSpace,
            leftControllerGripSpace,
            rightControllerAimSpace,
            rightControllerGripSpace,
        };

        bool controllerActive[] = {leftControllerActive, rightControllerActive};
        for (int i = 0; i < 4; i++) {
            if (controllerActive[i >> 1]) {
                XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
                OXR(xrLocateSpace(
                    controllerSpace[i], app.LocalSpace, frameState.predictedDisplayTime, &loc));
                app.appRenderer.scene.trackedController[i].Active =
                    (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                app.appRenderer.scene.trackedController[i].Pose = OvrFromXr(loc.pose);
            } else {
                app.appRenderer.scene.trackedController[i].Clear();
            }
        }

        // v0.3.15: passthrough locked to Basic (no style cycling)
        static bool passthroughInitDone = false;
        const Mode prevMode = passthroughInitDone ? Mode_Passthrough_Basic : Mode_Passthrough_Stopped;
        const Mode mode = Mode_Passthrough_Basic;
        passthroughInitDone = true;

        if (mode != prevMode) {
            // Unset any sticky state from the previous mode
            switch (prevMode) {
                case Mode_Passthrough_Basic:
                    OXR(xrPassthroughLayerPauseFB(passthroughLayer));
                    break;
                case Mode_Passthrough_DynamicRamp:
                    OXR(xrPassthroughLayerPauseFB(passthroughLayer));
                    break;
                case Mode_Passthrough_GreenRampYellowEdges:
                    OXR(xrPassthroughLayerPauseFB(passthroughLayer));
                    break;
                case Mode_Passthrough_Masked:
                    OXR(xrPassthroughLayerPauseFB(passthroughLayer));
                    clearColor[3] = 0.2f;
                    break;
                case Mode_Passthrough_ProjQuad:
                    OXR(xrPassthroughLayerPauseFB(passthroughLayer));
                    break;
                case Mode_Passthrough_Stopped:
                    OXR(xrPassthroughStartFB(passthrough));
                    break;
                default:
                    break;
            }
            XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
            switch (mode) {
                case Mode_Passthrough_Basic:
                    passthroughLayer = reconPassthroughLayer;
                    OXR(xrPassthroughLayerResumeFB(passthroughLayer));
                    style.textureOpacityFactor = 1.0f;
                    style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
                    OXR(xrPassthroughLayerSetStyleFB(passthroughLayer, &style));
                    break;
                case Mode_Passthrough_DynamicRamp:
                    passthroughLayer = reconPassthroughLayer;
                    OXR(xrPassthroughLayerResumeFB(passthroughLayer));
                    style.textureOpacityFactor = 0.5f;
                    style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
                    OXR(xrPassthroughLayerSetStyleFB(passthroughLayer, &style));
                    break;
                case Mode_Passthrough_GreenRampYellowEdges: {
                    passthroughLayer = reconPassthroughLayer;
                    OXR(xrPassthroughLayerResumeFB(passthroughLayer));
                    // Create a color map which maps each input value to a green ramp
                    XrPassthroughColorMapMonoToRgbaFB colorMap = {
                        XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB};
                    for (int i = 0; i < XR_PASSTHROUGH_COLOR_MAP_MONO_SIZE_FB; ++i) {
                        float colorValue = i / 255.0f;
                        colorMap.textureColorMap[i] = {0.0f, colorValue, 0.0f, 1.0f};
                    }
                    style.textureOpacityFactor = 0.5f;
                    style.edgeColor = {1.0f, 1.0f, 0.0f, 0.5f};
                    style.next = &colorMap;
                    OXR(xrPassthroughLayerSetStyleFB(passthroughLayer, &style));
                } break;
                case Mode_Passthrough_Masked:
                    passthroughLayer = reconPassthroughLayer;
                    OXR(xrPassthroughLayerResumeFB(passthroughLayer));
                    clearColor[3] = 1.0f;
                    style.textureOpacityFactor = 0.5f;
                    style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
                    OXR(xrPassthroughLayerSetStyleFB(passthroughLayer, &style));
                    break;
                case Mode_Passthrough_ProjQuad:
                    passthroughLayer = geomPassthroughLayer;
                    OXR(xrPassthroughLayerResumeFB(passthroughLayer));
                    break;
                case Mode_Passthrough_Stopped:
                    OXR(xrPassthroughPauseFB(passthrough));
                    break;
                default:
                    break;
            }
        }
        // per frame style adjustments for DynamicRamp
        if (mode == Mode_Passthrough_DynamicRamp) {
            const float frac =
                ((frameCount - framesCyclePaused) % framesPerMode) / (framesPerMode - 1.0f);
            // phase shifting color map
            XrPassthroughColorMapMonoToRgbaFB colorMap = {
                XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB};
            for (int i = 0; i < XR_PASSTHROUGH_COLOR_MAP_MONO_SIZE_FB; ++i) {
                float v = i / 64.0f;
                v -= floor(v) - 0.5f;
                v = fabsf(v);
                v = v * v;
                int idx = (i + int(4 * frac * XR_PASSTHROUGH_COLOR_MAP_MONO_SIZE_FB)) %
                    XR_PASSTHROUGH_COLOR_MAP_MONO_SIZE_FB;
                colorMap.textureColorMap[idx] = {v, v, v, 1.0f};
            }
            XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
            style.textureOpacityFactor = 0.5f;
            style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
            style.next = &colorMap;
            OXR(xrPassthroughLayerSetStyleFB(passthroughLayer, &style));
        } else if (mode == Mode_Passthrough_ProjQuad && leftControllerGripSpace != XR_NULL_HANDLE) {
            XrGeometryInstanceTransformFB git = {XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB};
            git.baseSpace = leftControllerGripSpace;
            git.time = frameState.predictedDisplayTime;
            // Approximate ring orientation relative to grip
            Quatf rx(Vector3f(1, 0, 0), DegreeToRad(-20.0f));
            Quatf ry(Vector3f(0, 1, 0), DegreeToRad(-11.0f));
            Quatf rz(Vector3f(0, 0, 1), DegreeToRad(0.0f));
            Quatf r = rz * ry * rx;
            git.pose.orientation = {r.x, r.y, r.z, r.w};
            git.pose.position = {0.05f, -0.15f, -0.05f};
            git.scale = {0.6f, 0.3f, 1.0f};
            OXR(xrGeometryInstanceSetTransformFB(geomInstance, &git));
        }
        // FB_passthrough sample end

        AppRenderer::FrameIn frameIn;
        uint32_t chainIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL};
        OXR(xrAcquireSwapchainImage(app.ColorSwapChain, &acquireInfo, &chainIndex));
        frameIn.SwapChainIndex = int(chainIndex);

        XrPosef xfLocalFromEye[NUM_EYES];

        for (int eye = 0; eye < NUM_EYES; eye++) {
            XrPosef xfHeadFromEye = projections[eye].pose;
            XrPosef_Multiply(&xfLocalFromEye[eye], &xfLocalFromHead, &xfHeadFromEye);

            XrPosef xfEyeFromLocal;
            XrPosef_Invert(&xfEyeFromLocal, &xfLocalFromEye[eye]);

            XrMatrix4x4f viewMat{};
            XrMatrix4x4f_CreateFromRigidTransform(&viewMat, &xfEyeFromLocal);

            const XrFovf fov = projections[eye].fov;
            XrMatrix4x4f projMat;
            XrMatrix4x4f_CreateProjectionFov(&projMat, GRAPHICS_OPENGL_ES, fov, 0.1f, 0.0f);

            frameIn.View[eye] = OvrFromXr(viewMat);
            frameIn.Proj[eye] = OvrFromXr(projMat);
        }

        if (app.StageSpace != XR_NULL_HANDLE) {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            OXR(xrLocateSpace(
                app.StageSpace, app.LocalSpace, frameState.predictedDisplayTime, &loc));
            XrPosef xfLocalFromStage = loc.pose;

            frameIn.HasStage = true;
            frameIn.StagePose = OvrFromXr(xfLocalFromStage);
            frameIn.StageScale = app.StageBounds;
        } else {
            frameIn.HasStage = false;
        }

        XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = 1000000000; /* timeout in nanoseconds */
        XrResult res = xrWaitSwapchainImage(app.ColorSwapChain, &waitInfo);
        int retry = 0;
        while (res == XR_TIMEOUT_EXPIRED) {
            res = xrWaitSwapchainImage(app.ColorSwapChain, &waitInfo);
            retry++;
            ALOGV(
                " Retry xrWaitSwapchainImage %d times due to XR_TIMEOUT_EXPIRED (duration %f seconds)",
                retry,
                waitInfo.timeout * (1E-9));
        }

        app.appRenderer.RenderFrame(frameIn);

        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL};
        OXR(xrReleaseSwapchainImage(app.ColorSwapChain, &releaseInfo));

        // Set-up the compositor layers for this frame.
        // NOTE: Multiple independent layers are allowed, but they need to be added
        // in a depth consistent order.

        XrCompositionLayerProjectionView proj_views[2] = {};

        app.LayerCount = 0;
        memset(app.Layers, 0, sizeof(CompositionLayerUnion) * MaxLayerCount);

        // FB_passthrough sample begin
        // passthrough layer is backmost layer (if available)
        if (passthroughLayer != XR_NULL_HANDLE) {
            XrCompositionLayerPassthroughFB passthrough_layer = {
                XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
            passthrough_layer.layerHandle = passthroughLayer;
            passthrough_layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            passthrough_layer.space = XR_NULL_HANDLE;
            app.Layers[app.LayerCount++].Passthrough = passthrough_layer;
        }
        // FB_passthrough sample end

        XrCompositionLayerProjection proj_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        proj_layer.layerFlags |= XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
        proj_layer.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        proj_layer.space = app.LocalSpace;
        proj_layer.viewCount = NUM_EYES;
        proj_layer.views = proj_views;

        for (int eye = 0; eye < NUM_EYES; eye++) {
            XrCompositionLayerProjectionView& proj_view = proj_views[eye];
            proj_view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            proj_view.pose = xfLocalFromEye[eye];
            proj_view.fov = projections[eye].fov;

            proj_view.subImage.swapchain = app.ColorSwapChain;
            proj_view.subImage.imageRect.offset.x = 0;
            proj_view.subImage.imageRect.offset.y = 0;
            proj_view.subImage.imageRect.extent.width = width;
            proj_view.subImage.imageRect.extent.height = height;
            proj_view.subImage.imageArrayIndex = eye;
        }

        // v0.2: Meta sample projection layer stripped — we render only panels + passthrough now.
        // app.Layers[app.LayerCount++].Projection = proj_layer;
        // v0.2: head pose in local space (used for BodyLocked/YawLocked)
        XrPosef headInLocal = {{0,0,0,1}, {0,0,0}};
        {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (xrLocateSpace(app.HeadSpace, app.LocalSpace, frameState.predictedDisplayTime, &loc) == XR_SUCCESS) {
                if ((loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    headInLocal = loc.pose;
                }
            }
        }

        // v0.3.13: both controllers, per-hand triggers, thumbstick for distance
        static int grabbedIdx = -1;
        static int grabbedHand = 0;
        static float grabDist = 1.5f;
        static XrVector3f grabOffsetWorld = {0,0,0};
        static XrQuaternionf grabOrient = {0,0,0,1};
        static int tapHand = 0;         // 0=none, 1=left, 2=right — currently tracking a tap
        static int tapPanelIdx = -1;
        static float tapStartU = 0.f, tapStartV = 0.f;  // UV (0..1, 0=top-left) at tap start
        static long tapStartTimeMs = 0;
        // v0.7.1: keyboard hold-to-repeat (backspace, arrows)
        static int kbHeldHand = 0;      // 0=none, 1=left, 2=right
        static float kbHeldU = 0.f, kbHeldV = 0.f;
        static long kbHeldStartMs = 0;
        static long kbHeldNextFireMs = 0;
        static int resizedIdx = -1;
        static int ri_ctr = 0;
        if ((ri_ctr++ % 30) == 0) ALOGE("[rover-dbg] tick resizedIdx=%d grabbedIdx=%d", resizedIdx, grabbedIdx);
        static int resizedHand = 0;
        static float resizeInitDist = 1.0f;
        static XrExtent2Df resizeInitSize = {1.024f, 0.640f};
        static float resizeInitGrabU = 0.f, resizeInitGrabV = 0.f;
        static int resizedCorner = -1;
        static bool prevLeftTrigger = false;
        static bool prevRightTrigger = false;
        static bool prevCaptureBtn = false;
        static int g_prevResizedIdx = -1;
        {
            bool cur = (rightBButtonState.type != 0 && rightBButtonState.currentState != XR_FALSE);
            if (cur && !prevCaptureBtn) {
                ALOGE("[rover] Right B pressed — request spawn Termux (multi-VD)");
                // v0.8-1a: use spawn API to create a NEW hosted window each press
                JavaVM* jvm = androidApp->activity->vm; JNIEnv* env = nullptr;
                jvm->AttachCurrentThread(&env, nullptr);
                if (env) {
                    CacheBridgeClass(androidApp, env);
                    if (g_bridgeCls) {
                        jmethodID m = env->GetStaticMethodID(g_bridgeCls, "requestSpawn",
                            "(Ljava/lang/String;Ljava/lang/String;)V");
                        if (m) {
                            jstring jp = env->NewStringUTF("com.termux");
                            jstring ja = env->NewStringUTF("com.termux.app.TermuxActivity");
                            env->CallStaticVoidMethod(g_bridgeCls, m, jp, ja);
                            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                            env->DeleteLocalRef(jp); env->DeleteLocalRef(ja);
                        } else { env->ExceptionClear(); }
                    }
                }
            }
            prevCaptureBtn = cur;
        }
        {
            static bool prevLeftX = false;
            bool cur = (leftXButtonState.type != 0 && leftXButtonState.currentState != XR_FALSE);
            if (cur && !prevLeftX) {
                ALOGE("[rover] Left X pressed — request spawn Telegram (multi-VD)");
                JavaVM* jvm = androidApp->activity->vm; JNIEnv* env = nullptr;
                jvm->AttachCurrentThread(&env, nullptr);
                if (env) {
                    CacheBridgeClass(androidApp, env);
                    if (g_bridgeCls) {
                        jmethodID m = env->GetStaticMethodID(g_bridgeCls, "requestSpawn",
                            "(Ljava/lang/String;Ljava/lang/String;)V");
                        if (m) {
                            jstring jp = env->NewStringUTF("org.telegram.messenger.web");
                            jstring ja = env->NewStringUTF("org.telegram.ui.LaunchActivity");
                            env->CallStaticVoidMethod(g_bridgeCls, m, jp, ja);
                            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                            env->DeleteLocalRef(jp); env->DeleteLocalRef(ja);
                        } else { env->ExceptionClear(); }
                    }
                }
            }
            prevLeftX = cur;
        }
        {
            // v0.7.2/6: Right-A toggles keyboard panel visibility
            static bool prevRightA = false;
            bool cur = (rightAButtonState.type != 0 && rightAButtonState.currentState != XR_FALSE);
            if (cur && !prevRightA && g_kbPanelIdx >= 0) {
                bool now = !panelMgr.PanelAt(g_kbPanelIdx).visible;
                panelMgr.PanelAt(g_kbPanelIdx).visible = now;
                ALOGE("[rover-kb] Right-A toggle visible=%d", now?1:0);
            }
            prevRightA = cur;
        }
        {
            // v0.7.2: Kotlin can request show/hide (close X, future focus detection)
            int req = CallPollKbVisRequest(androidApp);
            if (req >= 0 && g_kbPanelIdx >= 0) {
                panelMgr.PanelAt(g_kbPanelIdx).visible = (req == 1);
                ALOGE("[rover-kb] Kotlin req=%d -> visible=%d", req, req==1?1:0);
            }
        }
        {
            // v0.8-1a: process pending spawn — create OES tex, add panel, notify Kotlin
            std::string pa = CallPollSpawnRequest(androidApp);
            if (!pa.empty()) {
                GLuint newTex = 0;
                glGenTextures(1, &newTex);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, newTex);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

                // Placement: head-relative offset -> world pose captured at spawn time,
                // so BodyLocked panels keep their orientation once placed.
                static float g_spawnCursorX = -0.6f;
                g_spawnCursorX += 0.8f;
                auto qrotV = [](const XrQuaternionf& q, XrVector3f v) {
                    float x=q.x,y=q.y,z=q.z,w=q.w;
                    float ix =  w*v.x + y*v.z - z*v.y;
                    float iy =  w*v.y + z*v.x - x*v.z;
                    float iz =  w*v.z + x*v.y - y*v.x;
                    float iw = -x*v.x - y*v.y - z*v.z;
                    return XrVector3f{
                        ix*w + iw*-x + iy*-z - iz*-y,
                        iy*w + iw*-y + iz*-x - ix*-z,
                        iz*w + iw*-z + ix*-y - iy*-x
                    };
                };
                XrVector3f headRelOffset = {g_spawnCursorX, 0.0f, -1.5f};
                XrVector3f worldOffset = qrotV(headInLocal.orientation, headRelOffset);
                XrPosef spawnPose = {headInLocal.orientation, worldOffset};
                int w = 900, h = 600;
                int newIdx = panelMgr.AddPanel(rover::DofMode::BodyLocked, spawnPose,
                    {0.9f, 0.6f}, 0.10f, 0.10f, 0.10f, 1.0f, w, h);
                panelMgr.PanelAt(newIdx).oesSourced = true;
                panelMgr.PanelAt(newIdx).oesTextureId = newTex;
                panelMgr.PanelAt(newIdx).oesForceOpaque = true;
                ALOGE("[rover-spawn] panel=%d oesTex=%u pkgAct=%s", newIdx, newTex, pa.c_str());
                CallOnPanelSpawnedNative(androidApp, newIdx, (int)newTex, pa.c_str(), w, h);
                // v0.8-1b: also create bar OES tex for this panel
                GLuint barTex = 0;
                glGenTextures(1, &barTex);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, barTex);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
                panelMgr.PanelAt(newIdx).barOesTexId = barTex;
                // extract pkg from "pkg|activity"
                std::string pkgOnly = pa.substr(0, pa.find('|'));
                CallSetBarOesTextureId(androidApp, newIdx, (int)barTex, pkgOnly.c_str(), 1024, 64);
            }
            int closeIdx = CallPollCloseRequest(androidApp);
            if (closeIdx >= 0) {
                // For now: just mark invisible + notify Kotlin to release resources.
                // (True panel removal from panelMgr is TODO — would shift indices.)
                if (closeIdx < (int)panelMgr.Panels().size()) {
                    panelMgr.PanelAt(closeIdx).visible = false;
                }
                CallOnPanelClosedNative(androidApp, closeIdx);
                ALOGE("[rover-close] panel=%d hidden + Kotlin notified", closeIdx);
            }
        }

        auto locateCtrl = [&](XrSpace space, bool active, XrPosef* outPose, bool* outValid) {
            *outValid = false;
            if (!active) return;
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (xrLocateSpace(space, app.LocalSpace, frameState.predictedDisplayTime, &loc) == XR_SUCCESS) {
                if ((loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    *outPose = loc.pose;
                    *outValid = true;
                }
            }
        };
        XrPosef leftCtrl = {{0,0,0,1}, {0,0,0}}, rightCtrl = {{0,0,0,1}, {0,0,0}};
        bool leftCtrlValid = false, rightCtrlValid = false;
        locateCtrl(leftControllerAimSpace, leftControllerActive, &leftCtrl, &leftCtrlValid);
        locateCtrl(rightControllerAimSpace, rightControllerActive, &rightCtrl, &rightCtrlValid);

        bool leftTrigger = (leftTriggerState.type != 0 && leftTriggerState.currentState != XR_FALSE);
        bool rightTrigger = (rightTriggerState.type != 0 && rightTriggerState.currentState != XR_FALSE);
        bool triggerDown = leftTrigger || rightTrigger;

        auto computeRayDir = [](const XrQuaternionf& q) {
            float xx=q.x*q.x, yy=q.y*q.y;
            float wx=q.w*q.x, wy=q.w*q.y;
            float xz=q.x*q.z, yz=q.y*q.z;
            XrVector3f r;
            r.x = -(2*xz + 2*wy);
            r.y = -(2*yz - 2*wx);
            r.z = -(1 - 2*xx - 2*yy);
            return r;
        };
        rover::HitResult rightHit = {-1, false, -1, 0}, leftHit = {-1, false, -1, 0};
        // v0.8-fix-dof: initialize BodyLocked panels' pose from current head on first frame
        {
            auto qmul = [](const XrQuaternionf& a, const XrQuaternionf& b) {
                return XrQuaternionf{
                    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
                };
            };
            auto qrotVec = [](const XrQuaternionf& q, XrVector3f v) {
                float x=q.x,y=q.y,z=q.z,w=q.w;
                float ix =  w*v.x + y*v.z - z*v.y;
                float iy =  w*v.y + z*v.x - x*v.z;
                float iz =  w*v.z + x*v.y - y*v.x;
                float iw = -x*v.x - y*v.y - z*v.z;
                return XrVector3f{
                    ix*w + iw*-x + iy*-z - iz*-y,
                    iy*w + iw*-y + iz*-x - ix*-z,
                    iz*w + iw*-z + ix*-y - iy*-x
                };
            };
            for (size_t pi = 0; pi < panelMgr.Panels().size(); ++pi) {
                auto& pp = panelMgr.PanelAt((int)pi);
                if (!pp.pendingHeadAlign) continue;
                // Take p.pose (currently a head-relative offset) and rebase into world.
                XrVector3f worldOffset = qrotVec(headInLocal.orientation, pp.pose.position);
                pp.pose.position = worldOffset;
                pp.pose.orientation = qmul(headInLocal.orientation, pp.pose.orientation);
                pp.pendingHeadAlign = false;
                ALOGE("[rover-dof] panel=%zu head-aligned pose", pi);
            }
        }
        if (rightCtrlValid) rightHit = panelMgr.Raycast(rightCtrl.position, computeRayDir(rightCtrl.orientation), headInLocal);
        if (leftCtrlValid)  leftHit  = panelMgr.Raycast(leftCtrl.position,  computeRayDir(leftCtrl.orientation),  headInLocal);
        // v0.8-1b: propagate bar hover state to Kotlin renderer (only on transition)
        {
            static std::vector<bool> prevBarHov;
            if ((int)prevBarHov.size() != (int)panelMgr.Panels().size()) prevBarHov.resize(panelMgr.Panels().size(), false);
            // Reset hover for all panels first
            for (size_t pi = 0; pi < panelMgr.Panels().size(); ++pi) panelMgr.PanelAt((int)pi).barHovered = false;
            if (rightHit.panelIdx >= 0 && rightHit.hitBar) panelMgr.PanelAt(rightHit.panelIdx).barHovered = true;
            if (leftHit.panelIdx >= 0 && leftHit.hitBar) panelMgr.PanelAt(leftHit.panelIdx).barHovered = true;
            for (size_t pi = 0; pi < panelMgr.Panels().size(); ++pi) {
                bool now = panelMgr.PanelAt((int)pi).barHovered;
                if (now != prevBarHov[pi]) {
                    CallNotifyBarHover(androidApp, (int)pi, now);
                    prevBarHov[pi] = now;
                }
            }
        }
        {
            // v0.7.2-diag: log on trigger press-transition so we can see what user "clicked" on
            static bool prevL = false, prevR = false;
            if (rightTrigger && !prevR) {
                XrVector3f rd = computeRayDir(rightCtrl.orientation);
                ALOGE("[rover-diag] R-TRIG orig=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) hit=panel%d(bar=%d,corner=%d,dist=%.2f)",
                    rightCtrl.position.x, rightCtrl.position.y, rightCtrl.position.z,
                    rd.x, rd.y, rd.z,
                    rightHit.panelIdx, rightHit.hitBar?1:0, rightHit.cornerIdx, rightHit.distance);
                for (size_t pi = 0; pi < panelMgr.Panels().size(); ++pi) {
                    const auto& pp = panelMgr.PanelAt(pi);
                    XrPosef pw = panelMgr.ResolveWorldPose((int)pi, headInLocal);
                    ALOGE("[rover-diag]   panel%zu vis=%d oes=%d kb=%d pos=(%.2f,%.2f,%.2f) size=(%.2f,%.2f)",
                        pi, pp.visible?1:0, pp.oesSourced?1:0, pp.isKeyboard?1:0,
                        pw.position.x, pw.position.y, pw.position.z, pp.size.width, pp.size.height);
                }
            }
            if (leftTrigger && !prevL) {
                XrVector3f rd = computeRayDir(leftCtrl.orientation);
                ALOGE("[rover-diag] L-TRIG orig=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) hit=panel%d(bar=%d,corner=%d,dist=%.2f)",
                    leftCtrl.position.x, leftCtrl.position.y, leftCtrl.position.z,
                    rd.x, rd.y, rd.z,
                    leftHit.panelIdx, leftHit.hitBar?1:0, leftHit.cornerIdx, leftHit.distance);
            }
            prevL = leftTrigger; prevR = rightTrigger;
        }
        if (rightHit.cornerIdx >= 0 || leftHit.cornerIdx >= 0) {
            static int hitctr = 0;
            if ((hitctr++ % 15) == 0) ALOGE("[rover-dbg] corner hit R.pi=%d R.corner=%d L.pi=%d L.corner=%d",
                rightHit.panelIdx, rightHit.cornerIdx, leftHit.panelIdx, leftHit.cornerIdx);
        }
        // For legacy cursor path: use grabbing hand hit if grabbing, else right, else left
        rover::HitResult hit = rightHit;
        if (grabbedHand == 1) hit = leftHit;
        else if (!rightCtrlValid && leftCtrlValid) hit = leftHit;

        if (resizedIdx >= 0) {
            // v0.4.3c: PER-AXIS resize. Project ray onto panel plane, decompose delta from
            // initial grab-point into panel's local X/Y basis, and scale w/h independently.
            // Corner index (0=BL,1=BR,2=TL,3=TR) determines the sign of growth on each axis.
            bool grabTrigger = (resizedHand == 1) ? leftTrigger : rightTrigger;
            bool grabValid   = (resizedHand == 1) ? leftCtrlValid : rightCtrlValid;
            XrPosef grabCtrl = (resizedHand == 1) ? leftCtrl : rightCtrl;
            if (!grabTrigger || !grabValid) {
                resizedIdx = -1;
                resizedHand = 0;
                panelMgr.SetHovered(-1);
            } else {
                XrPosef panelWorld = panelMgr.ResolveWorldPose(resizedIdx, headInLocal);
                XrVector3f rd = computeRayDir(grabCtrl.orientation);

                // Panel local basis in world coords
                auto qrot = [](const XrQuaternionf& q, XrVector3f v) {
                    // v' = q * v * q^-1
                    float x=q.x,y=q.y,z=q.z,w=q.w;
                    float ix =  w*v.x + y*v.z - z*v.y;
                    float iy =  w*v.y + z*v.x - x*v.z;
                    float iz =  w*v.z + x*v.y - y*v.x;
                    float iw = -x*v.x - y*v.y - z*v.z;
                    return XrVector3f{
                        ix*w + iw*-x + iy*-z - iz*-y,
                        iy*w + iw*-y + iz*-x - ix*-z,
                        iz*w + iw*-z + ix*-y - iy*-x
                    };
                };
                XrVector3f axisX = qrot(panelWorld.orientation, {1,0,0});
                XrVector3f axisY = qrot(panelWorld.orientation, {0,1,0});
                XrVector3f axisZ = qrot(panelWorld.orientation, {0,0,1});

                // Ray-plane intersection: plane through panel center, normal = axisZ
                float denom = rd.x*axisZ.x + rd.y*axisZ.y + rd.z*axisZ.z;
                float gpx=0, gpy=0, gpz=0;
                if (std::fabs(denom) > 1e-4f) {
                    float d = (panelWorld.position.x - grabCtrl.position.x)*axisZ.x
                            + (panelWorld.position.y - grabCtrl.position.y)*axisZ.y
                            + (panelWorld.position.z - grabCtrl.position.z)*axisZ.z;
                    float t = d / denom;
                    gpx = grabCtrl.position.x + t * rd.x;
                    gpy = grabCtrl.position.y + t * rd.y;
                    gpz = grabCtrl.position.z + t * rd.z;
                } else {
                    gpx = panelWorld.position.x; gpy = panelWorld.position.y; gpz = panelWorld.position.z;
                }

                // Project current hit-point into panel local (u,v) meters from center
                float relX = gpx - panelWorld.position.x;
                float relY = gpy - panelWorld.position.y;
                float relZ = gpz - panelWorld.position.z;
                float u = relX*axisX.x + relY*axisX.y + relZ*axisX.z;
                float v = relX*axisY.x + relY*axisY.y + relZ*axisY.z;

                // Corner sign: BL(0)=(-1,-1) BR(1)=(+1,-1) TL(2)=(-1,+1) TR(3)=(+1,+1)
                float sX = (resizedCorner == 1 || resizedCorner == 3) ? +1.f : -1.f;
                float sY = (resizedCorner == 2 || resizedCorner == 3) ? +1.f : -1.f;

                // Meta-style: resize centered — new half-extent = signed distance from center along that axis
                // So new_width  = 2 * |u|, new_height = 2 * |v|. Guard against zero, apply corner-sign so
                // dragging OUTWARD grows and INWARD shrinks (per axis independently).
                float newW = resizeInitSize.width  + 2.f * sX * (u - resizeInitGrabU);
                float newH = resizeInitSize.height + 2.f * sY * (v - resizeInitGrabV);
                // v0.4.3f: caps removed on user request; keep tiny mins so panel can't collapse to zero
                if (newW < 0.1f) newW = 0.1f;
                if (newH < 0.1f) newH = 0.1f;
                panelMgr.PanelAt(resizedIdx).size.width  = newW;
                panelMgr.PanelAt(resizedIdx).size.height = newH;
                panelMgr.SetHovered(resizedIdx);
            }
            prevLeftTrigger = leftTrigger;
            prevRightTrigger = rightTrigger;
        } else if (grabbedIdx == -1) {
            int hoverIdx = rightHit.panelIdx >= 0 ? rightHit.panelIdx : leftHit.panelIdx;
            panelMgr.SetHovered(hoverIdx);

            bool leftPressed  = leftTrigger  && !prevLeftTrigger;
            bool rightPressed = rightTrigger && !prevRightTrigger;
            int hand = 0;
            rover::HitResult useHit = {-1, false, -1, 0};
            XrPosef useCtrl = {{0,0,0,1}, {0,0,0}};
            if (rightPressed && rightHit.panelIdx >= 0 && rightCtrlValid) {
                hand = 2; useHit = rightHit; useCtrl = rightCtrl;
            } else if (leftPressed && leftHit.panelIdx >= 0 && leftCtrlValid) {
                hand = 1; useHit = leftHit; useCtrl = leftCtrl;
            }
            if (hand != 0) {
                XrPosef panelWorld = panelMgr.ResolveWorldPose(useHit.panelIdx, headInLocal);
                XrVector3f rd = computeRayDir(useCtrl.orientation);
                XrVector3f grabPoint = {
                    useCtrl.position.x + useHit.distance * rd.x,
                    useCtrl.position.y + useHit.distance * rd.y,
                    useCtrl.position.z + useHit.distance * rd.z,
                };
                if (useHit.cornerIdx >= 0) {
                    ALOGE("[rover-dbg] START RESIZE pi=%d corner=%d hand=%d", useHit.panelIdx, useHit.cornerIdx, hand);
                    resizedIdx = useHit.panelIdx;
                    resizedHand = hand;
                    resizedCorner = useHit.cornerIdx;
                    resizeInitSize = panelMgr.PanelAt(useHit.panelIdx).size;
                    // Project initial grab point into panel local (u,v) so per-axis math has a baseline
                    auto qrot2 = [](const XrQuaternionf& q, XrVector3f v) {
                        float x=q.x,y=q.y,z=q.z,w=q.w;
                        float ix =  w*v.x + y*v.z - z*v.y;
                        float iy =  w*v.y + z*v.x - x*v.z;
                        float iz =  w*v.z + x*v.y - y*v.x;
                        float iw = -x*v.x - y*v.y - z*v.z;
                        return XrVector3f{
                            ix*w + iw*-x + iy*-z - iz*-y,
                            iy*w + iw*-y + iz*-x - ix*-z,
                            iz*w + iw*-z + ix*-y - iy*-x
                        };
                    };
                    XrVector3f axX = qrot2(panelWorld.orientation, {1,0,0});
                    XrVector3f axY = qrot2(panelWorld.orientation, {0,1,0});
                    float rx = grabPoint.x - panelWorld.position.x;
                    float ry = grabPoint.y - panelWorld.position.y;
                    float rz = grabPoint.z - panelWorld.position.z;
                    resizeInitGrabU = rx*axX.x + ry*axX.y + rz*axX.z;
                    resizeInitGrabV = rx*axY.x + ry*axY.y + rz*axY.z;
                } else {
                    const auto& pRef = panelMgr.PanelAt(useHit.panelIdx);
                    if (pRef.oesSourced && !useHit.hitBar) {
                        // v0.5.1: OES panel body — start TAP tracking, DON'T grab.
                        // Use same panel-local math as resize (axX, axY already computed above? no — recompute).
                        auto qrot3 = [](const XrQuaternionf& q, XrVector3f v) {
                            float x=q.x,y=q.y,z=q.z,w=q.w;
                            float ix =  w*v.x + y*v.z - z*v.y;
                            float iy =  w*v.y + z*v.x - x*v.z;
                            float iz =  w*v.z + x*v.y - y*v.x;
                            float iw = -x*v.x - y*v.y - z*v.z;
                            return XrVector3f{
                                ix*w + iw*-x + iy*-z - iz*-y,
                                iy*w + iw*-y + iz*-x - ix*-z,
                                iz*w + iw*-z + ix*-y - iy*-x
                            };
                        };
                        XrVector3f axX = qrot3(panelWorld.orientation, {1,0,0});
                        XrVector3f axY = qrot3(panelWorld.orientation, {0,1,0});
                        float rx = grabPoint.x - panelWorld.position.x;
                        float ry = grabPoint.y - panelWorld.position.y;
                        float rz = grabPoint.z - panelWorld.position.z;
                        float u_m = rx*axX.x + ry*axX.y + rz*axX.z;  // meters from center along panel X (+ = right)
                        float v_m = rx*axY.x + ry*axY.y + rz*axY.z;  // meters from center along panel Y (+ = up)
                        float uv_u = (u_m + 0.5f * pRef.size.width)  / pRef.size.width;   // 0=left  1=right
                        float uv_v = 1.0f - (v_m + 0.5f * pRef.size.height) / pRef.size.height;  // 0=top   1=bottom (Android UI)
                        if (pRef.isKeyboard) {
                            // v0.7: keyboard panel — instant-fire via Kotlin dispatch, no tap-tracking.
                            ALOGE("[rover-kb] press pi=%d uv=(%.3f,%.3f)", useHit.panelIdx, uv_u, uv_v);
                            CallHandleKeyboardHit(androidApp, uv_u, uv_v);
                            // v0.7.1: arm hold-to-repeat (backspace/arrows)
                            kbHeldHand = hand;
                            kbHeldU = uv_u;
                            kbHeldV = uv_v;
                            kbHeldStartMs = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                            kbHeldNextFireMs = kbHeldStartMs + 400;  // initial delay
                        } else {
                            tapHand = hand;
                            tapPanelIdx = useHit.panelIdx;
                            tapStartU = uv_u;
                            tapStartV = uv_v;
                            tapStartTimeMs = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                            ALOGE("[rover-tap] START pi=%d hand=%d uv=(%.3f,%.3f) px=(%d,%d)",
                                useHit.panelIdx, hand, uv_u, uv_v,
                                (int)(uv_u * pRef.width), (int)(uv_v * pRef.height));
                        }
                    } else {
                        // v0.8-1b: if bar hit and panel has actionBar, sub-hit-test for buttons/slider
                        if (useHit.hitBar && panelMgr.PanelAt(useHit.panelIdx).barOesTexId != 0) {
                            // Compute bar-local UV from world hit point.
                            auto qrot4 = [](const XrQuaternionf& q, XrVector3f vv) {
                                float x=q.x,y=q.y,z=q.z,w=q.w;
                                float ix =  w*vv.x + y*vv.z - z*vv.y;
                                float iy =  w*vv.y + z*vv.x - x*vv.z;
                                float iz =  w*vv.z + x*vv.y - y*vv.x;
                                float iw = -x*vv.x - y*vv.y - z*vv.z;
                                return XrVector3f{
                                    ix*w + iw*-x + iy*-z - iz*-y,
                                    iy*w + iw*-y + iz*-x - ix*-z,
                                    iz*w + iw*-z + ix*-y - iy*-x
                                };
                            };
                            const auto& p = panelMgr.PanelAt(useHit.panelIdx);
                            XrPosef pw = panelWorld;
                            // Bar world pose: shift down from panel center
                            // bar height is constant 0.030f; not needed for u-only sub-hit
                            XrVector3f axX2 = qrot4(pw.orientation, {1,0,0});
                            XrVector3f axY2 = qrot4(pw.orientation, {0,1,0});
                            // grabPoint is world hit
                            float rx = grabPoint.x - pw.position.x;
                            float ry = grabPoint.y - pw.position.y;
                            float rz = grabPoint.z - pw.position.z;
                            float u_m = rx*axX2.x + ry*axX2.y + rz*axX2.z;
                            float v_m = rx*axY2.x + ry*axY2.y + rz*axY2.z;
                            // bar sits below panel, so v_m is negative. Map to bar-local UV.
                            float barW = p.size.width * 1.0f;
                            float baruv_u = (u_m + 0.5f * barW) / barW;
                            // For hit-test scope, we don't need vertical UV; Kotlin cares only about u
                            int act = CallHandleBarHit(androidApp, useHit.panelIdx, baruv_u, 0.5f);
                            if (act == 4) {
                                g_sliderPanelIdx = useHit.panelIdx;
                                g_sliderHand = hand;
                                ALOGE("[rover-bar] slider grab start pi=%d", useHit.panelIdx);
                            } else if (act == 2) {
                                // Hide: keep VD alive, just make panel invisible
                                panelMgr.PanelAt(useHit.panelIdx).visible = false;
                                ALOGE("[rover-bar] hide pi=%d", useHit.panelIdx);
                            } else if (act == 3) {
                                // DoF cycle: HeadLocked(0) -> YawLocked(3) -> BodyLocked(2) -> WorldAnchored(1) -> repeat
                                rover::DofMode cur = panelMgr.PanelAt(useHit.panelIdx).dofMode;
                                rover::DofMode next;
                                switch (cur) {
                                    case rover::DofMode::HeadLocked: next = rover::DofMode::YawLocked; break;
                                    case rover::DofMode::YawLocked: next = rover::DofMode::BodyLocked; break;
                                    case rover::DofMode::BodyLocked: next = rover::DofMode::WorldAnchored; break;
                                    default: next = rover::DofMode::HeadLocked; break;
                                }
                                panelMgr.PanelAt(useHit.panelIdx).dofMode = next;
                                ALOGE("[rover-bar] dof pi=%d cycled", useHit.panelIdx);
                            } else if (act >= 1) {
                                ALOGE("[rover-bar] button pi=%d act=%d handled by Kotlin", useHit.panelIdx, act);
                            } else {
                                // No button / slider — normal drag
                                grabbedIdx = useHit.panelIdx;
                                grabbedHand = hand;
                                grabDist = useHit.distance;
                                if (grabDist < 0.2f) grabDist = 0.2f;
                                grabOffsetWorld.x = panelWorld.position.x - grabPoint.x;
                                grabOffsetWorld.y = panelWorld.position.y - grabPoint.y;
                                grabOffsetWorld.z = panelWorld.position.z - grabPoint.z;
                                grabOrient = panelWorld.orientation;
                            }
                        } else {
                            // Existing MOVE path
                            grabbedIdx = useHit.panelIdx;
                            grabbedHand = hand;
                            grabDist = useHit.distance;
                            if (grabDist < 0.2f) grabDist = 0.2f;
                            grabOffsetWorld.x = panelWorld.position.x - grabPoint.x;
                            grabOffsetWorld.y = panelWorld.position.y - grabPoint.y;
                            grabOffsetWorld.z = panelWorld.position.z - grabPoint.z;
                            grabOrient = panelWorld.orientation;
                        }
                    }
                }
            }
        } else {
            bool grabTrigger = (grabbedHand == 1) ? leftTrigger : rightTrigger;
            bool grabValid   = (grabbedHand == 1) ? leftCtrlValid : rightCtrlValid;
            XrPosef grabCtrl = (grabbedHand == 1) ? leftCtrl : rightCtrl;
            float thumbY = (grabbedHand == 1) ? leftThumbState.currentState.y : rightThumbState.currentState.y;
            const float THUMB_DEAD = 0.15f;
            const float THUMB_SPEED = 1.0f;   // meters/sec at full deflection, per-frame scale of ~1/90Hz
            if (thumbY > THUMB_DEAD) grabDist += (thumbY - THUMB_DEAD) * (1.0f / (1.0f - THUMB_DEAD)) * THUMB_SPEED / 90.0f;
            else if (thumbY < -THUMB_DEAD) grabDist += (thumbY + THUMB_DEAD) * (1.0f / (1.0f - THUMB_DEAD)) * THUMB_SPEED / 90.0f;
            if (grabDist < 0.2f) grabDist = 0.2f;
            if (grabDist > 10.0f) grabDist = 10.0f;

            XrVector3f rd = computeRayDir(grabCtrl.orientation);
            XrPosef liveWorld;
            liveWorld.position.x = grabCtrl.position.x + grabDist * rd.x + grabOffsetWorld.x;
            liveWorld.position.y = grabCtrl.position.y + grabDist * rd.y + grabOffsetWorld.y;
            liveWorld.position.z = grabCtrl.position.z + grabDist * rd.z + grabOffsetWorld.z;
            // Auto-face head: panel +Z aims at head, world up locked to avoid roll
            {
                float tx = headInLocal.position.x - liveWorld.position.x;
                float ty = headInLocal.position.y - liveWorld.position.y;
                float tz = headInLocal.position.z - liveWorld.position.z;
                float tlen = std::sqrt(tx*tx + ty*ty + tz*tz);
                if (tlen > 1e-4f) {
                    tx /= tlen; ty /= tlen; tz /= tlen;
                    // right = world_up × toUser
                    float rx = 1.0f * tz - 0.0f * ty;
                    float ry = 0.0f * tx - 0.0f * tz;
                    float rz = 0.0f * ty - 1.0f * tx;
                    float rlen = std::sqrt(rx*rx + ry*ry + rz*rz);
                    if (rlen > 1e-4f) {
                        rx /= rlen; ry /= rlen; rz /= rlen;
                        // up = toUser × right
                        float ux = ty * rz - tz * ry;
                        float uy = tz * rx - tx * rz;
                        float uz = tx * ry - ty * rx;
                        // Rotation matrix columns: [right, up, toUser]
                        // matrix-to-quat
                        float m00=rx, m01=ux, m02=tx;
                        float m10=ry, m11=uy, m12=ty;
                        float m20=rz, m21=uz, m22=tz;
                        float tr = m00 + m11 + m22;
                        XrQuaternionf q;
                        if (tr > 0.0f) {
                            float S = std::sqrt(tr + 1.0f) * 2.0f;
                            q.w = 0.25f * S; q.x = (m21 - m12)/S; q.y = (m02 - m20)/S; q.z = (m10 - m01)/S;
                        } else if (m00 > m11 && m00 > m22) {
                            float S = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                            q.w = (m21 - m12)/S; q.x = 0.25f*S; q.y = (m01 + m10)/S; q.z = (m02 + m20)/S;
                        } else if (m11 > m22) {
                            float S = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                            q.w = (m02 - m20)/S; q.x = (m01 + m10)/S; q.y = 0.25f*S; q.z = (m12 + m21)/S;
                        } else {
                            float S = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                            q.w = (m10 - m01)/S; q.x = (m02 + m20)/S; q.y = (m12 + m21)/S; q.z = 0.25f*S;
                        }
                        liveWorld.orientation = q;
                    } else {
                        liveWorld.orientation = grabOrient;
                    }
                } else {
                    liveWorld.orientation = grabOrient;
                }
            }
            panelMgr.CommitWorldPose(grabbedIdx, liveWorld, headInLocal);
            if (!grabTrigger || !grabValid) {
                grabbedIdx = -1;
                grabbedHand = 0;
                panelMgr.SetHovered(-1);
            } else {
                panelMgr.SetHovered(grabbedIdx);
            }
        }
        // v0.4.3e (revised): hybrid — physical resize feels immediate (world size updates every frame),
        // and on stability (~15 frames after last change) we fire VD.resize + swapchain rebuild
        // at physicalSize * pixelsPerMeter so the app re-layouts at new pixel resolution.
        {
            // v0.8-fix-reflow: iterate all oesSourced panels; per-panel stable-count + last-committed
            struct ReflowState { float lastW; float lastH; int stableCount; int lastCommittedW; int lastCommittedH; };
            static std::vector<ReflowState> refl;
            if ((int)refl.size() != (int)panelMgr.Panels().size())
                refl.resize(panelMgr.Panels().size(), {-1.0f, -1.0f, 0, 0, 0});
            float ppm = CallGetPixelsPerMeter(androidApp);
            int dpi = CallGetCurrentDpi(androidApp);
            const float eps = 0.001f;
            for (int pi = 0; pi < (int)panelMgr.Panels().size(); ++pi) {
                const auto& pp = panelMgr.PanelAt(pi);
                if (!pp.oesSourced) continue;
                if (pp.isKeyboard) continue;  // kb has its own fixed-size render
                ReflowState& st = refl[pi];
                bool sizeChanged = (std::fabs(pp.size.width  - st.lastW) > eps) ||
                                   (std::fabs(pp.size.height - st.lastH) > eps);
                if (sizeChanged) { st.lastW = pp.size.width; st.lastH = pp.size.height; st.stableCount = 0; }
                else st.stableCount++;
                int wantW = (int)(pp.size.width  * ppm);
                int wantH = (int)(pp.size.height * ppm);
                if (wantW < 320) wantW = 320; if (wantH < 240) wantH = 240;
                if (wantW > 8192) wantW = 8192; if (wantH > 8192) wantH = 8192;
                if (st.stableCount == 15 && (wantW != st.lastCommittedW || wantH != st.lastCommittedH)) {
                    ALOGE("[rover] COMMIT pi=%d %.3fx%.3fm -> %dx%dpx @ %ddpi (ppm=%.0f)",
                        pi, pp.size.width, pp.size.height, wantW, wantH, dpi, ppm);
                    if (pi == 0) {
                        CallResizeVirtualDisplay(androidApp, wantW, wantH, dpi);
                    } else {
                        CallResizeHostedApp(androidApp, pi, wantW, wantH, dpi);
                    }
                    panelMgr.ResizePanelSwapchain(pi, wantW, wantH);
                    st.lastCommittedW = wantW;
                    st.lastCommittedH = wantH;
                }
            }
        }
        g_prevResizedIdx = resizedIdx;

        // v0.8-1b: bar-slider drag — while held, map ray-x on bar to slider value 0..1
        if (g_sliderPanelIdx >= 0) {
            bool trig = (g_sliderHand == 1) ? leftTrigger : rightTrigger;
            if (!trig) {
                ALOGE("[rover-bar] slider release pi=%d", g_sliderPanelIdx);
                g_sliderPanelIdx = -1; g_sliderHand = 0;
            } else {
                // Raycast against this panel's bar to get current uv-x
                const rover::HitResult& h = (g_sliderHand == 1) ? leftHit : rightHit;
                if (h.panelIdx == g_sliderPanelIdx && h.hitBar) {
                    XrPosef panelW = panelMgr.ResolveWorldPose(g_sliderPanelIdx, headInLocal);
                    auto qrot5 = [](const XrQuaternionf& q, XrVector3f vv) {
                        float x=q.x,y=q.y,z=q.z,w=q.w;
                        float ix =  w*vv.x + y*vv.z - z*vv.y;
                        float iy =  w*vv.y + z*vv.x - x*vv.z;
                        float iz =  w*vv.z + x*vv.y - y*vv.x;
                        float iw = -x*vv.x - y*vv.y - z*vv.z;
                        return XrVector3f{
                            ix*w + iw*-x + iy*-z - iz*-y,
                            iy*w + iw*-y + iz*-x - ix*-z,
                            iz*w + iw*-z + ix*-y - iy*-x
                        };
                    };
                    XrVector3f axX3 = qrot5(panelW.orientation, {1,0,0});
                    // Ray-plane intersection with bar plane — reuse grabPoint from earlier if valid; else compute
                    // For simplicity, use panel's own hit at current frame — approximate as ray + hit.distance
                    XrPosef ctrl = (g_sliderHand == 1) ? leftCtrl : rightCtrl;
                    XrVector3f rd = computeRayDir(ctrl.orientation);
                    XrVector3f hitPt = {
                        ctrl.position.x + h.distance * rd.x,
                        ctrl.position.y + h.distance * rd.y,
                        ctrl.position.z + h.distance * rd.z
                    };
                    float rx = hitPt.x - panelW.position.x;
                    float ry = hitPt.y - panelW.position.y;
                    float rz = hitPt.z - panelW.position.z;
                    float u_m = rx*axX3.x + ry*axX3.y + rz*axX3.z;
                    float barW = panelMgr.PanelAt(g_sliderPanelIdx).size.width;
                    float baruv_u = (u_m + 0.5f * barW) / barW;
                    if (baruv_u < 0.f) baruv_u = 0.f; if (baruv_u > 1.f) baruv_u = 1.f;
                    CallUpdateBarSlider(androidApp, g_sliderPanelIdx, baruv_u);
                    panelMgr.PanelAt(g_sliderPanelIdx).panelAlpha = baruv_u;
                }
            }
        }

        // v0.7.1: keyboard hold-to-repeat — after 400ms, fire every 60ms while held
        if (kbHeldHand != 0) {
            bool stillHeld = (kbHeldHand == 1) ? leftTrigger : rightTrigger;
            if (!stillHeld) {
                kbHeldHand = 0;
            } else {
                long nowKb = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                if (nowKb >= kbHeldNextFireMs) {
                    CallHandleKeyboardHold(androidApp, kbHeldU, kbHeldV);
                    kbHeldNextFireMs = nowKb + 60;
                }
            }
        }

        // v0.5.1: tap release detection — fires tap or long-press based on held duration
        if (tapHand != 0 && tapPanelIdx >= 0) {
            bool stillHeld = (tapHand == 1) ? leftTrigger : rightTrigger;
            if (!stillHeld) {
                const auto& p = panelMgr.PanelAt(tapPanelIdx);
                int x = (int)(tapStartU * p.width);
                int y = (int)(tapStartV * p.height);
                if (x < 0) x = 0; if (x >= p.width) x = p.width - 1;
                if (y < 0) y = 0; if (y >= p.height) y = p.height - 1;
                int did = CallPanelIdxToDisplayId(androidApp, tapPanelIdx);
                long nowMs = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                long held = nowMs - tapStartTimeMs;
                ALOGE("[rover-tap] FIRE pi=%d did=%d px=(%d,%d) held=%ldms", tapPanelIdx, did, x, y, held);
                if (did >= 0) {
                    if (held < 500) {
                        CallInjectTap(androidApp, did, x, y);      // quick tap
                    } else {
                        // long-press: same start & end, hold for `held` ms
                        CallInjectSwipe(androidApp, did, x, y, x, y, (int)held);
                    }
                }
                tapHand = 0;
                tapPanelIdx = -1;
            }
        }

        // v0.5.1: scroll via thumbstick Y — while hovering over OES panel body
        {
            static long lastScrollMs = 0;
            long nowScroll = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (nowScroll - lastScrollMs >= 50) {   // fire at ~20Hz
                const float DEAD = 0.15f;
                for (int hand = 1; hand <= 2; hand++) {
                    const rover::HitResult& h = (hand == 1) ? leftHit : rightHit;
                    if (h.panelIdx < 0 || h.hitBar || h.cornerIdx >= 0) continue;
                    const auto& p = panelMgr.PanelAt(h.panelIdx);
                    if (!p.oesSourced) continue;
                    float sy = (hand == 1) ? leftThumbState.currentState.y : rightThumbState.currentState.y;
                    if (std::fabs(sy) < DEAD) continue;
                    // Compute hit UV in pixels via same math as tap
                    auto qrot4 = [](const XrQuaternionf& q, XrVector3f v) {
                        float x=q.x,y=q.y,z=q.z,w=q.w;
                        float ix =  w*v.x + y*v.z - z*v.y;
                        float iy =  w*v.y + z*v.x - x*v.z;
                        float iz =  w*v.z + x*v.y - y*v.x;
                        float iw = -x*v.x - y*v.y - z*v.z;
                        return XrVector3f{
                            ix*w + iw*-x + iy*-z - iz*-y,
                            iy*w + iw*-y + iz*-x - ix*-z,
                            iz*w + iw*-z + ix*-y - iy*-x
                        };
                    };
                    XrPosef panelWorld = panelMgr.ResolveWorldPose(h.panelIdx, headInLocal);
                    XrPosef ctrl = (hand == 1) ? leftCtrl : rightCtrl;
                    XrVector3f rd = computeRayDir(ctrl.orientation);
                    XrVector3f hp = { ctrl.position.x + h.distance*rd.x, ctrl.position.y + h.distance*rd.y, ctrl.position.z + h.distance*rd.z };
                    XrVector3f axX = qrot4(panelWorld.orientation, {1,0,0});
                    XrVector3f axY = qrot4(panelWorld.orientation, {0,1,0});
                    float rx = hp.x - panelWorld.position.x;
                    float ry = hp.y - panelWorld.position.y;
                    float rz = hp.z - panelWorld.position.z;
                    float u_m = rx*axX.x + ry*axX.y + rz*axX.z;
                    float v_m = rx*axY.x + ry*axY.y + rz*axY.z;
                    float uv_u = (u_m + 0.5f * p.size.width)  / p.size.width;
                    float uv_v = 1.0f - (v_m + 0.5f * p.size.height) / p.size.height;
                    int cx = (int)(uv_u * p.width);
                    int cy = (int)(uv_v * p.height);
                    if (cx < 0) cx = 0; if (cx >= p.width) cx = p.width - 1;
                    if (cy < 0) cy = 0; if (cy >= p.height) cy = p.height - 1;
                    // Swipe from (cx, cy - delta) to (cx, cy + delta), sy>0 = up on stick = scroll UP page = touch swipes DOWN (natural)
                    // Ensure delta always > tap-threshold so Android sees swipe not tap
                    int delta = (int)(sy * 400.0f);
                    int minDelta = 120; if (sy > 0) { if (delta < minDelta) delta = minDelta; } else { if (delta > -minDelta) delta = -minDelta; }
                    int y1 = cy - delta / 2;
                    int y2 = cy + delta / 2;
                    if (y1 < 0) y1 = 0; if (y2 >= p.height) y2 = p.height - 1;
                    int did = CallPanelIdxToDisplayId(androidApp, h.panelIdx);
                    if (did >= 0) {
                        CallInjectSwipe(androidApp, did, cx, y1, cx, y2, 100);
                    }
                    lastScrollMs = nowScroll;
                    break;  // one hand per tick
                }
            }
        }

        prevLeftTrigger = leftTrigger;
        prevRightTrigger = rightTrigger;

        // Cursor visibility per hand
        cursor.SetVisible(rightHit.panelIdx >= 0 && rightCtrlValid);
        cursor.SetActive(rightTrigger);
        leftCursor.SetVisible(leftHit.panelIdx >= 0 && leftCtrlValid);
        leftCursor.SetActive(leftTrigger);
        XrVector3f rightCursorPos = {0,0,0}, leftCursorPos = {0,0,0};
        if (rightHit.panelIdx >= 0 && rightCtrlValid) {
            XrVector3f rd = computeRayDir(rightCtrl.orientation);
            rightCursorPos.x = rightCtrl.position.x + rightHit.distance * rd.x;
            rightCursorPos.y = rightCtrl.position.y + rightHit.distance * rd.y;
            rightCursorPos.z = rightCtrl.position.z + rightHit.distance * rd.z;
        }
        if (leftHit.panelIdx >= 0 && leftCtrlValid) {
            XrVector3f rd = computeRayDir(leftCtrl.orientation);
            leftCursorPos.x = leftCtrl.position.x + leftHit.distance * rd.x;
            leftCursorPos.y = leftCtrl.position.y + leftHit.distance * rd.y;
            leftCursorPos.z = leftCtrl.position.z + leftHit.distance * rd.z;
        }

        // Ray-line per hand
        rayLine.SetVisible(rightCtrlValid);
        rayLine.SetActive(rightTrigger);
        leftRayLine.SetVisible(leftCtrlValid);
        leftRayLine.SetActive(leftTrigger);
        XrVector3f rightRayDir = rightCtrlValid ? computeRayDir(rightCtrl.orientation) : XrVector3f{0,0,-1};
        XrVector3f leftRayDir  = leftCtrlValid  ? computeRayDir(leftCtrl.orientation)  : XrVector3f{0,0,-1};
        float rightRayLen = (rightHit.panelIdx >= 0) ? rightHit.distance : 3.0f;
        float leftRayLen  = (leftHit.panelIdx >= 0)  ? leftHit.distance  : 3.0f;

        // v0.4.3d: only apply Kotlin panel size when broadcast actually changed it
        {
            float rw=0.f, rh=0.f;
            static float lastRw=1.024f, lastRh=0.640f;
            if (!panelMgr.Panels().empty() && CallGetPanelWorld(androidApp, &rw, &rh) && rw > 0 && rh > 0) {
                if (std::fabs(rw - lastRw) > 0.001f || std::fabs(rh - lastRh) > 0.001f) {
                    panelMgr.PanelAt(0).size.width = rw;
                    panelMgr.PanelAt(0).size.height = rh;
                    lastRw = rw; lastRh = rh;
                    ALOGE("[rover] applied broadcast panel size %.3fx%.3f", rw, rh);
                }
            }
        }
        { static int fs_ctr = 0;
            if ((fs_ctr++ % 30) == 0) {
                for (int i = 0; i < (int)panelMgr.Panels().size(); i++) {
                    const auto& pp = panelMgr.PanelAt(i);
                    ALOGE("[rover-dbg] panel %d worldWH=%.3fx%.3f swapWH=%dx%d oes=%d",
                        i, pp.size.width, pp.size.height, pp.width, pp.height, pp.oesSourced?1:0);
                }
            }
        }
        {
            panelMgr.UpdateDynamic(frameState.predictedDisplayTime * 1e-9f);
            // v0.4.2d2: pump SurfaceTexture + blit OES into any oesSourced panel
            static bool g_launchedTestApp = false;
            {  // v0.8-fix: was guarded by g_oesTextureId != 0 (primary OES) — no longer required
                if (g_oesTextureId != 0) CallUpdateSurfaceTexImage(androidApp);
            CallUpdateKbSurfaceTexImage(androidApp);
            CallUpdateAllHostedTexImages(androidApp);
            CallUpdateAllBarTexImages(androidApp);
for (int pi = 0; pi < (int)panelMgr.Panels().size(); pi++) {
                    if (panelMgr.PanelAt(pi).oesSourced) {
                        float kStMat[16]; bool haveMat = CallGetSTMatrixForPanel(androidApp, (int)pi, kStMat);
                        static int dbg_ctr = 0;
                        if ((dbg_ctr++ % 30) == 0) {
                            ALOGE("[rover-dbg] pi=%d oesTex=%u swapWH=%dx%d worldWH=%.3fx%.3f haveMat=%d ST=[%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f]",
                                pi, g_oesTextureId, panelMgr.PanelAt(pi).width, panelMgr.PanelAt(pi).height, panelMgr.PanelAt(pi).size.width, panelMgr.PanelAt(pi).size.height, haveMat?1:0,
                                kStMat[0],kStMat[1],kStMat[2],kStMat[3],
                                kStMat[4],kStMat[5],kStMat[6],kStMat[7],
                                kStMat[8],kStMat[9],kStMat[10],kStMat[11],
                                kStMat[12],kStMat[13],kStMat[14],kStMat[15]);
                        }
                        if (!panelMgr.PanelAt(pi).visible) continue;
                        GLuint useTex = panelMgr.PanelAt(pi).oesTextureId ? panelMgr.PanelAt(pi).oesTextureId : g_oesTextureId;
                        const float* useStMat = haveMat ? kStMat : nullptr;
                        oesBlitter.BlitToPanel(panelMgr.PanelAt(pi), useTex, useStMat);
                        // v0.8-1b: blit bar OES for this panel if present
                        if (panelMgr.PanelAt(pi).barOesTexId != 0) {
                            float barStMat[16]; bool haveBar = CallGetBarSTMatrix(androidApp, (int)pi, barStMat);
                            oesBlitter.BlitToBar(panelMgr.PanelAt(pi), panelMgr.PanelAt(pi).barOesTexId, haveBar ? barStMat : nullptr);
                        }
                    }
                }
            }
            XrCompositionLayerQuad panelQuads[MaxLayerCount];
            int panelCount = 0;
            panelMgr.BuildLayers(panelQuads, MaxLayerCount, &panelCount, headInLocal);
            for (int i = 0; i < panelCount && app.LayerCount < MaxLayerCount; i++) {
                app.Layers[app.LayerCount++].Quad = panelQuads[i];
            }
            // Cursors + rays for both hands (last, so they render on top)
            XrCompositionLayerQuad cursorLayer;
            if (cursor.BuildLayer(&cursorLayer, app.LocalSpace, rightCursorPos, headInLocal)
                && app.LayerCount < MaxLayerCount) {
                app.Layers[app.LayerCount++].Quad = cursorLayer;
            }
            XrCompositionLayerQuad leftCursorLayer;
            if (leftCursor.BuildLayer(&leftCursorLayer, app.LocalSpace, leftCursorPos, headInLocal)
                && app.LayerCount < MaxLayerCount) {
                app.Layers[app.LayerCount++].Quad = leftCursorLayer;
            }
            XrCompositionLayerQuad rayLayer;
            if (rightCtrlValid && rayLine.BuildLayer(&rayLayer, app.LocalSpace,
                    rightCtrl.position, rightRayDir, rightRayLen, headInLocal)
                && app.LayerCount < MaxLayerCount) {
                app.Layers[app.LayerCount++].Quad = rayLayer;
            }
            XrCompositionLayerQuad leftRayLayer;
            if (leftCtrlValid && leftRayLine.BuildLayer(&leftRayLayer, app.LocalSpace,
                    leftCtrl.position, leftRayDir, leftRayLen, headInLocal)
                && app.LayerCount < MaxLayerCount) {
                app.Layers[app.LayerCount++].Quad = leftRayLayer;
            }
        }


        // Compose the layers for this frame.
        const XrCompositionLayerBaseHeader* layers[MaxLayerCount] = {};
        for (int i = 0; i < app.LayerCount; i++) {
            layers[i] = (const XrCompositionLayerBaseHeader*)&app.Layers[i];
        }

        XrFrameEndInfo endFrameInfo = {XR_TYPE_FRAME_END_INFO};
        endFrameInfo.displayTime = frameState.predictedDisplayTime;
        endFrameInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endFrameInfo.layerCount = app.LayerCount;
        endFrameInfo.layers = layers;

        OXR(xrEndFrame(app.Session, &endFrameInfo));
    }

    rayLine.Shutdown();
    cursor.Shutdown();
    leftRayLine.Shutdown();
    leftCursor.Shutdown();
    panelMgr.Shutdown();
    app.appRenderer.Destroy();

    AppInput_shutdown();

    OXR(xrDestroySwapchain(app.ColorSwapChain));
    OXR(xrDestroySpace(app.HeadSpace));
    OXR(xrDestroySpace(app.LocalSpace));
    // StageSpace is optional.
    if (app.StageSpace != XR_NULL_HANDLE) {
        OXR(xrDestroySpace(app.StageSpace));
    }
    OXR(xrDestroySession(app.Session));

    app.egl.DestroyContext();

    OXR(xrDestroyInstance(app.Instance));

#if defined(XR_USE_PLATFORM_ANDROID)
    (*androidApp->activity->vm).DetachCurrentThread();
#endif // defined(XR_USE_PLATFORM_ANDROID)
}
