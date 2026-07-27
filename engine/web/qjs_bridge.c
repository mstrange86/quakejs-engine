/*
Copyright (C) 2026 quakejs contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

--------------------------------------------------------------------------

qjs_bridge.c -- the quakejs <-> JavaScript boundary. [quakejs patch P-1]

This is the ONLY place the browser is allowed to reach into the engine, and it
is deliberately tiny: four entry points, no gameplay logic, no policy. Anything
resembling a decision belongs in the TypeScript client or in QuakeC, not here --
every line added to this file is a line that has to be re-merged every time we
rebase onto upstream FTEQW.

Why a single JSON blob out of qjs_getstate() rather than one accessor per field:
  - FFI calls across the wasm boundary are not free, and the client polls state
    every frame for the HUD and the smoke tests.
  - More importantly it is ATOMIC. With per-field getters, a level change landing
    between two calls yields a torn read -- the old mapname next to the new client
    count -- and that bug is miserable to reproduce. One snapshot, one instant.

There is no security boundary here. Anything able to call qjs_cmd() is already
running in the page and could call it directly; RESTRICT_LOCAL is exactly the
privilege a user typing at the console has. The escaping discipline lives in
web/src/core/console.ts because it is about CORRECTNESS (a map name with a
semicolon in it must not become two commands), not about privilege.
*/

#include "quakedef.h"

#ifdef FTE_TARGET_WEB

#include <emscripten.h>

/* ======================================================================
   qjs_cmd -- queue a console command
   ====================================================================== */

/* Commands are queued, not executed inline: Cbuf_Execute runs at a defined
   point in the frame, so a command that changes level (or disconnects) cannot
   tear down state underneath a JS call that is still on the stack. */
EMSCRIPTEN_KEEPALIVE void qjs_cmd(const char *text)
{
	if (!text || !*text)
		return;
	Cbuf_AddText(text, RESTRICT_LOCAL);
	Cbuf_AddText("\n", RESTRICT_LOCAL);	/* all commands must be \n terminated */
}

/* ======================================================================
   qjs_getstate -- one atomic snapshot, as JSON
   ====================================================================== */

/* 16 slots * ~140 bytes of player JSON, plus the fixed header. */
static char qjs_statebuf[8192];

/* Append `src` to dest as a JSON string body, escaping what RFC 8259 requires.
   levelname comes from map data we do not control, so it gets escaped rather
   than trusted. Truncates rather than overflowing. */
static void QJS_AppendEscaped(char *dest, size_t destsize, const char *src)
{
	size_t len = strlen(dest);
	if (!src)
		src = "";
	while (*src && len + 7 < destsize)	/* 7 = worst-case \uXXXX plus terminator */
	{
		unsigned char c = (unsigned char)*src++;
		switch (c)
		{
		case '"':  dest[len++] = '\\'; dest[len++] = '"';  break;
		case '\\': dest[len++] = '\\'; dest[len++] = '\\'; break;
		case '\n': dest[len++] = '\\'; dest[len++] = 'n';  break;
		case '\r': dest[len++] = '\\'; dest[len++] = 'r';  break;
		case '\t': dest[len++] = '\\'; dest[len++] = 't';  break;
		default:
			if (c < 0x20 || c >= 0x7f)
			{
				/* Quake's charset is not UTF-8 -- it has its own high-bit
				   "coloured"/brown text and control glyphs. Emitting those raw
				   produces invalid UTF-8 and JSON.parse() throws, so escape
				   anything outside printable ASCII. */
				static const char hex[] = "0123456789abcdef";
				dest[len++] = '\\'; dest[len++] = 'u';
				dest[len++] = '0';  dest[len++] = '0';
				dest[len++] = hex[(c >> 4) & 0xf];
				dest[len++] = hex[c & 0xf];
			}
			else
				dest[len++] = c;
			break;
		}
	}
	dest[len] = 0;
}

/* Returns "e1m1" from a worldmodel name like "maps/e1m1.bsp". */
static const char *QJS_MapBaseName(void)
{
	static char base[MAX_QPATH];
	const char *name = NULL;
	const char *slash;
	char *dot;

	if (cl.worldmodel && cl.worldmodel->name[0])
		name = cl.worldmodel->name;
#ifndef CLIENTONLY
	/* Fall back to the server's own idea of the map. Covers the window during
	   level load where the client has no worldmodel yet. */
	else if (sv.state >= ss_active && svs.name[0])
		name = svs.name;
#endif

	if (!name)
		return "";

	slash = strrchr(name, '/');
	Q_strncpyz(base, slash ? slash + 1 : name, sizeof(base));
	dot = strrchr(base, '.');
	if (dot)
		*dot = 0;
	return base;
}

/* The returned pointer is a static buffer owned by the engine. JS must copy the
   string out (JSON.parse does) before the next call -- do not retain it. */
EMSCRIPTEN_KEEPALIVE const char *qjs_getstate(void)
{
	int clients = 0, maxclients = 0, svactive = 0;

#ifndef CLIENTONLY
	if (sv.state >= ss_active)
	{
		int i;
		svactive = 1;
		maxclients = sv.allocated_client_slots;
		for (i = 0; i < sv.allocated_client_slots; i++)
			if (svs.clients[i].state == cs_spawned)
				clients++;
	}
#endif

	Q_snprintfz(qjs_statebuf, sizeof(qjs_statebuf),
		"{\"state\":%i,\"mapname\":\"%s\",\"time\":%.3f,\"mtime\":%.3f,\"playernum\":%i,"
		"\"clients\":%i,\"maxclients\":%i,\"svactive\":%i,\"paused\":%i,"
		"\"levelname\":\"",
		(int)cls.state,
		QJS_MapBaseName(),
		cl.time,
		/* The server's own clock as of the LAST RECEIVED PACKET, so it changes
		   only when a server update actually arrives. cl.time above is
		   interpolated forward every client frame and keeps advancing smoothly
		   through a total network stall -- using it to measure network health
		   silently reports perfect health on a dead link.

		   This reads cl.gametime, NOT cl.mtime. Despite its comment ("server
		   time as on the server when we last received a packet") cl.mtime is a
		   QuakeWorld-era field that stays 0 on this path; cl.gametime is what
		   cl_parse.c/cl_ents.c actually assign per packet. */
		cl.gametime,
		/* Per-SEAT, not per-client: FTE supports splitscreen, so playernum lives
		   on playerview[] rather than on client_state_t. Seat 0 is the only one
		   quakejs uses -- the browser has one keyboard and one canvas. */
		cl.playerview[0].playernum,
		clients, maxclients, svactive,
		(int)cl.paused);

	QJS_AppendEscaped(qjs_statebuf, sizeof(qjs_statebuf), cl.levelname);
	Q_strncatz(qjs_statebuf, "\",\"players\":[", sizeof(qjs_statebuf));

#ifndef CLIENTONLY
	/*
	 * Per-player detail, needed to make bot assertions MECHANICAL rather than
	 * visual. "Three bots appeared" is checkable from a count, but "the bots are
	 * actually playing" is not -- a bot that spawned and then stood still at its
	 * spawn point (the exact failure when a map's waypoint graph never loaded)
	 * satisfies a count check perfectly. Origin and frags let the smoke test
	 * assert that positions CHANGE and frags INCREMENT.
	 *
	 * WHY THIS ITERATES EDICTS AND NOT svs.clients:
	 * FrikBot X bots are NOT network clients. BotConnect() calls
	 * GetClientEntity(n) and drives a reserved player-slot edict directly,
	 * calling ClientConnect()/PutClientInServer() from QuakeC -- the engine never
	 * sees a connection, so svs.clients[n].state stays cs_free. This is the
	 * classic NetQuake bot technique and it is why `clients` below counts 1 in a
	 * game with three bots happily fragging each other.
	 *
	 * Player slots are edicts 1..maxclients, permanently reserved whether or not
	 * anyone occupies them, so enumerating those edicts sees real players and
	 * QuakeC bots alike. `isbot` is reported rather than inferred: it is exactly
	 * the distinction that a count of network clients cannot express.
	 */
	if (sv.state >= ss_active && svprogfuncs)
	{
		int i, first = 1;
		for (i = 0; i < sv.allocated_client_slots; i++)
		{
			edict_t *ent = EDICT_NUM_PB(svprogfuncs, i + 1);
			const char *name;
			char entry[320];

			if (!ent || ent->ereftype != ER_ENTITY)
				continue;

			name = PR_GetString(svprogfuncs, ent->v->netname);
			if (!name || !*name)
				continue;	/* empty slot */

			Q_snprintfz(entry, sizeof(entry),
				"%s{\"slot\":%i,\"frags\":%i,\"health\":%i,\"isbot\":%s,"
				"\"origin\":[%.1f,%.1f,%.1f],\"name\":\"",
				first ? "" : ",", i,
				(int)ent->v->frags,
				(int)ent->v->health,
				/* real JSON booleans, so the TypeScript type is honest rather
				   than describing 0/1 as boolean */
				svs.clients[i].state == cs_spawned ? "false" : "true",
				ent->v->origin[0], ent->v->origin[1], ent->v->origin[2]);
			Q_strncatz(qjs_statebuf, entry, sizeof(qjs_statebuf));
			/* Names are gamecode-supplied and use Quake's non-UTF-8 charset, so
			   they go through the escaper like levelname does. */
			QJS_AppendEscaped(qjs_statebuf, sizeof(qjs_statebuf), name);
			Q_strncatz(qjs_statebuf, "\"}", sizeof(qjs_statebuf));
			first = 0;
		}
	}
#endif

	Q_strncatz(qjs_statebuf, "]}", sizeof(qjs_statebuf));

	return qjs_statebuf;
}

/* ======================================================================
   qjs_getdump -- P4 conformance ring buffer
   ====================================================================== */

/*
The native oracle dumps per-frame state to a file with -dumpstate. The web build
CANNOT: it is linked -sNO_FILESYSTEM=1 (FTE supplies its own VFS), so there is no
fopen to write to and no way for a node harness to collect it.

So the web side accumulates into a ring buffer that JS drains. That inverts the
driver: P4's conformance run is Chrome-driven, not node-driven. The ring is sized
to survive a slow drain without stalling the frame -- if it overflows we drop the
OLDEST record and set a flag, because silently returning a short dump would make
a conformance run look like it passed on fewer frames than it actually ran.

The PRODUCER (per-frame capture) is deliberately not here yet -- it lands with P4,
where the exact field set and quantisation get pinned against the golden files.
What is fixed now is the ABI and the drain semantics, so the client side can be
written against it and does not need retrofitting later.
*/

#define QJS_DUMP_SIZE (256 * 1024)
static char   qjs_dumpbuf[QJS_DUMP_SIZE];
static size_t qjs_dumplen = 0;
static int    qjs_dumpoverflowed = 0;

/* Called by the P4 capture hook. Records are newline-terminated JSON. */
void QJS_DumpAppend(const char *record)
{
	size_t rlen;
	if (!record)
		return;
	rlen = strlen(record);
	if (rlen + 1 > QJS_DUMP_SIZE)
		return;						/* single record larger than the ring: unrecoverable, drop */

	/* Drop oldest whole records until the new one fits. */
	while (qjs_dumplen + rlen + 1 > QJS_DUMP_SIZE)
	{
		char *nl = memchr(qjs_dumpbuf, '\n', qjs_dumplen);
		size_t drop = nl ? (size_t)(nl - qjs_dumpbuf) + 1 : qjs_dumplen;
		memmove(qjs_dumpbuf, qjs_dumpbuf + drop, qjs_dumplen - drop);
		qjs_dumplen -= drop;
		qjs_dumpoverflowed = 1;
	}

	memcpy(qjs_dumpbuf + qjs_dumplen, record, rlen);
	qjs_dumplen += rlen;
	qjs_dumpbuf[qjs_dumplen++] = '\n';
}

/* Drains the ring: returns everything buffered and empties it. An overflow since
   the last drain is reported as a leading "!overflow\n" line -- callers must treat
   that as a FAILED run, not as a warning. */
EMSCRIPTEN_KEEPALIVE const char *qjs_getdump(void)
{
	static char *out = NULL;
	static size_t outsize = 0;
	const char *prefix = qjs_dumpoverflowed ? "!overflow\n" : "";
	size_t plen = strlen(prefix);
	size_t need = plen + qjs_dumplen + 1;

	if (need > outsize)
	{
		char *n = BZ_Realloc(out, need);
		if (!n)
			return "";
		out = n;
		outsize = need;
	}

	memcpy(out, prefix, plen);
	memcpy(out + plen, qjs_dumpbuf, qjs_dumplen);
	out[plen + qjs_dumplen] = 0;

	qjs_dumplen = 0;
	qjs_dumpoverflowed = 0;
	return out;
}

/* ======================================================================
   qjs_move -- touch/gamepad movement injection [wired up in P7]
   ====================================================================== */

/*
iOS has no Pointer Lock at any version, so the touch UI cannot deliver mouse
deltas the way the desktop client does; it has to hand the engine an intended
movement directly. Declared now so the ABI is fixed before ui-mobile/ exists.

Values are engine units per second (forward/side/up) and DEGREES of delta
(dyaw/dpitch) -- not absolute angles, so a dropped frame degrades into a smaller
turn rather than a snap.
*/

typedef struct
{
	float forward, side, up;
	float dyaw, dpitch;
	int   buttons;
	int   impulse;
	int   active;		/* set by JS, cleared once the input path consumes it */
} qjs_moveinput_t;

qjs_moveinput_t qjs_moveinput;

EMSCRIPTEN_KEEPALIVE void qjs_move(float f, float s, float u,
                                   float dyaw, float dpitch,
                                   int buttons, int impulse)
{
	qjs_moveinput.forward = f;
	qjs_moveinput.side    = s;
	qjs_moveinput.up      = u;
	/* Accumulate rather than overwrite: JS may call faster than the engine
	   consumes, and dropping intermediate deltas would lose real rotation. */
	qjs_moveinput.dyaw   += dyaw;
	qjs_moveinput.dpitch += dpitch;
	qjs_moveinput.buttons = buttons;
	if (impulse)
		qjs_moveinput.impulse = impulse;
	qjs_moveinput.active = 1;
}

#endif	/* FTE_TARGET_WEB */
