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
// The Competitive HUD's atlas: one texture of one channel (white with that
// alpha, the renderer's CreateImage2D IMAGE2D_ALPHA) that every element is
// drawn from, so the whole HUD is one draw. Its glyphs, shapes and weapon
// silhouettes are paths built into cgame (cg_hud_data.h, made by
// hud/hudgen.py; the shapes below), rasterized by stb_truetype at the exact
// pixel size they are drawn at, the first time they are, then kept: each
// change uploads its texels alone. A team icon's colors are layers, drawn one
// over the other in their colors. The radar's picture of the map has a square
// of its own (cg_hud_radar.cpp). When the atlas is full, or the HUD's scale
// changed (cg_hud_scale), it is cleared and everything rasterized again
// (HudAtlas_Generation).

#include "cg_hud.h"

#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wsign-compare"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../thirdparty/stb/stb_truetype.h"
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

#include "cg_hud_data.h"

#define ATLAS_NAME "*hud_competitive"
#define ATLAS_PAD  1 // clear texels around each picture: linear filtering never reads its neighbour's

static const hudFontDef_t *const hud_faces[HUD_NUM_FACES] = {&hud_oswald_medium, &hud_oswald_semibold};

typedef struct {
    int y, h, x; // a row of pictures as tall as h at most, filled up to x
} hudShelf_t;

static struct {
    byte                                     *texels;
    int                                       w, h;
    qhandle_t                                 shader;
    int                                       generation;
    std::vector<hudShelf_t>                   shelves;
    int                                       nextY;
    std::unordered_map<uint64_t, hudSprite_t> sprites;
    hudSprite_t                               white;
    int                                       radarX, radarY, radarSize;
    int                                       dirty[16][4]; // x0, y0, x1, y1
    int                                       numDirty;
    std::vector<byte>                         scratch;
    std::vector<stbtt_vertex>                 path;
} atlas;

/*
====================================================================

PACKING

====================================================================
*/

static void HudAtlas_DirtyAll(void)
{
    atlas.numDirty   = 1;
    atlas.dirty[0][0] = 0;
    atlas.dirty[0][1] = 0;
    atlas.dirty[0][2] = atlas.w;
    atlas.dirty[0][3] = atlas.h;
}

void HudAtlas_Dirty(int x, int y, int w, int h)
{
    int i;

    if (w <= 0 || h <= 0) {
        return;
    }
    // next to or over one already there: the two become one
    for (i = 0; i < atlas.numDirty; i++) {
        int *d = atlas.dirty[i];

        if (x <= d[2] && x + w >= d[0] && y <= d[3] && y + h >= d[1]) {
            d[0] = Q_min(d[0], x);
            d[1] = Q_min(d[1], y);
            d[2] = Q_max(d[2], x + w);
            d[3] = Q_max(d[3], y + h);
            return;
        }
    }
    if (atlas.numDirty == (int)ARRAY_LEN(atlas.dirty)) {
        // too many: one around them all
        int *d = atlas.dirty[0];

        for (i = 1; i < atlas.numDirty; i++) {
            d[0] = Q_min(d[0], atlas.dirty[i][0]);
            d[1] = Q_min(d[1], atlas.dirty[i][1]);
            d[2] = Q_max(d[2], atlas.dirty[i][2]);
            d[3] = Q_max(d[3], atlas.dirty[i][3]);
        }
        atlas.numDirty = 1;
        HudAtlas_Dirty(x, y, w, h);
        return;
    }
    atlas.dirty[atlas.numDirty][0] = x;
    atlas.dirty[atlas.numDirty][1] = y;
    atlas.dirty[atlas.numDirty][2] = x + w;
    atlas.dirty[atlas.numDirty][3] = y + h;
    atlas.numDirty++;
}

void HudAtlas_Flush(void)
{
    int i;

    for (i = 0; i < atlas.numDirty; i++) {
        const int *d = atlas.dirty[i];
        const int  w = d[2] - d[0];
        const int  h = d[3] - d[1];

        if (w == atlas.w) {
            cgi.R_UpdateImage2D(atlas.shader, 0, d[1], w, h, atlas.texels + d[1] * atlas.w);
        } else {
            int row;

            atlas.scratch.resize((size_t)w * h);
            for (row = 0; row < h; row++) {
                memcpy(&atlas.scratch[(size_t)row * w], atlas.texels + (d[1] + row) * atlas.w + d[0], w);
            }
            cgi.R_UpdateImage2D(atlas.shader, d[0], d[1], w, h, atlas.scratch.data());
        }
    }
    atlas.numDirty = 0;
}

// everything gone: the pictures are rasterized again as they are asked for
static void HudAtlas_Clear(void)
{
    memset(atlas.texels, 0, (size_t)atlas.w * atlas.h);
    atlas.shelves.clear();
    atlas.sprites.clear();
    atlas.nextY     = 0;
    atlas.radarSize = 0;
    atlas.generation++;
    HudAtlas_DirtyAll();

    // the white block, 4 x 4: its middle is white with linear filtering too
    for (int y = 0; y < 4; y++) {
        memset(atlas.texels + y * atlas.w, 255, 4);
    }
    atlas.white.s0 = atlas.white.s1 = HudAtlas_U(2.0f);
    atlas.white.t0 = atlas.white.t1 = HudAtlas_V(2.0f);
    atlas.white.w = atlas.white.h = 1;
    atlas.white.x0 = atlas.white.y0 = 0;
    atlas.shelves.push_back({0, 4 + ATLAS_PAD, 4 + ATLAS_PAD});
    atlas.nextY = 4 + ATLAS_PAD;
}

// a place for w x h texels (and the clear ones around them)
static qboolean HudAtlas_Alloc(int w, int h, int *x, int *y)
{
    const int pw = w + ATLAS_PAD * 2;
    const int ph = h + ATLAS_PAD * 2;
    size_t    i;

    if (pw > atlas.w || ph > atlas.h) {
        return qfalse;
    }
    // the shortest row it fits in, not too much taller than it
    for (i = 0; i < atlas.shelves.size(); i++) {
        hudShelf_t &s = atlas.shelves[i];

        if (ph <= s.h && ph * 5 >= s.h * 4 - 8 && s.x + pw <= atlas.w) {
            *x = s.x + ATLAS_PAD;
            *y = s.y + ATLAS_PAD;
            s.x += pw;
            return qtrue;
        }
    }
    if (atlas.nextY + ph > atlas.h) {
        return qfalse;
    }
    atlas.shelves.push_back({atlas.nextY, ph, pw});
    *x = ATLAS_PAD;
    *y = atlas.nextY + ATLAS_PAD;
    atlas.nextY += ph;
    return qtrue;
}

float HudAtlas_U(float x)
{
    return x / atlas.w;
}

float HudAtlas_V(float y)
{
    return y / atlas.h;
}

byte *HudAtlas_Texels(int x, int y)
{
    return atlas.texels + y * atlas.w + x;
}

qhandle_t HudAtlas_Shader(void)
{
    return atlas.shader;
}

int HudAtlas_Generation(void)
{
    return atlas.generation;
}

const hudSprite_t *HudAtlas_White(void)
{
    return &atlas.white;
}

// the atlas's side for the HUD at scale (screen pixels a design pixel): big
// enough for a match's glyphs and pictures at that size, and the radar's
// picture (a quarter of it at 1080 lines)
static int HudAtlas_SizeFor(float scale)
{
    const int size  = scale * 1080.0f > 1200.0f ? 4096 : 2048;
    const int limit = cgs.glconfig.maxTextureSize > 0 ? cgs.glconfig.maxTextureSize : size;

    return Q_min(size, limit);
}

qboolean HudAtlas_Begin(float scale)
{
    const int size = HudAtlas_SizeFor(scale);

    if (atlas.w != size) {
        free(atlas.texels);
        atlas.w = atlas.h = size;
        atlas.texels      = (byte *)malloc((size_t)atlas.w * atlas.h);
        if (!atlas.texels) {
            atlas.w = atlas.h = 0;
            atlas.shader      = 0;
            return qfalse;
        }
    }
    HudAtlas_Clear();
    atlas.numDirty = 0;
    // the texture starts as the texels are: one upload
    atlas.shader = cgi.R_CreateImage2D(ATLAS_NAME, atlas.w, atlas.h, IMAGE2D_ALPHA, atlas.texels);
    return atlas.shader ? qtrue : qfalse;
}

qboolean HudAtlas_Rescale(float scale)
{
    if (!atlas.texels || !atlas.shader || atlas.w != HudAtlas_SizeFor(scale)) {
        // another size: the texture made again
        return HudAtlas_Begin(scale);
    }
    // the same texture emptied: all of it uploaded with the frame's changes
    HudAtlas_Clear();
    return qtrue;
}

void HudAtlas_Shutdown(void)
{
    free(atlas.texels);
    atlas.texels = NULL;
    atlas.w = atlas.h = 0;
    atlas.shader      = 0;
    atlas.shelves.clear();
    atlas.sprites.clear();
    atlas.numDirty = 0;
}

qboolean HudAtlas_RadarRegion(int size, int *x, int *y)
{
    if (!atlas.texels || size <= 0) {
        return qfalse;
    }
    if (atlas.radarSize != size) {
        int rx, ry;

        if (!HudAtlas_Alloc(size, size, &rx, &ry)) {
            HudAtlas_Clear();
            if (!HudAtlas_Alloc(size, size, &rx, &ry)) {
                return qfalse;
            }
        }
        atlas.radarX    = rx;
        atlas.radarY    = ry;
        atlas.radarSize = size;
    }
    *x = atlas.radarX;
    *y = atlas.radarY;
    return qtrue;
}

/*
====================================================================

RASTERIZING

====================================================================
*/

// a path into w x h texels at x y, a point p of it at p * scale + shift
static void HudAtlas_Raster(
    const stbtt_vertex *v, int n, float sx, float sy, float tx, float ty, int x, int y, int w, int h
)
{
    stbtt__bitmap bm;

    bm.w      = w;
    bm.h      = h;
    bm.stride = atlas.w;
    bm.pixels = atlas.texels + y * atlas.w + x;
    if (n > 0) {
        stbtt_Rasterize(&bm, 0.35f, (stbtt_vertex *)v, n, sx, sy, tx, ty, 0, 0, 0, NULL);
    } else {
        for (int row = 0; row < h; row++) {
            memset(bm.pixels + row * bm.stride, 0, w);
        }
    }
}

// a sprite of the texels at x y, w x h, drawn from x0 y0
static hudSprite_t HudAtlas_MakeSprite(int x, int y, int w, int h, int x0, int y0)
{
    hudSprite_t sp;

    sp.s0 = HudAtlas_U((float)x);
    sp.t0 = HudAtlas_V((float)y);
    sp.s1 = HudAtlas_U((float)(x + w));
    sp.t1 = HudAtlas_V((float)(y + h));
    sp.w  = w;
    sp.h  = h;
    sp.x0 = x0;
    sp.y0 = y0;
    HudAtlas_Dirty(x, y, w, h);
    return sp;
}

static uint64_t HudAtlas_Key(int kind, int a, int b, int c, int d)
{
    return ((uint64_t)(kind & 15) << 60) | ((uint64_t)(a & 0xFFF) << 48) | ((uint64_t)(b & 0xFFFF) << 32)
         | ((uint64_t)(c & 0xFFFF) << 16) | (uint64_t)(d & 0xFFFF);
}

// a picture of w x h texels made by fill (into the atlas at x y), kept under key
template<typename F>
static const hudSprite_t *HudAtlas_Make(uint64_t key, int w, int h, int x0, int y0, F fill)
{
    auto it = atlas.sprites.find(key);
    int  x, y;

    if (it != atlas.sprites.end()) {
        return &it->second;
    }
    if (!atlas.texels) {
        return &atlas.white;
    }
    w = Q_max(w, 1);
    h = Q_max(h, 1);
    if (!HudAtlas_Alloc(w, h, &x, &y)) {
        // full: everything again (the HUD's elements see the generation change)
        HudAtlas_Clear();
        if (!HudAtlas_Alloc(w, h, &x, &y)) {
            return &atlas.white;
        }
    }
    fill(x, y);
    return &(atlas.sprites[key] = HudAtlas_MakeSprite(x, y, w, h, x0, y0));
}

/*
====================================================================

FONTS

====================================================================
*/

static float HudFont_Scale(hudFace_t face, float px)
{
    return px / (float)hud_faces[face]->unitsPerEm;
}

static const hudGlyphDef_t *HudFont_Glyph(hudFace_t face, unsigned char c)
{
    const hudGlyphDef_t *g;

    if (c < 0x20) {
        c = '?';
    }
    g = &hud_faces[face]->glyphs[c - 0x20];
    if (!g->advance) {
        g = &hud_faces[face]->glyphs['?' - 0x20];
    }
    return g;
}

// glyph sizes a quarter pixel apart: one picture for them
static int HudFont_Quarters(float px)
{
    return Q_max(1, (int)(px * 4.0f + 0.5f));
}

float HudFont_Advance(hudFace_t face, float px, unsigned char c)
{
    return HudFont_Glyph(face, c)->advance * HudFont_Scale(face, HudFont_Quarters(px) * 0.25f);
}

float HudFont_Ascent(hudFace_t face, float px)
{
    return hud_faces[face]->ascent * HudFont_Scale(face, px);
}

float HudFont_Descent(hudFace_t face, float px)
{
    return -hud_faces[face]->descent * HudFont_Scale(face, px);
}

float HudFont_LineHeight(hudFace_t face, float px)
{
    const hudFontDef_t *f = hud_faces[face];

    return (f->ascent - f->descent + f->lineGap) * HudFont_Scale(face, px);
}

float HudFont_CapHeight(hudFace_t face, float px)
{
    return hud_faces[face]->capHeight * HudFont_Scale(face, px);
}

float HudFont_Width(hudFace_t face, float px, const char *text)
{
    float w = 0.0f;

    for (; *text; text++) {
        w += HudFont_Advance(face, px, (unsigned char)*text);
    }
    return w;
}

const hudSprite_t *HudAtlas_Glyph(hudFace_t face, float px, unsigned char c)
{
    const int            q = HudFont_Quarters(px);
    const hudGlyphDef_t *g = HudFont_Glyph(face, c);
    const float          scale = HudFont_Scale(face, q * 0.25f);
    const int            ix0   = (int)floorf(g->x0 * scale);
    const int            iy0   = (int)floorf(g->y0 * scale);
    const int            ix1   = (int)ceilf(g->x1 * scale);
    const int            iy1   = (int)ceilf(g->y1 * scale);

    if (!g->count) {
        return NULL;
    }
    return HudAtlas_Make(HudAtlas_Key(1, face, q, (int)(g - hud_faces[face]->glyphs), 0), ix1 - ix0, iy1 - iy0, ix0, iy0, [&](int x, int y) {
        HudAtlas_Raster(&hud_faces[face]->vertexes[g->first], g->count, scale, scale, (float)-ix0, (float)-iy0, x, y, ix1 - ix0, iy1 - iy0);
    });
}

/*
================
HudAtlas_GlyphShadow

Omaha's drop shadow (uid_widget.cpp, kShadowPasses) as one picture: the glyph
in black one pixel up, down, left and right at alpha 0.5 and 1.5 pixels down
and right at 0.33 (pixels of 1080 lines: times scale), each over the others,
so 1 - (1 - a) of them all: drawn under the glyph it is the same as the five
passes
================
*/
const hudSprite_t *HudAtlas_GlyphShadow(hudFace_t face, float px, unsigned char c, float scale)
{
    static const float passes[5][3] = {
        {0.0f, -1.0f, 0.50f},
        {0.0f, 1.0f, 0.50f},
        {-1.0f, 0.0f, 0.50f},
        {1.0f, 0.0f, 0.50f},
        {1.5f, 1.5f, 0.33f},
    };
    const hudSprite_t *glyph = HudAtlas_Glyph(face, px, c);
    const int          pad   = (int)ceilf(1.5f * scale) + 1;
    int                gw, gh, gx0, gy0;

    if (!glyph) {
        return NULL;
    }
    // (copied: the atlas may be cleared for the shadow, and the glyph with it)
    gw  = glyph->w;
    gh  = glyph->h;
    gx0 = glyph->x0;
    gy0 = glyph->y0;
    return HudAtlas_Make(
        HudAtlas_Key(2, face, HudFont_Quarters(px), c, (int)(scale * 64.0f + 0.5f)),
        gw + pad * 2,
        gh + pad * 2,
        gx0 - pad,
        gy0 - pad,
        [&](int x, int y) {
            // where the glyph's texels are now (made again after a clear)
            const hudSprite_t *g  = HudAtlas_Glyph(face, px, c);
            const int          gx = (int)(g->s0 * atlas.w + 0.5f);
            const int          gy = (int)(g->t0 * atlas.h + 0.5f);

            auto sample = [&](float fx, float fy) -> float {
                // the glyph's coverage at fx fy (its texels), linearly between texels
                const int   ix = (int)floorf(fx), iy = (int)floorf(fy);
                const float ax = fx - ix, ay = fy - iy;
                float       v  = 0.0f;

                for (int k = 0; k < 4; k++) {
                    const int   sx = ix + (k & 1), sy = iy + (k >> 1);
                    const float wgt = ((k & 1) ? ax : 1.0f - ax) * ((k >> 1) ? ay : 1.0f - ay);

                    if (wgt > 0.0f && sx >= 0 && sy >= 0 && sx < gw && sy < gh) {
                        v += wgt * atlas.texels[(gy + sy) * atlas.w + gx + sx];
                    }
                }
                return v / 255.0f;
            };

            for (int row = 0; row < gh + pad * 2; row++) {
                byte *out = atlas.texels + (y + row) * atlas.w + x;

                for (int col = 0; col < gw + pad * 2; col++) {
                    float clear = 1.0f;

                    for (int k = 0; k < 5; k++) {
                        const float m = sample(col - pad - passes[k][0] * scale, row - pad - passes[k][1] * scale);

                        clear *= 1.0f - passes[k][2] * m;
                    }
                    out[col] = (byte)((1.0f - clear) * 255.0f + 0.5f);
                }
            }
        }
    );
}

/*
====================================================================

SILHOUETTES

====================================================================
*/

int HudSilhouette_Find(const char *name)
{
    int i;

    for (i = 0; i < HUD_NUM_SILHOUETTES; i++) {
        if (!Q_stricmp(hud_silhouettes[i].name, name)) {
            return i;
        }
    }
    return -1;
}

void HudSilhouette_Size(int index, int *w, int *h)
{
    *w = hud_silhouettes[index].width;
    *h = hud_silhouettes[index].height;
}

const hudSprite_t *HudAtlas_Silhouette(int index, int w, int h)
{
    const hudSilhouetteDef_t *def;
    float                     scale;
    int                       sw, sh;

    if (index < 0 || index >= HUD_NUM_SILHOUETTES || w <= 0 || h <= 0) {
        return NULL;
    }
    def   = &hud_silhouettes[index];
    // fitted in the box (fit="contain")
    scale = Q_min((float)w / def->width, (float)h / def->height);
    sw    = Q_max(1, (int)(def->width * scale + 0.5f));
    sh    = Q_max(1, (int)(def->height * scale + 0.5f));
    return HudAtlas_Make(HudAtlas_Key(3, index, w, h, 0), sw, sh, 0, 0, [&](int x, int y) {
        const float s = (float)sw / def->width / HUD_SILHOUETTE_UNITS;
        const float t = (float)sh / def->height / HUD_SILHOUETTE_UNITS;

        // (the traced outline is in the picture's own space: texel i covers i .. i + 1)
        HudAtlas_Raster(&hud_silhouette_vertexes[def->first], def->count, s, t, 0.0f, 0.0f, x, y, sw, sh);
    });
}

/*
====================================================================

SHAPES

====================================================================
*/

static void HudPath_Move(float x, float y)
{
    stbtt_vertex v;

    memset(&v, 0, sizeof(v));
    v.type = STBTT_vmove;
    v.x    = (short)Q_rint(x * 16.0f);
    v.y    = (short)Q_rint(y * 16.0f);
    atlas.path.push_back(v);
}

static void HudPath_Line(float x, float y)
{
    stbtt_vertex v;

    memset(&v, 0, sizeof(v));
    v.type = STBTT_vline;
    v.x    = (short)Q_rint(x * 16.0f);
    v.y    = (short)Q_rint(y * 16.0f);
    atlas.path.push_back(v);
}

// a circle about cx cy as segments (clockwise, or counterclockwise for a hole)
static void HudPath_Circle(float cx, float cy, float r, qboolean hole)
{
    const int n = Q_max(16, Q_min(256, (int)(r * 1.5f)));

    for (int i = 0; i <= n; i++) {
        const float a = (hole ? -1.0f : 1.0f) * (float)i / n * 2.0f * M_PI;
        const float x = cx + r * cosf(a);
        const float y = cy + r * sinf(a);

        if (!i) {
            HudPath_Move(x, y);
        } else {
            HudPath_Line(x, y);
        }
    }
}

static void HudPath_Polygon(const float (*pts)[2], int n, float sx, float sy, float tx, float ty)
{
    for (int i = 0; i <= n; i++) {
        const float *p = pts[i % n];

        if (!i) {
            HudPath_Move(p[0] * sx + tx, p[1] * sy + ty);
        } else {
            HudPath_Line(p[0] * sx + tx, p[1] * sy + ty);
        }
    }
}

// an arc of the circle about cx cy, radius r, from a0 to a1 (radians), into the path
static void HudPath_Arc(float cx, float cy, float r, float a0, float a1)
{
    const int n = Q_max(8, Q_min(256, (int)(fabsf(a1 - a0) * r)));

    for (int i = 1; i <= n; i++) {
        const float a = a0 + (a1 - a0) * i / n;

        HudPath_Line(cx + r * cosf(a), cy + r * sinf(a));
    }
}

// the crescent of lib/shapes.xml in a box of size b: the box's circle from
// (x0, y0) to (b - x0, y0) over the top, back by a circle of radius rf whose
// middle is below
static void HudPath_Crescent(float b, float x0, float y0, float rf)
{
    const float c   = b * 0.5f;
    const float a0  = atan2f(y0 - c, x0 - c);
    const float a1  = atan2f(y0 - c, b - x0 - c);
    const float h   = sqrtf(rf * rf - (c - x0) * (c - x0));
    const float fcy = y0 + h;
    const float f0  = atan2f(y0 - fcy, b - x0 - c);
    const float f1  = atan2f(y0 - fcy, x0 - c);

    HudPath_Move(x0, y0);
    // left to right over the top (angles of the upper half, y down: -120 to
    // -60 degrees for the crescent), then back over the flatter circle's top
    HudPath_Arc(c, c, c, a0, a1);
    HudPath_Arc(c, fcy, rf, f0, f1);
}

// the path rasterized into w x h texels (its units: 1/16 texel)
static void HudAtlas_RasterPath(int x, int y, int w, int h)
{
    HudAtlas_Raster(atlas.path.data(), (int)atlas.path.size(), 1.0f / 16.0f, 1.0f / 16.0f, 0.0f, 0.0f, x, y, w, h);
}

const hudSprite_t *HudAtlas_Shape(hudShape_t shape, int w, int h, float p)
{
    const uint64_t key = HudAtlas_Key(4, shape, w, h, (int)(p * 16.0f + 0.5f));
    auto           it  = atlas.sprites.find(key);
    float          d   = (float)w;
    int            sw = w, sh = h, x0 = 0, y0 = 0;

    if (it != atlas.sprites.end()) {
        return &it->second;
    }
    atlas.path.clear();
    // (a round shape's box is square: h 0 is w)
    if (h <= 0) {
        h = sh = w;
    }

    switch (shape) {
    case HUD_SHAPE_DISC:
        HudPath_Circle(d * 0.5f, d * 0.5f, d * 0.5f, qfalse);
        break;
    case HUD_SHAPE_RING:
        HudPath_Circle(d * 0.5f, d * 0.5f, d * 0.5f, qfalse);
        HudPath_Circle(d * 0.5f, d * 0.5f, d * 0.5f - p, qtrue);
        break;
    case HUD_SHAPE_FRAME:
        {
            // the outside clockwise, the inside the other way: a hole
            const float outer[4][2] = {{0.0f, 0.0f}, {(float)w, 0.0f}, {(float)w, (float)h}, {0.0f, (float)h}};
            const float inner[4][2] = {{p, p}, {p, h - p}, {w - p, h - p}, {w - p, p}};

            HudPath_Polygon(outer, 4, 1.0f, 1.0f, 0.0f, 0.0f);
            HudPath_Polygon(inner, 4, 1.0f, 1.0f, 0.0f, 0.0f);
        }
        break;
    // the teams' icons: fortemoh/moh-icons team/allies.svg and team/axis.svg
    // (viewBox 100 x 100), a layer a shape
    case HUD_SHAPE_ALLIES_0:
        HudPath_Circle(d * 0.5f, d * 0.5f, d * 0.5f, qfalse);
        break;
    case HUD_SHAPE_ALLIES_1:
        {
            static const float star[10][2] = {
                {50.0f, 0.0f},
                {61.226f, 34.549f},
                {97.553f, 34.549f},
                {68.164f, 55.902f},
                {79.389f, 90.451f},
                {50.0f, 69.098f},
                {20.611f, 90.451f},
                {31.836f, 55.902f},
                {2.447f, 34.549f},
                {38.774f, 34.549f},
            };
            HudPath_Polygon(star, 10, d / 100.0f, d / 100.0f, 0.0f, 0.0f);
        }
        break;
    case HUD_SHAPE_ALLIES_2:
        HudPath_Circle(d * 0.5f, d * 0.5f, d * 0.15451f, qfalse);
        break;
    case HUD_SHAPE_AXIS_0:
        HudPath_Circle(d * 0.5f, d * 0.5f, d * 0.5f, qfalse);
        break;
    case HUD_SHAPE_AXIS_1:
        {
            static const float outline[12][2] = {
                {28.979f, 10.0f},
                {71.021f, 10.0f},
                {61.763f, 38.237f},
                {90.0f, 28.979f},
                {90.0f, 71.021f},
                {61.763f, 61.763f},
                {71.021f, 90.0f},
                {28.979f, 90.0f},
                {38.237f, 61.763f},
                {10.0f, 71.021f},
                {10.0f, 28.979f},
                {38.237f, 38.237f},
            };
            HudPath_Polygon(outline, 12, d / 100.0f, d / 100.0f, 0.0f, 0.0f);
        }
        break;
    case HUD_SHAPE_AXIS_2:
        {
            static const float cross[12][2] = {
                {34.5f, 14.0f},
                {65.5f, 14.0f},
                {55.5f, 44.5f},
                {86.0f, 34.5f},
                {86.0f, 65.5f},
                {55.5f, 55.5f},
                {65.5f, 86.0f},
                {34.5f, 86.0f},
                {44.5f, 55.5f},
                {14.0f, 65.5f},
                {14.0f, 34.5f},
                {44.5f, 44.5f},
            };
            HudPath_Polygon(cross, 12, d / 100.0f, d / 100.0f, 0.0f, 0.0f);
        }
        break;
    case HUD_SHAPE_BULLET:
        {
            // a bar w x h with corners of p, turned 45 degrees counterclockwise
            // (rotation="-45") about its middle, in a box that holds it
            const float c = 0.70710678f;
            const float hw = w * 0.5f, hh = h * 0.5f, r = Q_min(p, Q_min(hw, hh));
            const int   box = (int)ceilf((w + h) * c) + 2;
            float       pts[64][2];
            int         n = 0;

            for (int k = 0; k < 4; k++) {
                // the corners' arcs, clockwise from the top right
                const float ccx = (k == 0 || k == 1) ? hw - r : -hw + r;
                const float ccy = (k == 1 || k == 2) ? hh - r : -hh + r;
                for (int j = 0; j <= 7; j++) {
                    const float a = (-90.0f + 90.0f * k + 90.0f * j / 7.0f) * (M_PI / 180.0f);
                    const float px = ccx + r * cosf(a), py = ccy + r * sinf(a);

                    // (x, y) turned by -45 degrees, y down
                    pts[n][0] = px * c + py * c + box * 0.5f;
                    pts[n][1] = -px * c + py * c + box * 0.5f;
                    n++;
                }
            }
            HudPath_Polygon(pts, n, 1.0f, 1.0f, 0.0f, 0.0f);
            sw = sh = box;
            x0 = y0 = -box / 2;
        }
        break;
    case HUD_SHAPE_CRESCENT:
    case HUD_SHAPE_CRESCENT_GLOW:
        {
            // the top of a box of w; drawn turned about its middle
            const qboolean glow = shape == HUD_SHAPE_CRESCENT_GLOW;

            if (glow) {
                HudPath_Crescent(d, d * 0.235f, d * 0.076f, d * 1.6819f);
            } else {
                HudPath_Crescent(d, d * 0.25f, d * 0.067f, d * 0.8631f);
            }
            sw = (int)ceilf(d * (glow ? 0.54f : 0.52f)) + 2;
            sh = (int)ceilf(d * 0.08f) + 2;
            x0 = (int)floorf(d * 0.5f - sw * 0.5f);
            y0 = 0;
            // the path is in the box: moved to the picture's corner
            for (auto &v : atlas.path) {
                v.x -= (short)(x0 * 16);
            }
            x0 -= (int)(d * 0.5f);
            y0 -= (int)(d * 0.5f);
        }
        break;
    case HUD_SHAPE_WEDGE:
        {
            static const float wedge[3][2] = {{0.5f, 0.38f}, {0.64f, 0.02f}, {0.36f, 0.02f}};

            sw = (int)ceilf(d * 0.3f) + 2;
            sh = (int)ceilf(d * 0.38f) + 2;
            HudPath_Polygon(wedge, 3, d, d, -(d * 0.5f - sw * 0.5f), 0.0f);
            x0 = -sw / 2;
            y0 = -(int)(d * 0.5f);
        }
        break;
    case HUD_SHAPE_POINTER:
        {
            // a dot of radius r (w / 8) and its edge's p: the tip 2.6 r up
            const float r = d / 8.0f;
            const float pts[3][2] = {{0.0f, -2.6f * r}, {0.85f * p, -0.55f * p}, {-0.85f * p, -0.55f * p}};
            const int   half = (int)ceilf(Q_max(2.6f * r, p)) + 1;

            HudPath_Polygon(pts, 3, 1.0f, 1.0f, (float)half, (float)half);
            sw = sh = half * 2;
            x0 = y0 = -half;
        }
        break;
    case HUD_SHAPE_ARROW:
        {
            const float u = d / 16.0f;
            const float pts[3][2] = {{0.0f, -7.0f * u}, {5.0f * u, 5.0f * u}, {-5.0f * u, 5.0f * u}};
            const int   half = (int)ceilf(7.0f * u) + 1;

            HudPath_Polygon(pts, 3, 1.0f, 1.0f, (float)half, (float)half);
            sw = sh = half * 2;
            x0 = y0 = -half;
        }
        break;
    case HUD_SHAPE_DIAMOND:
        {
            const float pts[4][2] = {{0.0f, -1.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};
            const int   half = (int)ceilf(d * 0.5f) + 1;

            HudPath_Polygon(pts, 4, d * 0.5f, d * 0.5f, (float)half, (float)half);
            sw = sh = half * 2;
            x0 = y0 = -half;
        }
        break;
    case HUD_SHAPE_CHEVRON:
        {
            static const float chevron[6][2] = {
                {-0.7f, 0.1f}, {0.0f, -0.6f}, {0.7f, 0.1f}, {0.7f, 0.52f}, {0.0f, -0.18f}, {-0.7f, 0.52f}
            };
            const int half = (int)ceilf(d * 0.5f) + 1;

            HudPath_Polygon(chevron, 6, d * 0.5f, d * 0.5f, (float)half, (float)half);
            sw = sh = half * 2;
            x0 = y0 = -half;
        }
        break;
    default:
        return &atlas.white;
    }

    return HudAtlas_Make(key, sw, sh, x0, y0, [&](int x, int y) {
        HudAtlas_RasterPath(x, y, sw, sh);
        if (shape == HUD_SHAPE_CRESCENT || shape == HUD_SHAPE_CRESCENT_GLOW) {
            // the radial band of hud_messaging.xml's masks: the crescent's
            // visible part, soft at both edges and toward its tips
            const float c = d * 0.5f;
            for (int row = 0; row < sh; row++) {
                byte *out = atlas.texels + (y + row) * atlas.w + x;
                for (int col = 0; col < sw; col++) {
                    const float px = (float)(x0 + col) + 0.5f, py = (float)(y0 + row) + 0.5f;
                    const float t = sqrtf(px * px + py * py) / c;
                    float       m;

                    if (shape == HUD_SHAPE_CRESCENT_GLOW) {
                        m = t < 0.89f ? 0.0f : t < 0.965f ? 0.3f * (t - 0.89f) / 0.075f : t < 1.0f ? 0.3f * (1.0f - t) / 0.035f : 0.0f;
                    } else {
                        m = t < 0.945f   ? 0.0f
                          : t < 0.975f ? (t - 0.945f) / 0.03f
                          : t < 0.983f ? 1.0f
                          : t < 1.0f   ? (1.0f - t) / 0.017f
                                       : 0.0f;
                    }
                    out[col] = (byte)(out[col] * m + 0.5f);
                }
            }
        }
    });
}
