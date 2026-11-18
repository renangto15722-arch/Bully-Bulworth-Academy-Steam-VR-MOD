#include "vr_openxr.h"
#include "config.h"
#include "log.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_IMAGES 8

struct Chain
{
    XrSwapchain              sc;
    unsigned                 w, h;
    uint32_t                 count;
    XrSwapchainImageD3D11KHR images[MAX_IMAGES];
};

static struct
{
    XrInstance           inst;
    XrSystemId           sys;
    XrSession            session;
    XrSpace              space;
    XrSpace              view;       // the head, to place the screen
    XrPosef              screen;     // the cinema screen in `space`
    bool                 placed;
    XrSessionState       state;
    bool                 running;
    ID3D11Device*        dev;
    ID3D11DeviceContext* ctx;
    ID3D11Query*         done;
    int64_t              format;     // swapchain format (BGRA8, sRGB when offered)

    Chain screenChain, eyeChain[2], hudChain, menuChain;

    // the VR menu: a panel fixed in the world where the head looked when it opened
    ID3D11Texture2D* menuTex;
    unsigned         menuW, menuH;
    bool             menuOpen, menuPlaced;
    XrPosef          menuPose;

    // the HUD panel: given for one frame by VrXr_SetHud, follows the head's yaw
    ID3D11Texture2D* hud;
    unsigned         hudW, hudH;
    float            hudYaw;
    bool             hudYawSet;
    XrTime           hudTime;
    float            hudDist, hudWidth, hudDrop;
    float            hudDeadzone, hudFollowTime;   // degrees / seconds; 0 / 0 = turns with the head at once

    // a CPU picture for the cinema screen (VrXr_PresentImage)
    ID3D11Texture2D* imageTex;
    unsigned         imageW, imageH;

    // the open frame
    bool         frameOpen;
    XrFrameState fs;
    bool         viewsValid;
    XrView       views[2];

    float distance, width, height;   // the cinema screen, metres

    // controllers
    XrActionSet set;
    XrPath      handPath[2];
    XrAction    gripPose, aimPose;
    XrAction    trigger[2], squeeze[2], stick[2], stickClick[2], thumbrest[2];   // one per hand
    XrAction    btnA, btnB, btnX, btnY, menu;
    XrSpace     gripSpace[2], aimSpace[2];
    bool        inputValid;
    VrInput     input;
} x;

static bool Check(XrResult r, const char* what)
{
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (XR_SUCCEEDED(r))
        return true;
    if (x.inst)
        xrResultToString(x.inst, r, name);
    VrLog("openxr: %s failed: %d %s", what, (int)r, name);
    return false;
}

ID3D11Device* VrXr_Device()  { return x.dev; }
bool          VrXr_Running() { return x.running; }

// the adapter the runtime asks for (the one the headset is on)
static IDXGIAdapter1* FindAdapter(LUID luid)
{
    IDXGIFactory1* f = NULL;
    IDXGIAdapter1* a = NULL;
    UINT           i;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)))
        return NULL;
    for (i = 0; f->EnumAdapters1(i, &a) == S_OK; i++)
    {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        VrLog("openxr: adapter %u %ls, LUID %08lx:%08lx", i, d.Description, (unsigned long)d.AdapterLuid.HighPart,
              (unsigned long)d.AdapterLuid.LowPart);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart)
            break;
        a->Release();
        a = NULL;
    }
    f->Release();
    return a;
}

// ---- controllers ---------------------------------------------------------------------
static XrPath Path(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(x.inst, s, &p);
    return p;
}

static XrAction MakeAction(const char* name, const char* label, XrActionType type, bool perHand)
{
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    XrAction           a  = XR_NULL_HANDLE;
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, label);
    ci.actionType          = type;
    ci.countSubactionPaths = perHand ? 2 : 0;
    ci.subactionPaths      = perHand ? x.handPath : NULL;
    Check(xrCreateAction(x.set, &ci, &a), name);
    return a;
}

struct Binding
{
    XrAction*   action;
    const char* path;
};

static bool Suggest(const char* profile, const Binding* b, int n)
{
    XrActionSuggestedBinding sb[32];
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    int i;
    for (i = 0; i < n; i++)
    {
        sb[i].action  = *b[i].action;
        sb[i].binding = Path(b[i].path);
    }
    s.interactionProfile     = Path(profile);
    s.countSuggestedBindings = (uint32_t)n;
    s.suggestedBindings      = sb;
    return XR_SUCCEEDED(xrSuggestInteractionProfileBindings(x.inst, &s));
}

static bool CreateActions()
{
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    int                   i;

    strcpy(si.actionSetName, "bully");
    strcpy(si.localizedActionSetName, "Bully VR");
    if (!Check(xrCreateActionSet(x.inst, &si, &x.set), "xrCreateActionSet"))
        return false;
    x.handPath[0] = Path("/user/hand/left");
    x.handPath[1] = Path("/user/hand/right");

    x.gripPose   = MakeAction("grip_pose", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT, true);
    x.aimPose    = MakeAction("aim_pose", "Hand aim pose", XR_ACTION_TYPE_POSE_INPUT, true);
    // Separate actions per hand rather than subaction paths: some runtimes
    // (the simulator among them) give a shared action a single source.
    for (i = 0; i < 2; i++)
    {
        const char* side  = i ? "right" : "left";
        char        name[64], label[64];
        sprintf(name, "trigger_%s", side), sprintf(label, "Trigger (%s)", side);
        x.trigger[i] = MakeAction(name, label, XR_ACTION_TYPE_FLOAT_INPUT, false);
        sprintf(name, "squeeze_%s", side), sprintf(label, "Grip (%s)", side);
        x.squeeze[i] = MakeAction(name, label, XR_ACTION_TYPE_FLOAT_INPUT, false);
        sprintf(name, "stick_%s", side), sprintf(label, "Thumbstick (%s)", side);
        x.stick[i] = MakeAction(name, label, XR_ACTION_TYPE_VECTOR2F_INPUT, false);
        sprintf(name, "stick_click_%s", side), sprintf(label, "Thumbstick click (%s)", side);
        x.stickClick[i] = MakeAction(name, label, XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        sprintf(name, "thumbrest_%s", side), sprintf(label, "Thumbrest touch (%s)", side);
        x.thumbrest[i] = MakeAction(name, label, XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    }
    x.btnA       = MakeAction("button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    x.btnB       = MakeAction("button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    x.btnX       = MakeAction("button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    x.btnY       = MakeAction("button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    x.menu       = MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

    {
        const struct { XrAction* a; const char* p; } touch[] = {
            { &x.gripPose, "/user/hand/left/input/grip/pose" },     { &x.gripPose, "/user/hand/right/input/grip/pose" },
            { &x.aimPose, "/user/hand/left/input/aim/pose" },       { &x.aimPose, "/user/hand/right/input/aim/pose" },
            { &x.trigger[0], "/user/hand/left/input/trigger/value" },  { &x.trigger[1], "/user/hand/right/input/trigger/value" },
            { &x.squeeze[0], "/user/hand/left/input/squeeze/value" },  { &x.squeeze[1], "/user/hand/right/input/squeeze/value" },
            { &x.stick[0], "/user/hand/left/input/thumbstick" },       { &x.stick[1], "/user/hand/right/input/thumbstick" },
            { &x.stickClick[0], "/user/hand/left/input/thumbstick/click" },
            { &x.stickClick[1], "/user/hand/right/input/thumbstick/click" },
            { &x.thumbrest[0], "/user/hand/left/input/thumbrest/touch" },
            { &x.thumbrest[1], "/user/hand/right/input/thumbrest/touch" },
            { &x.btnA, "/user/hand/right/input/a/click" },          { &x.btnB, "/user/hand/right/input/b/click" },
            { &x.btnX, "/user/hand/left/input/x/click" },           { &x.btnY, "/user/hand/left/input/y/click" },
            { &x.menu, "/user/hand/left/input/menu/click" },
        };
        Binding b[32];
        int     n = (int)(sizeof(touch) / sizeof(touch[0]));
        for (i = 0; i < n; i++)
            b[i] = { touch[i].a, touch[i].p };
        VrLog("openxr: touch controller bindings %s",
              Suggest("/interaction_profiles/oculus/touch_controller", b, n) ? "ok" : "REJECTED");

        // Index: same layout without thumbrests (a/b on both hands -> left a/b = x/y)
        const struct { XrAction* a; const char* p; } index[] = {
            { &x.gripPose, "/user/hand/left/input/grip/pose" },     { &x.gripPose, "/user/hand/right/input/grip/pose" },
            { &x.aimPose, "/user/hand/left/input/aim/pose" },       { &x.aimPose, "/user/hand/right/input/aim/pose" },
            { &x.trigger[0], "/user/hand/left/input/trigger/value" },  { &x.trigger[1], "/user/hand/right/input/trigger/value" },
            { &x.squeeze[0], "/user/hand/left/input/squeeze/value" },  { &x.squeeze[1], "/user/hand/right/input/squeeze/value" },
            { &x.stick[0], "/user/hand/left/input/thumbstick" },       { &x.stick[1], "/user/hand/right/input/thumbstick" },
            { &x.stickClick[0], "/user/hand/left/input/thumbstick/click" },
            { &x.stickClick[1], "/user/hand/right/input/thumbstick/click" },
            { &x.btnA, "/user/hand/right/input/a/click" },          { &x.btnB, "/user/hand/right/input/b/click" },
            { &x.btnX, "/user/hand/left/input/a/click" },           { &x.btnY, "/user/hand/left/input/b/click" },
            { &x.menu, "/user/hand/left/input/system/click" },
        };
        n = (int)(sizeof(index) / sizeof(index[0]));
        for (i = 0; i < n; i++)
            b[i] = { index[i].a, index[i].p };
        VrLog("openxr: index controller bindings %s",
              Suggest("/interaction_profiles/valve/index_controller", b, n) ? "ok" : "REJECTED");
    }

    for (i = 0; i < 2; i++)
    {
        XrActionSpaceCreateInfo ai = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ai.subactionPath                      = x.handPath[i];
        ai.poseInActionSpace.orientation.w    = 1.0f;
        ai.action                             = x.gripPose;
        Check(xrCreateActionSpace(x.session, &ai, &x.gripSpace[i]), "xrCreateActionSpace (grip)");
        ai.action = x.aimPose;
        Check(xrCreateActionSpace(x.session, &ai, &x.aimSpace[i]), "xrCreateActionSpace (aim)");
    }

    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets      = &x.set;
    return Check(xrAttachSessionActionSets(x.session, &at), "xrAttachSessionActionSets");
}

static float GetFloat(XrAction a, int hand)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat   st = { XR_TYPE_ACTION_STATE_FLOAT };
    gi.action        = a;
    gi.subactionPath = hand >= 0 ? x.handPath[hand] : XR_NULL_PATH;
    return XR_SUCCEEDED(xrGetActionStateFloat(x.session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}

static bool GetBool(XrAction a, int hand)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    gi.action        = a;
    gi.subactionPath = hand >= 0 ? x.handPath[hand] : XR_NULL_PATH;
    return XR_SUCCEEDED(xrGetActionStateBoolean(x.session, &gi, &st)) && st.isActive && st.currentState;
}

static bool LocatePose(XrSpace s, XrTime t, float pos[3], float quat[4])
{
    XrSpaceLocation            loc  = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(s, x.space, t, &loc)) || (loc.locationFlags & need) != need)
        return false;
    pos[0]  = loc.pose.position.x;
    pos[1]  = loc.pose.position.y;
    pos[2]  = loc.pose.position.z;
    quat[0] = loc.pose.orientation.x;
    quat[1] = loc.pose.orientation.y;
    quat[2] = loc.pose.orientation.z;
    quat[3] = loc.pose.orientation.w;
    return true;
}

static void SyncInput(XrTime t)
{
    XrActiveActionSet aas = { x.set, XR_NULL_PATH };
    XrActionsSyncInfo si  = { XR_TYPE_ACTIONS_SYNC_INFO };
    VrInput&          in  = x.input;
    int               i;
    static bool       logged[2];

    if (!x.set)
        return;
    si.countActiveActionSets = 1;
    si.activeActionSets      = &aas;
    if (xrSyncActions(x.session, &si) != XR_SUCCESS)   // SESSION_NOT_FOCUSED: keep the last state
        return;
    for (i = 0; i < 2; i++)
    {
        VrHand& h = in.hand[i];
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };

        h.valid = LocatePose(x.gripSpace[i], t, h.gripPos, h.gripQuat) &&
                  LocatePose(x.aimSpace[i], t, h.aimPos, h.aimQuat);
        h.trigger    = GetFloat(x.trigger[i], -1);
        h.squeeze    = GetFloat(x.squeeze[i], -1);
        h.stickClick = GetBool(x.stickClick[i], -1);
        h.thumbrest  = GetBool(x.thumbrest[i], -1);
        gi.action        = x.stick[i];
        gi.subactionPath = XR_NULL_PATH;
        h.stick[0] = h.stick[1] = 0.0f;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(x.session, &gi, &v)) && v.isActive)
        {
            h.stick[0] = v.currentState.x;
            h.stick[1] = v.currentState.y;
        }
        if (h.valid && !logged[i])
        {
            VrLog("openxr: %s controller tracked, grip at %.2f %.2f %.2f", i ? "right" : "left", h.gripPos[0],
                  h.gripPos[1], h.gripPos[2]);
            logged[i] = true;
        }
    }
    in.a    = GetBool(x.btnA, -1);
    in.b    = GetBool(x.btnB, -1);
    in.x    = GetBool(x.btnX, -1);
    in.y    = GetBool(x.btnY, -1);
    in.menu = GetBool(x.menu, -1);
    x.inputValid = true;
}

bool VrXr_GetInput(VrInput* in)
{
    if (!x.inputValid)
        return false;
    *in = x.input;
    return true;
}

bool VrXr_Init()
{
    char        buf[MAX_PATH];
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    // Bully only starts through Steam, so an environment variable set in a
    // shell never reaches it: the simulator is chosen here instead.
    IniStr("VR", "RuntimeJson", "", buf, sizeof(buf));
    if (buf[0])
    {
        SetEnvironmentVariableA("XR_RUNTIME_JSON", buf);
        VrLog("openxr: runtime from BullyVR.ini: %s", buf);
    }
    IniStr("VR", "SimDir", "", buf, sizeof(buf));
    if (buf[0])
        SetEnvironmentVariableA("BVR_XRSIM_DIR", buf);
    x.distance = IniFloat("Cinema", "Distance", 2.5f);
    x.width    = IniFloat("Cinema", "Width", 3.2f);
    x.height   = IniFloat("Cinema", "Height", 0.0f);
    x.hudDist  = IniFloat("HUD", "Distance", 1.2f);
    x.hudWidth = IniFloat("HUD", "Width", 1.5f);
    x.hudDrop  = IniFloat("HUD", "Drop", 0.15f);
    Opt_Bind("Cinema", "Distance", &x.distance);
    Opt_Bind("Cinema", "Width", &x.width);
    Opt_Bind("HUD", "Distance", &x.hudDist);
    Opt_Bind("HUD", "Width", &x.hudWidth);
    Opt_Bind("HUD", "Drop", &x.hudDrop);
    x.hudDeadzone   = IniFloat("HUD", "FollowDeadzone", 0.0f);
    x.hudFollowTime = IniFloat("HUD", "FollowTime", 0.0f);
    Opt_Bind("HUD", "FollowDeadzone", &x.hudDeadzone);
    Opt_Bind("HUD", "FollowTime", &x.hudFollowTime);

    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "Bully VR PC");
    strcpy(ici.applicationInfo.engineName, "Gamebryo");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount      = 1;
    ici.enabledExtensionNames      = exts;
    if (!Check(xrCreateInstance(&ici, &x.inst), "xrCreateInstance"))
        return false;

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(x.inst, &ip);
    VrLog("openxr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
          XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(x.inst, &sgi, &x.sys), "xrGetSystem (headset connected?)"))
        return false;

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = NULL;
    XrGraphicsRequirementsD3D11KHR        req    = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    xrGetInstanceProcAddr(x.inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    if (!getReq || !Check(getReq(x.inst, x.sys, &req), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    IDXGIAdapter1*          adapter  = FindAdapter(req.adapterLuid);
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &x.dev, NULL, &x.ctx);
    if (adapter)
        adapter->Release();
    if (FAILED(hr))
    {
        VrLog("openxr: D3D11CreateDevice failed %08lx", (unsigned long)hr);
        return false;
    }
    VrLog("openxr: D3D11 device on LUID %08lx:%08lx%s", (unsigned long)req.adapterLuid.HighPart,
          (unsigned long)req.adapterLuid.LowPart, adapter ? "" : " (adapter not found, default used)");
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    x.dev->CreateQuery(&qd, &x.done);

    XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = x.dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next     = &binding;
    sci.systemId = x.sys;
    if (!Check(xrCreateSession(x.inst, &sci, &x.session), "xrCreateSession"))
        return false;

    XrReferenceSpaceCreateInfo rsi = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsi.referenceSpaceType                 = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(x.session, &rsi, &x.space), "xrCreateReferenceSpace"))
        return false;
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(x.session, &rsi, &x.view), "xrCreateReferenceSpace (view)"))
        return false;

    if (!CreateActions())
        VrLog("openxr: controllers unavailable (picture only)");

    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t                nv     = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(x.inst, x.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv)))
        VrLog("openxr: recommended eye size %ux%u", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);
    VrLog("openxr: session created, cinema screen %.2f m wide at %.2f m", x.width, x.distance);
    return true;
}

static void PollEvents()
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };

    while (xrPollEvent(x.inst, &ev) == XR_SUCCESS)
    {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
            x.state = e->state;
            VrLog("openxr: session state %d", (int)x.state);
            if (x.state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                x.running = Check(xrBeginSession(x.session, &bi), "xrBeginSession");
            }
            else if (x.state == XR_SESSION_STATE_STOPPING)
            {
                if (x.frameOpen)
                {
                    // a begun frame must be ended before the session is
                    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
                    ei.displayTime          = x.fs.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    xrEndFrame(x.session, &ei);
                    x.frameOpen = false;
                }
                xrEndSession(x.session);
                x.running = false;
            }
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static int64_t PickFormat()
{
    int64_t  formats[64], fmt = 0;
    uint32_t n = 0, i;

    if (x.format)
        return x.format;
    xrEnumerateSwapchainFormats(x.session, 64, &n, formats);
    // the game's bytes are sRGB-encoded: an _SRGB view shows them as they are
    for (i = 0; i < n && !fmt; i++)
        if (formats[i] == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            fmt = formats[i];
    for (i = 0; i < n && !fmt; i++)
        if (formats[i] == DXGI_FORMAT_B8G8R8A8_UNORM)
            fmt = formats[i];
    if (!fmt)
        VrLog("openxr: the runtime offers no BGRA8 swapchain format");
    x.format = fmt;
    return fmt;
}

static bool EnsureChain(Chain* c, unsigned w, unsigned h, const char* name)
{
    uint32_t i;
    int64_t  fmt = PickFormat();

    if (c->sc && c->w == w && c->h == h)
        return true;
    if (c->sc)
        xrDestroySwapchain(c->sc);
    c->sc = XR_NULL_HANDLE;
    if (!fmt)
        return false;

    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format      = fmt;
    ci.sampleCount = 1;
    ci.width       = w;
    ci.height      = h;
    ci.faceCount   = 1;
    ci.arraySize   = 1;
    ci.mipCount    = 1;
    if (!Check(xrCreateSwapchain(x.session, &ci, &c->sc), "xrCreateSwapchain"))
        return false;
    for (i = 0; i < MAX_IMAGES; i++)
        c->images[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
    xrEnumerateSwapchainImages(c->sc, MAX_IMAGES, &c->count, (XrSwapchainImageBaseHeader*)c->images);
    c->w = w;
    c->h = h;
    VrLog("openxr: %s swapchain %ux%u, format %d, %u images", name, w, h, (int)fmt, c->count);
    return true;
}

// src -> the chain's next image (sizes equal)
static bool Fill(Chain* c, ID3D11Texture2D* src)
{
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    uint32_t                 index;

    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(c->sc, NULL, &index)))
        return false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(c->sc, &wi)))
        x.ctx->CopyResource(c->images[index].texture, src);
    xrReleaseSwapchainImage(c->sc, NULL);
    return true;
}

// The screen goes where the head looks when the picture first shows: in
// front of it at eye height, upright, facing it. Where the LOCAL origin sits
// (at the head, or on the floor) is up to the runtime, so it is not assumed.
static bool PlaceScreen(XrTime time)
{
    XrSpaceLocation            loc  = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    float                      qx, qy, qz, qw, fx, fz, len, yaw;

    if (XR_FAILED(xrLocateSpace(x.view, x.space, time, &loc)) || (loc.locationFlags & need) != need)
        return false;
    qx = loc.pose.orientation.x;
    qy = loc.pose.orientation.y;
    qz = loc.pose.orientation.z;
    qw = loc.pose.orientation.w;
    // the head's forward (-Z) flattened onto the floor
    fx  = -2.0f * (qx * qz + qw * qy);
    fz  = -(1.0f - 2.0f * (qx * qx + qy * qy));
    len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-3f)
    {
        fx  = 0.0f;
        fz  = -1.0f;
        len = 1.0f;
    }
    fx /= len;
    fz /= len;
    yaw = atan2f(-fx, -fz);   // turn about +Y that takes -Z to (fx, 0, fz)
    x.screen.orientation = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    x.screen.position    = { loc.pose.position.x + fx * x.distance, loc.pose.position.y + x.height,
                             loc.pose.position.z + fz * x.distance };
    x.placed = true;
    VrLog("openxr: screen placed at %.2f %.2f %.2f, yaw %.0f deg", x.screen.position.x, x.screen.position.y,
          x.screen.position.z, yaw * 57.29578f);
    return true;
}

bool VrXr_EyeViews(VrEyeView eyes[2])
{
    int i;
    if (!x.frameOpen || !x.viewsValid)
        return false;
    for (i = 0; i < 2; i++)
    {
        const XrView& v = x.views[i];
        eyes[i].pos[0]     = v.pose.position.x;
        eyes[i].pos[1]     = v.pose.position.y;
        eyes[i].pos[2]     = v.pose.position.z;
        eyes[i].quat[0]    = v.pose.orientation.x;
        eyes[i].quat[1]    = v.pose.orientation.y;
        eyes[i].quat[2]    = v.pose.orientation.z;
        eyes[i].quat[3]    = v.pose.orientation.w;
        eyes[i].angleLeft  = v.fov.angleLeft;
        eyes[i].angleRight = v.fov.angleRight;
        eyes[i].angleUp    = v.fov.angleUp;
        eyes[i].angleDown  = v.fov.angleDown;
    }
    return true;
}

// The HUD panel in front of the head, below the eyes, upright. Its yaw trails
// the head's: it stays put for small looks (within 20 deg) and swings after
// bigger ones (~0.3 s), so a glance at a corner of it does not move it away.
static bool PlaceHud(XrTime time, XrPosef* pose)
{
    XrSpaceLocation            loc  = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    float                      qx, qy, qz, qw, fx, fz, yaw, d, dt;

    if (XR_FAILED(xrLocateSpace(x.view, x.space, time, &loc)) || (loc.locationFlags & need) != need)
        return false;
    qx = loc.pose.orientation.x;
    qy = loc.pose.orientation.y;
    qz = loc.pose.orientation.z;
    qw = loc.pose.orientation.w;
    fx = -2.0f * (qx * qz + qw * qy);
    fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    if (fx * fx + fz * fz < 1e-6f)
        fx = 0.0f, fz = -1.0f;
    yaw = atan2f(-fx, -fz);
    dt  = x.hudYawSet && x.hudTime ? (float)((time - x.hudTime) * 1e-9) : 0.0f;
    if (!x.hudYawSet || dt > 1.0f)
        x.hudYaw = yaw, x.hudYawSet = true;
    d = yaw - x.hudYaw;
    while (d > 3.14159265f)
        d -= 6.2831853f;
    while (d < -3.14159265f)
        d += 6.2831853f;
    if (fabsf(d) > x.hudDeadzone * 0.01745329f)
        x.hudYaw += d * (x.hudFollowTime <= 0.0f || dt >= x.hudFollowTime ? 1.0f : dt / x.hudFollowTime);
    x.hudTime = time;
    pose->orientation = { 0.0f, sinf(x.hudYaw * 0.5f), 0.0f, cosf(x.hudYaw * 0.5f) };
    pose->position    = { loc.pose.position.x - sinf(x.hudYaw) * x.hudDist, loc.pose.position.y - x.hudDrop,
                          loc.pose.position.z - cosf(x.hudYaw) * x.hudDist };
    return true;
}

// the menu: in front of the head at eye height (a little below), facing it,
// fixed where it was put until it closes
static bool PlaceMenu(XrTime time)
{
    XrSpaceLocation            loc  = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    float                      qx, qy, qz, qw, fx, fz, len, yaw;

    if (XR_FAILED(xrLocateSpace(x.view, x.space, time, &loc)) || (loc.locationFlags & need) != need)
        return false;
    qx = loc.pose.orientation.x;
    qy = loc.pose.orientation.y;
    qz = loc.pose.orientation.z;
    qw = loc.pose.orientation.w;
    fx  = -2.0f * (qx * qz + qw * qy);
    fz  = -(1.0f - 2.0f * (qx * qx + qy * qy));
    len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-3f)
        fx = 0.0f, fz = -1.0f, len = 1.0f;
    fx /= len;
    fz /= len;
    yaw = atan2f(-fx, -fz);
    x.menuPose.orientation = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    x.menuPose.position    = { loc.pose.position.x + fx * 1.0f, loc.pose.position.y - 0.10f,
                               loc.pose.position.z + fz * 1.0f };
    x.menuPlaced = true;
    return true;
}

void VrXr_SetMenu(const void* bgra, unsigned w, unsigned h, bool changed)
{
    if (!bgra)
    {
        x.menuOpen   = false;
        x.menuPlaced = false;
        return;
    }
    if (!x.dev)
        return;
    if (x.menuTex && (x.menuW != w || x.menuH != h))
    {
        x.menuTex->Release();
        x.menuTex = NULL;
    }
    if (!x.menuTex)
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        if (FAILED(x.dev->CreateTexture2D(&d, NULL, &x.menuTex)))
        {
            VrLog("openxr: menu texture %ux%u failed", w, h);
            x.menuTex = NULL;
            return;
        }
        x.menuW = w;
        x.menuH = h;
        changed = true;
    }
    if (changed)
        x.ctx->UpdateSubresource(x.menuTex, 0, NULL, bgra, w * 4, 0);
    x.menuOpen = true;
}

void VrXr_PresentImage(const void* bgra, unsigned w, unsigned h)
{
    if (!x.dev || !bgra)
        return;
    if (x.imageTex && (x.imageW != w || x.imageH != h))
    {
        x.imageTex->Release();
        x.imageTex = NULL;
    }
    if (!x.imageTex)
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        if (FAILED(x.dev->CreateTexture2D(&d, NULL, &x.imageTex)))
        {
            x.imageTex = NULL;
            return;
        }
        x.imageW = w;
        x.imageH = h;
    }
    x.ctx->UpdateSubresource(x.imageTex, 0, NULL, bgra, w * 4, 0);
    VrXr_Present(x.imageTex, w, h, NULL, 0, 0);
}

void VrXr_ReplaceScreen()
{
    x.placed = false;
}

void VrXr_SetHud(ID3D11Texture2D* hud, unsigned w, unsigned h)
{
    x.hud  = hud;
    x.hudW = w;
    x.hudH = h;
}

static void EndFrame(ID3D11Texture2D* screen, unsigned sw, unsigned sh, ID3D11Texture2D* const* eyes, unsigned ew,
                     unsigned eh)
{
    XrCompositionLayerProjection        proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView    pv[2];
    XrCompositionLayerQuad              quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad              hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad              menuQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t                            count = 0;
    bool                                filled = false;
    int                                 i;

    if (x.fs.shouldRender)
    {
        if (eyes && eyes[0] && eyes[1] && x.viewsValid && EnsureChain(&x.eyeChain[0], ew, eh, "left eye") &&
            EnsureChain(&x.eyeChain[1], ew, eh, "right eye"))
        {
            for (i = 0; i < 2; i++)
            {
                pv[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                Fill(&x.eyeChain[i], eyes[i]);
                // exactly the pose and field of view the eye was rendered with
                pv[i].pose                      = x.views[i].pose;
                pv[i].fov                       = x.views[i].fov;
                pv[i].subImage.swapchain        = x.eyeChain[i].sc;
                pv[i].subImage.imageRect.extent = { (int32_t)ew, (int32_t)eh };
            }
            proj.space     = x.space;
            proj.viewCount = 2;
            proj.views     = pv;
            layers[count++] = (const XrCompositionLayerBaseHeader*)&proj;
            filled = true;
            if (x.hud && PlaceHud(x.fs.predictedDisplayTime, &hudQuad.pose) &&
                EnsureChain(&x.hudChain, x.hudW, x.hudH, "hud"))
            {
                Fill(&x.hudChain, x.hud);
                hudQuad.layerFlags                = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudQuad.space                     = x.space;
                hudQuad.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
                hudQuad.subImage.swapchain        = x.hudChain.sc;
                hudQuad.subImage.imageRect.extent = { (int32_t)x.hudW, (int32_t)x.hudH };
                hudQuad.size                      = { x.hudWidth, x.hudWidth * (float)x.hudH / (float)x.hudW };
                layers[count++] = (const XrCompositionLayerBaseHeader*)&hudQuad;
            }
        }
        else if (screen && (x.placed || PlaceScreen(x.fs.predictedDisplayTime)) &&
                 EnsureChain(&x.screenChain, sw, sh, "screen"))
        {
            Fill(&x.screenChain, screen);
            quad.space                     = x.space;
            quad.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain        = x.screenChain.sc;
            quad.subImage.imageRect.extent = { (int32_t)sw, (int32_t)sh };
            quad.pose                      = x.screen;
            quad.size                      = { x.width, x.width * (float)sh / (float)sw };
            layers[count++] = (const XrCompositionLayerBaseHeader*)&quad;
            filled = true;
        }
        // the VR menu over everything, in stereo and on the cinema screen alike
        if (x.menuOpen && x.menuTex && (x.menuPlaced || PlaceMenu(x.fs.predictedDisplayTime)) &&
            EnsureChain(&x.menuChain, x.menuW, x.menuH, "menu"))
        {
            Fill(&x.menuChain, x.menuTex);
            menuQuad.space                     = x.space;
            menuQuad.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
            menuQuad.subImage.swapchain        = x.menuChain.sc;
            menuQuad.subImage.imageRect.extent = { (int32_t)x.menuW, (int32_t)x.menuH };
            menuQuad.pose                      = x.menuPose;
            menuQuad.size                      = { 0.9f, 0.9f * (float)x.menuH / (float)x.menuW };
            layers[count++] = (const XrCompositionLayerBaseHeader*)&menuQuad;
        }
        if (filled)
        {
            // The shared textures are written by Vulkan again next frame: the
            // copies out of them must be finished before that.
            x.ctx->End(x.done);
            x.ctx->Flush();
            while (x.ctx->GetData(x.done, NULL, 0, 0) == S_FALSE)
                YieldProcessor();
        }
    }

    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime          = x.fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount           = count;
    ei.layers               = layers;
    Check(xrEndFrame(x.session, &ei), "xrEndFrame");
    x.frameOpen = false;
    x.hud       = NULL;
}

static void BeginFrame()
{
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    XrViewState      vs = { XR_TYPE_VIEW_STATE };
    uint32_t         n  = 0;
    const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;

    x.fs = { XR_TYPE_FRAME_STATE };
    if (!Check(xrWaitFrame(x.session, NULL, &x.fs), "xrWaitFrame") || !Check(xrBeginFrame(x.session, NULL), "xrBeginFrame"))
        return;
    x.frameOpen = true;

    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime           = x.fs.predictedDisplayTime;
    li.space                 = x.space;
    x.views[0] = { XR_TYPE_VIEW };
    x.views[1] = { XR_TYPE_VIEW };
    x.viewsValid = XR_SUCCEEDED(xrLocateViews(x.session, &li, &vs, 2, &n, x.views)) && n == 2 &&
                   (vs.viewStateFlags & need) == need;
    SyncInput(x.fs.predictedDisplayTime);
}

void VrXr_Present(ID3D11Texture2D* screen, unsigned screenW, unsigned screenH, ID3D11Texture2D* const* eyes,
                  unsigned eyeW, unsigned eyeH)
{
    if (!x.session)
        return;
    PollEvents();
    if (x.frameOpen)
        EndFrame(screen, screenW, screenH, eyes, eyeW, eyeH);
    if (x.running)
        BeginFrame();
}
