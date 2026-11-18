// VR menu: both grips + Y opens / closes it. A panel in front of the head
// (OpenXR quad, src\vr_openxr.cpp) drawn here with GDI.
//   stick up/down    select            stick left/right  change the value
//   A                toggle / run      triggers          previous / next page
//   B                close
// Every change goes live at once (the modules bound their variables with
// Opt_Bind) and is written to BullyVR.ini right away. While the menu is open
// the game gets no controller input (input.cpp asks Menu_IsOpen).
#include "menu.h"
#include "config.h"
#include "fight.h"
#include "log.h"
#include "stereo.h"
#include "vr_openxr.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum Kind
{
    K_BOOL,
    K_FLOAT,
    K_ACTION,
};

enum Action
{
    A_NONE,
    A_RECENTRE,
    A_DEFAULTS,
    A_DUMMY,
};

struct Item
{
    const char* label;
    Kind        kind;
    const char* section;
    const char* key;
    float       def, min, max, step;
    float       scale;       // variable = shown value * scale (HandPitch: degrees -> radians)
    const char* fmt;         // shown value
    Action      action;
    bool        replaceScreen;
};

static const Item kBody[] = {
    { "First person (Jimmy's eyes)", K_BOOL, "Body", "FirstPerson", 1 },
    { "Body faces the headset", K_BOOL, "Body", "BodyFollowsHeadset", 1 },
    { "Walk without turning (strafe)", K_BOOL, "Body", "StrafeWalk", 1 },
    { "Still torso (no sway)", K_BOOL, "Body", "TorsoLock", 1 },
    { "Square the torso (auto)", K_BOOL, "Body", "TorsoSquare", 1 },
    { "Torso turn, waist up (+ = left)", K_FLOAT, "Body", "TorsoTurn", 0.0f, -15.0f, 15.0f, 1.0f, 1.0f, "%+.0f deg" },
    { "Torso leans with the head", K_BOOL, "Body", "TorsoLean", 1 },
    { "Lean settles after", K_FLOAT, "Body", "LeanSettle", 2.0f, 0.0f, 10.0f, 0.5f, 1.0f, "%.1f s" },
    { "Hide Jimmy's head", K_BOOL, "Body", "HideHeadMeshes", 1 },
    { "Eyes in front of the head", K_FLOAT, "Body", "EyeForward", 0.10f, 0.0f, 0.30f, 0.01f, 1.0f, "%.2f m" },
    { "Eyes above the head joint", K_FLOAT, "Body", "EyeUp", 0.08f, -0.10f, 0.20f, 0.01f, 1.0f, "%.2f m" },
    { "Reset this page to defaults", K_ACTION, NULL, NULL, 0, 0, 0, 0, 0, NULL, A_DEFAULTS },
};

static const Item kHands[] = {
    { "Arms follow the controllers (IK)", K_BOOL, "Body", "IK", 1 },
    { "Arm animation (game's)", K_BOOL, "Body", "ArmAnimation", 0 },
    { "Wrist behind the controller", K_FLOAT, "Body", "HandBack", 0.06f, -0.10f, 0.25f, 0.01f, 1.0f, "%.2f m" },
    { "Hand up / down", K_FLOAT, "Body", "HandUp", 0.0f, -0.15f, 0.15f, 0.01f, 1.0f, "%.2f m" },
    { "Hand pitch", K_FLOAT, "Body", "HandPitch", 0.0f, -180.0f, 180.0f, 5.0f, 0.01745329f, "%.0f deg" },
    { "Arm stretch limit", K_FLOAT, "Body", "ArmStretch", 1.5f, 1.0f, 2.0f, 0.05f, 1.0f, "x %.2f" },
    { "Extra reach, arm held out (punch / grab)", K_FLOAT, "Body", "ReachBonus", 0.20f, 0.0f, 0.50f, 0.05f, 1.0f, "%.2f m" },
    { "Reset this page to defaults", K_ACTION, NULL, NULL, 0, 0, 0, 0, 0, NULL, A_DEFAULTS },
};
static const Item kView[] = {
    { "Frame rate limit (0 = unlimited)", K_FLOAT, "Display", "FpsLimit", 0.0f, 0.0f, 240.0f, 5.0f, 1.0f, "%.0f fps" },
    { "Monitor shows the headset view", K_BOOL, "Display", "MirrorHeadset", 1 },
    { "Game camera at the head", K_BOOL, "Body", "GameCameraAtHead", 1 },
    { "Game camera FOV", K_FLOAT, "Body", "GameCameraFov", 100.0f, 60.0f, 140.0f, 5.0f, 1.0f, "%.0f deg" },
    { "World scale", K_FLOAT, "VR", "WorldScale", 1.0f, 0.5f, 2.0f, 0.05f, 1.0f, "x %.2f" },
    { "Snap turn", K_FLOAT, "Controls", "SnapDegrees", 30.0f, 15.0f, 90.0f, 15.0f, 1.0f, "%.0f deg" },
    { "Eye near plane", K_FLOAT, "Stereo", "EyeNear", 0.10f, 0.02f, 0.30f, 0.01f, 1.0f, "%.2f m" },
    { "HUD on its own panel", K_BOOL, "HUD", "Panel", 1 },
    { "HUD panel distance", K_FLOAT, "HUD", "Distance", 1.2f, 0.5f, 3.0f, 0.1f, 1.0f, "%.1f m" },
    { "HUD panel width", K_FLOAT, "HUD", "Width", 1.5f, 0.5f, 3.0f, 0.1f, 1.0f, "%.1f m" },
    { "HUD panel below the eyes", K_FLOAT, "HUD", "Drop", 0.15f, -0.5f, 0.8f, 0.05f, 1.0f, "%.2f m" },
    { "HUD turn dead zone", K_FLOAT, "HUD", "FollowDeadzone", 0.0f, 0.0f, 45.0f, 5.0f, 1.0f, "%.0f deg" },
    { "HUD turn delay", K_FLOAT, "HUD", "FollowTime", 0.0f, 0.0f, 1.0f, 0.05f, 1.0f, "%.2f s" },
    { "Cinema screen distance", K_FLOAT, "Cinema", "Distance", 2.5f, 1.0f, 6.0f, 0.25f, 1.0f, "%.2f m", A_NONE, true },
    { "Cinema screen width", K_FLOAT, "Cinema", "Width", 3.2f, 1.0f, 8.0f, 0.25f, 1.0f, "%.2f m", A_NONE, true },
    { "Recentre the view", K_ACTION, NULL, NULL, 0, 0, 0, 0, 0, NULL, A_RECENTRE },
    { "Reset this page to defaults", K_ACTION, NULL, NULL, 0, 0, 0, 0, 0, NULL, A_DEFAULTS },
};

static const Item kFight[] = {
    { "Punch with your fists (physical hits)", K_BOOL, "Fight", "PhysicalPunches", 1 },
    { "Punch speed needed", K_FLOAT, "Fight", "MinSpeed", 2.0f, 1.0f, 5.0f, 0.25f, 1.0f, "%.2f m/s" },
    { "Punch strength", K_FLOAT, "Fight", "Strength", 1.0f, 0.25f, 3.0f, 0.25f, 1.0f, "x %.2f" },
    { "Fists up at the face = block", K_BOOL, "Fight", "GuardGesture", 1 },
    { "Grab / push / hit the held kid with your hands", K_BOOL, "Fight", "WrestleGestures", 1 },
    { "Held kid never breaks free (let go with the grips)", K_BOOL, "Fight", "KeepHold", 1 },
    { "Test dummy: nearest NPC in front (on/off)", K_ACTION, NULL, NULL, 0, 0, 0, 0, 0, NULL, A_DUMMY },
    { "Reset this page to defaults", K_ACTION, NULL, NULL, 0, 0, 0, 0, 0, NULL, A_DEFAULTS },
};
static const char* const kGuide[] = {
    "Left stick: walk (forward = where you look)",
    "A: sprint     B: attack     X: jump / back     Y: action",
    "Menu button: pause (Start)",
    "Left trigger: lock-on (L1)     Right trigger: R1",
    "Hand on someone + its grip: grab (that arm holds, the other is free)",
    "Holding: that grip + push forward + release = push; grip + release = let go",
    "Holding: punch with the free hand = hit him",
    "Right stick left / right: snap turn     up: jump",
    "Right thumbrest + left stick: Y / A / X / B (up / down / left / right)",
    "Left thumbrest + right stick: d-pad",
    "Stick clicks: L3 / R3",
    "Punch with your real fists     Fists up at the face: block",
    "Both grips + X: recentre     Both grips + Y: this menu",
};

struct Page
{
    const char* title;
    const Item* items;
    int         count;
};

static const Page kPages[] = {
    { "Body", kBody, (int)(sizeof(kBody) / sizeof(kBody[0])) },
    { "Hands", kHands, (int)(sizeof(kHands) / sizeof(kHands[0])) },
    { "View", kView, (int)(sizeof(kView) / sizeof(kView[0])) },
    { "Fight", kFight, (int)(sizeof(kFight) / sizeof(kFight[0])) },
    { "Controls guide", NULL, 0 },
};
static const int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));

enum
{
    W = 1024,
    H = 1024,
};

static bool     s_open, s_dirty = true;
static bool     s_swallow;                    // closed, but the buttons that closed it are still down
static int      s_page, s_sel;
static HDC      s_dc;
static HBITMAP  s_bmp;
static void*    s_bits;
static HFONT    s_font, s_bold, s_small;
static bool     s_comboHeld;                  // grips + Y still down from the last toggle
static bool     s_prevA, s_prevB, s_prevLT, s_prevRT;
static int      s_repeatDir;                  // stick direction held: 1 up 2 down 3 left 4 right
static DWORD    s_repeatAt;

bool Menu_IsOpen()
{
    return s_open || s_swallow;
}

// ---- values ---------------------------------------------------------------------------
static bool GetValue(const Item& it, float* v)
{
    void* p = Opt_Ptr(it.section, it.key);
    if (!p)
        return false;
    if (it.kind == K_BOOL)
        *v = *(bool*)p ? 1.0f : 0.0f;
    else
        *v = *(float*)p / it.scale;
    return true;
}

static void SetValue(const Item& it, float v)
{
    void* p = Opt_Ptr(it.section, it.key);
    char  buf[32];
    if (!p)
        return;
    if (it.kind == K_BOOL)
    {
        *(bool*)p = v != 0.0f;
        IniWrite(it.section, it.key, v != 0.0f ? "1" : "0");
    }
    else
    {
        if (v < it.min)
            v = it.min;
        if (v > it.max)
            v = it.max;
        v = floorf(v / it.step + 0.5f) * it.step;   // no 0.30000001 in the ini
        *(float*)p = v * it.scale;
        _snprintf(buf, sizeof(buf), "%g", v);
        buf[sizeof(buf) - 1] = 0;
        IniWrite(it.section, it.key, buf);
    }
    if (it.replaceScreen)
        VrXr_ReplaceScreen();
    VrLog("[menu] [%s] %s = %g", it.section, it.key, v);
}

static void Run(const Page& pg, const Item& it)
{
    int i;
    if (it.action == A_RECENTRE)
    {
        Stereo_Recentre();
        VrXr_ReplaceScreen();
        VrLog("[menu] recentre");
    }
    else if (it.action == A_DUMMY)
    {
        Fight_ToggleDummy();
        VrLog("[menu] test dummy %s", Fight_DummyActive() ? "on" : "off");
    }
    else if (it.action == A_DEFAULTS)
    {
        for (i = 0; i < pg.count; i++)
            if (pg.items[i].kind != K_ACTION)
                SetValue(pg.items[i], pg.items[i].def);
        VrLog("[menu] page \"%s\" back to defaults", pg.title);
    }
}

// ---- drawing --------------------------------------------------------------------------
static bool EnsureCanvas()
{
    BITMAPINFO bi;
    if (s_dc)
        return true;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;   // top row first
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_dc  = CreateCompatibleDC(NULL);
    s_bmp = s_dc ? CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, &s_bits, NULL, 0) : NULL;
    if (!s_bmp)
    {
        VrLog("[menu] canvas failed");
        if (s_dc)
            DeleteDC(s_dc);
        s_dc = NULL;
        return false;
    }
    SelectObject(s_dc, s_bmp);
    s_font  = CreateFontA(34, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    s_bold  = CreateFontA(40, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    s_small = CreateFontA(28, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    SetBkMode(s_dc, TRANSPARENT);
    return true;
}

static void Fill(int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT   r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(s_dc, &r, b);
    DeleteObject(b);
}

static void Text(int x0, int y0, int x1, int y1, const char* s, HFONT f, COLORREF c, UINT align)
{
    RECT r = { x0, y0, x1, y1 };
    SelectObject(s_dc, f);
    SetTextColor(s_dc, c);
    DrawTextA(s_dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void Draw()
{
    const Page& pg  = kPages[s_page];
    const int   top = 120, row = 54;
    char        buf[64];
    int         i, x;
    DWORD*      px;

    Fill(0, 0, W, H, RGB(22, 24, 30));
    Fill(0, 0, W, 64, RGB(36, 52, 92));
    Text(24, 0, W - 24, 64, "BULLY VR - settings", s_bold, RGB(255, 255, 255), DT_LEFT);
    // page tabs
    x = 24;
    for (i = 0; i < kPageCount; i++)
    {
        SIZE sz;
        SelectObject(s_dc, s_font);
        GetTextExtentPoint32A(s_dc, kPages[i].title, (int)strlen(kPages[i].title), &sz);
        if (i == s_page)
            Fill(x - 10, 72, x + sz.cx + 10, 112, RGB(70, 110, 190));
        Text(x, 72, x + sz.cx, 112, kPages[i].title, s_font, i == s_page ? RGB(255, 255, 255) : RGB(150, 160, 180),
             DT_LEFT);
        x += sz.cx + 40;
    }
    if (pg.items)
    {
        // rows that fit; the list scrolls with the selection
        const int visible = (H - 56 - top) / row;
        const int first   = s_sel >= visible ? s_sel - visible + 1 : 0;
        for (i = first; i < pg.count && i < first + visible; i++)
        {
            const Item& it = pg.items[i];
            const int   y  = top + (i - first) * row;
            float       v;
            if (i == s_sel)
                Fill(12, y, W - 12, y + row - 4, RGB(52, 72, 120));
            Text(32, y, W - 260, y + row - 4, it.label, s_font, RGB(235, 235, 240), DT_LEFT);
            buf[0] = 0;
            if (it.kind == K_ACTION)
                strcpy(buf, i == s_sel ? "press A" : "");
            else if (!GetValue(it, &v))
                strcpy(buf, "n/a");
            else if (it.kind == K_BOOL)
                strcpy(buf, v != 0.0f ? "ON" : "off");
            else
                _snprintf(buf, sizeof(buf), it.fmt, v);
            buf[sizeof(buf) - 1] = 0;
            if (it.kind == K_FLOAT && i == s_sel)
            {
                char tmp[64];
                _snprintf(tmp, sizeof(tmp), "<  %s  >", buf);
                tmp[sizeof(tmp) - 1] = 0;
                strcpy(buf, tmp);
            }
            Text(W - 300, y, W - 32, y + row - 4, buf, s_font,
                 it.kind == K_BOOL && !strcmp(buf, "ON") ? RGB(120, 230, 140) : RGB(255, 220, 120), DT_RIGHT);
        }
    }
    else
    {
        for (i = 0; i < (int)(sizeof(kGuide) / sizeof(kGuide[0])); i++)
            Text(32, top + i * 54, W - 24, top + i * 54 + 50, kGuide[i], s_small, RGB(225, 228, 235), DT_LEFT);
    }
    Fill(0, H - 56, W, H, RGB(36, 40, 50));
    Text(24, H - 56, W - 24, H, "Stick: select / change    A: toggle    Triggers: page    B: close", s_small,
         RGB(170, 180, 200), DT_LEFT);
    GdiFlush();
    // GDI leaves alpha at 0: the panel is opaque
    px = (DWORD*)s_bits;
    for (i = 0; i < W * H; i++)
        px[i] |= 0xff000000u;
}

// ---- input ----------------------------------------------------------------------------
// one step per push, repeating after 0.4 s held (every 0.12 s)
static int StickStep(const float* s)
{
    const float ax = fabsf(s[0]), ay = fabsf(s[1]);
    const DWORD now = GetTickCount();
    int         dir = 0;
    if (ax > 0.6f || ay > 0.6f)
        dir = ay >= ax ? (s[1] > 0 ? 1 : 2) : (s[0] < 0 ? 3 : 4);
    else if (ax > 0.3f || ay > 0.3f)
        return 0;   // between: no new step, keep the hold
    if (!dir)
    {
        s_repeatDir = 0;
        return 0;
    }
    if (dir != s_repeatDir)
    {
        s_repeatDir = dir;
        s_repeatAt  = now + 500;
        return dir;
    }
    if ((int)(now - s_repeatAt) >= 0)
    {
        s_repeatAt = now + 120;
        return dir;
    }
    return 0;
}

static void Handle(const VrInput& in)
{
    const Page& pg = kPages[s_page];
    const bool  lt = in.hand[0].trigger > 0.6f, rt = in.hand[1].trigger > 0.6f;
    float       stick[2];
    int         step;

    // either stick navigates
    stick[0] = fabsf(in.hand[0].stick[0]) > fabsf(in.hand[1].stick[0]) ? in.hand[0].stick[0] : in.hand[1].stick[0];
    stick[1] = fabsf(in.hand[0].stick[1]) > fabsf(in.hand[1].stick[1]) ? in.hand[0].stick[1] : in.hand[1].stick[1];
    step     = StickStep(stick);

    if ((lt && !s_prevLT) || (rt && !s_prevRT))
    {
        s_page = (s_page + (rt && !s_prevRT ? 1 : kPageCount - 1)) % kPageCount;
        s_sel  = 0;
        s_dirty = true;
    }
    else if (pg.items && (step == 1 || step == 2))
    {
        s_sel   = (s_sel + (step == 2 ? 1 : pg.count - 1)) % pg.count;
        s_dirty = true;
    }
    else if (pg.items && (step == 3 || step == 4))
    {
        const Item& it = pg.items[s_sel];
        float       v;
        if (it.kind == K_FLOAT && GetValue(it, &v))
            SetValue(it, v + (step == 4 ? it.step : -it.step));
        else if (it.kind == K_BOOL && GetValue(it, &v))
            SetValue(it, v != 0.0f ? 0.0f : 1.0f);
        s_dirty = true;
    }
    if (in.a && !s_prevA && pg.items)
    {
        const Item& it = pg.items[s_sel];
        float       v;
        if (it.kind == K_BOOL && GetValue(it, &v))
            SetValue(it, v != 0.0f ? 0.0f : 1.0f);
        else if (it.kind == K_ACTION)
            Run(pg, it);
        s_dirty = true;
    }
    if (in.b && !s_prevB)
    {
        s_open    = false;
        s_swallow = true;
        VrLog("[menu] closed (B)");
    }
    s_prevA  = in.a;
    s_prevB  = in.b;
    s_prevLT = lt;
    s_prevRT = rt;
}

void Menu_Frame()
{
    VrInput    in;
    const bool have = VrXr_Running() && VrXr_GetInput(&in);
    bool       combo;

    combo = have && in.hand[0].squeeze > 0.7f && in.hand[1].squeeze > 0.7f && in.y;
    if (combo && !s_comboHeld)
    {
        s_open = !s_open;
        s_dirty = true;
        // what is held now does not count as a fresh press inside the menu
        s_prevA = in.a, s_prevB = in.b;
        s_prevLT = in.hand[0].trigger > 0.6f, s_prevRT = in.hand[1].trigger > 0.6f;
        s_repeatDir = 0;
        VrLog("[menu] %s (both grips + Y)", s_open ? "opened" : "closed");
    }
    s_comboHeld = combo || (s_comboHeld && have && in.y);
    if (combo && !s_open)
        s_swallow = true;
    if (s_open && have && !combo)
        Handle(in);

    // after closing, the game gets controls back only once they are all let go
    if (s_swallow && !s_open &&
        (!have || (!in.a && !in.b && !in.x && !in.y && !in.menu && in.hand[0].trigger < 0.3f &&
                   in.hand[1].trigger < 0.3f && in.hand[0].squeeze < 0.3f && in.hand[1].squeeze < 0.3f)))
        s_swallow = false;
    if (!s_open || !EnsureCanvas())
    {
        VrXr_SetMenu(NULL, 0, 0, false);
        return;
    }
    if (s_dirty)
        Draw();
    VrXr_SetMenu(s_bits, W, H, s_dirty);
    s_dirty = false;
}
