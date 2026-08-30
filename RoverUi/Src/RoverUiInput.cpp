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

#if defined(ANDROID)
#include <unistd.h>
#include <android/log.h>
#include <android/native_window_jni.h> // for native window JNI
#include <android_native_app_glue.h>
#endif // defined(ANDROID)

#include <assert.h>

#include <vector>

#include "RoverUi.h"
#include "RoverUiInput.h"

using namespace OVR;

#if !defined(EGL_OPENGL_ES3_BIT_KHR)
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#define LOG_TAG "XrPassthrough"

#if defined(ANDROID)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#else
#include <cinttypes>
#define ALOGE(...)       \
    printf("ERROR: ");   \
    printf(__VA_ARGS__); \
    printf("\n")
#define ALOGV(...)       \
    printf("VERBOSE: "); \
    printf(__VA_ARGS__); \
    printf("\n")
#endif

namespace {

XrActionSet
CreateActionSet(XrInstance instance, int priority, const char* name, const char* localizedName) {
    XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    asci.priority = priority;
    strcpy(asci.actionSetName, name);
    strcpy(asci.localizedActionSetName, localizedName);
    XrActionSet actionSet = XR_NULL_HANDLE;
    OXR(xrCreateActionSet(instance, &asci, &actionSet));
    return actionSet;
}

XrAction CreateAction(
    XrActionSet actionSet,
    XrActionType type,
    const char* actionName,
    const char* localizedName,
    int countSubactionPaths = 0,
    XrPath* subactionPaths = nullptr) {
    ALOGV("CreateAction %s, %" PRIi32, actionName, countSubactionPaths);

    XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = type;
    if (countSubactionPaths > 0) {
        aci.countSubactionPaths = countSubactionPaths;
        aci.subactionPaths = subactionPaths;
    }
    strcpy(aci.actionName, actionName);
    strcpy(aci.localizedActionName, localizedName ? localizedName : actionName);
    XrAction action = XR_NULL_HANDLE;
    OXR(xrCreateAction(actionSet, &aci, &action));
    return action;
}

XrActionSuggestedBinding
ActionSuggestedBinding(App& app, XrAction action, const char* bindingString) {
    XrActionSuggestedBinding asb;
    asb.action = action;
    XrPath bindingPath;
    OXR(xrStringToPath(app.Instance, bindingString, &bindingPath));
    asb.binding = bindingPath;
    return asb;
}

XrSpace CreateActionSpace(App& app, XrAction poseAction, XrPath subactionPath) {
    XrActionSpaceCreateInfo asci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asci.action = poseAction;
    asci.poseInActionSpace.orientation.w = 1.0f;
    asci.subactionPath = subactionPath;
    XrSpace actionSpace = XR_NULL_HANDLE;
    OXR(xrCreateActionSpace(app.Session, &asci, &actionSpace));
    return actionSpace;
}

XrActionStateBoolean GetActionStateBoolean(App& app, XrAction action) {
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;

    XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};

    OXR(xrGetActionStateBoolean(app.Session, &getInfo, &state));
    return state;
}

bool ActionPoseIsActive(App& app, XrAction action, XrPath subactionPath) {
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;

    XrActionStatePose state = {XR_TYPE_ACTION_STATE_POSE};
    OXR(xrGetActionStatePose(app.Session, &getInfo, &state));
    return state.isActive != XR_FALSE;
}

XrActionSet runningActionSet = XR_NULL_HANDLE;

XrAction aimPoseAction = XR_NULL_HANDLE;
XrAction gripPoseAction = XR_NULL_HANDLE;

XrAction boolAction = XR_NULL_HANDLE;
XrPath leftHandPath = XR_NULL_PATH;
XrPath rightHandPath = XR_NULL_PATH;
} // namespace

XrActionStateBoolean boolState;
XrActionStateBoolean leftTriggerState;
XrActionStateBoolean rightTriggerState;
XrActionStateVector2f leftThumbState;
XrActionStateVector2f rightThumbState;
XrActionStateBoolean rightBButtonState;
XrActionStateBoolean leftXButtonState;
XrActionStateBoolean rightAButtonState;
static XrAction captureButtonAction = XR_NULL_HANDLE;
static XrAction leftXButtonAction = XR_NULL_HANDLE;
static XrAction rightAButtonAction = XR_NULL_HANDLE;
static XrAction thumbstickAction = XR_NULL_HANDLE;

bool leftControllerActive = false;
bool rightControllerActive = false;

XrSpace leftControllerAimSpace = XR_NULL_HANDLE;
XrSpace rightControllerAimSpace = XR_NULL_HANDLE;
XrSpace leftControllerGripSpace = XR_NULL_HANDLE;
XrSpace rightControllerGripSpace = XR_NULL_HANDLE;

void AppInput_init(App& app) {
    // Actions
    runningActionSet =
        CreateActionSet(app.Instance, 1, "running_action_set", "Action Set used on main loop");
        // v0.3.13: boolAction moved below (needs handSubactionPaths for per-hand queries)


    OXR(xrStringToPath(app.Instance, "/user/hand/left", &leftHandPath));
    OXR(xrStringToPath(app.Instance, "/user/hand/right", &rightHandPath));
    XrPath handSubactionPaths[2] = {leftHandPath, rightHandPath};

    aimPoseAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", nullptr, 2, handSubactionPaths);

    boolAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "toggle", "Toggle", 2, handSubactionPaths);

    thumbstickAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", nullptr, 2, handSubactionPaths);

    captureButtonAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "capture_btn", "Capture Button");

    leftXButtonAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "left_x_btn", "Left X Button");

    rightAButtonAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "right_a_btn", "Right A Button");

    gripPoseAction = CreateAction(
        runningActionSet, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", nullptr, 2, handSubactionPaths);

    XrPath interactionProfilePath = XR_NULL_PATH;

    OXR(xrStringToPath(
        app.Instance, "/interaction_profiles/oculus/touch_controller", &interactionProfilePath));

    // Action creation
    {
        // Map bindings

        std::vector<XrActionSuggestedBinding> bindings;
        bindings.push_back(
            ActionSuggestedBinding(app, boolAction, "/user/hand/left/input/trigger"));
        bindings.push_back(
            ActionSuggestedBinding(app, boolAction, "/user/hand/right/input/trigger"));
        bindings.push_back(
            ActionSuggestedBinding(app, aimPoseAction, "/user/hand/left/input/aim/pose"));
        bindings.push_back(
            ActionSuggestedBinding(app, aimPoseAction, "/user/hand/right/input/aim/pose"));
        bindings.push_back(
            ActionSuggestedBinding(app, gripPoseAction, "/user/hand/left/input/grip/pose"));
        bindings.push_back(
            ActionSuggestedBinding(app, gripPoseAction, "/user/hand/right/input/grip/pose"));
        bindings.push_back(
            ActionSuggestedBinding(app, thumbstickAction, "/user/hand/left/input/thumbstick"));
        bindings.push_back(
            ActionSuggestedBinding(app, thumbstickAction, "/user/hand/right/input/thumbstick"));
        bindings.push_back(
            ActionSuggestedBinding(app, captureButtonAction, "/user/hand/right/input/b/click"));
        bindings.push_back(
            ActionSuggestedBinding(app, leftXButtonAction, "/user/hand/left/input/x/click"));
        bindings.push_back(
            ActionSuggestedBinding(app, rightAButtonAction, "/user/hand/right/input/a/click"));

        XrInteractionProfileSuggestedBinding suggestedBindings = {
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = interactionProfilePath;
        suggestedBindings.suggestedBindings = &bindings[0];
        suggestedBindings.countSuggestedBindings = bindings.size();
        OXR(xrSuggestInteractionProfileBindings(app.Instance, &suggestedBindings));

        // Attach to session
        XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &runningActionSet;
        OXR(xrAttachSessionActionSets(app.Session, &attachInfo));
    }
}

void AppInput_shutdown() {
    if (leftControllerAimSpace != XR_NULL_HANDLE) {
        OXR(xrDestroySpace(leftControllerAimSpace));
        OXR(xrDestroySpace(rightControllerAimSpace));
        OXR(xrDestroySpace(leftControllerGripSpace));
        OXR(xrDestroySpace(rightControllerGripSpace));
        leftControllerAimSpace = XR_NULL_HANDLE;
        rightControllerAimSpace = XR_NULL_HANDLE;
        leftControllerGripSpace = XR_NULL_HANDLE;
        rightControllerGripSpace = XR_NULL_HANDLE;
    }
}

void AppInput_syncActions(App& app) {
    // sync action data
    XrActiveActionSet activeActionSet = {};
    activeActionSet.actionSet = runningActionSet;
    activeActionSet.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    OXR(xrSyncActions(app.Session, &syncInfo));

    // query input action states
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.subactionPath = XR_NULL_PATH;

    boolState = GetActionStateBoolean(app, boolAction);

    if (leftControllerAimSpace == XR_NULL_HANDLE && app.SessionActive == true) {
        leftControllerAimSpace = CreateActionSpace(app, aimPoseAction, leftHandPath);
        rightControllerAimSpace = CreateActionSpace(app, aimPoseAction, rightHandPath);
        leftControllerGripSpace = CreateActionSpace(app, gripPoseAction, leftHandPath);
        rightControllerGripSpace = CreateActionSpace(app, gripPoseAction, rightHandPath);
    }

    leftControllerActive = ActionPoseIsActive(app, aimPoseAction, leftHandPath);
    rightControllerActive = ActionPoseIsActive(app, aimPoseAction, rightHandPath);

    // v0.3.13: per-hand trigger + thumbstick state
    {
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = boolAction;
        gi.subactionPath = leftHandPath;
        leftTriggerState = {XR_TYPE_ACTION_STATE_BOOLEAN};
        OXR(xrGetActionStateBoolean(app.Session, &gi, &leftTriggerState));
        gi.subactionPath = rightHandPath;
        rightTriggerState = {XR_TYPE_ACTION_STATE_BOOLEAN};
        OXR(xrGetActionStateBoolean(app.Session, &gi, &rightTriggerState));
        gi.action = thumbstickAction;
        gi.subactionPath = leftHandPath;
        leftThumbState = {XR_TYPE_ACTION_STATE_VECTOR2F};
        OXR(xrGetActionStateVector2f(app.Session, &gi, &leftThumbState));
        gi.subactionPath = rightHandPath;
        rightThumbState = {XR_TYPE_ACTION_STATE_VECTOR2F};
        OXR(xrGetActionStateVector2f(app.Session, &gi, &rightThumbState));

        gi.action = captureButtonAction;
        gi.subactionPath = XR_NULL_PATH;
        rightBButtonState = {XR_TYPE_ACTION_STATE_BOOLEAN};
        OXR(xrGetActionStateBoolean(app.Session, &gi, &rightBButtonState));

        gi.action = leftXButtonAction;
        gi.subactionPath = XR_NULL_PATH;
        leftXButtonState = {XR_TYPE_ACTION_STATE_BOOLEAN};
        OXR(xrGetActionStateBoolean(app.Session, &gi, &leftXButtonState));

        gi.action = rightAButtonAction;
        gi.subactionPath = XR_NULL_PATH;
        rightAButtonState = {XR_TYPE_ACTION_STATE_BOOLEAN};
        OXR(xrGetActionStateBoolean(app.Session, &gi, &rightAButtonState));
    }
}
