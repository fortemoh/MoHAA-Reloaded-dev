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
// The Competitive HUD's radar (cg_hud_radar 1), Project: Omaha's
// (cl_hud_radar.cpp, cg_hud_radar.cpp) drawn in the HUD's one draw: the map's
// picture made from the collision model (cg_hud_radar_map.h: the walk at the
// map's load, for a second at most, then a little in each frame it needs
// more; the pictures on a thread of their own), turned about the player, in
// the atlas's square for it (only the rows a new picture changed uploaded).
// On it, as the stock radar has them: the player, their teammates (in the
// snapshot, and out of it from the server's radar on Spearhead and
// Breakthrough servers), the objectives and the damage's direction. Enemies
// never: Omaha's client-side spotting isn't here.

#include "cg_hud.h"
#include "cg_hud_radar_map.h"

#include <chrono>
#include <thread>

#define RADAR_DESIGN_PX    220.0f  // Omaha's sizes are of a 220 px radar
#define RADAR_RANGE        1000.0f // units from the center to the edge
#define RADAR_TEAMMATE_MS  5000    // the server's radar's reports, as the stock radar
#define RADAR_LOAD_MS      1000.0  // the walk at the map's load, at most
#define RADAR_FRAME_MS     0.5     // and in a frame after that
#define RADAR_SEED_MS      250     // how often players' positions are checked against the map
#define RADAR_LEVEL_DZ     64.0f   // a player this much higher or lower gets an arrow in their dot
#define RADAR_FOCUS_STEP   24.0f   // the picture is drawn again when the player is this much higher or lower
#define RADAR_FLOOR_ABOVE  18.0f
#define RADAR_FLOOR_BELOW  72.0f
#define RADAR_PICTURE_STEP (RADAR_RANGE * 0.3f) // or this far from its middle
#define RADAR_PICTURE_HALF (RADAR_RANGE * 1.6f) // the picture's reach each way from its middle
#define RADAR_MASK         (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_FENCE | CONTENTS_UNKNOWN2 | CONTENTS_BBOX)
#define RADAR_MAX_SOLIDS   256
#define RADAR_MAX_BLIPS    (MAX_CLIENTS + MAX_OBJECTIVES + 1)

typedef struct {
    int          number;
    clipHandle_t model;
    vec3_t       origin;
    vec3_t       mins, maxs; // in the world
} radarSolid_t;

enum {
    BLIP_OBJECTIVE,       // the current objective
    BLIP_OBJECTIVE_OTHER, // another current one
    BLIP_TEAMMATE,
};

typedef struct {
    int    kind;
    int    team;
    vec3_t origin;
    qboolean hasYaw, hasZ;
    float  yaw;
} radarBlip_t;

static radarmap::Map           radar_map;
static radarmap::Picture       radar_pic, radar_back;
static radarmap::PictureWorker radar_worker;

static struct {
    qboolean     loaded;
    radarSolid_t solids[RADAR_MAX_SOLIDS];
    int          numSolids;
    float        unitsPerTexel;
    float        focusZ, picX, picY;
    qboolean     stale;
    int          seedTime;
    // the picture in the atlas
    int          size, regionX, regionY;
    int          generation;
    qboolean     uploaded;
    unsigned     picSerial, uploadedSerial; // a picture finished, the one in the atlas
    float        texX0, texY0, texUnits;
    int          texW, texH;
    std::vector<byte> rows; // the region as uploaded
    // this frame
    vec3_t       origin;
    float        yaw, northYaw;
    radarBlip_t  blips[RADAR_MAX_BLIPS];
    int          numBlips;
} radar;

/*
====================================================================

THE WALK

====================================================================
*/

static void Radar_Trace(trace_t *tr, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs)
{
    int i, k;

    cgi.CM_BoxTrace(tr, start, end, mins, maxs, 0, RADAR_MASK, qfalse);
    for (i = 0; i < radar.numSolids; i++) {
        const radarSolid_t *solid = &radar.solids[i];
        trace_t             t;

        for (k = 0; k < 3; k++) {
            const float lo = (start[k] < end[k] ? start[k] : end[k]) + mins[k];
            const float hi = (start[k] > end[k] ? start[k] : end[k]) + maxs[k];

            if (lo > solid->maxs[k] || hi < solid->mins[k]) {
                break;
            }
        }
        if (k < 3) {
            continue;
        }
        cgi.CM_TransformedBoxTrace(&t, start, end, mins, maxs, solid->model, RADAR_MASK, solid->origin, vec3_origin, qfalse);
        if (t.allsolid || t.fraction < tr->fraction) {
            *tr = t;
        } else if (t.startsolid) {
            tr->startsolid = qtrue;
        }
    }
}

// a pad nearly a cell wide comes down, not a point: a floor of slats or a
// grating (a catwalk) is a floor, though a point falls through its gaps
static bool Radar_FloorBelow(void *ctx, float x, float y, float zTop, float zBottom, float *z)
{
    static const vec3_t mins = {-7.0f, -7.0f, 0.0f};
    static const vec3_t maxs = {7.0f, 7.0f, 1.0f};
    vec3_t              start, end;
    trace_t             tr;

    (void)ctx;
    VectorSet(start, x, y, zTop);
    VectorSet(end, x, y, zBottom);
    Radar_Trace(&tr, start, end, mins, maxs);
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal[2] < 0.7f) {
        return false;
    }
    *z = tr.endpos[2];
    return true;
}

// a crouched player's box, from the height a step is taken at
static bool Radar_Fits(void *ctx, float x, float y, float z)
{
    static const vec3_t mins = {-15.0f, -15.0f, 18.0f};
    static const vec3_t maxs = {15.0f, 15.0f, 50.0f};
    vec3_t              at;
    trace_t             tr;

    (void)ctx;
    VectorSet(at, x, y, z);
    Radar_Trace(&tr, at, at, mins, maxs);
    return !tr.startsolid && !tr.allsolid;
}

static const radarmap::World radar_world = {Radar_FloorBelow, Radar_Fits, NULL};

static double Radar_Ms(void)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// the map's entities: the walk starts at every spawn point and on the floor
// at the top of every ladder
static void Radar_SeedFromEntities(void)
{
    const char *p    = cgi.CM_EntityString();
    const float cell = radar_map.CellSize();

    if (!p) {
        return;
    }
    for (;;) {
        char        classname[64] = "";
        char        model[64]     = "";
        vec3_t      origin        = {0.0f, 0.0f, 0.0f};
        qboolean    haveOrigin    = qfalse;
        const char *token         = COM_Parse((char **)&p);

        if (!p || token[0] != '{') {
            break;
        }
        for (;;) {
            char key[64];

            token = COM_Parse((char **)&p);
            if (!p || token[0] == '}') {
                break;
            }
            Q_strncpyz(key, token, sizeof(key));
            token = COM_Parse((char **)&p);
            if (!p) {
                break;
            }
            if (!Q_stricmp(key, "classname")) {
                Q_strncpyz(classname, token, sizeof(classname));
            } else if (!Q_stricmp(key, "model")) {
                Q_strncpyz(model, token, sizeof(model));
            } else if (!Q_stricmp(key, "origin")) {
                haveOrigin = sscanf(token, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3 ? qtrue : qfalse;
            }
        }
        if (!p) {
            break;
        }

        if (!Q_stricmpn(classname, "info_player_", 12) && Q_stricmp(classname, "info_player_intermission")) {
            if (haveOrigin) {
                radar_map.Seed(radar_world, origin[0], origin[1], origin[2]);
            }
        } else if (!Q_stricmp(classname, "func_ladder") && model[0] == '*') {
            const int index = atoi(model + 1);
            vec3_t    mins, maxs;
            float     x, y;

            if (index <= 0 || index >= cgi.CM_NumInlineModels()) {
                continue;
            }
            cgi.R_GetInlineModelBounds(index, mins, maxs);
            for (y = mins[1] + origin[1] - 40.0f; y <= maxs[1] + origin[1] + 40.0f; y += cell) {
                for (x = mins[0] + origin[0] - 40.0f; x <= maxs[0] + origin[0] + 40.0f; x += cell) {
                    float z;

                    if (Radar_FloorBelow(NULL, x, y, maxs[2] + origin[2] + 48.0f, maxs[2] + origin[2] - 40.0f, &z)) {
                        radar_map.Seed(radar_world, x, y, z);
                    }
                }
            }
        }
    }
}

// whether pictures are drawn on a thread of their own: with a core for it
// besides the game's and the renderer's
static bool Radar_Threaded(void)
{
    static const bool threaded = std::thread::hardware_concurrency() >= 3;

    return threaded;
}

// the walk for up to ms, and the next picture when the one shown is no
// longer the map's, the player's storey's or of where they are
static void Radar_Work(double ms)
{
    const double end = Radar_Ms() + ms;

    if (!radar_map.Ready() || !radar.size) {
        return;
    }
    if (radar_worker.Take(radar_pic)) {
        radar.picSerial++;
    }
    do {
        // the worker begins a picture from the map: it stands still meanwhile
        if (radar_worker.ReadsMap()) {
            return;
        }
        if (radar_map.Pending()) {
            radar_map.Step(radar_world, 64);
        } else if (radar_back.Busy()) {
            if (radarmap::Map::PictureRows(radar_back, 8)) {
                std::swap(radar_pic, radar_back);
                radar.picSerial++;
                return;
            }
        } else if (radar_worker.Busy()) {
            return;
        } else if (radar.stale || !radar_pic.done || radar_pic.revision != radar_map.Revision()) {
            radar.stale = qfalse;
            if (radar_map.PictureIs(radar_pic, radar.size, radar.unitsPerTexel, radar.focusZ, radar.picX, radar.picY, RADAR_PICTURE_HALF)) {
                return;
            }
            if (Radar_Threaded()
                && radar_worker.Start(radar_map, radar.size, radar.unitsPerTexel, radar.focusZ, radar.picX, radar.picY, RADAR_PICTURE_HALF)) {
                return;
            }
            if (!radar_map.BeginPicture(radar_back, radar.size, radar.unitsPerTexel, radar.focusZ, radar.picX, radar.picY, RADAR_PICTURE_HALF)) {
                return;
            }
        } else {
            return;
        }
    } while (Radar_Ms() < end);
}

void HudRadar_MapLoaded(void)
{
    vec3_t mins, maxs;
    double start;

    // the last map's pictures, the one being drawn too
    radar_worker.Cancel();
    radar_pic.done = false;
    radar_back.row = -1;
    radar.numSolids = 0;
    radar.uploaded  = qfalse;
    radar.size      = 0;
    radar.loaded    = qtrue;

    cgi.R_GetInlineModelBounds(0, mins, maxs);
    radar_map.Reset(mins[0], mins[1], maxs[0], maxs[1]);
    Radar_SeedFromEntities();
    start = Radar_Ms();
    // (the picture's size isn't known before the first draw: the walk alone)
    while (radar_map.Pending() && Radar_Ms() - start < RADAR_LOAD_MS) {
        radar_map.Step(radar_world, 64);
    }
    cgi.DPrintf(
        "Radar: %u floors (%.0f x %.0f units) in %.0f ms%s\n",
        (unsigned)radar_map.NodeCount(),
        radar_map.ExtentX(),
        radar_map.ExtentY(),
        Radar_Ms() - start,
        radar_map.Pending() ? ", more to walk" : ""
    );
}

void HudRadar_Shutdown(void)
{
    radar_worker.Stop();
    radar_map.Clear();
    radar_pic.pixels.clear();
    radar_back.pixels.clear();
    radar.loaded = qfalse;
}

// solid brush entities the server has sent that stay put (a bridge, crates):
// floors like the world's, from when they are first seen until the map changes
static void Radar_AddSolids(void)
{
    const float cell = radar_map.CellSize();
    int         i, k;

    for (i = 0; i < cg.snap->numEntities && radar.numSolids < RADAR_MAX_SOLIDS; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        radarSolid_t        *solid;
        vec3_t               mins, maxs;
        float                x, y;

        if (es->solid != SOLID_BMODEL || es->pos.trDelta[0] != 0.0f || es->pos.trDelta[1] != 0.0f
            || es->pos.trDelta[2] != 0.0f || es->angles[0] != 0.0f || es->angles[1] != 0.0f || es->angles[2] != 0.0f
            || es->modelindex <= 0 || es->modelindex >= cgi.CM_NumInlineModels()) {
            continue;
        }
        for (k = 0; k < radar.numSolids; k++) {
            if (radar.solids[k].number == es->number) {
                break;
            }
        }
        if (k < radar.numSolids) {
            continue;
        }
        solid         = &radar.solids[radar.numSolids++];
        solid->number = es->number;
        solid->model  = cgi.CM_InlineModel(es->modelindex);
        VectorCopy(es->origin, solid->origin);
        cgi.R_GetInlineModelBounds(es->modelindex, mins, maxs);
        VectorAdd(mins, es->origin, solid->mins);
        VectorAdd(maxs, es->origin, solid->maxs);

        for (y = solid->mins[1]; y <= solid->maxs[1]; y += cell) {
            for (x = solid->mins[0]; x <= solid->maxs[0]; x += cell) {
                float z;

                if (Radar_FloorBelow(NULL, x, y, solid->maxs[2] + 40.0f, solid->mins[2], &z)) {
                    radar_map.Seed(radar_world, x, y, z);
                }
            }
        }
    }
}

// players standing where the walk didn't get start it again from there
static void Radar_SeedFromPlayers(void)
{
    int i;

    if (cg.time >= radar.seedTime && cg.time - radar.seedTime < RADAR_SEED_MS) {
        return;
    }
    if (radar_worker.ReadsMap()) {
        return;
    }
    radar.seedTime = cg.time;
    Radar_AddSolids();
    if (!radar_map.Known(radar.origin[0], radar.origin[1], radar.origin[2])) {
        radar_map.Seed(radar_world, radar.origin[0], radar.origin[1], radar.origin[2]);
    }
    for (i = 0; i < radar.numBlips; i++) {
        const radarBlip_t *b = &radar.blips[i];

        if (b->kind == BLIP_TEAMMATE && b->hasZ && !radar_map.Known(b->origin[0], b->origin[1], b->origin[2])) {
            radar_map.Seed(radar_world, b->origin[0], b->origin[1], b->origin[2]);
        }
    }
}

/*
================
Radar_Upload

The finished picture into the atlas's square: only its rows that changed
================
*/
static void Radar_Upload(void)
{
    const radarmap::Picture &pic = radar_pic;
    int                      row, first = -1;

    if (!pic.done || pic.size != radar.size) {
        return;
    }
    // only a new picture (or the region made again): compared row by row
    if (radar.uploaded && radar.uploadedSerial == radar.picSerial && radar.generation == HudAtlas_Generation()) {
        return;
    }
    if (radar.generation != HudAtlas_Generation() || radar.rows.size() != (size_t)radar.size * radar.size) {
        radar.rows.assign((size_t)radar.size * radar.size, 0);
        radar.generation = HudAtlas_Generation();
        // the region is 0 after a clear, as the rows are
    }
    for (row = 0; row <= radar.size; row++) {
        const bool changed = row < radar.size
                          && memcmp(&radar.rows[(size_t)row * radar.size], &pic.pixels[(size_t)row * pic.size], radar.size);

        if (changed) {
            memcpy(&radar.rows[(size_t)row * radar.size], &pic.pixels[(size_t)row * pic.size], radar.size);
            memcpy(HudAtlas_Texels(radar.regionX, radar.regionY + row), &pic.pixels[(size_t)row * pic.size], radar.size);
            if (first < 0) {
                first = row;
            }
        } else if (first >= 0) {
            HudAtlas_Dirty(radar.regionX, radar.regionY + first, radar.size, row - first);
            first = -1;
        }
    }
    radar.texX0    = pic.x0;
    radar.texY0    = pic.y0;
    radar.texUnits = pic.unitsPerTexel;
    radar.texW     = pic.w;
    radar.texH           = pic.h;
    radar.uploaded       = qtrue;
    radar.uploadedSerial = radar.picSerial;
}

/*
====================================================================

WHAT IS ON IT

====================================================================
*/

static radarBlip_t *Radar_AddBlip(int kind, int team, const vec3_t origin, qboolean hasYaw, float yaw, qboolean hasZ)
{
    radarBlip_t *b;

    if (radar.numBlips >= RADAR_MAX_BLIPS) {
        return NULL;
    }
    b         = &radar.blips[radar.numBlips++];
    b->kind   = kind;
    b->team   = team;
    b->hasYaw = hasYaw;
    b->yaw    = yaw;
    b->hasZ   = hasZ;
    VectorCopy(origin, b->origin);
    return b;
}

static qboolean Radar_IsOtherPlayer(const entityState_t *es)
{
    return es->eType == ET_PLAYER && es->number >= 0 && es->number < MAX_CLIENTS && es->number != cg.snap->ps.clientNum
        && !(es->eFlags & EF_DEAD);
}

// the teammates: those in the snapshot, and the server's radar's (Spearhead
// and Breakthrough servers) for those out of it
static void Radar_AddTeammates(void)
{
    const int myTeam = cg.snap->ps.stats[STAT_TEAM];
    qboolean  inSnapshot[MAX_CLIENTS];
    int       i;

    if (cgs.gametype < GT_TEAM || (myTeam != TEAM_ALLIES && myTeam != TEAM_AXIS)) {
        return;
    }
    memset(inSnapshot, 0, sizeof(inSnapshot));
    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        const centity_t     *cent;

        if (!Radar_IsOtherPlayer(es) || cg.clientinfo[es->number].team != myTeam) {
            continue;
        }
        cent                   = &cg_entities[es->number];
        inSnapshot[es->number] = qtrue;
        Radar_AddBlip(BLIP_TEAMMATE, myTeam, cent->lerpOrigin, qtrue, cent->lerpAngles[YAW], qtrue);
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        vec3_t origin;

        if (i == cg.snap->ps.clientNum || inSnapshot[i] || !cg.radars[i].time || cg.time - cg.radars[i].time >= RADAR_TEAMMATE_MS
            || cg.clientinfo[i].team != myTeam) {
            continue;
        }
        VectorSet(origin, cg.radars[i].origin[0], cg.radars[i].origin[1], 0.0f);
        Radar_AddBlip(BLIP_TEAMMATE, myTeam, origin, qtrue, RAD2DEG(atan2(cg.radars[i].axis[1], cg.radars[i].axis[0])), qfalse);
    }
}

// the objectives with a place, and where the compass points (Omaha's
// CG_HudRadar_AddObjectives)
static void Radar_AddObjectives(void)
{
    const playerState_t *ps          = &cg.snap->ps;
    const int            center      = ps->stats[STAT_OBJECTIVECENTER];
    const int            left        = ps->stats[STAT_OBJECTIVELEFT];
    const int            right       = ps->stats[STAT_OBJECTIVERIGHT];
    qboolean             haveCurrent = qfalse;
    int                  i;

    for (i = 0; i < MAX_OBJECTIVES; i++) {
        const char *s = CG_ConfigString(CS_OBJECTIVES + i);
        vec3_t      loc;

        if (!s || !s[0] || !(cg.Objectives[i].flags & OBJ_FLAG_CURRENT)
            || (cg.Objectives[i].flags & (OBJ_FLAG_HIDDEN | OBJ_FLAG_COMPLETED))) {
            continue;
        }
        if (sscanf(Info_ValueForKey(s, "loc"), "%f %f %f", &loc[0], &loc[1], &loc[2]) != 3 || VectorCompare(loc, vec3_origin)) {
            continue;
        }
        if (i == cg.ObjectivesCurrentIndex) {
            haveCurrent = qtrue;
        }
        Radar_AddBlip(i == cg.ObjectivesCurrentIndex ? BLIP_OBJECTIVE : BLIP_OBJECTIVE_OTHER, TEAM_NONE, loc, qfalse, 0.0f, qtrue);
    }

    // the compass's middle mark is the objective's yaw from the view's, turned
    // half around; the side marks are 60 degrees less the angle 300 units have
    // at its distance apart, 7 at least (Player::UpdateStats)
    if (!haveCurrent && (center != 1800 || left != 1730 || right != 1870)) {
        const float yaw    = DEG2RAD(ps->viewangles[YAW] + 180.0f - center / 10.0f);
        const float spread = fabs(AngleSubtract(right / 10.0f, center / 10.0f));
        const float angle  = 60.0f - spread;
        float       distance;
        vec3_t      at;

        if (spread <= 7.05f) {
            distance = 150.0f;
        } else if (angle < 0.5f) {
            distance = 30000.0f;
        } else {
            distance = 300.0f / tan(DEG2RAD(angle));
        }
        VectorSet(at, ps->origin[0] + cos(yaw) * distance, ps->origin[1] + sin(yaw) * distance, ps->origin[2]);
        // 1.11 and older: a team game with no objective points the compass at
        // the nearest teammate (the stock compass shows them so)
        if (cg_protocol < PROTOCOL_MOHTA_MIN && cgs.gametype >= GT_TEAM) {
            const int myTeam = ps->stats[STAT_TEAM];

            if ((myTeam != TEAM_ALLIES && myTeam != TEAM_AXIS) || ps->stats[STAT_HEALTH] <= 0) {
                return;
            }
            for (i = 0; i < cg.snap->numEntities; i++) {
                const entityState_t *es = &cg.snap->entities[i];
                vec3_t               delta;

                if (!Radar_IsOtherPlayer(es) || cg.clientinfo[es->number].team != myTeam) {
                    continue;
                }
                // the one the snapshot has: already on the radar
                VectorSubtract(es->origin, ps->origin, delta);
                if (fabs(VectorLength(delta) - distance) < 64.0f
                    && (delta[0] * delta[0] + delta[1] * delta[1] < 64.0f * 64.0f
                        || fabs(AngleSubtract(RAD2DEG(yaw), RAD2DEG(atan2(delta[1], delta[0])))) < 10.0f)) {
                    return;
                }
            }
            Radar_AddBlip(BLIP_TEAMMATE, myTeam, at, qfalse, 0.0f, qfalse);
            return;
        }
        Radar_AddBlip(BLIP_OBJECTIVE, TEAM_NONE, at, qfalse, 0.0f, qfalse);
    }
}

/*
================
HudRadar_Frame

What the radar shows this frame, and its walk and pictures
================
*/
uint64_t HudRadar_Frame(void)
{
    float    z;
    uint64_t key = 1469598103934665603ULL;
    int      i;

    if (!radar.loaded || !cg.snap) {
        return 0;
    }
    VectorCopy(cg.validPPS ? cg.predicted_player_state.origin : cg.snap->ps.origin, radar.origin);
    radar.yaw      = cg.refdefViewAngles[YAW];
    radar.northYaw = SHORT2ANGLE(cg.snap->ps.stats[STAT_COMPASSNORTH]);
    radar.numBlips = 0;
    Radar_AddObjectives();
    if (cgs.gametype != GT_SINGLE_PLAYER) {
        Radar_AddTeammates();
    }

    // the storey the player is on: the floor they stand on or are in the air
    // over by no more than a jump, else their own height
    z = radar.origin[2];
    radar_map.FloorUnder(radar.origin[0], radar.origin[1], z, RADAR_FLOOR_ABOVE, RADAR_FLOOR_BELOW, &z);
    if (fabsf(z - radar.focusZ) > RADAR_FOCUS_STEP) {
        radar.focusZ = z;
        radar.stale  = qtrue;
    }
    if (fabsf(radar.origin[0] - radar.picX) > RADAR_PICTURE_STEP || fabsf(radar.origin[1] - radar.picY) > RADAR_PICTURE_STEP) {
        radar.picX  = radar.origin[0];
        radar.picY  = radar.origin[1];
        radar.stale = qtrue;
    }
    Radar_SeedFromPlayers();
    Radar_Work(RADAR_FRAME_MS);
    Radar_Upload();

    // what is drawn depends on: the view (to a hundredth of a degree and a
    // tenth of a unit), the blips, the picture
    auto mix = [&key](int64_t v) {
        key ^= (uint64_t)v;
        key *= 1099511628211ULL;
        key ^= key >> 31;
    };
    mix((int64_t)(radar.yaw * 100.0f));
    mix((int64_t)(radar.northYaw * 100.0f));
    mix((int64_t)(radar.origin[0] * 10.0f));
    mix((int64_t)(radar.origin[1] * 10.0f));
    mix((int64_t)(radar.origin[2] * 10.0f));
    mix(radar.uploaded ? (int64_t)radar.uploadedSerial : -1);
    mix(radar.size);
    for (i = 0; i < radar.numBlips; i++) {
        const radarBlip_t *b = &radar.blips[i];

        mix(b->kind);
        mix(b->team);
        mix((int64_t)(b->origin[0] * 10.0f));
        mix((int64_t)(b->origin[1] * 10.0f));
        mix((int64_t)(b->origin[2] * 10.0f));
        mix((int64_t)(b->yaw * 100.0f));
    }
    return key;
}

/*
====================================================================

DRAWING

====================================================================
*/

typedef struct {
    float cx, cy, r, k, scale;
    float sinYaw, cosYaw;
} radarPaint_t;

// a point of the world on the radar; one past limit (from the center) is
// brought in to it. Returns whether it was
static qboolean Radar_Place(const radarPaint_t *p, const float *origin, float limit, float *x, float *y)
{
    const float dx  = (origin[0] - radar.origin[0]) * p->scale;
    const float dy  = (origin[1] - radar.origin[1]) * p->scale;
    float       sx  = dx * p->sinYaw - dy * p->cosYaw;
    float       sy  = -(dx * p->cosYaw + dy * p->sinYaw);
    const float len = sqrtf(sx * sx + sy * sy);
    qboolean    clamped = qfalse;

    if (len > limit && len > 0.0f) {
        sx *= limit / len;
        sy *= limit / len;
        clamped = qtrue;
    }
    *x = p->cx + sx;
    *y = p->cy + sy;
    return clamped;
}

typedef struct {
    float x, y, u, v; // screen, picture texels
} radarPoint_t;

// the part of a convex polygon with u in [lo, hi] (which = 0) or v (which = 1)
static int Radar_Clip(const radarPoint_t *in, int n, radarPoint_t *out, int which, float bound, float sign)
{
    int count = 0;

    for (int i = 0; i < n; i++) {
        const radarPoint_t &a  = in[i];
        const radarPoint_t &b  = in[(i + 1) % n];
        const float         da = sign * ((which ? a.v : a.u) - bound);
        const float         db = sign * ((which ? b.v : b.u) - bound);

        if (da >= 0.0f) {
            out[count++] = a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float  t = da / (da - db);
            radarPoint_t c;

            c.x          = a.x + (b.x - a.x) * t;
            c.y          = a.y + (b.y - a.y) * t;
            c.u          = a.u + (b.u - a.u) * t;
            c.v          = a.v + (b.v - a.v) * t;
            out[count++] = c;
        }
    }
    return count;
}

// the map's picture, turned about the player, in the radar's circle: a
// polygon of the circle cut to the picture, as quads from its first point
static void Radar_DrawMap(const radarPaint_t *p)
{
    enum { N = 48 };
    static const hudColor_t tint = {0xEB, 0xF0, 0xF5, 0xFF};
    radarPoint_t            a[N + 8], b[N + 8];
    int                     n = N, i;

    static float            circle[N][2]; // the unit circle's points, made once
    float                   toTexel, u0, v0;

    if (!radar.uploaded || radar.texUnits <= 0.0f) {
        return;
    }
    if (!circle[0][0]) {
        for (i = 0; i < N; i++) {
            circle[i][0] = cosf((float)i / N * 2.0f * M_PI);
            circle[i][1] = sinf((float)i / N * 2.0f * M_PI);
        }
    }
    // screen offset to the picture's texels (Radar_Place turned back): world
    // units a pixel over the units a texel
    toTexel = 1.0f / (p->scale * radar.texUnits);
    u0      = (radar.origin[0] - radar.texX0) / radar.texUnits;
    v0      = (radar.origin[1] - radar.texY0) / radar.texUnits;
    for (i = 0; i < N; i++) {
        const float ux = circle[i][0] * p->r, uy = circle[i][1] * p->r;
        const float u  = ux, v = -uy;

        a[i].x = p->cx + ux;
        a[i].y = p->cy + uy;
        a[i].u = u0 + (u * p->sinYaw + v * p->cosYaw) * toTexel;
        a[i].v = v0 + (-u * p->cosYaw + v * p->sinYaw) * toTexel;
    }
    n = Radar_Clip(a, n, b, 0, 0.0f, 1.0f);
    n = Radar_Clip(b, n, a, 0, (float)radar.texW, -1.0f);
    n = Radar_Clip(a, n, b, 1, 0.0f, 1.0f);
    n = Radar_Clip(b, n, a, 1, (float)radar.texH, -1.0f);
    for (i = 1; i + 1 < n; i += 2) {
        const radarPoint_t *q[4] = {&a[0], &a[i], &a[i + 1], &a[i + 2 < n ? i + 2 : i + 1]};
        vert2D_t            v[4];

        for (int k = 0; k < 4; k++) {
            v[k].xy[0] = q[k]->x;
            v[k].xy[1] = q[k]->y;
            v[k].st[0] = HudAtlas_U(radar.regionX + q[k]->u);
            v[k].st[1] = HudAtlas_V(radar.regionY + q[k]->v);
            memcpy(v[k].rgba, &tint, 4);
        }
        Hud_Vertexes(v);
    }
}

// the radar's shapes as the atlas has them, kept while the atlas is (a frame
// asks for a dozen of a few sizes)
static const hudSprite_t *Radar_Shape(hudShape_t shape, int w, float p)
{
    static struct {
        int                generation;
        hudShape_t         shape;
        int                w;
        float              p;
        const hudSprite_t *sprite;
    } cache[24];
    static int next;
    const int  generation = HudAtlas_Generation();
    int        i;

    for (i = 0; i < (int)ARRAY_LEN(cache); i++) {
        if (cache[i].sprite && cache[i].generation == generation && cache[i].shape == shape && cache[i].w == w && cache[i].p == p) {
            return cache[i].sprite;
        }
    }
    i                   = next++ % (int)ARRAY_LEN(cache);
    cache[i].generation = generation;
    cache[i].shape      = shape;
    cache[i].w          = w;
    cache[i].p          = p;
    cache[i].sprite     = HudAtlas_Shape(shape, w, 0, p);
    return cache[i].sprite;
}

static hudColor_t Radar_TeamColor(int team)
{
    return Hud_Color(team == TEAM_AXIS ? 0xCC8888FF : team == TEAM_ALLIES ? 0x8891CCFF : 0xFFFFFFFF);
}

// a teammate: a dot with a dark edge, and a point toward where they look
static void Radar_DrawPlayer(const radarPaint_t *p, const radarBlip_t *b)
{
    const hudColor_t color = Radar_TeamColor(b->team);
    const hudColor_t edge  = Hud_Color(0x0C0E12E6);
    float            x, y, radius;
    qboolean         clamped;

    clamped = Radar_Place(p, b->origin, p->r - 7.0f * p->k, &x, &y);
    radius  = (clamped ? 3.9f : 5.5f) * p->k;

    if (b->hasYaw && !clamped) {
        // up on the radar is the view's yaw
        const hudSprite_t *pointer =
            Radar_Shape(HUD_SHAPE_POINTER, (int)(radius * 8.0f + 0.5f), radius + 1.25f * p->k);

        Hud_SpriteTurned(pointer, x, y, 0.0f, 0.0f, radar.yaw - b->yaw, color);
    }
    Hud_Sprite(Radar_Shape(HUD_SHAPE_DISC, (int)((radius + 1.25f * p->k) * 2.0f + 0.5f), 0.0f), x - radius - 1.25f * p->k, y - radius - 1.25f * p->k, edge);
    Hud_Sprite(Radar_Shape(HUD_SHAPE_DISC, (int)(radius * 2.0f + 0.5f), 0.0f), x - radius, y - radius, color);

    // a storey above or below: a dark ^ or v in the dot
    if (b->hasZ && fabsf(b->origin[2] - radar.origin[2]) > RADAR_LEVEL_DZ) {
        const hudSprite_t *chevron = Radar_Shape(HUD_SHAPE_CHEVRON, (int)(radius * 2.0f + 0.5f), 0.0f);

        Hud_SpriteTurned(chevron, x, y, 0.0f, 0.0f, b->origin[2] > radar.origin[2] ? 0.0f : 180.0f, edge);
    }
}

// an objective: a diamond, white for the current one
static void Radar_DrawObjective(const radarPaint_t *p, const radarBlip_t *b)
{
    const qboolean   current = b->kind == BLIP_OBJECTIVE;
    const hudColor_t color   = Hud_Color(current ? 0xFFFFFFF2 : 0xBFBFBFCC);
    const hudColor_t edge    = Hud_Color(0x0C0E12E6);
    const float      half    = (current ? 5.5f : 4.25f) * p->k;
    float            x, y;

    Radar_Place(p, b->origin, p->r - 7.0f * p->k, &x, &y);
    Hud_SpriteTurned(Radar_Shape(HUD_SHAPE_DIAMOND, (int)((half + 1.75f * p->k) * 2.0f + 0.5f), 0.0f), x, y, 0.0f, 0.0f, 0.0f, edge);
    Hud_SpriteTurned(Radar_Shape(HUD_SHAPE_DIAMOND, (int)(half * 2.0f + 0.5f), 0.0f), x, y, 0.0f, 0.0f, 0.0f, color);
}

/*
================
HudRadar_Draw

The radar in its square (x, y, size pixels across): its disc, the map, what
is on it, the compass's letters, the damage wedge and the ring
================
*/
void HudRadar_Draw(float x, float y, float size, float damageAngle, float damageAlpha)
{
    static const char *const letters[4] = {"N", "E", "S", "W"};
    radarPaint_t             p;
    const float              rad = DEG2RAD(radar.yaw);
    int                      i, pass;

    p.r      = size * 0.5f;
    p.cx     = x + p.r;
    p.cy     = y + p.r;
    p.k      = size / RADAR_DESIGN_PX;
    p.scale  = p.r / RADAR_RANGE;
    p.sinYaw = sinf(rad);
    p.cosYaw = cosf(rad);

    // the picture's detail and place in the atlas: about three quarters of a
    // pixel a texel, of what the radar can show from its middle and a little more
    {
        const float units = RADAR_RANGE * 2.0f / size * 0.75f;
        const int   texels = Q_min(2048, ((int)(RADAR_PICTURE_HALF * 2.0f / units) + 7) & ~7);
        int         rx, ry;

        if (fabsf(units - radar.unitsPerTexel) > radar.unitsPerTexel * 0.1f) {
            radar.unitsPerTexel = units;
            radar.stale         = qtrue;
        }
        if (texels != radar.size || radar.generation != HudAtlas_Generation()) {
            if (HudAtlas_RadarRegion(texels, &rx, &ry)) {
                if (texels != radar.size) {
                    radar_worker.Cancel();
                    radar_pic.done = false;
                    radar_back.row = -1;
                    radar.stale    = qtrue;
                }
                radar.size     = texels;
                radar.regionX  = rx;
                radar.regionY  = ry;
                radar.uploaded = qfalse;
                radar.rows.clear();
            } else {
                radar.size = 0;
            }
        }
    }

    Hud_Sprite(Radar_Shape(HUD_SHAPE_DISC, (int)(size + 0.5f), 0.0f), x, y, Hud_Color(0x0C0E12C7));
    Hud_Sprite(Radar_Shape(HUD_SHAPE_RING, (int)(size + 0.5f), p.k), x, y, Hud_Color(0xFFFFFF1A));

    if (radar.loaded) {
        Radar_DrawMap(&p);
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < radar.numBlips; i++) {
                const radarBlip_t *b = &radar.blips[i];

                if ((b->kind == BLIP_TEAMMATE) != (pass == 1)) {
                    continue;
                }
                if (pass) {
                    Radar_DrawPlayer(&p, b);
                } else {
                    Radar_DrawObjective(&p, b);
                }
            }
        }
        // the player: an arrow at the center, up
        Hud_SpriteTurned(Radar_Shape(HUD_SHAPE_ARROW, (int)(16.0f * 1.4f * p.k + 0.5f), 0.0f), p.cx, p.cy, 0.0f, 0.0f, 0.0f, Hud_Color(0x0C0E12E6));
        Hud_SpriteTurned(Radar_Shape(HUD_SHAPE_ARROW, (int)(16.0f * p.k + 0.5f), 0.0f), p.cx, p.cy, 0.0f, 0.0f, 0.0f, Hud_Color(0xFFFFFFFF));
    }

    // N, E, S and W inside the edge, turning with the view: the capitals'
    // middle on the point, a shadow a pixel down
    for (i = 0; i < 4; i++) {
        const float      a     = DEG2RAD(radar.northYaw - 90.0f * i - radar.yaw);
        const float      lx    = p.cx - sinf(a) * (p.r - 10.0f * p.k);
        const float      ly    = p.cy - cosf(a) * (p.r - 10.0f * p.k);
        const float      px    = 12.0f * p.k;
        const float      w     = HudFont_Width(HUD_FACE_SEMIBOLD, px, letters[i]);
        const float      base  = ly + HudFont_CapHeight(HUD_FACE_SEMIBOLD, px) * 0.5f;
        const hudColor_t color = Hud_Color(i == 0 ? 0xFFFFFFFF : 0xBFBFBFFF);

        Hud_Text(HUD_FACE_SEMIBOLD, px, lx - w * 0.5f, base + p.k, letters[i], Hud_Color(0x000000B3), qfalse);
        Hud_Text(HUD_FACE_SEMIBOLD, px, lx - w * 0.5f, base, letters[i], color, qfalse);
    }

    // the damage's direction (the compass's wedge), and the ring
    if (damageAlpha > 0.0f) {
        Hud_SpriteTurned(Radar_Shape(HUD_SHAPE_WEDGE, (int)(size + 0.5f), 0.0f), p.cx, p.cy, 0.0f, 0.0f, damageAngle, Hud_Fade(Hud_Color(0xC03021FF), damageAlpha));
    }
    Hud_Sprite(Radar_Shape(HUD_SHAPE_RING, (int)(size + 0.5f), 2.0f * Hud_Scale()), x, y, Hud_Color(0xDCE4A4E6));
}
