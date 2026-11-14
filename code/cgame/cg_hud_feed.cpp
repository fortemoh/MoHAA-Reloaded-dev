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
// The Competitive HUD's feeds: the kills (Project: Omaha's kill feed: the
// server's printdeathmsg on Spearhead and Breakthrough servers, the printed
// line on Allied Assault's, cl_killfeed.cpp and cl_killfeed_classify.cpp),
// the chat and the game messages (the lines the server prints, which the
// stock HUD's boxes showed), and who is alive in round modes: the scoreboard
// (asked for silently when it can change, not on a timer where the server
// takes one command a second) and the kills since.

#include "cg_hud.h"

#include <ctype.h>

#define KILL_LIFETIME    6000
#define LINE_LIFETIME    8000
#define SCORE_INTERVAL   2000 // the stock scoreboard's own request interval

static struct {
    hudKill_t kills[HUD_FEED_KILLS];
    int       numKills;
    hudLine_t chat[HUD_FEED_LINES];
    int       numChat;
    hudLine_t messages[HUD_FEED_LINES];
    int       numMessages;
    int       revision;

    // who is alive
    hudRoster_t roster;
    int         team[MAX_CLIENTS];   // as the scoreboard or the client's info had it, 0 unknown
    char        name[MAX_CLIENTS][MAX_NAME_LENGTH]; // the clients' names as the server sends them in kills
    qboolean    scoresWanted;
    int         lastTeam;
    qboolean    wasPlaying;
    char        roundStart[64];
    int         rosterKey;
    int         floodTime;
    qboolean    floodOn; // sv_floodProtect: no score requests on a timer
} feed;

/*
====================================================================

LINES

====================================================================
*/

static void HudFeed_AddLine(hudLine_t *lines, int *count, const char *text, hudColor_t color, qboolean bold)
{
    hudLine_t *line;
    char      *p;

    if (*count == HUD_FEED_LINES) {
        memmove(lines, lines + 1, sizeof(lines[0]) * (HUD_FEED_LINES - 1));
        (*count)--;
    }
    line = &lines[(*count)++];
    Q_strncpyz(line->text, text, sizeof(line->text));
    // one line: the newline the server ends it with, and others, are spaces
    for (p = line->text; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    for (p = line->text + strlen(line->text); p > line->text && p[-1] == ' '; p--) {
        p[-1] = 0;
    }
    line->time  = cg.time ? cg.time : 1;
    line->color = color;
    line->bold  = bold;
    feed.revision++;
}

static const hudLine_t *HudFeed_Live(const hudLine_t *lines, int count, int *live)
{
    int first = 0;

    while (first < count && cg.time - lines[first].time >= LINE_LIFETIME) {
        first++;
    }
    *live = count - first;
    return lines + first;
}

const hudLine_t *HudFeed_Chat(int *count)
{
    return HudFeed_Live(feed.chat, feed.numChat, count);
}

const hudLine_t *HudFeed_Messages(int *count)
{
    return HudFeed_Live(feed.messages, feed.numMessages, count);
}

int HudFeed_Revision(void)
{
    return feed.revision;
}

/*
====================================================================

NAMES

====================================================================
*/

// a name as the server writes it in a kill (fgame's SanitizeName): its last
// character a '?' when it is a space or a control character
static void HudFeed_Sanitize(const char *in, char *out, int size)
{
    int len = (int)strlen(in);
    int i;

    for (i = 0; i < len && i < size - 1; i++) {
        out[i] = (i >= len - 1 && (unsigned char)in[i] <= ' ') ? '?' : in[i];
    }
    out[i] = 0;
}

// the clients called name, on that team only when team is one; the first
// returned, how many in count
static int HudFeed_FindName(const char *name, int team, int *count)
{
    int found = -1;
    int i;

    *count = 0;
    if (!name || !name[0]) {
        return -1;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        char sanitized[MAX_NAME_LENGTH];

        if (!cg.clientinfo[i].name[0]) {
            continue;
        }
        if ((team == TEAM_ALLIES || team == TEAM_AXIS) && cg.clientinfo[i].team != team) {
            continue;
        }
        HudFeed_Sanitize(cg.clientinfo[i].name, sanitized, sizeof(sanitized));
        if (strcmp(sanitized, name) && strcmp(cg.clientinfo[i].name, name)) {
            continue;
        }
        if (found < 0) {
            found = i;
        }
        (*count)++;
    }
    return found;
}

static int HudFeed_TeamOfName(const char *name)
{
    int count;
    int client = HudFeed_FindName(name, 0, &count);

    return client >= 0 && count == 1 ? cg.clientinfo[client].team : TEAM_NONE;
}

/*
====================================================================

KILLS

====================================================================
*/

typedef struct {
    const char *s1;
    const char *s2Prefix; // NULL: s2 not looked at; "": none
    const char *weaponClass;
} hudPhrase_t;

// Omaha's (cl_killfeed_classify.cpp): the obituaries' English keys
static const hudPhrase_t hud_playerPhrases[] = {
    {"was gunned down by",             "",                               "pistol"  },
    {"was rifled by",                  "",                               "rifle"   },
    {"was sniped by",                  "",                               "sniper"  },
    {"was perforated by",              "'s' SMG",                        "smg"     },
    {"was machine-gunned by",          "",                               "mg"      },
    {"was hunted down by",             "",                               "shotgun" },
    {"was pumped full of buckshot by", "",                               "shotgun" },
    {"tripped on",                     "'s grenade",                     "grenade" },
    {"is picking",                     "'s shrapnel out of his teeth",   "grenade" },
    {"took",                           "'s rocket right in the kisser",  "rocket"  },
    {"took",                           "'s rocket in the face",          "rocket"  },
    {"stepped on",                     "'s landmine",                    "landmine"},
    {"was bashed by",                  "",                               "bash"    },
    {"was clubbed by",                 "",                               "bash"    },
    {"was crushed by",                 "",                               "crush"   },
    {"was telefragged by",             "",                               "telefrag"},
    {"was shot by",                    "",                               "unknown" },
    {"was pushed over the edge by",    "",                               "none"    },
    {"was burned up by",               "",                               "none"    },
    {"was melted by",                  "",                               "none"    },
    {"was knocked out by",             "",                               "none"    },
    {"was run over by",                "",                               "none"    },
    {"was impaled by",                 "",                               "none"    },
    {"was blown away by",              "",                               "none"    },
    {"was killed by",                  "",                               "none"    },
    {NULL,                             NULL,                             NULL      },
};

static const hudPhrase_t hud_suicidePhrases[] = {
    {"played catch with himself",     NULL, "grenade" },
    {"tripped on his own grenade",    NULL, "grenade" },
    {"rocketed himself",              NULL, "rocket"  },
    {"was hoist on his own pitard",   NULL, "landmine"},
    {"cratered",                      NULL, "none"    },
    {"took himself out of commision", NULL, "none"    },
    {"was burned to a crisp",         NULL, "none"    },
    {"was melted to nothing",         NULL, "none"    },
    {"blew himself up",               NULL, "none"    },
    {"died",                          NULL, "none"    },
    {NULL,                            NULL, NULL      },
};

static const hudPhrase_t hud_worldPhrases[] = {
    {"caught some shrapnel",   NULL, "grenade" },
    {"caught a rocket",        NULL, "rocket"  },
    {"stepped on a land mine", NULL, "landmine"},
    {"cratered",               NULL, "none"    },
    {"was burned to a crisp",  NULL, "none"    },
    {"was melted to nothing",  NULL, "none"    },
    {"blew up",                NULL, "none"    },
    {"died",                   NULL, "none"    },
    {NULL,                     NULL, NULL      },
};

// the icon of a weapon class (Omaha's s_killFeedIcons): the Allies' and the
// Axis' silhouettes and their sizes in design pixels, pointing right
typedef struct {
    const char *weaponClass;
    const char *allies;
    float       alliesW, alliesH;
    const char *axis;
    float       axisW, axisH;
} hudKillIcon_t;

static const hudKillIcon_t hud_killIcons[] = {
    {"pistol",   "colt45_filled",          33.9f,  21.9f, "p38_filled",              33.9f,  22.2f},
    {"rifle",    "m1_garand_filled",       90.0f,  16.2f, "kar98_filled",            90.5f,  16.3f},
    {"sniper",   "springfield_filled",     89.9f,  17.5f, "kar98sniper_filled",      90.5f,  17.6f},
    {"smg",      "thompsonsmg_filled",     75.1f,  24.2f, "mp40_filled",             74.8f,  26.0f},
    {"mg",       "bar_filled",             95.5f,  15.3f, "mp44_filled",             70.9f,  26.0f},
    {"shotgun",  "shotgun_filled",         85.0f,  15.9f, "shotgun_filled",          85.0f,  15.9f},
    {"grenade",  "m2frag_grenade_filled",  12.5f,  22.0f, "steilhandgranate_filled", 5.6f,   26.0f},
    {"rocket",   "bazooka_filled",         110.6f, 20.0f, "panzerschreck_filled",    113.5f, 26.0f},
    {"bash",     "glove_50",               20.0f,  20.0f, "glove_50",                20.0f,  20.0f},
    {"none",     "skull_50",               20.0f,  20.0f, "skull_50",                20.0f,  20.0f},
    {"crush",    "skull_50",               20.0f,  20.0f, "skull_50",                20.0f,  20.0f},
    {"telefrag", "skull_50",               20.0f,  20.0f, "skull_50",                20.0f,  20.0f},
    {"landmine", "skull_50",               20.0f,  20.0f, "skull_50",                20.0f,  20.0f},
};

static qboolean HudFeed_S2Empty(const char *s2)
{
    return !s2 || !s2[0] || (s2[0] == 'x' && !s2[1]) ? qtrue : qfalse;
}

static const char *HudFeed_MatchPhrase(const hudPhrase_t *table, const char *s1, const char *s2)
{
    char        base[256];
    const char *hit;
    int         i;

    // s2 without its " in the <place>"
    Q_strncpyz(base, s2 ? s2 : "", sizeof(base));
    hit = strstr(base, " in the ");
    if (hit) {
        base[hit - base] = 0;
    }
    for (i = 0; table[i].s1; i++) {
        if (Q_stricmp(s1, table[i].s1)) {
            continue;
        }
        if (!table[i].s2Prefix) {
            return table[i].weaponClass;
        }
        if (!table[i].s2Prefix[0]) {
            if (HudFeed_S2Empty(base)) {
                return table[i].weaponClass;
            }
            continue;
        }
        if (s2 && !Q_stricmpn(s2, table[i].s2Prefix, strlen(table[i].s2Prefix))) {
            return table[i].weaponClass;
        }
    }
    return "unknown";
}

static qboolean HudFeed_Headshot(const char *s2)
{
    const char *p = s2 ? strstr(s2, " in the ") : NULL;

    if (!p) {
        return qfalse;
    }
    p += 8;
    return (!Q_stricmpn(p, "head", 4) && (!p[4] || p[4] == ' ')) || (!Q_stricmpn(p, "helmet", 6) && (!p[6] || p[6] == ' '))
        || (!Q_stricmpn(p, "neck", 4) && (!p[4] || p[4] == ' '));
}

/*
================
HudFeed_MarkDead

A kill between scoreboards: the victim's card turns. Only a client of the
victim's team when the kill tells it, and only one: two players called the
same on one team leave it to the next scoreboard
================
*/
static void HudFeed_MarkDead(const char *victim, int team)
{
    int count;
    int client = HudFeed_FindName(victim, team, &count);

    if (client < 0 || count != 1 || feed.roster.dead[client]) {
        return;
    }
    feed.roster.dead[client] = qtrue;
    feed.rosterKey++;
}

static void HudFeed_AddKill(const char *attacker, const char *victim, const char *weaponClass, char kind, qboolean headshot, int victimTeam)
{
    hudKill_t           *k;
    const hudKillIcon_t *icon = &hud_killIcons[1]; // a weapon not known: the rifle
    qboolean             axis;
    size_t               i;

    for (i = 0; i < ARRAY_LEN(hud_killIcons); i++) {
        if (!Q_stricmp(weaponClass, hud_killIcons[i].weaponClass)) {
            icon = &hud_killIcons[i];
            break;
        }
    }

    if (feed.numKills == HUD_FEED_KILLS) {
        memmove(feed.kills, feed.kills + 1, sizeof(feed.kills[0]) * (HUD_FEED_KILLS - 1));
        feed.numKills--;
    }
    k = &feed.kills[feed.numKills++];
    memset(k, 0, sizeof(*k));
    Q_strncpyz(k->killer, attacker, sizeof(k->killer));
    Q_strncpyz(k->victim, victim, sizeof(k->victim));
    k->killerTeam = HudFeed_TeamOfName(attacker);
    k->victimTeam = victimTeam == TEAM_ALLIES || victimTeam == TEAM_AXIS ? victimTeam : HudFeed_TeamOfName(victim);
    // the killer's weapon, of the victim's side when there is no killer
    axis          = (attacker[0] ? k->killerTeam : k->victimTeam) == TEAM_AXIS;
    k->icon       = HudSilhouette_Find(axis ? icon->axis : icon->allies);
    k->iconW      = (int)((axis ? icon->axisW : icon->alliesW) + 0.5f);
    k->iconH      = (int)((axis ? icon->axisH : icon->alliesH) + 0.5f);
    k->suicide    = kind == 's';
    k->headshot   = headshot;
    k->time       = cg.time ? cg.time : 1;
    feed.revision++;

    HudFeed_MarkDead(victim, victimTeam);
}

// the victim's team a kill sent to this client tells: its type's case
// (fgame's G_PrintDeathMessage), lower when the victim is on our team
static int HudFeed_VictimTeam(char type)
{
    const int myTeam = cg.snap ? cg.snap->ps.stats[STAT_TEAM] : TEAM_NONE;
    const int lower  = type == tolower((unsigned char)type);

    if (myTeam == TEAM_ALLIES || myTeam == TEAM_AXIS) {
        return lower ? myTeam : (myTeam == TEAM_ALLIES ? TEAM_AXIS : TEAM_ALLIES);
    }
    // a spectator: lower for the Allies
    return lower ? TEAM_ALLIES : TEAM_AXIS;
}

void CG_HudFeed_DeathMessage(const char *s1, const char *s2, const char *attacker, const char *victim, char type)
{
    const char kind = (char)tolower((unsigned char)type);
    const char *weaponClass;

    if (!CG_HudCompetitive_Active()) {
        return;
    }

    if (attacker && attacker[0] == 'x' && !attacker[1]) {
        attacker = "";
    }
    if (victim && victim[0] == 'x' && !victim[1]) {
        victim = "";
    }
    if (kind == 's') {
        weaponClass = HudFeed_MatchPhrase(hud_suicidePhrases, s1 ? s1 : "", s2);
    } else if (kind == 'w') {
        weaponClass = HudFeed_MatchPhrase(hud_worldPhrases, s1 ? s1 : "", s2);
    } else {
        weaponClass = HudFeed_MatchPhrase(hud_playerPhrases, s1 ? s1 : "", s2);
    }
    HudFeed_AddKill(
        kind == 'p' ? (attacker ? attacker : "") : "",
        victim ? victim : "",
        weaponClass,
        kind,
        HudFeed_Headshot(s2),
        cgs.gametype >= GT_TEAM ? HudFeed_VictimTeam(type) : TEAM_NONE
    );
}

static void HudFeed_Trim(const char *src, int len, char *out, int size)
{
    while (len > 0 && (*src == ' ' || *src == '\t')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\n' || src[len - 1] == '\r')) {
        len--;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(out, src, len);
    out[len] = 0;
}

/*
================
HudFeed_DeathPrint

A kill as an Allied Assault server prints it: "<victim> <s1> <attacker><s2>",
s1 and s2 in English. Not one Omaha knows: the line as the victim
================
*/
static void HudFeed_DeathPrint(const char *text)
{
    static const struct {
        const hudPhrase_t *table;
        char               kind;
    } tables[] = {
        {hud_playerPhrases,  'p'},
        {hud_suicidePhrases, 's'},
        {hud_worldPhrases,   'w'},
    };
    char victim[MAX_NAME_LENGTH], attacker[MAX_NAME_LENGTH];
    int  t, i;

    for (t = 0; t < (int)ARRAY_LEN(tables); t++) {
        const hudPhrase_t *table = tables[t].table;

        for (i = 0; table[i].s1; i++) {
            const char *hit = Q_stristr(text, table[i].s1);
            const char *after;
            const char *s2 = "";

            if (!hit) {
                continue;
            }
            HudFeed_Trim(text, (int)(hit - text), victim, sizeof(victim));
            after = hit + strlen(table[i].s1);
            attacker[0] = 0;
            if (tables[t].kind == 'p') {
                const char *end = NULL;

                if (table[i].s2Prefix && table[i].s2Prefix[0]) {
                    end = Q_stristr(after, table[i].s2Prefix);
                    if (!end) {
                        continue;
                    }
                } else {
                    end = Q_stristr(after, " in the ");
                }
                if (end) {
                    s2 = end;
                    HudFeed_Trim(after, (int)(end - after), attacker, sizeof(attacker));
                } else {
                    HudFeed_Trim(after, (int)strlen(after), attacker, sizeof(attacker));
                }
            }
            HudFeed_AddKill(
                attacker,
                victim,
                HudFeed_MatchPhrase(table, table[i].s1, s2),
                tables[t].kind,
                HudFeed_Headshot(s2),
                TEAM_NONE
            );
            return;
        }
    }
    HudFeed_Trim(text, (int)strlen(text), victim, sizeof(victim));
    HudFeed_AddKill("", victim, "unknown", 'p', qfalse, TEAM_NONE);
}

const hudKill_t *HudFeed_Kills(int *count)
{
    int first = 0;

    while (first < feed.numKills && cg.time - feed.kills[first].time >= KILL_LIFETIME) {
        first++;
    }
    *count = feed.numKills - first;
    return feed.kills + first;
}

/*
====================================================================

THE SERVER'S LINES

====================================================================
*/

void CG_HudFeed_Print(const char *msg)
{
    static const hudColor_t yellow = {179, 153, 13, 255}; // the stock HUD's (UHudColor)
    static const hudColor_t white  = {255, 255, 255, 255};

    if (!msg || !msg[0] || !CG_HudCompetitive_Active()) {
        return;
    }
    switch ((unsigned char)msg[0]) {
    case MESSAGE_YELLOW:
        HudFeed_AddLine(feed.messages, &feed.numMessages, msg + 1, yellow, qfalse);
        CG_HudCompetitive_Pickup(msg + 1);
        break;
    case MESSAGE_WHITE:
        HudFeed_AddLine(feed.messages, &feed.numMessages, msg + 1, white, qtrue);
        break;
    case MESSAGE_CHAT_WHITE:
        HudFeed_AddLine(feed.chat, &feed.numChat, msg + 1, white, qfalse);
        break;
    case MESSAGE_CHAT_RED:
    case MESSAGE_CHAT_GREEN:
        // an Allied Assault server's kill (a newer one sends printdeathmsg)
        HudFeed_DeathPrint(msg + 1);
        break;
    default:
        break;
    }
}

/*
====================================================================

WHO IS ALIVE

====================================================================
*/

void CG_HudFeed_ScoresBegin(void)
{
    feed.roster.teamScore[0] = feed.roster.teamScore[1] = -1;
}

// a row of the scoreboard: a player (dead when the server says so), or a
// team's header with its score (clientNum -1)
void CG_HudFeed_ScoreEntry(int clientNum, int team, qboolean dead, const char *score)
{
    if (clientNum == -1) {
        if (team == TEAM_ALLIES || team == TEAM_AXIS) {
            feed.roster.teamScore[team == TEAM_AXIS] = atoi(score ? score : "0");
        }
        return;
    }
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
        return;
    }
    // (the team comes from the clients' info, which the scoreboard can't be newer than)
    feed.roster.dead[clientNum] = dead;
}

void CG_HudFeed_ScoresEnd(void)
{
    feed.rosterKey++;
}

// the next silent score request goes out (a new round)
static void HudFeed_WantScores(void)
{
    feed.scoresWanted = qtrue;
}

/*
================
HudFeed_RequestScores

The scoreboard without opening it, sharing the stock 2 s between requests
with TAB. A server with flood protection takes one command a second, so the
HUD asks once when the cards can change (the first frame on a map, a new
round, the player's death, a team joined), not on a timer, and the kills
mark deaths between; with sv_floodProtect 0 on the timer too
================
*/
static void HudFeed_RequestScores(void)
{
    const playerState_t *ps = &cg.snap->ps;
    const qboolean       playing = !(ps->pm_flags & PMF_SPECTATING) && ps->stats[STAT_HEALTH] > 0;
    const char          *flood;

    if (cgs.gametype <= GT_FFA) {
        return;
    }
    if (feed.wasPlaying && !playing) {
        feed.scoresWanted = qtrue;
    }
    if (playing && ps->stats[STAT_TEAM] != feed.lastTeam) {
        feed.lastTeam     = ps->stats[STAT_TEAM];
        feed.scoresWanted = qtrue;
    }
    feed.wasPlaying = playing;
    if (!cg.scoresRequestTime) {
        feed.scoresWanted = qtrue;
    }
    if (cg.scoresRequestTime && cg.scoresRequestTime + SCORE_INTERVAL >= cg.time) {
        return;
    }
    if (!feed.scoresWanted) {
        // (the server's info read again once a second at most)
        if (cg.time < feed.floodTime || cg.time - feed.floodTime >= 1000) {
            flood          = Info_ValueForKey(CG_ConfigString(CS_SERVERINFO), "sv_floodProtect");
            feed.floodOn   = !flood[0] || atoi(flood);
            feed.floodTime = cg.time;
        }
        if (feed.floodOn) {
            return;
        }
    }
    feed.scoresWanted     = qfalse;
    cg.scoresRequestTime = cg.time;
    cgi.SendClientCommand("score");
}

void HudFeed_Frame(void)
{
    const char *roundStart;
    int         key = 0;
    int         i, t;

    if (!cg.snap || cgs.gametype < GT_TEAM) {
        return;
    }
    // the other team's score, in every team game
    HudFeed_RequestScores();
    if (cgs.gametype < GT_TEAM_ROUNDS || cgs.gametype > GT_OBJECTIVE) {
        return;
    }

    // a new round (the server sets the warmup configstring to its start
    // time): everyone alive again
    roundStart = CG_ConfigString(CS_WARMUP);
    if (strcmp(roundStart, feed.roundStart)) {
        Q_strncpyz(feed.roundStart, roundStart, sizeof(feed.roundStart));
        memset(feed.roster.dead, 0, sizeof(feed.roster.dead));
        feed.rosterKey++;
        HudFeed_WantScores();
    }

    // what the snapshot shows: the players in it, and the player watched
    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];

        if (es->eType != ET_PLAYER || es->number < 0 || es->number >= MAX_CLIENTS) {
            continue;
        }
        if (feed.roster.dead[es->number] != ((es->eFlags & EF_DEAD) != 0)) {
            feed.roster.dead[es->number] = (es->eFlags & EF_DEAD) != 0;
            feed.rosterKey++;
        }
    }
    if (!(cg.snap->ps.pm_flags & PMF_SPECTATING)) {
        const qboolean dead = cg.snap->ps.stats[STAT_HEALTH] <= 0;

        if (feed.roster.dead[cg.snap->ps.clientNum] != dead) {
            feed.roster.dead[cg.snap->ps.clientNum] = dead;
            feed.rosterKey++;
        }
    }

    // the cards from the clients' info (it changes with a team switch and a
    // disconnect at once): a player who left has no card, one who switched
    // teams waits for the next round dead
    for (i = 0; i < MAX_CLIENTS; i++) {
        const int team = cg.clientinfo[i].name[0] ? cg.clientinfo[i].team : 0;

        if (team != feed.team[i]) {
            if (feed.team[i] && team && (team == TEAM_ALLIES || team == TEAM_AXIS)) {
                feed.roster.dead[i] = qtrue;
            }
            feed.team[i] = team;
            feed.rosterKey++;
            // (a join: the scoreboard says whether they play this round)
            HudFeed_WantScores();
        }
    }

    key = feed.rosterKey;
    if (key == feed.roster.revision) {
        return;
    }
    feed.roster.revision = key;
    for (t = 0; t < 2; t++) {
        const int team = t ? TEAM_AXIS : TEAM_ALLIES;
        int       n    = 0;
        int       pass;

        feed.roster.alive[t] = 0;
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (feed.team[i] != team || feed.roster.dead[i] != (pass == 1)) {
                    continue;
                }
                feed.roster.players[t][n++] = i;
                if (!pass) {
                    feed.roster.alive[t]++;
                }
            }
        }
        feed.roster.count[t] = n;
    }
}

const hudRoster_t *HudFeed_Roster(void)
{
    return &feed.roster;
}

void CG_HudFeed_Reset(void)
{
    memset(&feed, 0, sizeof(feed));
    feed.roster.teamScore[0] = feed.roster.teamScore[1] = -1;
    feed.roster.revision                                = -1;
    feed.lastTeam                                       = TEAM_NONE;
}
