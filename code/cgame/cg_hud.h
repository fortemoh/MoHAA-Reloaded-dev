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
// The Competitive HUD (cg_hudstyle 1), Project: Omaha's Competitive HUD pack
// drawn by cgame: shared between its files (C++ only)
//
// cg_hud_atlas.cpp       the one texture everything is drawn from: glyphs,
//                        shapes and silhouettes rasterized at the size drawn
// cg_hud_competitive.cpp the elements, laid out and kept as quads, one draw
// cg_hud_feed.cpp        the kill feed, the chat, the game messages, who is alive
// cg_hud_radar.cpp       the radar and its picture of the map

#pragma once

#include "cg_local.h"

#include <stdint.h>

//
// the atlas
//

typedef enum {
    HUD_FACE_MEDIUM,   // Oswald Medium: the Omaha HUD's "body" text (weights 400, 500, 700)
    HUD_FACE_SEMIBOLD, // Oswald SemiBold: its weight 600
    HUD_NUM_FACES
} hudFace_t;

// a picture in the atlas: its texture coordinates, its size in pixels, and
// where its top left is from the point it is drawn at (a glyph's from the
// pen on the baseline, a shape's from its box's top left)
typedef struct {
    float s0, t0, s1, t1;
    short w, h;
    short x0, y0;
} hudSprite_t;

// the shapes, rasterized at the size asked
typedef enum {
    HUD_SHAPE_DISC,            // a circle filled, w across
    HUD_SHAPE_RING,            // a circle's edge, w across, p pixels thick (inside)
    HUD_SHAPE_ALLIES_0,        // the Allies' roundel (lib/team_icons.xml): its four layers, w across
    HUD_SHAPE_ALLIES_1,
    HUD_SHAPE_ALLIES_2,
    HUD_SHAPE_ALLIES_3,
    HUD_SHAPE_AXIS_0,          // the Axis' cross: its three layers
    HUD_SHAPE_AXIS_1,
    HUD_SHAPE_AXIS_2,
    HUD_SHAPE_BULLET,          // the ammo's bullet: a rounded bar w x h turned -45 degrees
    HUD_SHAPE_CRESCENT,        // the damage indicator's crescent (w: its box), masked by its band
    HUD_SHAPE_CRESCENT_GLOW,   // and its glow
    HUD_SHAPE_WEDGE,           // the radar's damage wedge (w: the radar), at the top
    HUD_SHAPE_POINTER,         // a radar dot's pointer, up, about the dot (w: the dot's radius x 8)
    HUD_SHAPE_ARROW,           // the radar's own arrow, up (w: its size unit x 16)
    HUD_SHAPE_DIAMOND,         // an objective's diamond, w across
    HUD_SHAPE_CHEVRON,         // a storey up: ^ in a dot w across
    HUD_NUM_SHAPES
} hudShape_t;

qboolean           HudAtlas_Begin(void); // at a map load / vid_restart: the texture and the shader
void               HudAtlas_Shutdown(void);
qhandle_t          HudAtlas_Shader(void);
int                HudAtlas_Generation(void); // changes when the atlas is cleared: every sprite moved
const hudSprite_t *HudAtlas_White(void);      // texels of 1, for plain rectangles
const hudSprite_t *HudAtlas_Glyph(hudFace_t face, float px, unsigned char c);
const hudSprite_t *HudAtlas_GlyphShadow(hudFace_t face, float px, unsigned char c, float scale);
const hudSprite_t *HudAtlas_Silhouette(int index, int w, int h); // fitted in w x h
const hudSprite_t *HudAtlas_Shape(hudShape_t shape, int w, int h, float p);
// the radar's square, size texels a side (0: none); rows changed are uploaded with
// HudAtlas_Dirty; false when there is no room (the atlas is cleared for it once)
qboolean           HudAtlas_RadarRegion(int size, int *x, int *y);
byte              *HudAtlas_Texels(int x, int y);
void               HudAtlas_Dirty(int x, int y, int w, int h);
void               HudAtlas_Flush(void); // the changed texels to the texture
float              HudAtlas_U(float x);
float              HudAtlas_V(float y);

// fonts
float HudFont_Advance(hudFace_t face, float px, unsigned char c);
float HudFont_Ascent(hudFace_t face, float px);
float HudFont_Descent(hudFace_t face, float px); // below the baseline, positive
float HudFont_LineHeight(hudFace_t face, float px);
float HudFont_CapHeight(hudFace_t face, float px);
float HudFont_Width(hudFace_t face, float px, const char *text);

int         HudSilhouette_Find(const char *name); // -1 none
void        HudSilhouette_Size(int index, int *w, int *h);

//
// drawing: quads in their elements (cg_hud_competitive.cpp)
//

typedef struct {
    byte r, g, b, a;
} hudColor_t;

hudColor_t Hud_Color(unsigned int rgba);   // 0xRRGGBBAA
hudColor_t Hud_Fade(hudColor_t c, float a); // its alpha times a

void Hud_Vertexes(const vert2D_t v[4]); // a quad as it is
void Hud_Sprite(const hudSprite_t *sp, float x, float y, hudColor_t c);
void Hud_SpriteMirrored(const hudSprite_t *sp, float x, float y, hudColor_t c);
void Hud_SpriteTurned(const hudSprite_t *sp, float cx, float cy, float ox, float oy, float angle, hudColor_t c);
void Hud_Rect(float x, float y, float w, float h, hudColor_t c);
void Hud_RectV(float x, float y, float w, float h, hudColor_t top, hudColor_t bottom);
void Hud_RectH(float x, float y, float w, float h, hudColor_t left, hudColor_t right);
void Hud_Frame(float x, float y, float w, float h, float t, hudColor_t c); // an edge t thick, inside
// text from its pen position on the baseline; the drop shadow (Omaha's: one pixel
// each way, then 1.5 down and right) when shadow; returns the width
float Hud_Text(hudFace_t face, float px, float x, float baseline, const char *text, hudColor_t c, qboolean shadow);
float Hud_TextTurned(
    hudFace_t face, float px, float cx, float cy, float ox, float baseline, const char *text, float angle, hudColor_t c,
    qboolean shadow
);

float Hud_Scale(void); // design pixels (1080 lines) to screen pixels

//
// the feeds (cg_hud_feed.cpp)
//

#define HUD_FEED_KILLS    6
#define HUD_FEED_LINES    8
#define HUD_FEED_TEXT     160

typedef struct {
    int  time;           // cg.time added, 0 none
    char killer[MAX_NAME_LENGTH];
    char victim[MAX_NAME_LENGTH];
    int  killerTeam;     // TEAM_ALLIES, TEAM_AXIS, else none
    int  victimTeam;
    int  icon;           // silhouette, -1 none
    int  iconW, iconH;   // design px
    qboolean suicide;
    qboolean headshot;
} hudKill_t;

typedef struct {
    int      time;
    char     text[HUD_FEED_TEXT];
    hudColor_t color;
    qboolean bold;
} hudLine_t;

const hudKill_t *HudFeed_Kills(int *count);      // newest last
const hudLine_t *HudFeed_Chat(int *count);
const hudLine_t *HudFeed_Messages(int *count);
int              HudFeed_Revision(void);         // changes with every line or kill added

// who is alive, from the scoreboard and the kills since (round modes)
typedef struct {
    int      count[2], alive[2]; // [0] Allies, [1] Axis
    int      teamScore[2];       // from the scoreboard's headers, -1 unknown
    int      players[2][MAX_CLIENTS]; // client numbers: alive first, then dead
    qboolean dead[MAX_CLIENTS];
    int      revision;
} hudRoster_t;

const hudRoster_t *HudFeed_Roster(void);
void               HudFeed_Frame(void); // the score requests, the roster's checks

//
// the radar (cg_hud_radar.cpp)
//

void HudRadar_MapLoaded(void);
void HudRadar_Shutdown(void);
uint64_t HudRadar_Frame(void); // its walk and pictures, while the radar shows; returns a key of what it shows
void HudRadar_Draw(float x, float y, float size, float damageAngle, float damageAlpha);
