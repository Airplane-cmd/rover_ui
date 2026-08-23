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
#endif // defined(ANDROID)

#include <assert.h>

#include "RoverUi.h"
#include "Panel.h"
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
    // v0.4.2d1: create OES texture for MediaProjection SurfaceTexture; push id to Kotlin
    CallSetupOesTexture(androidApp);
    {
        // Center panel: 0DoF (head-locked). Blue-ish. 40x30 cm at 1.5 m ahead.
        XrPosef pose0 = {{0,0,0,1}, {0.0f, 0.0f, -1.5f}};
        panelMgr.AddPanel(rover::DofMode::HeadLocked, pose0, {0.40f, 0.30f}, 0.15f, 0.35f, 0.60f, 0.85f);
        panelMgr.PanelAt(0).dynamic = true;

        // Left panel: 6DoF (world-anchored). Green. 40x30 cm at 1.5 m ahead + 0.6 m left.
        XrPosef pose1 = {{0,0,0,1}, {-0.6f, 0.0f, -1.5f}};
        panelMgr.AddPanel(rover::DofMode::WorldAnchored, pose1, {0.40f, 0.30f}, 0.20f, 0.55f, 0.25f, 0.85f);

        // Right panel: 3DoF (yaw-locked, rotates with head yaw but stays put on pitch/roll/translation-drift).
        // Red. Pose is offset from head-yaw origin: 0.6 m right, 1.5 m ahead.
        XrPosef pose2 = {{0,0,0,1}, {0.6f, 0.0f, -1.5f}};
        panelMgr.AddPanel(rover::DofMode::BodyLocked, pose2, {0.40f, 0.30f}, 0.65f, 0.25f, 0.25f, 0.85f);
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
        static int resizedIdx = -1;
        static int resizedHand = 0;
        static float resizeInitDist = 1.0f;
        static XrExtent2Df resizeInitSize = {0.4f, 0.3f};
        static bool prevLeftTrigger = false;
        static bool prevRightTrigger = false;
        static bool prevCaptureBtn = false;
        {
            bool cur = (rightBButtonState.type != 0 && rightBButtonState.currentState != XR_FALSE);
            if (cur && !prevCaptureBtn) {
                ALOGE("[rover] Right B pressed — requesting MediaProjection");
                CallRequestMediaProjection(androidApp);
            }
            prevCaptureBtn = cur;
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
        if (rightCtrlValid) rightHit = panelMgr.Raycast(rightCtrl.position, computeRayDir(rightCtrl.orientation), headInLocal);
        if (leftCtrlValid)  leftHit  = panelMgr.Raycast(leftCtrl.position,  computeRayDir(leftCtrl.orientation),  headInLocal);
        // For legacy cursor path: use grabbing hand hit if grabbing, else right, else left
        rover::HitResult hit = rightHit;
        if (grabbedHand == 1) hit = leftHit;
        else if (!rightCtrlValid && leftCtrlValid) hit = leftHit;

        if (resizedIdx >= 0) {
            // RESIZE update: scale panel by current-dist / init-dist
            bool grabTrigger = (resizedHand == 1) ? leftTrigger : rightTrigger;
            bool grabValid   = (resizedHand == 1) ? leftCtrlValid : rightCtrlValid;
            XrPosef grabCtrl = (resizedHand == 1) ? leftCtrl : rightCtrl;
            if (!grabTrigger || !grabValid) {
                resizedIdx = -1;
                resizedHand = 0;
                panelMgr.SetHovered(-1);
            } else {
                XrVector3f rd = computeRayDir(grabCtrl.orientation);
                XrPosef panelWorld = panelMgr.ResolveWorldPose(resizedIdx, headInLocal);
                // Use same grabDist-along-ray as when starting; simplest: intersect ray with plane through panel center parallel to panel face
                // For MVP: use fixed initial distance approximation via current controller-to-panel-center dist
                float px = grabCtrl.position.x - panelWorld.position.x;
                float py = grabCtrl.position.y - panelWorld.position.y;
                float pz = grabCtrl.position.z - panelWorld.position.z;
                float ctrl_to_center = std::sqrt(px*px + py*py + pz*pz);
                // grab point ~ controller + ctrl_to_center * ray direction
                float gpx = grabCtrl.position.x + ctrl_to_center * rd.x;
                float gpy = grabCtrl.position.y + ctrl_to_center * rd.y;
                float gpz = grabCtrl.position.z + ctrl_to_center * rd.z;
                float ddx = gpx - panelWorld.position.x;
                float ddy = gpy - panelWorld.position.y;
                float ddz = gpz - panelWorld.position.z;
                float cur = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
                float scale = cur / resizeInitDist;
                if (scale < 0.2f) scale = 0.2f;
                if (scale > 5.0f) scale = 5.0f;
                panelMgr.PanelAt(resizedIdx).size.width  = resizeInitSize.width  * scale;
                panelMgr.PanelAt(resizedIdx).size.height = resizeInitSize.height * scale;
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
                    // Start RESIZE
                    resizedIdx = useHit.panelIdx;
                    resizedHand = hand;
                    float dx = grabPoint.x - panelWorld.position.x;
                    float dy = grabPoint.y - panelWorld.position.y;
                    float dz = grabPoint.z - panelWorld.position.z;
                    resizeInitDist = std::sqrt(dx*dx + dy*dy + dz*dz);
                    if (resizeInitDist < 0.02f) resizeInitDist = 0.02f;
                    resizeInitSize = panelMgr.PanelAt(useHit.panelIdx).size;
                } else {
                    // Start MOVE
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

        {
            panelMgr.UpdateDynamic(frameState.predictedDisplayTime * 1e-9f);
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
