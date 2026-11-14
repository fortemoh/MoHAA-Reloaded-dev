/*
===========================================================================
Copyright (C) 2026 Project: Omaha

This file is part of Project: Omaha source code.

Project: Omaha builds upon OpenMoHAA / ioquake3 / F.A.K.K. foundations.
Project: Omaha source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Project: Omaha source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Project: Omaha source code; if not, see COPYING.txt in the
source tree, or write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// DESCRIPTION:
// The Competitive HUD's radar's picture of a map (cg_hud_radar.cpp), without
// the engine. From Project: Omaha's cl_hud_radar_map.h (changed: a byte a
// texel, the alpha the atlas keeps): the floors a player can reach, found by
// walking out from the spawn points over a grid (a flood fill of floor
// traces, a few at a time), then drawn top-down into a texture: the floors
// filled, brighter the higher they are, and their walls and ledges drawn as
// lines: the player's storey's bright, the other storeys' faint. A picture
// is begun from the map and its rows are then drawn from what that put in
// it alone, on another thread where there is one for it (PictureWorker).

#pragma once

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace radarmap
{

// what the map is asked
struct World {
	// the first floor a player can stand on straight down from zTop, no lower
	// than zBottom; false when there is none, it is too steep, or zTop is
	// inside something solid
	bool (*floorBelow)(void *ctx, float x, float y, float zTop, float zBottom, float *z);
	// whether a crouched player has room standing on a floor at z
	bool (*fits)(void *ctx, float x, float y, float z);
	void *ctx;
};

static const float CELL_MIN = 16.0f;    // the grid's cells, units
static const int   MAX_DIM = 2048;      // cells a side; a larger map gets larger cells
// a player gets onto what is this much higher: a jump is 56 units (the state
// files' "jump 56"), and at its top a step (STEPSIZE, 18) is still taken
static const float STEP_UP = 72.0f;
static const float DROP_MAX = 4096.0f;  // and down from any height
static const float LAYER_MERGE = 24.0f; // floors of a column closer than this are one
static const float WALK_DZ = 40.0f;     // neighbours farther apart in height have a ledge between them
static const int   PAD = 3;             // empty cells around the picture
static const float TEXELS_PER_CELL_MAX = 6.0f;
// the player's storey: floors within FOCUS_NEAR of the height they stand at
// in full, fading to the other storeys' look at FOCUS_FAR
static const float FOCUS_NEAR = 64.0f;
static const float FOCUS_FAR = 120.0f;
// stairs and ramps: floors that rise this much a cell or more are striped
// across the way up, a stripe every STAIR_STRIPE cells
static const float STAIR_RISE = 7.0f;
static const int   STAIR_STRIPE = 2;

struct Node {
	float          z;
	int            next; // the column's next floor, -1 none
	unsigned short cx, cy;
	unsigned char  open; // bit per side: a neighbour floor to walk to
};

// the cells a picture is of (the map's cells and PAD around them) and its
// texels: w x h of size x size are used, tpc of them a cell
struct PictureFrame {
	int   size = 0, w = 0, h = 0;
	int   cx0 = 0, cy0 = 0, cw = 0, ch = 0;
	float tpc = 1.0f;
};

//
// a picture of the map: size x size texels of a byte, the alpha of white
// (the drawing tints it), top row first, a texel unitsPerTexel wide, for a
// player standing at the height focusZ. Texel i j's center is at
// x0 + (i + 0.5) * unitsPerTexel (and y likewise); w x h texels of it are
// used, the others are 0
//
struct Picture : PictureFrame {
	std::vector<unsigned char> pixels;
	float                      unitsPerTexel = 0.0f, x0 = 0.0f, y0 = 0.0f, focusZ = 0.0f;
	unsigned                   revision = 0; // the map's Revision() it was begun at
	int                        row = -1;     // the next row to draw, -1: not drawing
	bool                       done = false; // every row is drawn

	bool Busy() const { return row >= 0; }

	// what its rows are drawn from, per cell: whether it has a floor, its
	// marked borders and stair stripes, how much of the player's storey its
	// floors are and the height of the one nearest to it
	std::vector<unsigned char> cov, edgeX, edgeY, stairX, stairY;
	std::vector<float>         nearness, height;
	float                      zMin = 0.0f, zMax = 0.0f;
	// the texels that may not be 0: this picture's and, until its rows are
	// drawn, those of the one drawn into pixels before it (a row first
	// clears clearW of its texels)
	int setW = 0, setH = 0, clearW = 0;
};

class Map
{
public:
	void Reset(float minX, float minY, float maxX, float maxY)
	{
		const float ex = maxX - minX;
		const float ey = maxY - minY;
		const float e = ex > ey ? ex : ey;

		Clear();
		if (!(ex > 0.0f) || !(ey > 0.0f)) {
			return;
		}
		m_cell = CELL_MIN;
		if (e / m_cell > (float)MAX_DIM) {
			m_cell = e / (float)MAX_DIM;
		}
		m_minX = minX;
		m_minY = minY;
		m_dimX = (int)std::ceil(ex / m_cell);
		m_dimY = (int)std::ceil(ey / m_cell);
		if (m_dimX < 1) {
			m_dimX = 1;
		}
		if (m_dimY < 1) {
			m_dimY = 1;
		}
		m_head.assign((size_t)m_dimX * (size_t)m_dimY, -1);
	}

	void Clear()
	{
		m_dimX = m_dimY = 0;
		m_head.clear();
		m_nodes.clear();
		m_queue.clear();
		m_queueAt = 0;
		m_revision++;
	}

	bool   Ready() const { return m_dimX > 0; }
	bool   Pending() const { return m_queueAt < m_queue.size(); }
	size_t NodeCount() const { return m_nodes.size(); }
	// changes whenever a floor is added
	unsigned Revision() const { return m_revision; }
	float    CellSize() const { return m_cell; }
	// how far the floors reach across, units
	float ExtentX() const { return m_nodes.empty() ? 0.0f : (float)(m_maxCx - m_minCx + 1) * m_cell; }
	float ExtentY() const { return m_nodes.empty() ? 0.0f : (float)(m_maxCy - m_minCy + 1) * m_cell; }

	// whether the floor under a player standing at x y z is on the map
	bool Known(float x, float y, float z) const
	{
		int cx, cy;

		if (!CellOf(x, y, &cx, &cy)) {
			return false;
		}
		return FindNode(cx, cy, z, 64.0f) >= 0;
	}

	// starts the walk where a player stands (a spawn point, a player seen
	// there); true when that floor is on the map now
	bool Seed(const World &w, float x, float y, float z)
	{
		static const int order[9][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
		int              cx, cy, i;

		if (!CellOf(x, y, &cx, &cy)) {
			return false;
		}
		// the cell's center may be in a wall the player stands against
		for (i = 0; i < 9; i++) {
			const int nx = cx + order[i][0];
			const int ny = cy + order[i][1];
			float     fz;

			if (nx < 0 || ny < 0 || nx >= m_dimX || ny >= m_dimY) {
				continue;
			}
			if (!w.floorBelow(w.ctx, CenterX(nx), CenterY(ny), z + STEP_UP, z - 96.0f, &fz)) {
				continue;
			}
			if (FindNode(nx, ny, fz, LAYER_MERGE) >= 0) {
				return true;
			}
			if (w.fits(w.ctx, CenterX(nx), CenterY(ny), fz)) {
				AddNode(nx, ny, fz);
				return true;
			}
		}
		return false;
	}

	// walks on from up to maxNodes floors; returns how many
	int Step(const World &w, int maxNodes)
	{
		static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		int              done = 0;

		while (done < maxNodes && m_queueAt < m_queue.size()) {
			const int   index = m_queue[m_queueAt++];
			const int   cx = m_nodes[(size_t)index].cx;
			const int   cy = m_nodes[(size_t)index].cy;
			const float z = m_nodes[(size_t)index].z;
			int         d;

			for (d = 0; d < 4; d++) {
				const int nx = cx + dirs[d][0];
				const int ny = cy + dirs[d][1];
				float     fz;
				int       other;

				if (nx < 0 || ny < 0 || nx >= m_dimX || ny >= m_dimY) {
					continue;
				}
				if (!w.floorBelow(w.ctx, CenterX(nx), CenterY(ny), z + STEP_UP, z - DROP_MAX, &fz)) {
					continue;
				}
				other = FindNode(nx, ny, fz, LAYER_MERGE);
				if (other < 0) {
					if (!w.fits(w.ctx, CenterX(nx), CenterY(ny), fz)) {
						continue;
					}
					other = AddNode(nx, ny, fz);
				}
				if (std::fabs(m_nodes[(size_t)other].z - z) <= WALK_DZ) {
					// both ways: the other floor may have been walked from
					// before this one was there (a bridge seen later)
					m_nodes[(size_t)index].open |= (unsigned char)(1 << d);
					m_nodes[(size_t)other].open |= (unsigned char)(1 << (d ^ 1));
				}
			}
			done++;
		}
		if (m_queueAt == m_queue.size() && !m_queue.empty()) {
			m_queue.clear();
			m_queueAt = 0;
		}
		return done;
	}

	// the floor a player at x y z stands on or is in the air over: the
	// highest of the column that is no more than above over z and no more
	// than below under it
	bool FloorUnder(float x, float y, float z, float above, float below, float *floorZ) const
	{
		int  cx, cy, i;
		bool found = false;

		if (!CellOf(x, y, &cx, &cy)) {
			return false;
		}
		for (i = m_head[(size_t)cy * (size_t)m_dimX + (size_t)cx]; i >= 0; i = m_nodes[(size_t)i].next) {
			const float fz = m_nodes[(size_t)i].z;

			if (fz <= z + above && fz >= z - below && (!found || fz > *floorZ)) {
				*floorZ = fz;
				found = true;
			}
		}
		return found;
	}

	//
	// begins a picture in p of size x size texels, a texel about
	// unitsPerTexel wide (more when the map wouldn't fit), for a player
	// standing at the height focusZ: everything its rows need of the map is
	// put in p, so the map may change while PictureRows draws them.
	//
	// With halfExtent > 0 it is only of the square that far each way from
	// centerX centerY: a large map keeps its detail there
	//
	bool BeginPicture(
		Picture &p, int size, float unitsPerTexel, float focusZ, float centerX = 0.0f, float centerY = 0.0f,
		float halfExtent = 0.0f
	) const
	{
		static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		const int        oldW = p.setW;
		const int        oldH = p.setH;
		PictureFrame     f;
		size_t           cells;
		int              ix0, iy0, ix1, iy1, cx, cy, i, d;

		p.row = -1;
		p.done = false;
		if (!Frame(size, unitsPerTexel, centerX, centerY, halfExtent, &f)) {
			return false;
		}
		static_cast<PictureFrame &>(p) = f;
		p.unitsPerTexel = m_cell / p.tpc;
		p.x0 = m_minX + (float)p.cx0 * m_cell;
		p.y0 = m_minY + (float)p.cy0 * m_cell;
		p.focusZ = focusZ;
		p.revision = m_revision;
		p.zMin = m_zMin;
		p.zMax = m_zMax;

		cells = (size_t)p.cw * (size_t)p.ch;
		p.cov.assign(cells, 0);
		p.edgeX.assign(cells, 0);
		p.edgeY.assign(cells, 0);
		p.stairX.assign(cells, 0);
		p.stairY.assign(cells, 0);
		p.nearness.assign(cells, 0.0f);
		p.height.assign(cells, 0.0f);

		// the floors in the picture's rectangle, a cell in from its edge (their
		// neighbours are in it too), column by column: only those are gone
		// through, however large the map
		ix0 = p.cx0 + 1 > 0 ? p.cx0 + 1 : 0;
		iy0 = p.cy0 + 1 > 0 ? p.cy0 + 1 : 0;
		ix1 = p.cx0 + p.cw - 2 < m_dimX - 1 ? p.cx0 + p.cw - 2 : m_dimX - 1;
		iy1 = p.cy0 + p.ch - 2 < m_dimY - 1 ? p.cy0 + p.ch - 2 : m_dimY - 1;

		// how much of the player's storey a column's floors are, and the
		// height of the one nearest to the player's
		for (cy = iy0; cy <= iy1; cy++) {
			for (cx = ix0; cx <= ix1; cx++) {
				const size_t c = PicCell(p, cx, cy);

				for (i = m_head[(size_t)cy * (size_t)m_dimX + (size_t)cx]; i >= 0; i = m_nodes[(size_t)i].next) {
					const Node &n = m_nodes[(size_t)i];
					const float focus = FocusWeight(n.z, focusZ);

					if (focus > p.nearness[c]) {
						p.nearness[c] = focus;
					}
					// (a column's floors come newest first: of two as near, the older)
					if (!p.cov[c] || std::fabs(n.z - focusZ) <= std::fabs(p.height[c] - focusZ)) {
						p.height[c] = n.z;
					}
					p.cov[c] = 1;
				}
			}
		}
		for (cy = iy0; cy <= iy1; cy++) {
			for (cx = ix0; cx <= ix1; cx++) {
				for (i = m_head[(size_t)cy * (size_t)m_dimX + (size_t)cx]; i >= 0; i = m_nodes[(size_t)i].next) {
					const Node         &n = m_nodes[(size_t)i];
					const unsigned char mark = (unsigned char)(1.5f + FocusWeight(n.z, focusZ) * 254.0f);
					float               riseX, riseY;

					// a side with no floor to walk to, where another floor of the
					// map is: a wall or a ledge inside the outline, a line on the
					// cells' border (edgeX: a cell's border with the next in x,
					// edgeY: in y; 1 for another storey's, up to 255 for the
					// player's)
					for (d = 0; d < 4; d++) {
						const int      nx = cx + dirs[d][0];
						const int      ny = cy + dirs[d][1];
						unsigned char *edge;

						if ((n.open & (1 << d)) || !p.cov[PicCell(p, nx, ny)]) {
							continue;
						}
						if (d < 2) {
							edge = &p.edgeX[PicCell(p, d == 0 ? cx : nx, cy)];
						} else {
							edge = &p.edgeY[PicCell(p, cx, d == 2 ? cy : ny)];
						}
						if (mark > *edge) {
							*edge = mark;
						}
					}
					// stairs and ramps: stripes on the cells' borders, across the way up
					riseX = Rise(n, 0, 1);
					riseY = Rise(n, 2, 3);
					if (riseX >= STAIR_RISE || riseY >= STAIR_RISE) {
						unsigned char *stair = NULL;

						if (riseX >= riseY) {
							if (!(cx % STAIR_STRIPE)) {
								stair = &p.stairX[PicCell(p, cx, cy)];
							}
						} else if (!(cy % STAIR_STRIPE)) {
							stair = &p.stairY[PicCell(p, cx, cy)];
						}
						if (stair && mark > *stair) {
							*stair = mark;
						}
					}
				}
			}
		}

		// the rows clear what the picture before this one set in them; the
		// rows this one doesn't have are cleared here
		if (p.pixels.size() != (size_t)size * (size_t)size) {
			p.pixels.assign((size_t)size * (size_t)size, 0);
			p.clearW = 0;
			p.setW = p.w;
			p.setH = p.h;
		} else {
			for (i = p.h; i < oldH; i++) {
				std::memset(&p.pixels[(size_t)i * (size_t)size], 0, (size_t)oldW);
			}
			p.clearW = oldW;
			p.setW = oldW > p.w ? oldW : p.w;
			p.setH = oldH > p.h ? oldH : p.h;
		}
		p.row = 0;
		return true;
	}

	// whether a picture begun now with these would be p over again
	bool PictureIs(
		const Picture &p, int size, float unitsPerTexel, float focusZ, float centerX = 0.0f, float centerY = 0.0f,
		float halfExtent = 0.0f
	) const
	{
		PictureFrame f;

		return p.done && p.revision == m_revision && p.focusZ == focusZ
			&& Frame(size, unitsPerTexel, centerX, centerY, halfExtent, &f) && f.size == p.size && f.w == p.w && f.h == p.h
			&& f.cx0 == p.cx0 && f.cy0 == p.cy0 && f.cw == p.cw && f.ch == p.ch && f.tpc == p.tpc;
	}

	// draws up to rows more rows of a picture; true when it is done. It
	// needs nothing of the map
	static bool PictureRows(Picture &p, int rows)
	{
		const float lineW = 1.5f; // texels
		const float zRange = p.zMax - p.zMin;

		if (p.row < 0) {
			return false;
		}
		for (; rows > 0 && p.row < p.h; rows--, p.row++) {
			const float    cellV = ((float)p.row + 0.5f) / p.tpc;
			const int      cj = Clampi((int)cellV, 1, p.ch - 2);
			const float    v = cellV - 0.5f;
			const int      j0 = Clampi((int)std::floor(v), 0, p.ch - 2);
			const float    fy = Clampf(v - (float)j0, 0.0f, 1.0f);
			unsigned char *out = &p.pixels[(size_t)p.row * (size_t)p.size];
			int            i;

			if (p.clearW > 0) {
				std::memset(out, 0, (size_t)p.clearW);
			}
			for (i = 0; i < p.w; i++, out++) {
				const float  cellU = ((float)i + 0.5f) / p.tpc;
				const int    ci = Clampi((int)cellU, 1, p.cw - 2);
				const size_t cc = (size_t)cj * (size_t)p.cw + (size_t)ci;
				const float  u = cellU - 0.5f;
				const int    i0 = Clampi((int)std::floor(u), 0, p.cw - 2);
				const float  fx = Clampf(u - (float)i0, 0.0f, 1.0f);
				const size_t c = (size_t)j0 * (size_t)p.cw + (size_t)i0;
				const float  w00 = (1.0f - fx) * (1.0f - fy);
				const float  w10 = fx * (1.0f - fy);
				const float  w01 = (1.0f - fx) * fy;
				const float  w11 = fx * fy;
				const float  cov = w00 * p.cov[c] + w10 * p.cov[c + 1] + w01 * p.cov[c + p.cw] + w11 * p.cov[c + p.cw + 1];
				float        d, inside, line, edge, stair, focus, height, a;
				int          mark = 1, stairMark = 1;
				int          byte;

				if (cov <= 0.0f) {
					continue;
				}
				// about the distance to the outline, in texels
				d = (cov - 0.5f) * p.tpc;
				inside = Clampf(d + 0.5f, 0.0f, 1.0f);
				if (inside <= 0.0f) {
					continue;
				}
				// (the distance is only known up to half a cell: none further in)
				line = Clampf(lineW - d + 0.5f, 0.0f, 1.0f) * Clampf((1.0f - cov) * 4.0f, 0.0f, 1.0f);
				// the nearest marked border of the texel's cell, in texels
				edge = 1e9f;
				if (p.edgeX[cc] && (float)(ci + 1) - cellU < edge) {
					edge = (float)(ci + 1) - cellU;
					mark = p.edgeX[cc];
				}
				if (p.edgeX[cc - 1] && cellU - (float)ci < edge) {
					edge = cellU - (float)ci;
					mark = p.edgeX[cc - 1];
				}
				if (p.edgeY[cc] && (float)(cj + 1) - cellV < edge) {
					edge = (float)(cj + 1) - cellV;
					mark = p.edgeY[cc];
				}
				if (p.edgeY[cc - (size_t)p.cw] && cellV - (float)cj < edge) {
					edge = cellV - (float)cj;
					mark = p.edgeY[cc - (size_t)p.cw];
				}
				edge = Clampf(lineW * 0.5f - edge * p.tpc + 0.5f, 0.0f, 1.0f);
				// and the nearest stripe of a stair
				stair = 1e9f;
				if (p.stairX[cc] && (float)(ci + 1) - cellU < stair) {
					stair = (float)(ci + 1) - cellU;
					stairMark = p.stairX[cc];
				}
				if (p.stairX[cc - 1] && cellU - (float)ci < stair) {
					stair = cellU - (float)ci;
					stairMark = p.stairX[cc - 1];
				}
				if (p.stairY[cc] && (float)(cj + 1) - cellV < stair) {
					stair = (float)(cj + 1) - cellV;
					stairMark = p.stairY[cc];
				}
				if (p.stairY[cc - (size_t)p.cw] && cellV - (float)cj < stair) {
					stair = cellV - (float)cj;
					stairMark = p.stairY[cc - (size_t)p.cw];
				}
				stair = Clampf(0.5f - stair * p.tpc + 0.5f, 0.0f, 1.0f);
				focus = (w00 * p.nearness[c] + w10 * p.nearness[c + 1] + w01 * p.nearness[c + p.cw] + w11 * p.nearness[c + p.cw + 1])
					/ cov;

				height = (w00 * p.cov[c] * p.height[c] + w10 * p.cov[c + 1] * p.height[c + 1]
					+ w01 * p.cov[c + p.cw] * p.height[c + p.cw] + w11 * p.cov[c + p.cw + 1] * p.height[c + p.cw + 1]) / cov;
				height = zRange > 64.0f ? Clampf((height - p.zMin) / zRange, 0.0f, 1.0f) : 0.5f;

				// the floor by its height; the player's storey's walls, stairs
				// and outline over the others'
				a = 0.2f + 0.24f * height;
				a += (0.22f + 0.4f * (float)(stairMark - 1) / 254.0f - a) * stair;
				a += (0.2f + 0.7f * (float)(mark - 1) / 254.0f - a) * edge;
				a += (0.3f + 0.6f * focus - a) * line;
				a *= inside;
				byte = (int)(a * 255.0f + 0.5f);
				*out = (unsigned char)byte;
			}
		}
		if (p.row >= p.h) {
			p.row = -1;
			p.done = true;
			p.setW = p.w;
			p.setH = p.h;
			p.clearW = 0;
			return true;
		}
		return false;
	}

private:
	static int   Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
	static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

	// how much a floor rises a cell along one axis (sides a and b of it),
	// from the neighbour floors it walks to
	float Rise(const Node &n, int a, int b) const
	{
		static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		const int        ia = (n.open & (1 << a)) ? FindNode(n.cx + dirs[a][0], n.cy + dirs[a][1], n.z, WALK_DZ) : -1;
		const int        ib = (n.open & (1 << b)) ? FindNode(n.cx + dirs[b][0], n.cy + dirs[b][1], n.z, WALK_DZ) : -1;

		if (ia >= 0 && ib >= 0) {
			// both the same way: a step up to a landing isn't a stair
			const float up = m_nodes[(size_t)ia].z - n.z;
			const float down = n.z - m_nodes[(size_t)ib].z;

			return up * down > 0.0f ? std::fabs(up + down) * 0.5f : 0.0f;
		}
		return 0.0f;
	}

	static float FocusWeight(float z, float focusZ)
	{
		return Clampf((FOCUS_FAR - std::fabs(z - focusZ)) / (FOCUS_FAR - FOCUS_NEAR), 0.0f, 1.0f);
	}

	float CenterX(int cx) const { return m_minX + ((float)cx + 0.5f) * m_cell; }
	float CenterY(int cy) const { return m_minY + ((float)cy + 0.5f) * m_cell; }

	bool CellOf(float x, float y, int *cx, int *cy) const
	{
		if (m_dimX <= 0) {
			return false;
		}
		*cx = (int)std::floor((x - m_minX) / m_cell);
		*cy = (int)std::floor((y - m_minY) / m_cell);
		return *cx >= 0 && *cy >= 0 && *cx < m_dimX && *cy < m_dimY;
	}

	int FindNode(int cx, int cy, float z, float within) const
	{
		int i;

		for (i = m_head[(size_t)cy * (size_t)m_dimX + (size_t)cx]; i >= 0; i = m_nodes[(size_t)i].next) {
			if (std::fabs(m_nodes[(size_t)i].z - z) <= within) {
				return i;
			}
		}
		return -1;
	}

	int AddNode(int cx, int cy, float z)
	{
		Node      n;
		const int index = (int)m_nodes.size();
		int      &head = m_head[(size_t)cy * (size_t)m_dimX + (size_t)cx];

		n.z = z;
		n.next = head;
		n.cx = (unsigned short)cx;
		n.cy = (unsigned short)cy;
		n.open = 0;
		if (m_nodes.empty()) {
			m_minCx = m_maxCx = cx;
			m_minCy = m_maxCy = cy;
			m_zMin = m_zMax = z;
		} else {
			if (z < m_zMin) {
				m_zMin = z;
			}
			if (z > m_zMax) {
				m_zMax = z;
			}
			if (cx < m_minCx) {
				m_minCx = cx;
			}
			if (cx > m_maxCx) {
				m_maxCx = cx;
			}
			if (cy < m_minCy) {
				m_minCy = cy;
			}
			if (cy > m_maxCy) {
				m_maxCy = cy;
			}
		}
		m_nodes.push_back(n);
		head = index;
		m_queue.push_back(index);
		m_revision++;
		return index;
	}

	// the rectangle of cells and the texels of a picture asked for (BeginPicture)
	bool Frame(int size, float unitsPerTexel, float centerX, float centerY, float halfExtent, PictureFrame *f) const
	{
		int   x0, y0, x1, y1;
		float tpc;

		if (m_nodes.empty() || size < 16) {
			return false;
		}
		x0 = m_minCx - PAD;
		y0 = m_minCy - PAD;
		x1 = m_maxCx + PAD;
		y1 = m_maxCy + PAD;
		if (halfExtent > 0.0f) {
			const int wx0 = (int)std::floor((centerX - halfExtent - m_minX) / m_cell);
			const int wy0 = (int)std::floor((centerY - halfExtent - m_minY) / m_cell);
			const int wx1 = (int)std::floor((centerX + halfExtent - m_minX) / m_cell);
			const int wy1 = (int)std::floor((centerY + halfExtent - m_minY) / m_cell);

			x0 = wx0 > x0 ? wx0 : x0;
			y0 = wy0 > y0 ? wy0 : y0;
			x1 = wx1 < x1 ? wx1 : x1;
			y1 = wy1 < y1 ? wy1 : y1;
			// away from the map: a few empty cells
			if (x1 < x0 + 7) {
				x1 = x0 + 7;
			}
			if (y1 < y0 + 7) {
				y1 = y0 + 7;
			}
		}
		f->cx0 = x0;
		f->cy0 = y0;
		f->cw = x1 - x0 + 1;
		f->ch = y1 - y0 + 1;
		tpc = (float)size / (float)(f->cw > f->ch ? f->cw : f->ch);
		if (unitsPerTexel > 0.0f && tpc > m_cell / unitsPerTexel) {
			tpc = m_cell / unitsPerTexel;
		}
		if (tpc > TEXELS_PER_CELL_MAX) {
			tpc = TEXELS_PER_CELL_MAX;
		}
		f->tpc = tpc;
		f->size = size;
		f->w = (int)((float)f->cw * tpc);
		f->h = (int)((float)f->ch * tpc);
		if (f->w > size) {
			f->w = size;
		}
		if (f->h > size) {
			f->h = size;
		}
		return true;
	}

	// a cell of a picture's rectangle
	static size_t PicCell(const PictureFrame &f, int cx, int cy)
	{
		return (size_t)(cy - f.cy0) * (size_t)f.cw + (size_t)(cx - f.cx0);
	}

	float             m_cell = CELL_MIN;
	float             m_minX = 0.0f, m_minY = 0.0f;
	int               m_dimX = 0, m_dimY = 0;
	std::vector<int>  m_head; // a column's first floor, -1 none
	std::vector<Node> m_nodes;
	std::vector<int>  m_queue; // floors to walk on from
	size_t            m_queueAt = 0;
	unsigned          m_revision = 0;
	int               m_minCx = 0, m_maxCx = 0, m_minCy = 0, m_maxCy = 0;
	float             m_zMin = 0.0f, m_zMax = 0.0f; // the lowest and the highest floor

};

//
// draws pictures on a thread of its own, one at a time. BeginPicture reads
// the map there, so from Start until ReadsMap() is false again the map must
// not change (no Seed, Step or Reset); the rows then need only the picture
//
class PictureWorker
{
public:
	~PictureWorker() { Stop(); }

	// starts a picture; false when one is being drawn or waits to be taken,
	// or no thread could be started (then draw it with the map's own calls)
	bool Start(const Map &map, int size, float unitsPerTexel, float focusZ, float centerX, float centerY, float halfExtent)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_state != IDLE || m_noThread) {
			return false;
		}
		if (!m_thread.joinable()) {
			try {
				m_thread = std::thread(&PictureWorker::Run, this);
			} catch (...) {
				m_noThread = true;
				return false;
			}
		}
		m_map = &map;
		m_size = size;
		m_unitsPerTexel = unitsPerTexel;
		m_focusZ = focusZ;
		m_centerX = centerX;
		m_centerY = centerY;
		m_halfExtent = halfExtent;
		m_cancel = false;
		m_state = QUEUED;
		m_wake.notify_one();
		return true;
	}

	bool ReadsMap()
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		return m_state == QUEUED || m_state == BEGIN;
	}

	// a picture is being drawn or waits to be taken
	bool Busy()
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		return m_state != IDLE;
	}

	// the finished picture, swapped with p (whose texels the next one is
	// drawn into); false when there is none
	bool Take(Picture &p)
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_state != DONE) {
			return false;
		}
		m_state = IDLE;
		if (!m_back.done) {
			return false;
		}
		std::swap(p, m_back);
		return true;
	}

	// drops the picture being drawn, if any: the thread no longer reads the
	// map when this returns
	void Cancel()
	{
		std::unique_lock<std::mutex> lock(m_mutex);

		m_cancel = true;
		m_idle.wait(lock, [this] { return m_state == IDLE || m_state == DONE; });
		m_state = IDLE;
	}

	void Stop()
	{
		if (!m_thread.joinable()) {
			return;
		}
		Cancel();
		{
			std::lock_guard<std::mutex> lock(m_mutex);

			m_quit = true;
			m_wake.notify_one();
		}
		m_thread.join();
		m_quit = false;
	}

private:
	enum State { IDLE, QUEUED, BEGIN, ROWS, DONE };

	void Run()
	{
		std::unique_lock<std::mutex> lock(m_mutex);

		for (;;) {
			bool ok;

			m_wake.wait(lock, [this] { return m_quit || m_state == QUEUED; });
			if (m_quit) {
				return;
			}
			m_state = BEGIN;
			lock.unlock();
			ok = m_map->BeginPicture(m_back, m_size, m_unitsPerTexel, m_focusZ, m_centerX, m_centerY, m_halfExtent);
			lock.lock();
			m_state = ROWS;
			lock.unlock();
			while (ok && !m_cancel && !Map::PictureRows(m_back, 8)) {
			}
			lock.lock();
			if (!ok || m_cancel) {
				m_back.done = false;
			}
			m_state = DONE;
			m_idle.notify_all();
		}
	}

	std::thread             m_thread;
	std::mutex              m_mutex;
	std::condition_variable m_wake, m_idle;
	State                   m_state = IDLE;
	bool                    m_quit = false, m_noThread = false;
	std::atomic<bool>       m_cancel{false};
	Picture                 m_back;
	const Map              *m_map = NULL;
	int                     m_size = 0;
	float                   m_unitsPerTexel = 0.0f, m_focusZ = 0.0f, m_centerX = 0.0f, m_centerY = 0.0f, m_halfExtent = 0.0f;
};

} // namespace radarmap
