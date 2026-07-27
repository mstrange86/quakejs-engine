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

--------------------------------------------------------------------------

qjs_dump.c -- per-frame state capture for the fidelity gate. [quakejs patch P-6]

Emits one JSON record per client frame so a native "oracle" build and the wasm
build can be diffed frame-by-frame while replaying the same demo under the same
faithful.cfg. A divergence means OUR build changed behaviour; that is the entire
point of the exercise.

THE SINK DIFFERS BY TARGET, THE FIELDS MUST NOT
-----------------------------------------------
Native writes to a file. The web build CANNOT: it is linked -sNO_FILESYSTEM=1
(FTE supplies its own VFS), so there is no fopen and no way for a node harness
to collect anything. The web build therefore appends into the ring buffer in
web/qjs_bridge.c, which JavaScript drains via qjs_getdump(). That inverts the
driver -- the conformance run is Chrome-driven, not node-driven -- but both
sides must produce byte-identical records for the same state or the comparison
is meaningless. Hence one shared capture function, two sinks.

EXACT-MATCH vs TOLERANCE (PLAN.md §9)
-------------------------------------
Fields are split deliberately, because demanding bit-exact floats across
arm64-native and wasm is not achievable and chasing it wastes the gate:
  - arm64 contracts a*b+c into FMA; wasm32 does not.
  - Apple libm and musl differ in the last ulp of sinf/cosf.

  EXACT   f (frame), ents (entity count), eh (quantized entity hash), st (stats)
  TOLER   t (cl.time), org (view origin), ang (view angles)

The entity hash is over QUANTIZED values -- origins at 1/8 unit, angles at
360/256 -- which is the precision Quake's network protocol itself uses. Two
builds that agree on the wire must agree here; float noise below the wire
precision cannot cause a false divergence.
*/

#include "quakedef.h"

#ifndef SERVERONLY

cvar_t qjs_dumpstate = CVARD("qjs_dumpstate", "0",
	"Emit one per-frame JSON state record for the quakejs fidelity gate. "
	"Native writes to qjs_dumpfile; the web build appends to a ring buffer "
	"drained by qjs_getdump().");
cvar_t qjs_dumpfile = CVARD("qjs_dumpfile", "qjs-dump.jsonl",
	"Native only: path the state dump is written to.");

#ifdef FTE_TARGET_WEB
	/* Provided by web/qjs_bridge.c. */
	void QJS_DumpAppend(const char *record);
#else
	#include <stdio.h>
	static FILE *qjs_dumpfh;
	static char  qjs_dumpfh_name[MAX_OSPATH];
#endif

/* FNV-1a. Chosen because it is trivial to reimplement identically in the
   comparison tooling -- a hash whose value depends on a library version would
   defeat the purpose. */
static unsigned int QJS_HashBytes(unsigned int h, const void *data, size_t len)
{
	const unsigned char *p = data;
	while (len--)
	{
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

static unsigned int QJS_HashInt(unsigned int h, int v)
{
	return QJS_HashBytes(h, &v, sizeof(v));
}

/* Quantize to the protocol's own precision so sub-wire float noise cannot
   register as a divergence. */
static int QJS_QuantCoord(float v)  { return (int)floor(v * 8.0f + 0.5f); }   /* 1/8 unit */
static int QJS_QuantAngle(float v)
{
	int a = (int)floor(v * (256.0f / 360.0f) + 0.5f) & 255;                   /* 1 byte */
	return a;
}

void QJS_DumpFrame(void)
{
	char record[1024];
	unsigned int h = 2166136261u;   /* FNV offset basis */
	int nents = 0;
	playerview_t *pv;
	packet_entities_t *pe;

	if (!qjs_dumpstate.ival)
		return;
	if (cls.state < ca_active)
		return;   /* nothing meaningful to compare before the level is live */

	pv = &cl.playerview[0];

	pe = cl.currentpackentities;
	if (pe)
	{
		int i;
		nents = pe->num_entities;
		for (i = 0; i < pe->num_entities; i++)
		{
			entity_state_t *e = &pe->entities[i];
			h = QJS_HashInt(h, (int)e->number);
			h = QJS_HashInt(h, (int)e->modelindex);
			h = QJS_HashInt(h, (int)e->frame);
			h = QJS_HashInt(h, (int)e->skinnum);
			h = QJS_HashInt(h, (int)e->effects);
			h = QJS_HashInt(h, QJS_QuantCoord(e->origin[0]));
			h = QJS_HashInt(h, QJS_QuantCoord(e->origin[1]));
			h = QJS_HashInt(h, QJS_QuantCoord(e->origin[2]));
			h = QJS_HashInt(h, QJS_QuantAngle(e->angles[0]));
			h = QJS_HashInt(h, QJS_QuantAngle(e->angles[1]));
			h = QJS_HashInt(h, QJS_QuantAngle(e->angles[2]));
		}
	}

	Q_snprintfz(record, sizeof(record),
		"{\"f\":%i,\"t\":%.4f,\"ft\":%.6f,\"org\":[%.3f,%.3f,%.3f],\"ang\":[%.3f,%.3f,%.3f],"
		"\"ents\":%i,\"eh\":\"%08x\",\"st\":[%i,%i,%i,%i,%i,%i,%i]}",
		host_framecount,
		cl.time,
		/* WALL-CLOCK frame duration, which is NOT cl.time and is the reason this
		   field exists. Quake's step-view smoothing integrates against
		   host_frametime (cl_pred.c: oldz += host_frametime * crouchspeed), so a
		   build running a timedemo unthrottled at 1200fps smooths ~20x slower
		   per frame than one paced to 60fps by requestAnimationFrame -- while
		   cl.time, being demo time, matches exactly. Diffing cl.time alone makes
		   that look like a behavioural divergence. It is not. */
		host_frametime,
		r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2],
		pv->viewangles[0], pv->viewangles[1], pv->viewangles[2],
		nents, h,
		pv->stats[STAT_HEALTH],
		pv->stats[STAT_ARMOR],
		pv->stats[STAT_SHELLS],
		pv->stats[STAT_NAILS],
		pv->stats[STAT_ROCKETS],
		pv->stats[STAT_CELLS],
		pv->stats[STAT_ACTIVEWEAPON]);

#ifdef FTE_TARGET_WEB
	QJS_DumpAppend(record);
#else
	if (!qjs_dumpfh || strcmp(qjs_dumpfh_name, qjs_dumpfile.string))
	{
		if (qjs_dumpfh)
			fclose(qjs_dumpfh);
		Q_strncpyz(qjs_dumpfh_name, qjs_dumpfile.string, sizeof(qjs_dumpfh_name));
		qjs_dumpfh = fopen(qjs_dumpfh_name, "wb");
		if (!qjs_dumpfh)
		{
			Con_Printf("qjs_dump: cannot open %s for writing\n", qjs_dumpfh_name);
			Cvar_SetValue(&qjs_dumpstate, 0);
			return;
		}
	}
	fputs(record, qjs_dumpfh);
	fputc('\n', qjs_dumpfh);
	/* Flushed every frame on purpose: a conformance run that crashes must still
	   leave every frame it completed, so the harness can report the LAST good
	   frame rather than losing the buffered tail that contains the divergence. */
	fflush(qjs_dumpfh);
#endif
}

void QJS_Dump_Init(void)
{
	Cvar_Register(&qjs_dumpstate, "quakejs");
	Cvar_Register(&qjs_dumpfile, "quakejs");
}

#endif	/* !SERVERONLY */
