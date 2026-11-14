/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// DESCRIPTION:
// The Competitive HUD (cg_hudstyle 1): Project: Omaha's Competitive HUD pack
// (ui/modern/huds/competitive.xml and the lib/ files it uses), laid out as
// there at 1920x1080 and scaled with the screen's height, drawn by cgame
// from the snapshot and its own state:
//
//   the radar top left (or the compass), the game messages under it; the
//   score panel top center, the timer, who is alive and the players' cards
//   either side; the kill feed top right; the chat bottom left, who you
//   follow or who last hurt you under it; the health, the team's badge and
//   the ammo along the bottom; the weapons you carry on the right; banners;
//   the stopwatch and the grenade's charge; who killed you and the player
//   you aim at; the vote; the damage's direction around the crosshair; the
//   crosshair.
//
// Everything is quads of one atlas (cg_hud_atlas.cpp), in ONE draw a frame
// (DrawTriangles2D, 511 quads a draw: a second only past that). Each element
// keeps its quads and a key of what they were made from: a frame writes
// again only the elements whose key changed, and the array drawn only from
// the first that moved. The colors and fades are the vertexes': nothing ends
// the batch. Hidden (cg_hud 0), nothing is done.

#include "cg_hud.h"

#include <vector>

#define HUD_DRAW_QUADS 511 // a draw's quads (SHADER_MAX_VERTEXES 2048, less the renderer's one)
#define HUD_MAX_QUADS  (HUD_DRAW_QUADS * 4)

static cvar_t *cg_hudstyle;
static cvar_t *cg_hud_radar;
static cvar_t *cg_hud_killfeed;
static cvar_t *cg_hud_chat;
static cvar_t *cg_hud_messages;
static cvar_t *cg_hud_health;
static cvar_t *cg_hud_ammo;
static cvar_t *cg_hud_grenades;
static cvar_t *cg_hud_grenadecharge;
static cvar_t *cg_hud_score;
static cvar_t *cg_hud_timer;
static cvar_t *cg_hud_alive;
static cvar_t *cg_hud_teamicon;
static cvar_t *cg_hud_badge;
static cvar_t *cg_hud_killer;
static cvar_t *cg_hud_targetinfo;
static cvar_t *cg_hud_vote;
static cvar_t *cg_hud_spectator;
static cvar_t *cg_hud_following;
static cvar_t *cg_hud_attacker;
static cvar_t *cg_hud_stopwatch;
static cvar_t *cg_hud_damage;
static cvar_t *cg_hud_lowhealth;

static const struct {
    cvar_t    **var;
    const char *name;
    const char *value;
} hud_cvars[] = {
    {&cg_hudstyle,          "cg_hudstyle",          "0"  },
    {&cg_hud_radar,         "cg_hud_radar",         "1"  },
    {&cg_hud_killfeed,      "cg_hud_killfeed",      "1"  },
    {&cg_hud_chat,          "cg_hud_chat",          "1"  },
    {&cg_hud_messages,      "cg_hud_messages",      "1"  },
    {&cg_hud_health,        "cg_hud_health",        "1"  },
    {&cg_hud_ammo,          "cg_hud_ammo",          "1"  },
    {&cg_hud_grenades,      "cg_hud_grenades",      "1"  },
    {&cg_hud_grenadecharge, "cg_hud_grenadecharge", "1"  },
    {&cg_hud_score,         "cg_hud_score",         "1"  },
    {&cg_hud_timer,         "cg_hud_timer",         "1"  },
    {&cg_hud_alive,         "cg_hud_alive",         "1"  },
    {&cg_hud_teamicon,      "cg_hud_teamicon",      "1"  },
    {&cg_hud_badge,         "cg_hud_badge",         "1"  },
    {&cg_hud_killer,        "cg_hud_killer",        "1"  },
    {&cg_hud_targetinfo,    "cg_hud_targetinfo",    "1"  },
    {&cg_hud_vote,          "cg_hud_vote",          "1"  },
    {&cg_hud_spectator,     "cg_hud_spectator",     "1"  },
    {&cg_hud_following,     "cg_hud_following",     "1"  },
    {&cg_hud_attacker,      "cg_hud_attacker",      "1"  },
    {&cg_hud_stopwatch,     "cg_hud_stopwatch",     "1"  },
    {&cg_hud_damage,        "cg_hud_damage",        "1"  },
    {&cg_hud_lowhealth,     "cg_hud_lowhealth",     "0.4"},
};

// the elements, in the order drawn
typedef enum {
    E_SCORE,
    E_CARDS,
    E_RADAR,
    E_MESSAGES,
    E_KILLFEED,
    E_CHAT,
    E_BOTTOMLEFT,
    E_HEALTH,
    E_BADGE,
    E_AMMO,
    E_WEAPONS,
    E_BANNERS,
    E_STOPWATCH,
    E_INFO,
    E_VOTE,
    E_DAMAGE,
    E_CROSSHAIR,
    E_COUNT
} hudElemId_t;

typedef struct {
    std::vector<vert2D_t> v;
    uint64_t              key;
    qboolean              valid;
    qboolean              dirty;
    int                   first; // its first vertex in the array drawn
} hudElem_t;

// a key of what an element is made from
struct hudKey {
    uint64_t h;

    hudKey()
        : h(14695981039346656037ULL)
    {}

    hudKey &i(int64_t v)
    {
        h ^= (uint64_t)v;
        h *= 1099511628211ULL;
        h ^= h >> 31;
        return *this;
    }

    hudKey &f(float v)
    {
        int32_t b;

        memcpy(&b, &v, sizeof(b));
        return i(b);
    }

    hudKey &s(const char *t)
    {
        for (; *t; t++) {
            h ^= (unsigned char)*t;
            h *= 1099511628211ULL;
        }
        return i(0x5A);
    }
};

static struct {
    qboolean       ready;
    float          scale;
    int            w, h;
    hudElem_t      elems[E_COUNT];
    std::vector<vert2D_t> *cur;
    vert2D_t       out[HUD_MAX_QUADS * 4];
    int            outVerts;
    unsigned short indexes[HUD_DRAW_QUADS * 6];
    float          skew; // text's slant: x moves this much a pixel up from the baseline
    cvar_t        *uiCompass;
    qboolean       failed;      // no atlas: the stock HUD
    qboolean       radarLoaded; // the radar's walk done for this map

    // the damage's directions (Omaha: the last three, 1.5 s each)
    struct {
        int stat;
        int time;
    } arcs[3];
    int lastDamageDir, lastDamageHealth, damageFlashTime;
} hud;

static void Hud_Start(void);

/*
====================================================================

QUADS

====================================================================
*/

float Hud_Scale(void)
{
    return hud.scale;
}

hudColor_t Hud_Color(unsigned int rgba)
{
    hudColor_t c;

    c.r = (byte)(rgba >> 24);
    c.g = (byte)(rgba >> 16);
    c.b = (byte)(rgba >> 8);
    c.a = (byte)rgba;
    return c;
}

hudColor_t Hud_Fade(hudColor_t c, float a)
{
    if (a < 0.0f) {
        a = 0.0f;
    } else if (a > 1.0f) {
        a = 1.0f;
    }
    c.a = (byte)(c.a * a + 0.5f);
    return c;
}

static inline void Hud_Vertex(float x, float y, float s, float t, hudColor_t c)
{
    vert2D_t v;

    v.xy[0]   = x;
    v.xy[1]   = y;
    v.st[0]   = s;
    v.st[1]   = t;
    v.rgba[0] = c.r;
    v.rgba[1] = c.g;
    v.rgba[2] = c.b;
    v.rgba[3] = c.a;
    hud.cur->push_back(v);
}

void Hud_Vertexes(const vert2D_t v[4])
{
    if (hud.cur) {
        hud.cur->insert(hud.cur->end(), v, v + 4);
    }
}

// a sprite's rectangle; its top moved right by skewTop, its bottom by skewBottom
static void Hud_SpriteAt(const hudSprite_t *sp, float x0, float y0, hudColor_t c, qboolean mirror, float skewTop, float skewBottom)
{
    const float x1 = x0 + sp->w, y1 = y0 + sp->h;
    const float s0 = mirror ? sp->s1 : sp->s0, s1 = mirror ? sp->s0 : sp->s1;

    if (!hud.cur || !c.a) {
        return;
    }
    Hud_Vertex(x0 + skewTop, y0, s0, sp->t0, c);
    Hud_Vertex(x1 + skewTop, y0, s1, sp->t0, c);
    Hud_Vertex(x1 + skewBottom, y1, s1, sp->t1, c);
    Hud_Vertex(x0 + skewBottom, y1, s0, sp->t1, c);
}

void Hud_Sprite(const hudSprite_t *sp, float x, float y, hudColor_t c)
{
    if (sp) {
        Hud_SpriteAt(sp, x + sp->x0, y + sp->y0, c, qfalse, 0.0f, 0.0f);
    }
}

void Hud_SpriteMirrored(const hudSprite_t *sp, float x, float y, hudColor_t c)
{
    if (sp) {
        Hud_SpriteAt(sp, x + sp->x0, y + sp->y0, c, qtrue, 0.0f, 0.0f);
    }
}

// a sprite at cx + ox, cy + oy turned by angle degrees (clockwise) about cx cy
void Hud_SpriteTurned(const hudSprite_t *sp, float cx, float cy, float ox, float oy, float angle, hudColor_t c)
{
    const float a  = DEG2RAD(angle);
    const float co = cosf(a), si = sinf(a);
    float       x[4], y[4];
    int         k;

    if (!sp || !hud.cur || !c.a) {
        return;
    }
    x[0] = x[3] = ox + sp->x0;
    x[1] = x[2] = ox + sp->x0 + sp->w;
    y[0] = y[1] = oy + sp->y0;
    y[2] = y[3] = oy + sp->y0 + sp->h;
    for (k = 0; k < 4; k++) {
        const float s = (k == 1 || k == 2) ? sp->s1 : sp->s0;
        const float t = (k >= 2) ? sp->t1 : sp->t0;

        Hud_Vertex(cx + x[k] * co - y[k] * si, cy + x[k] * si + y[k] * co, s, t, c);
    }
}

void Hud_RectV(float x, float y, float w, float h, hudColor_t top, hudColor_t bottom)
{
    const hudSprite_t *white = HudAtlas_White();

    if (!hud.cur || (!top.a && !bottom.a) || w <= 0.0f || h <= 0.0f) {
        return;
    }
    Hud_Vertex(x, y, white->s0, white->t0, top);
    Hud_Vertex(x + w, y, white->s0, white->t0, top);
    Hud_Vertex(x + w, y + h, white->s0, white->t0, bottom);
    Hud_Vertex(x, y + h, white->s0, white->t0, bottom);
}

void Hud_RectH(float x, float y, float w, float h, hudColor_t left, hudColor_t right)
{
    const hudSprite_t *white = HudAtlas_White();

    if (!hud.cur || (!left.a && !right.a) || w <= 0.0f || h <= 0.0f) {
        return;
    }
    Hud_Vertex(x, y, white->s0, white->t0, left);
    Hud_Vertex(x + w, y, white->s0, white->t0, right);
    Hud_Vertex(x + w, y + h, white->s0, white->t0, right);
    Hud_Vertex(x, y + h, white->s0, white->t0, left);
}

void Hud_Rect(float x, float y, float w, float h, hudColor_t c)
{
    Hud_RectV(x, y, w, h, c, c);
}

void Hud_Frame(float x, float y, float w, float h, float t, hudColor_t c)
{
    Hud_Rect(x, y, w, t, c);
    Hud_Rect(x, y + h - t, w, t, c);
    Hud_Rect(x, y + t, t, h - t * 2.0f, c);
    Hud_Rect(x + w - t, y + t, t, h - t * 2.0f, c);
}

// a whole pixel
static float Px(float v)
{
    return floorf(v + 0.5f);
}

float Hud_Text(hudFace_t face, float px, float x, float baseline, const char *text, hudColor_t c, qboolean shadow)
{
    const float base = Px(baseline);
    float       pen;
    int         pass;

    if (!text || !text[0]) {
        return 0.0f;
    }
    // the shadows under all the glyphs, then the glyphs
    for (pass = shadow && c.a ? 0 : 1; pass < 2; pass++) {
        const hudColor_t color = pass ? c : Hud_Fade(Hud_Color(0x000000FF), (c.a / 255.0f) * (c.a / 255.0f));
        const char      *p;

        pen = x;
        for (p = text; *p; p++) {
            const unsigned char ch = (unsigned char)*p;
            const hudSprite_t  *sp = pass ? HudAtlas_Glyph(face, px, ch) : HudAtlas_GlyphShadow(face, px, ch, hud.scale);

            if (sp && hud.cur) {
                const float y0 = base + sp->y0;

                Hud_SpriteAt(sp, Px(pen) + sp->x0, y0, color, qfalse, hud.skew * (base - y0), hud.skew * (base - y0 - sp->h));
            }
            pen += HudFont_Advance(face, px, ch);
        }
    }
    return pen - x;
}

// text from cx + ox on the baseline cy + baseline, turned by angle about cx cy
float Hud_TextTurned(
    hudFace_t face, float px, float cx, float cy, float ox, float baseline, const char *text, float angle, hudColor_t c,
    qboolean shadow
)
{
    float pen = ox;
    int   pass;

    for (pass = shadow && c.a ? 0 : 1; pass < 2; pass++) {
        const hudColor_t color = pass ? c : Hud_Fade(Hud_Color(0x000000FF), (c.a / 255.0f) * (c.a / 255.0f));

        pen = ox;
        for (const char *p = text; *p; p++) {
            const unsigned char ch = (unsigned char)*p;
            const hudSprite_t  *sp = pass ? HudAtlas_Glyph(face, px, ch) : HudAtlas_GlyphShadow(face, px, ch, hud.scale);

            Hud_SpriteTurned(sp, cx, cy, pen, baseline, angle, color);
            pen += HudFont_Advance(face, px, ch);
        }
    }
    return pen - ox;
}

// text in a box: centered across it when center, its capitals' middle on cy
static float Hud_Label(hudFace_t face, float px, float x, float w, float cy, const char *text, hudColor_t c, qboolean center)
{
    const float tw = HudFont_Width(face, px, text);

    return Hud_Text(face, px, center ? x + (w - tw) * 0.5f : x, cy + HudFont_CapHeight(face, px) * 0.5f, text, c, qtrue);
}

// the size text is drawn at to fit in w (no smaller than half of px)
static float Hud_Fit(hudFace_t face, float px, const char *text, float w)
{
    const float tw = HudFont_Width(face, px, text);

    if (tw <= w || tw <= 0.0f) {
        return px;
    }
    return Q_max(px * 0.5f, floorf(px * w / tw * 4.0f) / 4.0f);
}

/*
====================================================================

ELEMENTS

====================================================================
*/

// starts writing an element made from key; false when its quads are still those
static qboolean Hud_Begin(hudElemId_t id, hudKey key)
{
    hudElem_t &e = hud.elems[id];

    key.i(HudAtlas_Generation()).i(hud.w).i(hud.h);
    if (e.valid && e.key == key.h) {
        hud.cur = NULL;
        return qfalse;
    }
    e.key   = key.h;
    e.valid = qtrue;
    e.dirty = qtrue;
    e.v.clear();
    hud.cur = &e.v;
    return qtrue;
}

// an element not shown
static void Hud_Hide(hudElemId_t id)
{
    Hud_Begin(id, hudKey().i(-1));
}

static float S(float design)
{
    return design * hud.scale;
}

static hudColor_t Hud_TeamColor(int team)
{
    return Hud_Color(team == TEAM_AXIS ? 0xCC8888FF : 0x8891CCFF);
}

/*
================
Hud_TeamIcon

A team's icon (fortemoh/moh-icons team/allies.svg, team/axis.svg: a full disc
each) d pixels across at x y, its layers each in its color
================
*/
static void Hud_TeamIcon(int team, float x, float y, int d, float alpha)
{
    static const unsigned int allies[3] = {0x1A1E3BFF, 0xFFFFFFFF, 0xDE0000FF};
    static const unsigned int axis[3]   = {0xA90303FF, 0xFFFFFFFF, 0x000000FF};
    int                       i;

    if (team == TEAM_ALLIES) {
        for (i = 0; i < 3; i++) {
            Hud_Sprite(HudAtlas_Shape((hudShape_t)(HUD_SHAPE_ALLIES_0 + i), d, d, 0.0f), x, y, Hud_Fade(Hud_Color(allies[i]), alpha));
        }
    } else if (team == TEAM_AXIS) {
        for (i = 0; i < 3; i++) {
            Hud_Sprite(HudAtlas_Shape((hudShape_t)(HUD_SHAPE_AXIS_0 + i), d, d, 0.0f), x, y, Hud_Fade(Hud_Color(axis[i]), alpha));
        }
    }
}

/*
====================================================================

WEAPONS

The weapons the player carries, as Omaha's host had them (cl_hud_host.cpp,
cl_hud_nation.cpp): the snapshot names only the weapon in hand, so the
primary and the sidearm are the last ones seen in hand, the one picked up
("Picked Up <name>"), or the nation's pistol until then; the grenades the
pouch and the one loaded

====================================================================
*/

typedef enum {
    NATION_NONE = -1,
    NATION_AMERICAN,
    NATION_BRITISH,
    NATION_RUSSIAN,
    NATION_GERMAN,
    NATION_ITALIAN
} hudNation_t;

typedef enum {
    WK_PRIMARY,
    WK_SIDEARM,
    WK_FRAG,
    WK_SMOKE
} hudWeaponKind_t;

typedef struct {
    const char     *name;
    const char     *silhouette;
    hudWeaponKind_t kind;
    hudNation_t     nation;
} hudWeapon_t;

static const hudWeapon_t hud_weapons[] = {
    {"M1 Garand",              "m1_garand_filled",            WK_PRIMARY, NATION_AMERICAN},
    {"Thompson",               "thompsonsmg_filled",          WK_PRIMARY, NATION_AMERICAN},
    {"BAR",                    "bar_filled",                  WK_PRIMARY, NATION_NONE    },
    {"Springfield '03 Sniper", "springfield_filled",          WK_PRIMARY, NATION_NONE    },
    {"Shotgun",                "shotgun_filled",              WK_PRIMARY, NATION_NONE    },
    {"Bazooka",                "bazooka_filled",              WK_PRIMARY, NATION_NONE    },
    {"Colt 45",                "colt45_filled",               WK_SIDEARM, NATION_AMERICAN},
    {"Hi-Standard Silenced",   "silencedpistol_filled",       WK_SIDEARM, NATION_NONE    },
    {"Frag Grenade",           "m2frag_grenade_filled",       WK_FRAG,    NATION_AMERICAN},
    {"M18 Smoke Grenade",      "m18_smoke_grenade_filled",    WK_SMOKE,   NATION_NONE    },
    {"Lee-Enfield",            "enfield_filled",              WK_PRIMARY, NATION_BRITISH },
    {"Sten Mark II",           "sten_filled",                 WK_PRIMARY, NATION_BRITISH },
    {"Enfield L42A1",          "uk_w_l42a1_filled",           WK_PRIMARY, NATION_BRITISH },
    {"DeLisle",                "delisle_filled",              WK_PRIMARY, NATION_BRITISH },
    {"Vickers-Berthier",       "uk_w_vickers_filled",         WK_PRIMARY, NATION_BRITISH },
    {"PIAT",                   "uk_w_piat_filled",            WK_PRIMARY, NATION_BRITISH },
    {"Webley Revolver",        "webley_revolver_filled",      WK_SIDEARM, NATION_BRITISH },
    {"Mills Grenade",          "mills_grenade_filled",        WK_FRAG,    NATION_BRITISH },
    {"Mosin Nagant Rifle",     "mosin_nagant_rifle_filled",   WK_PRIMARY, NATION_RUSSIAN },
    {"PPSH SMG",               "ppsh_smg_filled",             WK_PRIMARY, NATION_RUSSIAN },
    {"SVT 40",                 "svt_rifle_filled",            WK_PRIMARY, NATION_RUSSIAN },
    {"Nagant Revolver",        "nagant_revolver_filled",      WK_SIDEARM, NATION_RUSSIAN },
    {"Russian F1 Grenade",     "russian_f1_grenade_filled",   WK_FRAG,    NATION_RUSSIAN },
    {"RDG-1 Smoke Grenade",    "rdg-1_smoke_grenade_filled",  WK_SMOKE,   NATION_RUSSIAN },
    {"Mauser KAR 98K",         "kar98_filled",                WK_PRIMARY, NATION_GERMAN  },
    {"KAR98 - Sniper",         "kar98sniper_filled",          WK_PRIMARY, NATION_NONE    },
    {"G 43",                   "g43_filled",                  WK_PRIMARY, NATION_GERMAN  },
    {"MP40",                   "mp40_filled",                 WK_PRIMARY, NATION_GERMAN  },
    {"StG 44",                 "mp44_filled",                 WK_PRIMARY, NATION_GERMAN  },
    {"Panzerschreck",          "panzerschreck_filled",        WK_PRIMARY, NATION_NONE    },
    {"Gewehrgranate",          "kar98_mortar_filled",         WK_PRIMARY, NATION_GERMAN  },
    {"Walther P38",            "p38_filled",                  WK_SIDEARM, NATION_GERMAN  },
    {"Stielhandgranate",       "steilhandgranate_filled",     WK_FRAG,    NATION_GERMAN  },
    {"Nebelhandgranate",       "nebelhandgranate_filled",     WK_SMOKE,   NATION_GERMAN  },
    {"Carcano",                "it_w_carcano_filled",         WK_PRIMARY, NATION_ITALIAN },
    {"Moschetto",              "it_w_moschetto_filled",       WK_PRIMARY, NATION_ITALIAN },
    {"Breda",                  "it_w_breda_filled",           WK_PRIMARY, NATION_ITALIAN },
    {"Beretta",                "it_w_beretta_filled",         WK_SIDEARM, NATION_ITALIAN },
    {"Bomba A Mano",           "it_w_bomba_filled",           WK_FRAG,    NATION_ITALIAN },
    {"Bomba A Mano Breda",     "it_w_bombabreda_filled",      WK_SMOKE,   NATION_ITALIAN },
};

// the weapon slots' bits (STAT_WEAPONS) and keys: 1 pistol, 2 rifle, 3 SMG, 4 MG, 5 grenade, 6 heavy
static const int hud_slotBits[6] = {
    WEAPON_CLASS_PISTOL, WEAPON_CLASS_RIFLE, WEAPON_CLASS_SMG, WEAPON_CLASS_MG, WEAPON_CLASS_GRENADE, WEAPON_CLASS_HEAVY
};
#define PRIMARY_CLASSES (WEAPON_CLASS_RIFLE | WEAPON_CLASS_SMG | WEAPON_CLASS_MG | WEAPON_CLASS_HEAVY)

static struct {
    char        primary[MAX_QPATH];
    char        sidearm[MAX_QPATH];
    char        pending[MAX_QPATH]; // picked up, not seen in hand yet
    int         prevOwnPrimary, prevOwnSidearm;
    qboolean    sidearmGuessed;
    int         follow; // -2 playing, -1 free spectating, else the client followed
    int         grenades, smokes;
    int         loaded[2]; // the grenade in the frag [0] and smoke [1] weapons, -1 not seen since the spawn
    char        roundStart[64];
    hudNation_t nation;
    qboolean    charging;
    int         chargeStart;
    qboolean    chargeSecondary;
    int         silhouette[ARRAY_LEN(hud_weapons)];
} wb;

static const hudWeapon_t *Hud_Weapon(const char *name)
{
    size_t i;

    if (!name || !name[0]) {
        return NULL;
    }
    for (i = 0; i < ARRAY_LEN(hud_weapons); i++) {
        if (!Q_stricmp(name, hud_weapons[i].name)) {
            return &hud_weapons[i];
        }
    }
    return NULL;
}

static int Hud_WeaponSilhouette(const char *name)
{
    const hudWeapon_t *w = Hud_Weapon(name);

    return w ? wb.silhouette[w - hud_weapons] : -1;
}

static const char *Hud_NationSidearm(hudNation_t nation)
{
    switch (nation) {
    case NATION_BRITISH:
        return "Webley Revolver";
    case NATION_RUSSIAN:
        return "Nagant Revolver";
    case NATION_GERMAN:
        return "Walther P38";
    case NATION_ITALIAN:
        return "Beretta";
    default:
        return "Colt 45";
    }
}

static const char *Hud_NationGrenade(hudNation_t nation, qboolean smoke)
{
    if (smoke) {
        switch (nation) {
        case NATION_RUSSIAN:
            return "RDG-1 Smoke Grenade";
        case NATION_GERMAN:
            return "Nebelhandgranate";
        case NATION_ITALIAN:
            return "Bomba A Mano Breda";
        default:
            return "M18 Smoke Grenade";
        }
    }
    switch (nation) {
    case NATION_BRITISH:
        return "Mills Grenade";
    case NATION_RUSSIAN:
        return "Russian F1 Grenade";
    case NATION_GERMAN:
        return "Stielhandgranate";
    case NATION_ITALIAN:
        return "Bomba A Mano";
    default:
        return "Frag Grenade";
    }
}

// as GetPlayerAlliedTeamType and GetPlayerAxisTeamType in fgame/player.cpp
static hudNation_t Hud_NationForModel(const char *model, qboolean axis)
{
    if (axis) {
        if (cg_target_game >= TG_MOHTT && (!Q_stricmpn(model, "it", 2) || !Q_stricmpn(model, "sc", 2))) {
            return NATION_ITALIAN;
        }
        return NATION_GERMAN;
    }
    if (cg_target_game >= TG_MOHTA) {
        if (!Q_stricmpn(model, "allied_russian", 14)) {
            return NATION_RUSSIAN;
        }
        if (!Q_stricmpn(model, "allied_british", 14) || !Q_stricmpn(model, "allied_sas", 10)) {
            return NATION_BRITISH;
        }
    }
    return NATION_AMERICAN;
}

// the nation whose loadout the shown player has: a weapon only one nation
// gets says it, else the player's own model, else the team
static hudNation_t Hud_ResolveNation(const char *weaponName, qboolean following)
{
    const qboolean     axis = cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS;
    const hudWeapon_t *w;

    if (cg_target_game < TG_MOHTA) {
        return axis ? NATION_GERMAN : NATION_AMERICAN;
    }
    w = Hud_Weapon(weaponName);
    if (!w || w->nation == NATION_NONE) {
        w = Hud_Weapon(wb.primary);
    }
    if (w && w->nation != NATION_NONE) {
        return w->nation;
    }
    if (!following) {
        cvar_t *model = cgi.Cvar_Find(axis ? "dm_playergermanmodel" : "dm_playermodel");

        return Hud_NationForModel(model ? model->string : "", axis);
    }
    return axis ? NATION_GERMAN : NATION_AMERICAN;
}

static void Hud_ClearWeapons(void)
{
    wb.primary[0] = wb.sidearm[0] = wb.pending[0] = 0;
    wb.prevOwnPrimary = wb.prevOwnSidearm = -1;
    wb.sidearmGuessed = qfalse;
    wb.grenades       = 0;
    wb.loaded[0] = wb.loaded[1] = -1;
    wb.charging       = qfalse;
}

void CG_HudCompetitive_Pickup(const char *message)
{
    char               name[MAX_QPATH];
    const hudWeapon_t *w;
    int                len;

    while (*message == ' ' || *message == '\t') {
        message++;
    }
    // LV_ConvertString("Picked Up <item>"): the name is the configstring's
    if (Q_stricmpn(message, "Picked Up ", 10)) {
        return;
    }
    Q_strncpyz(name, message + 10, sizeof(name));
    for (len = (int)strlen(name); len > 0 && (unsigned char)name[len - 1] <= ' '; len--) {
        name[len - 1] = 0;
    }
    w = Hud_Weapon(name);
    if (w && (w->kind == WK_PRIMARY || w->kind == WK_SIDEARM)) {
        Q_strncpyz(wb.pending, name, sizeof(wb.pending));
    }
}

/*
================
Hud_SyncWeapons

The primary's and the sidearm's names (cl_hud_host.cpp,
UIR_Hud_SyncModernWeaponsBar), the grenades and the grenade's charge
================
*/
static void Hud_SyncWeapons(const char *weaponName, qboolean following, int clip, int ammo)
{
    const playerState_t *ps       = &cg.snap->ps;
    const int            owned    = ps->stats[STAT_WEAPONS] & 0x3F;
    const int            equipped = ps->stats[STAT_EQUIPPED_WEAPON] & 0x3F;
    const qboolean       ownPrimary = (owned & PRIMARY_CLASSES) != 0;
    const qboolean       ownSidearm = (owned & WEAPON_CLASS_PISTOL) != 0;
    const qboolean       equipGrenade = (equipped & WEAPON_CLASS_GRENADE) != 0;
    const hudWeapon_t   *inHand = Hud_Weapon(weaponName);
    int                  followKey = -2;
    const char          *roundStart;
    int                  i;

    if (ps->pm_flags & PMF_SPECTATING) {
        followKey = (ps->pm_flags & PMF_CAMERA_VIEW) ? ps->stats[STAT_INFOCLIENT] : -1;
    }
    // into or out of following, or another player: none of the last one's names
    if (followKey != wb.follow) {
        Hud_ClearWeapons();
        wb.follow = followKey;
    }
    wb.nation = Hud_ResolveNation(weaponName, following);

    // the one picked up: the primary, or the sidearm when it is one
    if (wb.pending[0]) {
        const hudWeapon_t *w = Hud_Weapon(wb.pending);

        if (w && w->kind == WK_SIDEARM && ownSidearm) {
            Q_strncpyz(wb.sidearm, wb.pending, sizeof(wb.sidearm));
            wb.sidearmGuessed = qfalse;
            wb.pending[0]     = 0;
        } else if (w && w->kind == WK_PRIMARY && ownPrimary) {
            Q_strncpyz(wb.primary, wb.pending, sizeof(wb.primary));
            wb.pending[0] = 0;
        }
    }
    if (inHand && !equipGrenade) {
        if (inHand->kind == WK_SIDEARM) {
            Q_strncpyz(wb.sidearm, inHand->name, sizeof(wb.sidearm));
            wb.sidearmGuessed = qfalse;
        } else if (inHand->kind == WK_PRIMARY) {
            Q_strncpyz(wb.primary, inHand->name, sizeof(wb.primary));
        }
    }
    if (!ownPrimary) {
        wb.primary[0] = 0;
    }
    if (!ownSidearm) {
        wb.sidearm[0]     = 0;
        wb.sidearmGuessed = qfalse;
    } else if (!wb.sidearm[0] || wb.sidearmGuessed) {
        // the loadout's pistol is never named until it is in hand: the nation's
        Q_strncpyz(wb.sidearm, Hud_NationSidearm(wb.nation), sizeof(wb.sidearm));
        wb.sidearmGuessed = qtrue;
    }
    wb.prevOwnPrimary = ownPrimary;
    wb.prevOwnSidearm = ownSidearm;

    // the grenades: the pouch, and the one a grenade weapon keeps loaded (also
    // put away). A spawn, a map or a round gives new ones, loaded as the game
    // loads them
    {
        const qboolean equipSmoke = equipGrenade && inHand && inHand->kind == WK_SMOKE;
        const qboolean equipFrag  = equipGrenade && !equipSmoke;
        int            frags = 0, smokes = 0;

        for (i = 0; i < MAX_AMMO; i++) {
            const int   index = ps->ammo_name_index[i];
            const char *name;

            if (index <= 0 || index >= MAX_CONFIGSTRINGS) {
                continue;
            }
            name = CG_ConfigString(index);
            if (!name[0] || Q_stristr(name, "riflegrenade")) {
                continue;
            }
            if (Q_stristr(name, "smoke")) {
                smokes = Q_max(smokes, ps->ammo_amount[i]);
            } else if (Q_stristr(name, "grenade") || !Q_stricmp(name, "agrenade")) {
                frags = Q_max(frags, ps->ammo_amount[i]);
            }
        }
        roundStart = CG_ConfigString(CS_WARMUP);
        if (ps->stats[STAT_HEALTH] <= 0 || (ps->pm_flags & PMF_RESPAWNED) || ((ps->pm_flags & PMF_SPECTATING) && !following)
            || strcmp(roundStart, wb.roundStart)) {
            wb.loaded[0] = wb.loaded[1] = -1;
            Q_strncpyz(wb.roundStart, roundStart, sizeof(wb.roundStart));
        } else if (equipGrenade) {
            // in hand, the server sends the loaded one as the clip
            wb.loaded[equipSmoke ? 1 : 0] = clip > 0 ? 1 : 0;
        }
        if (wb.loaded[0] >= 0) {
            frags += wb.loaded[0];
        } else if ((owned & WEAPON_CLASS_GRENADE) && cg_target_game < TG_MOHTA) {
            // Allied Assault's dmstartammo 6 loads one; Spearhead and Breakthrough none
            frags += 1;
        }
        if (wb.loaded[1] > 0) {
            smokes += wb.loaded[1];
        }
        // in hand, the ammo stat is the pouch and the loaded one (no ammo list while following)
        if (equipFrag && ammo > frags) {
            frags = ammo;
        }
        if (equipSmoke && ammo > smokes) {
            smokes = ammo;
        }
        if (!(owned & WEAPON_CLASS_GRENADE)) {
            wb.grenades = 0;
            smokes      = 0;
        } else if (frags > 0 || equipFrag) {
            // (the pouch can read 0 for a frame while switching: the last count stays)
            wb.grenades = frags;
        }
        wb.smokes = smokes;
    }

    // the grenade's charge (client-side: the rising edge of the charge
    // animation with a grenade in hand), for its bar
    if (equipGrenade && ps->iViewModelAnim == VM_ANIM_CHARGE) {
        if (!wb.charging) {
            usercmd_t cmd;

            wb.charging    = qtrue;
            wb.chargeStart = cg.time;
            cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &cmd);
            wb.chargeSecondary = (cmd.buttons & BUTTON_ATTACKRIGHT) && !(cmd.buttons & BUTTON_ATTACKLEFT);
        }
    } else {
        wb.charging = qfalse;
    }
}

/*
====================================================================

THE FRAME

====================================================================
*/

typedef struct {
    const playerState_t *ps;
    qboolean             show;      // the play chrome (Omaha's ui_om_hud_show)
    qboolean             strip;     // the health, ammo and weapons: not free spectating
    qboolean             following; // a player followed
    int                  team;
    int                  health, maxHealth;
    int                  clip, ammo;
    const char          *weapon; // in hand, "" none
    int                  owned, equipped;
    int                  handSlot, primarySlot;
    int                  grenadeHand; // 1 frag, 2 smoke
    char                 spectatorText[256];
    int                  followClient;
    float                viewYaw;
    float                damageAngle, damageAlpha; // the radar's and the compass's wedge
} hudFrame_t;

static hudFrame_t fr;

static void Hud_SpectatorText(void)
{
    static int  lastFlags = -1, lastTeam = -1, lastTime;
    const int   pmFlags = cg.predicted_player_state.pm_flags;
    const int   team    = cg.snap->ps.stats[STAT_TEAM];
    int         key1, key2;
    const char *text;

    // made again when what it says changes (and once a second, for a key bound again)
    if ((pmFlags & (PMF_SPECTATING | PMF_CAMERA_VIEW)) == lastFlags && team == lastTeam && cg.time >= lastTime
        && cg.time - lastTime < 1000) {
        return;
    }
    lastFlags = pmFlags & (PMF_SPECTATING | PMF_CAMERA_VIEW);
    lastTeam  = team;
    lastTime  = cg.time;
    fr.spectatorText[0] = 0;
    if (!(pmFlags & PMF_SPECTATING)) {
        return;
    }
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        if (team != TEAM_ALLIES && team != TEAM_AXIS) {
            cgi.Key_GetKeysForCommand("+attackprimary", &key1, &key2);
            text = va("Press Fire(%s) to join the battle!", cgi.Key_KeynumToBindString(key1));
        } else if (pmFlags & PMF_CAMERA_VIEW) {
            cgi.Key_GetKeysForCommand("+use", &key1, &key2);
            text = va("Press Use(%s) to enter free spectate mode.", cgi.Key_KeynumToBindString(key1));
        } else {
            cgi.Key_GetKeysForCommand("+use", &key1, &key2);
            text = va("Press Use(%s) to enter player following spectate mode.", cgi.Key_KeynumToBindString(key1));
        }
    } else {
        cgi.Key_GetKeysForCommand("+use", &key1, &key2);
        if (pmFlags & PMF_CAMERA_VIEW) {
            text = va("Press Use(%s) to follow a different player.", cgi.Key_KeynumToBindString(key1));
        } else {
            text = va("Press Use(%s) to follow a player.", cgi.Key_KeynumToBindString(key1));
        }
    }
    Q_strncpyz(fr.spectatorText, cgi.LV_ConvertString(text), sizeof(fr.spectatorText));
}

// a damage direction (STAT_DAMAGEDIR, a world yaw in tenths) as the wedges
// turn: up is where the view looks
static float Hud_DamageAngle(int stat)
{
    return AngleSubtract(fr.viewYaw, stat / 10.0f + 180.0f);
}

static void Hud_GatherFrame(void)
{
    const playerState_t *ps = &cg.snap->ps;
    const int            pmFlags = cg.predicted_player_state.pm_flags;
    const int            dmgStat = ps->stats[STAT_DAMAGEDIR];
    int                  i;

    fr.ps        = ps;
    fr.team      = ps->stats[STAT_TEAM];
    fr.show      = !(ps->pm_flags & (PMF_NO_HUD | PMF_INTERMISSION));
    fr.following = (pmFlags & PMF_SPECTATING) && (pmFlags & PMF_CAMERA_VIEW) && ps->stats[STAT_INFOCLIENT] >= 0
                && ps->stats[STAT_INFOCLIENT] < MAX_CLIENTS;
    fr.followClient = fr.following ? ps->stats[STAT_INFOCLIENT] : -1;
    Hud_SpectatorText();
    fr.strip   = fr.show && (!fr.spectatorText[0] || fr.following);
    fr.viewYaw = cg.refdefViewAngles[YAW];

    fr.health    = ps->stats[STAT_HEALTH];
    fr.maxHealth = Q_max(1, ps->stats[STAT_MAXHEALTH]);
    // following, before the server copied the player's stats: the info health (percent)
    if (fr.following && !(ps->stats[STAT_WEAPONS] & 0x3F) && ps->stats[STAT_INFOCLIENT_HEALTH] > 0) {
        fr.health    = ps->stats[STAT_INFOCLIENT_HEALTH];
        fr.maxHealth = 100;
    }
    fr.clip     = ps->stats[STAT_CLIPAMMO];
    fr.ammo     = ps->stats[STAT_AMMO];
    fr.weapon   = ps->activeItems[ITEM_WEAPON] >= 0 ? CG_ConfigString(CS_WEAPONS + ps->activeItems[ITEM_WEAPON]) : "";
    fr.owned    = ps->stats[STAT_WEAPONS] & 0x3F;
    fr.equipped = ps->stats[STAT_EQUIPPED_WEAPON] & 0x3F;
    fr.handSlot = fr.primarySlot = 0;
    for (i = 0; i < 6; i++) {
        if (!fr.handSlot && (fr.equipped & hud_slotBits[i])) {
            fr.handSlot = i + 1;
        }
        if (!fr.primarySlot && (fr.owned & hud_slotBits[i]) && (hud_slotBits[i] & PRIMARY_CLASSES)) {
            fr.primarySlot = i + 1;
        }
    }
    Hud_SyncWeapons(fr.weapon, fr.following, fr.clip, fr.ammo);
    {
        const hudWeapon_t *w = Hud_Weapon(fr.weapon);

        fr.grenadeHand = !(fr.equipped & WEAPON_CLASS_GRENADE) ? 0 : (w && w->kind == WK_SMOKE) ? 2 : 1;
    }

    // the damage arcs: a hit renews the one within 30 degrees, else the oldest
    // takes it. Repeated hits from one side leave the stat as it was: a drop
    // in health with a direction renews it too
    if (dmgStat != hud.lastDamageDir || (dmgStat > 0 && hud.lastDamageHealth >= 0 && ps->stats[STAT_HEALTH] < hud.lastDamageHealth)) {
        int slot = 0;

        // (the first frame's stat is no hit)
        if (hud.lastDamageDir != -999999) {
            hud.damageFlashTime = cg.time;
        }
        for (i = 0; i < (int)ARRAY_LEN(hud.arcs); i++) {
            if (hud.arcs[i].time && fabs(AngleSubtract(dmgStat / 10.0f, hud.arcs[i].stat / 10.0f)) < 30.0f) {
                slot = i;
                break;
            }
            if (hud.arcs[i].time < hud.arcs[slot].time) {
                slot = i;
            }
        }
        if (hud.lastDamageDir != -999999) {
            hud.arcs[slot].stat = dmgStat;
            hud.arcs[slot].time = cg.time ? cg.time : 1;
        }
    }
    hud.lastDamageDir    = dmgStat;
    hud.lastDamageHealth = ps->stats[STAT_HEALTH];
    fr.damageAlpha       = 0.0f;
    if (dmgStat > 0 && hud.damageFlashTime) {
        fr.damageAlpha = Com_Clamp(0.0f, 1.0f, 1.0f - (cg.time - hud.damageFlashTime) / 1500.0f);
    }
    fr.damageAngle = Hud_DamageAngle(dmgStat);
}

/*
====================================================================

THE ELEMENTS

====================================================================
*/

/*
================
Hud_ElemScore

Top center: Allied | timer | Axis (team games) or self | timer | the
leader (free-for-all) on a blue panel fading down, "N vs M" alive under it
in round modes. The other team's score: the scoreboard's, or the highest
score when it is above ours (the server sends it every frame; the
scoreboard comes when asked)
================
*/
static float hud_panelX, hud_panelW; // for the cards

static void Hud_ElemScore(void)
{
    const hudRoster_t *roster = HudFeed_Roster();
    const qboolean     rounds = cgs.gametype == GT_TEAM_ROUNDS || cgs.gametype == GT_OBJECTIVE;
    char               left[16] = "", right[16] = "", timer[16] = "", alive[2][8] = {"", ""};
    unsigned int       leftColor = 0xFCFCFCFF, rightColor = 0xFCFCFCFF;
    hudKey             key;

    hud_panelX = hud.w * 0.5f;
    hud_panelW = 0.0f;
    if (!fr.show || cgs.gametype == GT_SINGLE_PLAYER) {
        Hud_Hide(E_SCORE);
        return;
    }
    if (cg_hud_timer->integer && cg.matchStartTime != -1 && cgs.gametype != GT_LIBERATION && cgs.matchEndTime) {
        const int seconds = (cgs.matchEndTime - cg.time) / 1000;

        if (seconds >= 0) {
            Com_sprintf(timer, sizeof(timer), "%d:%02d", seconds / 60, seconds % 60);
        }
    }
    if (cg_hud_score->integer) {
        if (cgs.gametype >= GT_TEAM) {
            const int own     = fr.ps->stats[STAT_KILLS];
            const int highest = fr.ps->stats[STAT_HIGHEST_SCORE];
            int       score[2];

            score[0] = roster->teamScore[0];
            score[1] = roster->teamScore[1];
            if (fr.team == TEAM_ALLIES || fr.team == TEAM_AXIS) {
                const int mine = fr.team == TEAM_AXIS;

                score[mine] = own;
                // the scoreboard's can be old (it is asked for at times): the
                // highest score is the other team's when it is above ours
                if (highest > own) {
                    score[!mine] = highest;
                }
            }
            if (score[0] >= 0) {
                Com_sprintf(left, sizeof(left), "%d", score[0]);
            }
            if (score[1] >= 0) {
                Com_sprintf(right, sizeof(right), "%d", score[1]);
            }
            leftColor  = 0xCDEFFAFF;
            rightColor = 0xF5CFC8FF;
        } else {
            Com_sprintf(left, sizeof(left), "%d", fr.ps->stats[STAT_KILLS]);
            Com_sprintf(right, sizeof(right), "%d", fr.ps->stats[STAT_HIGHEST_SCORE]);
        }
    }
    if (rounds && cg_hud_alive->integer) {
        Com_sprintf(alive[0], sizeof(alive[0]), "%d", roster->alive[0]);
        Com_sprintf(alive[1], sizeof(alive[1]), "%d", roster->alive[1]);
    }

    {
        const float labelW = S(48.0f);
        const float rowH   = S(44.0f);
        const float timerW = timer[0] ? HudFont_Width(HUD_FACE_MEDIUM, S(22.0f), timer) + S(16.0f) : 0.0f;
        const float aliveH = alive[0][0] ? HudFont_LineHeight(HUD_FACE_MEDIUM, S(15.0f)) + S(10.0f) : 0.0f;

        hud_panelW = (left[0] ? labelW : 0.0f) + timerW + (right[0] ? labelW : 0.0f);
        if (alive[0][0]) {
            const float gap = S(4.0f);
            const float w   = HudFont_Width(HUD_FACE_MEDIUM, S(15.0f), alive[0]) + HudFont_Width(HUD_FACE_MEDIUM, S(15.0f), "vs")
                          + HudFont_Width(HUD_FACE_MEDIUM, S(15.0f), alive[1]) + gap * 2.0f;

            hud_panelW = Q_max(hud_panelW, w);
        }
        hud_panelX = Px(hud.w * 0.5f - hud_panelW * 0.5f);

        key.s(left).s(right).s(timer).s(alive[0]).s(alive[1]).i(leftColor);
        if (!Hud_Begin(E_SCORE, key)) {
            return;
        }
        if (hud_panelW <= 0.0f) {
            return;
        }
        {
            const float x = hud_panelX, h = rowH + aliveH;
            float       cx = x;

            Hud_RectV(x, 0.0f, hud_panelW, Px(h * 0.62f), Hud_Color(0x36607EE6), Hud_Color(0x426C90B3));
            Hud_RectV(x, Px(h * 0.62f), hud_panelW, h - Px(h * 0.62f), Hud_Color(0x426C90B3), Hud_Color(0x48789C00));
            // the row's own width (the alive row can be the wider)
            cx = x + (hud_panelW - ((left[0] ? labelW : 0.0f) + timerW + (right[0] ? labelW : 0.0f))) * 0.5f;
            if (left[0]) {
                Hud_Label(HUD_FACE_MEDIUM, S(22.0f), cx, labelW, rowH * 0.5f, left, Hud_Color(leftColor), qtrue);
                cx += labelW;
            }
            if (timer[0]) {
                Hud_Label(HUD_FACE_MEDIUM, S(22.0f), cx, timerW, rowH * 0.5f, timer, Hud_Color(0xFCFCFCFF), qtrue);
                cx += timerW;
            }
            if (right[0]) {
                Hud_Label(HUD_FACE_MEDIUM, S(22.0f), cx, labelW, rowH * 0.5f, right, Hud_Color(rightColor), qtrue);
            }
            if (alive[0][0]) {
                const float px   = S(15.0f);
                const float gap  = S(4.0f);
                const float w    = HudFont_Width(HUD_FACE_MEDIUM, px, alive[0]) + HudFont_Width(HUD_FACE_MEDIUM, px, "vs")
                              + HudFont_Width(HUD_FACE_MEDIUM, px, alive[1]) + gap * 2.0f;
                const float base = rowH + HudFont_Ascent(HUD_FACE_MEDIUM, px);
                float       ax   = x + (hud_panelW - w) * 0.5f;

                // text-skew -12deg: slanted
                hud.skew = 0.21256f;
                ax += Hud_Text(HUD_FACE_MEDIUM, px, ax, base, alive[0], Hud_Color(0xFCFCFCFF), qtrue) + gap;
                ax += Hud_Text(HUD_FACE_MEDIUM, px, ax, base, "vs", Hud_Color(0xFCFCFCFF), qtrue) + gap;
                Hud_Text(HUD_FACE_MEDIUM, px, ax, base, alive[1], Hud_Color(0xFCFCFCFF), qtrue);
                hud.skew = 0.0f;
            }
        }
    }
}

/*
================
Hud_ElemCards

A card for each player of a team in round modes, beside the score panel:
the team's icon, framed in yellow-green for you, a white bar under it while
alive; a skull once dead (dead cards outermost). They shrink so the bigger
team fits in about 400 px, and never reach the radar or the kill feed: past
what fits, smaller, then a count of the rest
================
*/
static void Hud_ElemCards(void)
{
    const hudRoster_t *roster = HudFeed_Roster();
    hudKey             key;
    int                most, size, fit[2], t;
    float              avail;

    if (!fr.show || !cg_hud_alive->integer || cgs.gametype < GT_TEAM_ROUNDS || cgs.gametype > GT_OBJECTIVE || hud_panelW <= 0.0f) {
        Hud_Hide(E_CARDS);
        return;
    }
    most = Q_max(1, Q_max(roster->count[0], roster->count[1]));
    // Omaha's size: 400 px for the bigger team, 20 to 48 a card
    size = Q_max(20, Q_min(48, 400 / most - 4));
    // the room either side: from the radar's edge, and to the kill feed's
    avail = Q_min(hud_panelX - S(6.0f) - S(25.0f + 248.0f + 8.0f), hud.w - S(25.0f + 420.0f + 8.0f) - (hud_panelX + hud_panelW + S(6.0f)));
    if (S((float)(size + 4) * most) > avail) {
        size = Q_max(8, (int)(avail / hud.scale / most) - 4);
    }
    for (t = 0; t < 2; t++) {
        fit[t] = Q_min(roster->count[t], Q_max(1, (int)(avail / S((float)(size + 4)))));
        if (fit[t] < roster->count[t]) {
            // room for the count of the rest
            fit[t] = Q_max(0, fit[t] - 1);
        }
    }

    key.i(roster->revision).i(size).f(hud_panelX).f(hud_panelW).i(cg.snap->ps.clientNum);
    if (!Hud_Begin(E_CARDS, key)) {
        return;
    }
    for (t = 0; t < 2; t++) {
        const int   team  = t ? TEAM_AXIS : TEAM_ALLIES;
        const float step  = S((float)(size + 4));
        const float card  = Px(S((float)size));
        const float edge  = Q_max(1.0f, Px(S(1.0f)));
        float       x     = t ? hud_panelX + hud_panelW + S(6.0f) + S(2.0f) : hud_panelX - S(6.0f) - S(2.0f) - card;
        const int   shown = fit[t];
        int         k;

        // nearest the panel first: the alive, then the dead, outward
        for (k = 0; k < shown; k++) {
            const int      client = roster->players[t][k];
            const qboolean dead   = roster->dead[client];
            const float    cx     = Px(x);

            if (dead) {
                Hud_Rect(cx, 0.0f, card, card, Hud_Color(0x1C2024B3));
                Hud_Frame(cx, 0.0f, card, card, edge, Hud_Color(0xFFFFFF33));
                if (size >= 16) {
                    const int pad = (int)S(6.0f);

                    Hud_Sprite(HudAtlas_Silhouette(HudSilhouette_Find("skull_50"), (int)card - pad * 2, (int)card - pad * 2), cx + pad, (float)pad, Hud_Color(0xFFFFFF80));
                }
            } else {
                const qboolean me = client == cg.snap->ps.clientNum;

                Hud_Rect(cx, 0.0f, card, card, Hud_Color(team == TEAM_ALLIES ? 0x4B6F8CD9 : 0x8C554BD9));
                Hud_Frame(cx, 0.0f, card, card, me ? edge * 2.0f : edge, Hud_Color(me ? 0xCFDF7DFF : 0xFFFFFF33));
                if (size >= 16) {
                    const int pad = (int)S(5.0f);

                    Hud_TeamIcon(team, cx + pad, (float)pad, (int)card - pad * 2, 1.0f);
                }
                Hud_Rect(cx, card + Px(S(4.0f)), card, Px(S(4.0f)), Hud_Color(0xFCFCFCFF));
            }
            x += t ? step : -step;
        }
        if (shown < roster->count[t]) {
            const char *more = va("+%d", roster->count[t] - shown);
            const float px   = S(15.0f);
            const float w    = HudFont_Width(HUD_FACE_MEDIUM, px, more);

            Hud_Text(HUD_FACE_MEDIUM, px, t ? x : x + card - w, card * 0.5f + HudFont_CapHeight(HUD_FACE_MEDIUM, px) * 0.5f, more, Hud_Color(0xFCFCFCFF), qtrue);
        }
    }
}

/*
================
Hud_ElemRadar

Top left: the radar (cg_hud_radar), or Omaha's competitive compass: a disc,
its eight bearings turning with the view, the objectives' balls on its edge,
the damage wedge. Neither while the HUD is hidden
================
*/
static void Hud_ElemRadar(void)
{
    hudKey key;

    if (!fr.show) {
        Hud_Hide(E_RADAR);
        return;
    }
    // it turns with the view: made again when the view, what is on it or the damage's fade changed
    key.i((int)(fr.damageAlpha * 255.0f)).i(fr.damageAlpha > 0.0f ? (int)(fr.damageAngle * 100.0f) : 0);
    if (cg_hud_radar->integer) {
        key.i(1).i((int64_t)HudRadar_Frame());
        if (Hud_Begin(E_RADAR, key)) {
            HudRadar_Draw(Px(S(25.0f)), Px(S(25.0f)), Px(S(248.0f)), fr.damageAngle, fr.damageAlpha);
        }
        return;
    }
    if (!hud.uiCompass) {
        hud.uiCompass = cgi.Cvar_Get("ui_compass", "1", 0);
    }
    if (!hud.uiCompass->integer) {
        Hud_Hide(E_RADAR);
        return;
    }
    key.i(2).i((int)(anglemod(fr.viewYaw - fr.ps->stats[STAT_COMPASSNORTH] / 182.0f) * 100.0f));
    key.i(fr.ps->stats[STAT_OBJECTIVECENTER]).i(fr.ps->stats[STAT_OBJECTIVELEFT]).i(fr.ps->stats[STAT_OBJECTIVERIGHT]);
    if (!Hud_Begin(E_RADAR, key)) {
        return;
    }
    {
        static const char *const labels[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        const float              size  = Px(S(160.0f));
        const float              x     = Px(S(25.0f)), y = Px(S(25.0f));
        const float              cx    = x + size * 0.5f, cy = y + size * 0.5f;
        const float              heading = anglemod(fr.viewYaw - fr.ps->stats[STAT_COMPASSNORTH] / 182.0f);
        const float              px    = S(13.0f);
        const int                center = fr.ps->stats[STAT_OBJECTIVECENTER];
        const int                left   = fr.ps->stats[STAT_OBJECTIVELEFT];
        const int                right  = fr.ps->stats[STAT_OBJECTIVERIGHT];
        int                      i;

        Hud_Sprite(HudAtlas_Shape(HUD_SHAPE_DISC, (int)size, 0, 0.0f), x, y, Hud_Color(0x0C0E12C7));
        Hud_Sprite(HudAtlas_Shape(HUD_SHAPE_RING, (int)size, 0, Q_max(1.0f, Px(S(1.0f)))), x, y, Hud_Color(0xFFFFFF1A));
        if (fr.damageAlpha > 0.0f) {
            Hud_SpriteTurned(HudAtlas_Shape(HUD_SHAPE_WEDGE, (int)size, 0, 0.0f), cx, cy, 0.0f, 0.0f, fr.damageAngle, Hud_Fade(Hud_Color(0xC03021FF), fr.damageAlpha));
        }
        // the bearings: a 36 x 16 label at the top, 1.25 % of the dial in,
        // turned about the middle
        for (i = 0; i < 8; i++) {
            const float w    = HudFont_Width(HUD_FACE_SEMIBOLD, px, labels[i]);
            const float top  = -size * 0.5f + size * 0.0125f;
            const float base = top + S(8.0f) + HudFont_CapHeight(HUD_FACE_SEMIBOLD, px) * 0.5f;

            Hud_TextTurned(HUD_FACE_SEMIBOLD, px, cx, cy, -w * 0.5f, base, labels[i], heading + 45.0f * i, Hud_Color(0xFFFFFFFF), qtrue);
        }
        // the objectives on the rim: the middle mark's ball and the two sides'
        {
            const struct {
                int          stat, rest;
                float        d;
                unsigned int color;
            } balls[3] = {
                {center, 1800, 12.0f, 0xFCBA0380},
                {left,   1730, 10.0f, 0x4A9EFF80},
                {right,  1870, 10.0f, 0x4A9EFF80},
            };

            for (i = 0; i < 3; i++) {
                const int d = (int)Px(S(balls[i].d));

                if (balls[i].stat == balls[i].rest) {
                    continue;
                }
                {
                    const float angle = (balls[i].stat - 1800) / 10.0f;
                    const float oy    = -size * 0.5f;

                    Hud_SpriteTurned(HudAtlas_Shape(HUD_SHAPE_DISC, d, 0, 0.0f), cx, cy, -d * 0.5f, oy - d * 0.5f, angle, Hud_Color(balls[i].color));
                    Hud_SpriteTurned(HudAtlas_Shape(HUD_SHAPE_RING, d, 0, Q_max(1.0f, Px(S(1.0f)))), cx, cy, -d * 0.5f, oy - d * 0.5f, angle, Hud_Color(0xFFFFFF1A));
                }
            }
        }
        // the fixed frame at the top (north's slot)
        Hud_Frame(Px(cx - S(11.0f)), y + Px(S(2.0f)), Px(S(22.0f)), Px(S(22.0f)), Q_max(1.0f, Px(S(1.0f))), Hud_Color(0xFFFFFFFF));
    }
}

// the lines of text that fit in width, broken between words
static int Hud_Wrap(hudFace_t face, float px, const char *text, float width, const char **starts, int *lens, int max)
{
    int n = 0;

    while (*text && n < max) {
        const char *p = text, *lastBreak = NULL;
        float       w = 0.0f;

        while (*p) {
            const float a = HudFont_Advance(face, px, (unsigned char)*p);

            if (*p == ' ') {
                lastBreak = p;
            }
            if (w + a > width && p > text) {
                break;
            }
            w += a;
            p++;
        }
        if (*p && lastBreak && lastBreak > text) {
            p = lastBreak;
        }
        starts[n] = text;
        lens[n]   = (int)(p - text);
        n++;
        text = p;
        while (*text == ' ') {
            text++;
        }
    }
    return n;
}

static float Hud_LineAlpha(int time, int life, int fade)
{
    return Com_Clamp(0.0f, 1.0f, (float)(life - (cg.time - time)) / fade);
}

/*
================
Hud_Lines

Lines of text (the game messages, the chat) in a box w wide and h tall at x,
from its top (newest under) or its bottom up (fromBottom): the newest that
fit, each fading out in its last 2 of 8 seconds
================
*/
static void Hud_Lines(const hudLine_t *lines, int count, float x, float y, float w, float h, qboolean fromBottom)
{
    const float px    = S(16.0f);
    const float gap   = S(2.0f);
    float       used  = 0.0f;
    int         first = count;
    int         i;

    // the newest that fit
    for (i = count - 1; i >= 0; i--) {
        const hudFace_t face = lines[i].bold ? HUD_FACE_SEMIBOLD : HUD_FACE_MEDIUM;
        const char     *starts[8];
        int             lens[8];
        const int       n = Hud_Wrap(face, px, lines[i].text, w, starts, lens, 8);
        const float     lh = HudFont_LineHeight(face, px) * n + (used > 0.0f ? gap : 0.0f);

        if (used + lh > h) {
            break;
        }
        used += lh;
        first = i;
    }
    y = fromBottom ? y + h - used : y;
    for (i = first; i < count; i++) {
        const hudFace_t  face  = lines[i].bold ? HUD_FACE_SEMIBOLD : HUD_FACE_MEDIUM;
        const hudColor_t color = Hud_Fade(lines[i].color, Hud_LineAlpha(lines[i].time, 8000, 2000));
        const char      *starts[8];
        int              lens[8];
        const int        n = Hud_Wrap(face, px, lines[i].text, w, starts, lens, 8);

        for (int k = 0; k < n; k++) {
            char buf[HUD_FEED_TEXT];

            Q_strncpyz(buf, starts[k], Q_min((int)sizeof(buf), lens[k] + 1));
            Hud_Text(face, px, x, y + HudFont_Ascent(face, px), buf, color, qtrue);
            y += HudFont_LineHeight(face, px);
        }
        y += gap;
    }
}

static void Hud_LinesKey(hudKey &key, const hudLine_t *lines, int count)
{
    key.i(HudFeed_Revision()).i(count);
    for (int i = 0; i < count; i++) {
        key.i((int)(Hud_LineAlpha(lines[i].time, 8000, 2000) * 255.0f));
    }
}

// under the radar (or the compass): the game messages
static void Hud_ElemMessages(void)
{
    int              count;
    const hudLine_t *lines = HudFeed_Messages(&count);
    hudKey           key;

    if (!cg_hud_messages->integer || !count) {
        Hud_Hide(E_MESSAGES);
        return;
    }
    Hud_LinesKey(key, lines, count);
    key.i(cg_hud_radar->integer);
    if (!Hud_Begin(E_MESSAGES, key)) {
        return;
    }
    Hud_Lines(lines, count, Px(S(25.0f)), Px(S(25.0f + (cg_hud_radar->integer ? 248.0f : 160.0f) + 16.0f)), S(360.0f), S(120.0f), qfalse);
}

// bottom left: the chat, up from over who you follow
static void Hud_ElemChat(void)
{
    int              count;
    const hudLine_t *lines = HudFeed_Chat(&count);
    hudKey           key;

    if (!cg_hud_chat->integer || !count) {
        Hud_Hide(E_CHAT);
        return;
    }
    Hud_LinesKey(key, lines, count);
    if (!Hud_Begin(E_CHAT, key)) {
        return;
    }
    Hud_Lines(lines, count, Px(S(25.0f)), Px(hud.h - S(96.0f + 160.0f)), S(360.0f), S(160.0f), qtrue);
}

/*
================
Hud_ElemKillFeed

Top right: the kills of the last 6 seconds, fading in their last 1.5: the
killer, the weapon's silhouette, a skull for a suicide, a headshot's mark,
the victim, in their teams' colors
================
*/
static void Hud_ElemKillFeed(void)
{
    int              count, first, i;
    const hudKill_t *kills = HudFeed_Kills(&count);
    const float      px    = S(16.0f);
    const float      gap   = S(6.0f);
    const float      lineH = HudFont_LineHeight(HUD_FACE_MEDIUM, px);
    const int        skull = HudSilhouette_Find("skull_50");
    const int        headshot = HudSilhouette_Find("headshot_50");
    const int        glove = HudSilhouette_Find("glove_50");
    hudKey           key;
    float            y, used = 0.0f;

    if (!cg_hud_killfeed->integer || !count) {
        Hud_Hide(E_KILLFEED);
        return;
    }
    key.i(HudFeed_Revision()).i(count);
    for (i = 0; i < count; i++) {
        key.i((int)(Hud_LineAlpha(kills[i].time, 6000, 1500) * 255.0f));
    }
    if (!Hud_Begin(E_KILLFEED, key)) {
        return;
    }
    // the newest that fit in 140 px
    for (first = count; first > 0; first--) {
        const float h = Q_max(lineH, S((float)kills[first - 1].iconH)) + (used > 0.0f ? S(3.0f) : 0.0f);

        if (used + h > S(140.0f)) {
            break;
        }
        used += h;
    }
    y = S(25.0f);
    for (i = first; i < count; i++) {
        const hudKill_t   *k     = &kills[i];
        const float        alpha = Hud_LineAlpha(k->time, 6000, 1500);
        const float        rowH  = Q_max(lineH, S((float)k->iconH));
        const float        mid   = y + rowH * 0.5f;
        // a long name smaller, to fit 150 px
        const float        pxK   = Hud_Fit(HUD_FACE_MEDIUM, px, k->killer, S(150.0f));
        const float        pxV   = Hud_Fit(HUD_FACE_MEDIUM, px, k->victim, S(150.0f));
        const hudSprite_t *icon  = k->icon >= 0 ? HudAtlas_Silhouette(k->icon, (int)S((float)k->iconW), (int)S((float)k->iconH)) : NULL;
        const qboolean     mark  = k->suicide && k->icon != skull;
        float              w     = 0.0f, x;

        if (k->killer[0]) {
            w += HudFont_Width(HUD_FACE_MEDIUM, pxK, k->killer) + gap;
        }
        if (icon) {
            w += icon->w + gap;
        }
        if (mark) {
            w += S(18.0f) + gap;
        }
        if (k->headshot) {
            w += S(18.0f) + gap;
        }
        w += HudFont_Width(HUD_FACE_MEDIUM, pxV, k->victim);
        x = Px(hud.w - S(25.0f) - w);

        if (k->killer[0]) {
            x += Hud_Text(HUD_FACE_MEDIUM, pxK, x, mid + HudFont_CapHeight(HUD_FACE_MEDIUM, pxK) * 0.5f, k->killer, Hud_Fade(Hud_TeamColor(k->killerTeam), alpha), qtrue) + gap;
        }
        if (icon) {
            const float iy = Px(mid - icon->h * 0.5f);

            // the pictures point left; the feed's point at the victim
            if (k->icon == skull || k->icon == glove) {
                Hud_Sprite(icon, Px(x), iy, Hud_Fade(Hud_Color(0xFFFFFFFF), alpha));
            } else {
                Hud_SpriteMirrored(icon, Px(x), iy, Hud_Fade(Hud_Color(0xFFFFFFFF), alpha));
            }
            x += icon->w + gap;
        }
        if (mark) {
            const hudSprite_t *sp = HudAtlas_Silhouette(skull, (int)S(18.0f), (int)S(18.0f));

            Hud_Sprite(sp, Px(x), Px(mid - sp->h * 0.5f), Hud_Fade(Hud_Color(0xFFFFFFFF), alpha));
            x += S(18.0f) + gap;
        }
        if (k->headshot) {
            const hudSprite_t *sp = HudAtlas_Silhouette(headshot, (int)S(18.0f), (int)S(18.0f));

            Hud_Sprite(sp, Px(x), Px(mid - sp->h * 0.5f), Hud_Fade(Hud_Color(0xFFFFFFFF), alpha));
            x += S(18.0f) + gap;
        }
        Hud_Text(HUD_FACE_MEDIUM, pxV, x, mid + HudFont_CapHeight(HUD_FACE_MEDIUM, pxV) * 0.5f, k->victim, Hud_Fade(Hud_TeamColor(k->victimTeam), alpha), qtrue);
        y += rowH + S(3.0f);
    }
}

// bottom left: who you follow, the last player who hurt you
static void Hud_ElemBottomLeft(void)
{
    const int   attacker = fr.ps->stats[STAT_ATTACKERCLIENT];
    const char *follow   = fr.show && cg_hud_following->integer && fr.following ? cg.clientinfo[fr.followClient].name : "";
    const char *hurt     = fr.show && cg_hud_attacker->integer && attacker >= 0 && attacker < MAX_CLIENTS
                             ? Info_ValueForKey(CG_ConfigString(CS_PLAYERS + attacker), "name")
                             : "";
    const float px = S(18.0f);
    hudKey      key;
    float       y = hud.h - S(30.0f);

    key.s(follow).s(hurt).i(fr.following ? cg.clientinfo[fr.followClient].team : 0).i(attacker >= 0 && attacker < MAX_CLIENTS ? cg.clientinfo[attacker].team : 0);
    if (!follow[0] && !hurt[0]) {
        Hud_Hide(E_BOTTOMLEFT);
        return;
    }
    if (!Hud_Begin(E_BOTTOMLEFT, key)) {
        return;
    }
    // stacked up from the bottom
    if (hurt[0]) {
        const int team = cg.clientinfo[attacker].team;

        Hud_Text(HUD_FACE_MEDIUM, Hud_Fit(HUD_FACE_MEDIUM, px, hurt, S(360.0f)), Px(S(25.0f)), y - HudFont_Descent(HUD_FACE_MEDIUM, px), hurt,
                 Hud_Color(team == TEAM_AXIS ? 0xCC8888FF : team == TEAM_ALLIES ? 0x8891CCFF : 0xFFFFFFFF), qtrue);
        y -= HudFont_LineHeight(HUD_FACE_MEDIUM, px) + S(4.0f);
    }
    if (follow[0]) {
        Hud_Text(HUD_FACE_MEDIUM, Hud_Fit(HUD_FACE_MEDIUM, px, follow, S(360.0f)), Px(S(25.0f)), y - HudFont_Descent(HUD_FACE_MEDIUM, px), follow,
                 Hud_TeamColor(cg.clientinfo[fr.followClient].team), qtrue);
    }
}

/*
================
The status strip

Bottom center, along an 800 px strip whose middle is the screen's: the
health over its bar, a thin line in to the team's badge in the middle, a
thin line out to the ammo (clip | reserve and a bullet)
================
*/
static float Hud_StripX(void)
{
    return hud.w * 0.5f - S(400.0f);
}

static float Hud_StripMid(void)
{
    return hud.h - S(52.0f);
}

static void Hud_ElemHealth(void)
{
    const qboolean low   = fr.health < cg_hud_lowhealth->value * fr.maxHealth;
    char           text[16];
    hudKey         key;

    if (!fr.strip || !cg_hud_health->integer) {
        Hud_Hide(E_HEALTH);
        return;
    }
    Com_sprintf(text, sizeof(text), "%d", fr.health);
    key.s(text).i(low);
    if (!Hud_Begin(E_HEALTH, key)) {
        return;
    }
    // the number in the middle of a fixed yellow underline as wide as "100"
    Hud_Label(HUD_FACE_MEDIUM, S(32.0f), Hud_StripX() + S(59.0f), S(47.0f), Hud_StripMid(), text, Hud_Color(low ? 0xC03021FF : 0xF6F67AFF), qtrue);
    Hud_Rect(Px(Hud_StripX() + S(59.0f)), Px(hud.h - S(104.0f) + S(72.0f)), Px(S(47.0f)), Px(S(5.0f)), Hud_Color(0xF6F67AFF));
}

static void Hud_ElemBadge(void)
{
    const int team = cg_hud_teamicon->integer && cgs.gametype > GT_FFA && (fr.team == TEAM_ALLIES || fr.team == TEAM_AXIS) ? fr.team : 0;
    hudKey    key;

    if (!fr.strip || !cg_hud_badge->integer) {
        Hud_Hide(E_BADGE);
        return;
    }
    key.i(team);
    if (!Hud_Begin(E_BADGE, key)) {
        return;
    }
    {
        const float mid  = Px(Hud_StripMid());
        const float line = Q_max(1.0f, Px(S(1.0f)));
        const int   d    = (int)Px(S(56.0f));
        const float bx   = Px(hud.w * 0.5f - d * 0.5f), by = Px(mid - d * 0.5f);

        // the lines fade out away from the badge
        Hud_RectH(Px(Hud_StripX() + S(194.0f)), mid, Px(S(172.0f)), line, Hud_Color(0xEEF0A800), Hud_Color(0xEEF0A8B3));
        Hud_RectH(Px(Hud_StripX() + S(432.0f)), mid, Px(S(190.0f)), line, Hud_Color(0xEEF0A8B3), Hud_Color(0xEEF0A800));
        // the badge: the team's icon filling the circle, the ring over it
        Hud_Sprite(HudAtlas_Shape(HUD_SHAPE_DISC, d, 0, 0.0f), bx, by, Hud_Color(0x161C1E8C));
        Hud_TeamIcon(team, bx, by, d, 1.0f);
        Hud_Sprite(HudAtlas_Shape(HUD_SHAPE_RING, d, 0, Px(S(2.0f))), bx, by, Hud_Color(0xE8EBA4FF));
    }
}

static void Hud_ElemAmmo(void)
{
    char   clip[16], ammo[16];
    hudKey key;

    if (!fr.strip || !cg_hud_ammo->integer || !fr.weapon[0]) {
        Hud_Hide(E_AMMO);
        return;
    }
    Com_sprintf(clip, sizeof(clip), "%d", fr.clip);
    Com_sprintf(ammo, sizeof(ammo), "%d", fr.ammo);
    key.s(clip).s(ammo);
    if (!Hud_Begin(E_AMMO, key)) {
        return;
    }
    {
        const float      mid    = Hud_StripMid();
        const hudColor_t yellow = Hud_Color(0xF6F67AFF);
        float            x      = Hud_StripX() + S(655.0f);
        const int        bw = (int)Q_max(2.0f, Px(S(5.0f))), bh = (int)Px(S(17.0f));

        x += Hud_Label(HUD_FACE_MEDIUM, S(36.0f), x, 0.0f, mid, clip, yellow, qfalse);
        x += S(24.0f);
        Hud_Rect(Px(x), Px(mid - S(19.0f) * 0.5f), Q_max(1.0f, Px(S(2.0f))), Px(S(19.0f)), Hud_Color(0xEEF0A899));
        x += S(2.0f) + S(10.0f);
        x += Hud_Label(HUD_FACE_MEDIUM, S(21.0f), x, 0.0f, mid, ammo, yellow, qfalse);
        x += S(25.0f);
        // the bullet: a rounded bar turned 45 degrees, its box's middle where the bar's is
        Hud_Sprite(HudAtlas_Shape(HUD_SHAPE_BULLET, bw, bh, S(2.0f)), Px(x + S(2.5f)), Px(mid), yellow);
    }
}

/*
================
Hud_ElemWeapons

The right edge, top to bottom: the grenades (frags then smokes, each with
how many), the primary, the sidearm, each with its key; the one in hand
bright and named under it
================
*/
static void Hud_WeaponItem(float right, float *bottom, const hudSprite_t *const *icons, const char *const *counts, int n, int handIndex, const char *slot, qboolean inHand)
{
    const float pxName  = S(12.0f);
    const float pxCount = S(14.0f);
    float       iconH   = 0.0f, w = 0.0f, x, top;
    int         i;

    // the row: an icon, 4 px, its count, 8 px, 4 px, the next icon...
    for (i = 0; i < n; i++) {
        if (icons[i]) {
            iconH = Q_max(iconH, (float)icons[i]->h);
            w += icons[i]->w;
        }
        if (counts && counts[i]) {
            w += S(4.0f) + HudFont_Width(HUD_FACE_MEDIUM, pxCount, counts[i]);
        }
        if (i + 1 < n) {
            w += (counts && counts[i] ? S(8.0f) : 0.0f) + S(4.0f);
        }
    }
    top = *bottom - (iconH + (inHand ? S(8.0f) + HudFont_LineHeight(HUD_FACE_MEDIUM, pxName) : 0.0f));

    // the key, its top 8 px over the item's
    {
        const float kw = HudFont_Width(HUD_FACE_MEDIUM, pxName, slot);

        Hud_Text(HUD_FACE_MEDIUM, pxName, Px(hud.w - S(14.0f) - kw), top - S(8.0f) + HudFont_Ascent(HUD_FACE_MEDIUM, pxName), slot, Hud_Color(0xFCFCFCFF), qtrue);
    }
    x = Px(right - w);
    for (i = 0; i < n; i++) {
        const float alpha = handIndex == i ? 1.0f : 0.55f;

        if (icons[i]) {
            Hud_Sprite(icons[i], Px(x), Px(top + (iconH - icons[i]->h) * 0.5f), Hud_Fade(Hud_Color(0xFAFA90FF), alpha));
            x += icons[i]->w;
        }
        if (counts && counts[i]) {
            // at the row's bottom
            x += S(4.0f);
            x += Hud_Text(HUD_FACE_MEDIUM, pxCount, Px(x), top + iconH - HudFont_Descent(HUD_FACE_MEDIUM, pxCount), counts[i], Hud_Fade(Hud_Color(0xF6F67AFF), alpha), qtrue);
        }
        if (i + 1 < n) {
            x += (counts && counts[i] ? S(8.0f) : 0.0f) + S(4.0f);
        }
    }
    if (inHand) {
        const float px = Hud_Fit(HUD_FACE_MEDIUM, pxName, fr.weapon, S(200.0f));
        const float nw = HudFont_Width(HUD_FACE_MEDIUM, px, fr.weapon);

        Hud_Text(HUD_FACE_MEDIUM, px, Px(right - nw), top + iconH + S(8.0f) + HudFont_Ascent(HUD_FACE_MEDIUM, pxName), fr.weapon, Hud_Color(0xFCFCFCFF), qtrue);
    }
    *bottom = top - S(14.0f);
}

static void Hud_ElemWeapons(void)
{
    const qboolean primaryHand = fr.primarySlot && fr.handSlot == fr.primarySlot;
    const qboolean grenades    = cg_hud_grenades->integer && (wb.grenades > 0 || wb.smokes > 0);
    const int      sidearm     = (fr.owned & WEAPON_CLASS_PISTOL) ? Hud_WeaponSilhouette(wb.sidearm) : -1;
    const int      primary     = fr.primarySlot ? Hud_WeaponSilhouette(wb.primary) : -1;
    hudKey         key;

    if (!fr.strip || !cg_hud_ammo->integer) {
        Hud_Hide(E_WEAPONS);
        return;
    }
    key.i(sidearm).i(primary).i(fr.primarySlot).i(fr.handSlot).i(fr.grenadeHand).i(grenades ? wb.grenades : -1).i(grenades ? wb.smokes : -1).i(wb.nation).s(fr.weapon);
    if (!Hud_Begin(E_WEAPONS, key)) {
        return;
    }
    {
        const float right  = hud.w - S(14.0f) - S(12.0f) - S(6.0f);
        float       bottom = hud.h - S(125.0f);

        if (fr.owned & WEAPON_CLASS_PISTOL) {
            const hudSprite_t *icon = HudAtlas_Silhouette(sidearm, (int)S(110.0f), (int)S(30.0f));

            Hud_WeaponItem(right, &bottom, &icon, NULL, 1, fr.handSlot == 1 ? 0 : -1, "1", fr.handSlot == 1);
        }
        if (fr.primarySlot) {
            const hudSprite_t *icon = HudAtlas_Silhouette(primary, (int)S(160.0f), (int)S(34.0f));

            Hud_WeaponItem(right, &bottom, &icon, NULL, 1, primaryHand ? 0 : -1, va("%d", fr.primarySlot), primaryHand);
        }
        if (grenades) {
            const hudSprite_t *icons[2] = {NULL, NULL};
            const char        *counts[2] = {NULL, NULL};
            char               frag[8], smoke[8];
            int                n = 0, hand = -1;

            if (wb.grenades > 0) {
                Com_sprintf(frag, sizeof(frag), "%d", wb.grenades);
                icons[n]  = HudAtlas_Silhouette(Hud_WeaponSilhouette(Hud_NationGrenade(wb.nation, qfalse)), (int)S(200.0f), (int)S(38.0f));
                counts[n] = frag;
                if (fr.grenadeHand == 1) {
                    hand = n;
                }
                n++;
            }
            if (wb.smokes > 0) {
                Com_sprintf(smoke, sizeof(smoke), "%d", wb.smokes);
                icons[n]  = HudAtlas_Silhouette(Hud_WeaponSilhouette(Hud_NationGrenade(wb.nation, qtrue)), (int)S(200.0f), (int)S(38.0f));
                counts[n] = smoke;
                if (fr.grenadeHand == 2) {
                    hand = n;
                }
                n++;
            }
            Hud_WeaponItem(right, &bottom, icons, counts, n, hand, "5", fr.grenadeHand != 0);
        }
    }
}

// a banner 70 % of the way down: grey with yellow edges, at least 300 px
static float Hud_Banner(const char *text, float y)
{
    const float px = Hud_Fit(HUD_FACE_MEDIUM, S(17.0f), text, hud.w * 0.8f);
    const float h  = Px(S(35.0f));
    const float w  = Px(Q_max(S(300.0f), HudFont_Width(HUD_FACE_MEDIUM, px, text) + S(48.0f)));
    const float x  = Px(hud.w * 0.5f - w * 0.5f);
    const float e  = Q_max(1.0f, Px(S(2.0f)));

    Hud_Rect(x, y, w, h, Hud_Color(0x32342E99));
    Hud_Rect(x, y, e, h, Hud_Color(0xB4C08CFF));
    Hud_Rect(x + w - e, y, e, h, Hud_Color(0xB4C08CFF));
    Hud_Label(HUD_FACE_MEDIUM, px, x, w, y + h * 0.5f, text, Hud_Color(0xFCFCFCFF), qtrue);
    return h + S(6.0f);
}

static void Hud_ElemBanners(void)
{
    const char *timeMessage = fr.show && cg_hud_timer->integer && !(cg.matchStartTime != -1 && cgs.matchEndTime) ? ui_timemessage->string : "";
    const char *spectator   = fr.show && cg_hud_spectator->integer ? fr.spectatorText : "";
    hudKey      key;

    if (!timeMessage[0] && !spectator[0]) {
        Hud_Hide(E_BANNERS);
        return;
    }
    key.s(timeMessage).s(spectator);
    if (!Hud_Begin(E_BANNERS, key)) {
        return;
    }
    {
        float y = Px(hud.h * 0.694f);

        if (timeMessage[0]) {
            y += Hud_Banner(cgi.LV_ConvertString(timeMessage), y);
        }
        if (spectator[0]) {
            Hud_Banner(spectator, y);
        }
    }
}

/*
================
Hud_ElemStopwatch

Under the middle: the grenade's charge (its throw's strength, five parts)
and the stopwatch of a bomb being planted or defused (a bar shrinking to 0)
================
*/
static void Hud_ElemStopwatch(void)
{
    float  stopwatch = 0.0f, charge = -1.0f;
    hudKey key;

    if (fr.show && cg_hud_stopwatch->integer && cgi.stopWatch->iStartTime && cgi.stopWatch->iStartTime < cgi.stopWatch->iEndTime
        && cg.ObjectivesCurrentAlpha < 0.02f && fr.ps->stats[STAT_HEALTH] > 0) {
        // Omaha's: the bar outlasts the script's wait by a quarter second
        const int total = cgi.stopWatch->iEndTime - cgi.stopWatch->iStartTime;

        if (cgi.stopWatch->eType >= SWT_FUSE_WET) {
            stopwatch = cgi.stopWatch->iEndTime > cg.time ? 1.0f : 0.0f;
        } else if (cgi.stopWatch->iEndTime + 250 > cg.time) {
            stopwatch = Com_Clamp(0.0f, 1.0f, (float)(cgi.stopWatch->iEndTime + 250 - cg.time) / (total + 250));
        }
    }
    if (fr.show && cg_hud_grenadecharge->integer && wb.charging) {
        // the stock scripts' charge: 0.6 to 4 s, 0.5 to 1.5 for the secondary throw
        const float lo = wb.chargeSecondary ? 0.5f : 0.6f, hi = wb.chargeSecondary ? 1.5f : 4.0f;
        const float t  = Com_Clamp(0.0f, hi, (cg.time - wb.chargeStart) / 1000.0f);

        charge = Com_Clamp(0.0f, 1.0f, (t - lo) / (hi - lo));
    }
    if (stopwatch <= 0.0f && charge < 0.0f) {
        Hud_Hide(E_STOPWATCH);
        return;
    }
    key.i((int)(stopwatch * 300.0f * hud.scale)).i((int)(charge * 300.0f * hud.scale));
    if (!Hud_Begin(E_STOPWATCH, key)) {
        return;
    }
    {
        const float w    = Px(S(300.0f)), h = Px(S(24.0f));
        const float x    = Px(hud.w * 0.5f - w * 0.5f);
        const float line = Q_max(1.0f, Px(S(1.0f)));
        const float both = (charge >= 0.0f ? h : 0.0f) + (stopwatch > 0.0f ? h : 0.0f) + (charge >= 0.0f && stopwatch > 0.0f ? S(8.0f) : 0.0f);
        float       y    = Px(hud.h * 0.75f - S(8.0f) - both * 0.5f);

        if (charge >= 0.0f) {
            const float fw = Px(w * charge);

            Hud_Rect(x, y, w, h, Hud_Color(0x0C0E12C7));
            Hud_Rect(Px(hud.w * 0.5f - fw * 0.5f), y, fw, h, Hud_Color(0x1A6FD4AA));
            for (int i = 1; i <= 4; i++) {
                Hud_Rect(Px(x + S(60.0f) * i), y, line, h, Hud_Color(0xFFFFFF1A));
            }
            Hud_Frame(x, y, w, h, line, Hud_Color(0xFFFFFF1A));
            y += h + S(8.0f);
        }
        if (stopwatch > 0.0f) {
            const float fw = Px(w * stopwatch);

            Hud_Rect(Px(hud.w * 0.5f - fw * 0.5f), y, fw, h, Hud_Color(0x1A6FD4AA));
        }
    }
}

/*
================
Hud_ElemInfo

Who killed you, a quarter of the way down; the player you aim at (the
server names teammates only, anyone while you spectate) with a health bar,
three quarters down
================
*/
static void Hud_ElemInfo(void)
{
    const int   attacker = fr.ps->stats[STAT_ATTACKERCLIENT];
    const int   info     = fr.ps->stats[STAT_INFOCLIENT];
    const int   infoHealth = fr.ps->stats[STAT_INFOCLIENT_HEALTH];
    const char *killer = "";
    const char *target = "";
    int         killerTeam = 0, targetTeam = 0;
    hudKey      key;

    if (fr.show && cg_hud_killer->integer && attacker >= 0 && attacker < MAX_CLIENTS && fr.ps->stats[STAT_HEALTH] <= 0
        && !(fr.ps->pm_flags & PMF_SPECTATING) && attacker != fr.ps->clientNum) {
        killer     = Info_ValueForKey(CG_ConfigString(CS_PLAYERS + attacker), "name");
        killerTeam = cg.clientinfo[attacker].team;
    }
    if (fr.show && cg_hud_targetinfo->integer && info >= 0 && info < MAX_CLIENTS && infoHealth > 0 && !fr.following) {
        target     = Info_ValueForKey(CG_ConfigString(CS_PLAYERS + info), "name");
        targetTeam = cg.clientinfo[info].team;
    }
    if (!killer[0] && !target[0]) {
        Hud_Hide(E_INFO);
        return;
    }
    key.s(killer).i(killerTeam).s(target).i(targetTeam).i(target[0] ? infoHealth : 0);
    if (!Hud_Begin(E_INFO, key)) {
        return;
    }
    if (killer[0]) {
        Hud_Label(HUD_FACE_MEDIUM, Hud_Fit(HUD_FACE_MEDIUM, S(16.0f), killer, S(400.0f)), 0.0f, (float)hud.w, Px(hud.h * 0.25f), killer,
                  Hud_Color(killerTeam == TEAM_AXIS ? 0xCC8888FF : killerTeam == TEAM_ALLIES ? 0x8891CCFF : 0xFFFFFFFF), qtrue);
    }
    if (target[0]) {
        const float px   = Hud_Fit(HUD_FACE_MEDIUM, S(16.0f), target, S(400.0f));
        const float lh   = HudFont_LineHeight(HUD_FACE_MEDIUM, S(16.0f));
        const float top  = Px(hud.h * 0.75f - (lh + S(5.0f) + S(6.0f)) * 0.5f);
        const float bw   = Px(S(120.0f)), bh = Px(S(6.0f));
        const float bx   = Px(hud.w * 0.5f - bw * 0.5f), by = Px(top + lh + S(5.0f));
        const float line = Q_max(1.0f, Px(S(1.0f)));
        const float tw   = HudFont_Width(HUD_FACE_MEDIUM, px, target);

        Hud_Text(HUD_FACE_MEDIUM, px, Px(hud.w * 0.5f - tw * 0.5f), top + HudFont_Ascent(HUD_FACE_MEDIUM, px), target,
                 Hud_Color(targetTeam == TEAM_AXIS ? 0xCC8888FF : targetTeam == TEAM_ALLIES ? 0x8891CCFF : 0xFFFFFFFF), qtrue);
        Hud_Rect(bx, by, bw, bh, Hud_Color(0x0C0E12C7));
        Hud_Rect(bx, by, Px(S(1.2f * Com_Clamp(0, 100, infoHealth))), bh, Hud_Color(infoHealth < 40 ? 0xC03021FF : 0xFFFFFF9C));
        Hud_Frame(bx, by, bw, bh, line, Hud_Color(0xFFFFFF1A));
    }
}

/*
================
Hud_ElemVote

A vote: what it is about, the yes and no counts, the seconds left, and how
to vote until you have
================
*/
static void Hud_ElemVote(void)
{
    int    seconds;
    hudKey key;

    if (!cg_hud_vote->integer || !cgs.voteTime) {
        Hud_Hide(E_VOTE);
        return;
    }
    seconds = Q_max(0, (30000 - (cg.time - cgs.voteTime)) / 1000 + 1);
    key.s(cgs.voteString).i(cgs.numVotesYes).i(cgs.numVotesNo).i(seconds).i(fr.ps->voted);
    if (!Hud_Begin(E_VOTE, key)) {
        return;
    }
    {
        const float      w   = Px(S(280.0f));
        const float      pad = S(14.0f), padY = S(10.0f), gap = S(6.0f);
        const float      px  = S(16.0f), small = S(12.0f);
        const char      *subjectStarts[6], *promptStarts[6];
        int              subjectLens[6], promptLens[6];
        const char      *prompt = fr.ps->voted ? "" : cgi.LV_ConvertString("To vote Yes, press F1. To vote No, press F2.");
        const int        ns = Hud_Wrap(HUD_FACE_MEDIUM, px, cgs.voteString, w - pad * 2.0f, subjectStarts, subjectLens, 6);
        const int        np = prompt[0] ? Hud_Wrap(HUD_FACE_MEDIUM, small, prompt, w - pad * 2.0f, promptStarts, promptLens, 6) : 0;
        const float      lh = HudFont_LineHeight(HUD_FACE_MEDIUM, px), lhs = HudFont_LineHeight(HUD_FACE_MEDIUM, small);
        const float      h  = padY * 2.0f + lhs + gap + lh * ns + gap + lh + (np ? gap + lhs * np : 0.0f);
        const float      x  = Px(hud.w - S(25.0f) - w);
        float            y  = Px(hud.h * 0.5f - h * 0.5f);
        const float      line = Q_max(1.0f, Px(S(1.0f)));
        const char      *yes = va("%s  %d", cgi.LV_ConvertString("Yes"), cgs.numVotesYes);
        const char      *no;
        char             buf[256];
        float            cx;

        Hud_Rect(x, y, w, h, Hud_Color(0x0C0E12C7));
        Hud_Frame(x, y, w, h, line, Hud_Color(0xFFFFFF1A));
        y += padY;
        // VOTE, spaced out, and the seconds left
        cx = x + pad;
        for (const char *p = "VOTE"; *p; p++) {
            const char c[2] = {*p, 0};

            cx += Hud_Text(HUD_FACE_MEDIUM, small, cx, y + HudFont_Ascent(HUD_FACE_MEDIUM, small), c, Hud_Color(0x1A6FD4FF), qfalse) + S(2.0f);
        }
        {
            const char *s = va("%ds", seconds);

            Hud_Text(HUD_FACE_MEDIUM, small, x + w - pad - HudFont_Width(HUD_FACE_MEDIUM, small, s), y + HudFont_Ascent(HUD_FACE_MEDIUM, small), s, Hud_Color(0xBFBFBFFF), qtrue);
        }
        y += lhs + gap;
        for (int i = 0; i < ns; i++) {
            Q_strncpyz(buf, subjectStarts[i], Q_min((int)sizeof(buf), subjectLens[i] + 1));
            Hud_Text(HUD_FACE_MEDIUM, px, x + pad, y + HudFont_Ascent(HUD_FACE_MEDIUM, px), buf, Hud_Color(0xFFFFFFFF), qtrue);
            y += lh;
        }
        y += gap;
        cx = x + pad;
        Q_strncpyz(buf, yes, sizeof(buf));
        cx += Hud_Text(HUD_FACE_MEDIUM, px, cx, y + HudFont_Ascent(HUD_FACE_MEDIUM, px), buf, Hud_Color(0xFFFFFFFF), qtrue) + S(18.0f);
        no = va("%s  %d", cgi.LV_ConvertString("No"), cgs.numVotesNo);
        Hud_Text(HUD_FACE_MEDIUM, px, cx, y + HudFont_Ascent(HUD_FACE_MEDIUM, px), no, Hud_Color(0xFFFFFFFF), qtrue);
        y += lh;
        if (np) {
            y += gap;
            for (int i = 0; i < np; i++) {
                Q_strncpyz(buf, promptStarts[i], Q_min((int)sizeof(buf), promptLens[i] + 1));
                Hud_Text(HUD_FACE_MEDIUM, small, x + pad, y + HudFont_Ascent(HUD_FACE_MEDIUM, small), buf, Hud_Color(0xBFBFBFFF), qtrue);
                y += lhs;
            }
        }
    }
}

/*
================
Hud_ElemDamage

The damage's direction: thin red crescents well out from the crosshair
toward where it came from, up to three at once, each turning with the view
and fading in 1.5 s, a faint glow under each (Omaha's hud-damage-indicator)
================
*/
static void Hud_ElemDamage(void)
{
    float  alpha[3];
    hudKey key;
    int    i, any = 0;

    for (i = 0; i < 3; i++) {
        const int age = cg.time - hud.arcs[i].time;

        alpha[i] = hud.arcs[i].time && age >= 0 && age < 1500 ? 1.0f - age / 1500.0f : 0.0f;
        if (alpha[i] <= 0.02f) {
            alpha[i] = 0.0f;
        }
        any |= alpha[i] > 0.0f;
    }
    if (!fr.show || !cg_hud_damage->integer || !any) {
        Hud_Hide(E_DAMAGE);
        return;
    }
    for (i = 0; i < 3; i++) {
        key.i((int)(alpha[i] * 255.0f)).f(alpha[i] > 0.0f ? Hud_DamageAngle(hud.arcs[i].stat) : 0.0f);
    }
    if (!Hud_Begin(E_DAMAGE, key)) {
        return;
    }
    {
        const int   box = (int)Px(S(640.0f));
        const float cx = hud.w * 0.5f, cy = hud.h * 0.5f;
        int         pass;

        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < 3; i++) {
                if (alpha[i] <= 0.0f) {
                    continue;
                }
                Hud_SpriteTurned(HudAtlas_Shape(pass ? HUD_SHAPE_CRESCENT : HUD_SHAPE_CRESCENT_GLOW, box, 0, 0.0f), cx, cy, 0.0f, 0.0f,
                                 Hud_DamageAngle(hud.arcs[i].stat), Hud_Fade(Hud_Color(pass ? 0xD2241CFF : 0xB41414FF), alpha[i]));
            }
        }
    }
}

// the crosshair (cg_drawtools.cpp lays it out), in the same draw
static void Hud_ElemCrosshair(void)
{
    const float (*arms)[4];
    const float *color;
    hudKey       key;

    if (!CG_CrosshairBoxes(&arms, &color)) {
        Hud_Hide(E_CROSSHAIR);
        return;
    }
    for (int i = 0; i < 4; i++) {
        key.f(arms[i][0]).f(arms[i][1]).f(arms[i][2]).f(arms[i][3]);
    }
    key.f(color[0]).f(color[1]).f(color[2]).f(color[3]);
    if (!Hud_Begin(E_CROSSHAIR, key)) {
        return;
    }
    {
        hudColor_t c;

        c.r = (byte)(color[0] * 255.0f + 0.5f);
        c.g = (byte)(color[1] * 255.0f + 0.5f);
        c.b = (byte)(color[2] * 255.0f + 0.5f);
        c.a = (byte)(color[3] * 255.0f + 0.5f);
        for (int i = 0; i < 4; i++) {
            Hud_Rect(arms[i][0], arms[i][1], arms[i][2], arms[i][3], c);
        }
    }
}

/*
====================================================================

THE DRAW

====================================================================
*/

static void Hud_Elements(void)
{
    Hud_ElemScore();
    Hud_ElemCards();
    Hud_ElemRadar();
    Hud_ElemMessages();
    Hud_ElemKillFeed();
    Hud_ElemChat();
    Hud_ElemBottomLeft();
    Hud_ElemHealth();
    Hud_ElemBadge();
    Hud_ElemAmmo();
    Hud_ElemWeapons();
    Hud_ElemBanners();
    Hud_ElemStopwatch();
    Hud_ElemInfo();
    Hud_ElemVote();
    Hud_ElemDamage();
    Hud_ElemCrosshair();
    hud.cur = NULL;
}

// the elements' quads into the array drawn: those from the first that changed
static void Hud_Compact(void)
{
    int at = 0;
    int i;

    for (i = 0; i < E_COUNT; i++) {
        hudElem_t &e = hud.elems[i];
        const int  n = Q_min((int)e.v.size(), HUD_MAX_QUADS * 4 - at);

        // (one before it that grew or shrank moved it)
        if (e.dirty || e.first != at) {
            if (n) {
                memcpy(&hud.out[at], e.v.data(), sizeof(vert2D_t) * n);
            }
            e.first = at;
        }
        e.dirty = qfalse;
        at += n;
    }
    hud.outVerts = at;
}

qboolean CG_HudCompetitive_Active(void)
{
    if (!cg_hudstyle || cg_hudstyle->integer != 1) {
        return qfalse;
    }
    // chosen in the middle of a map: made now (the stock HUD's pieces are already hidden)
    if (!hud.ready && !hud.failed && cg.snap) {
        Hud_Start();
    }
    return hud.ready;
}

void CG_HudCompetitive_Draw(void)
{
    int quads, q, pass;

    if (!cg_hudstyle || cg_hudstyle->integer != 1 || !cg_hud->integer || !cg.snap) {
        return;
    }
    if (!hud.ready) {
        return;
    }
    if (!hud.radarLoaded && cg_hud_radar->integer) {
        // the map's walk the first time the radar shows (with the map's load when it is on)
        HudRadar_MapLoaded();
        hud.radarLoaded = qtrue;
    }

    Hud_GatherFrame();
    HudFeed_Frame();
    for (pass = 0; pass < 2; pass++) {
        const int generation = HudAtlas_Generation();

        Hud_Elements();
        if (generation == HudAtlas_Generation()) {
            break;
        }
        // the atlas was cleared while the elements were written: all of them again
        for (int i = 0; i < E_COUNT; i++) {
            hud.elems[i].valid = qfalse;
        }
    }
    Hud_Compact();
    HudAtlas_Flush();

    quads = hud.outVerts / 4;
    for (q = 0; q < quads; q += HUD_DRAW_QUADS) {
        const int n = Q_min(HUD_DRAW_QUADS, quads - q);

        cgi.R_DrawTriangles2D(HudAtlas_Shader(), &hud.out[q * 4], n * 4, hud.indexes, n * 6);
    }
}

/*
================
Hud_Bounds_f

hudbounds: each element's rectangle on the screen, as drawn last (the layout's check)
================
*/
void CG_HudBounds_f(void)
{
    static const char *const names[E_COUNT] = {"score", "cards", "radar", "messages", "killfeed", "chat", "bottomleft",
                                                "health", "badge", "ammo", "weapons", "banners", "stopwatch", "info", "vote",
                                                "damage", "crosshair"};

    cgi.Printf("Competitive HUD at %dx%d, %d quads\n", hud.w, hud.h, hud.outVerts / 4);
    for (int i = 0; i < E_COUNT; i++) {
        const hudElem_t &e = hud.elems[i];
        float            x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;

        if (e.v.empty()) {
            continue;
        }
        for (const vert2D_t &v : e.v) {
            x0 = Q_min(x0, v.xy[0]);
            y0 = Q_min(y0, v.xy[1]);
            x1 = Q_max(x1, v.xy[0]);
            y1 = Q_max(y1, v.xy[1]);
        }
        cgi.Printf("%-10s %4d quads  x %7.1f .. %7.1f  y %7.1f .. %7.1f\n", names[i], (int)e.v.size() / 4, x0, x1, y0, y1);
    }
}

void CG_HudCompetitive_Init(void)
{
    size_t i;

    for (i = 0; i < ARRAY_LEN(hud_cvars); i++) {
        *hud_cvars[i].var = cgi.Cvar_Get(hud_cvars[i].name, hud_cvars[i].value, CVAR_ARCHIVE);
    }
    CG_HudFeed_Reset();
    for (i = 0; i < E_COUNT; i++) {
        hud.elems[i].v.clear();
        hud.elems[i].valid = qfalse;
        hud.elems[i].dirty = qfalse;
        hud.elems[i].first = 0;
    }
    hud.ready          = qfalse;
    hud.radarLoaded    = qfalse;
    hud.outVerts       = 0;
    hud.lastDamageDir  = -999999;
    hud.lastDamageHealth = -1;
    hud.damageFlashTime = 0;
    memset(hud.arcs, 0, sizeof(hud.arcs));
    memset(&wb, 0, sizeof(wb));
    wb.follow = -3;
    Hud_ClearWeapons();

    hud.failed = qfalse;
    // nothing made for the stock HUD; chosen later in the map, made at its first frame
    if (cg_hudstyle->integer == 1) {
        Hud_Start();
    }
}

/*
================
Hud_Start

The atlas (and the radar's walk when it shows): at the load, or at the first
frame drawn with cg_hudstyle 1
================
*/
static void Hud_Start(void)
{
    size_t i;
    int    q;

    hud.w     = cgs.glconfig.vidWidth;
    hud.h     = cgs.glconfig.vidHeight;
    hud.scale = hud.h / 1080.0f;
    for (q = 0; q < HUD_DRAW_QUADS; q++) {
        hud.indexes[q * 6 + 0] = q * 4 + 0;
        hud.indexes[q * 6 + 1] = q * 4 + 1;
        hud.indexes[q * 6 + 2] = q * 4 + 2;
        hud.indexes[q * 6 + 3] = q * 4 + 0;
        hud.indexes[q * 6 + 4] = q * 4 + 2;
        hud.indexes[q * 6 + 5] = q * 4 + 3;
    }
    for (i = 0; i < ARRAY_LEN(hud_weapons); i++) {
        wb.silhouette[i] = HudSilhouette_Find(hud_weapons[i].silhouette);
    }
    if (!cgi.R_CreateImage2D || !HudAtlas_Begin()) {
        cgi.Printf("Competitive HUD: no atlas, the stock HUD is drawn\n");
        hud.failed = qtrue;
        return;
    }
    hud.ready = qtrue;
}

void CG_HudCompetitive_Load(void)
{
    if (hud.ready && cg_hud_radar->integer && !hud.radarLoaded) {
        HudRadar_MapLoaded();
        hud.radarLoaded = qtrue;
    }
}

void CG_HudCompetitive_Shutdown(void)
{
    HudRadar_Shutdown();
    HudAtlas_Shutdown();
    hud.ready = qfalse;
}
