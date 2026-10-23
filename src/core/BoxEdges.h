#pragma once
#include <algorithm>
#include <cstdlib>

namespace td::boxedit
{
/** A box in pixels (x, y = top left). */
struct IRect
{
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

/** Which edge of a selection box the point (px, py) takes hold of, for resizing it: 1 = left, 2 = right, 4 = top, 8 = bottom, two bits at a corner, 0 = none.

    The rule that matters: a side belongs to ONE axis. Taking hold of the left or right edge changes the width and nothing else; the top or bottom edge changes the
    height and nothing else. Only a real corner (a box big enough to have one, and the point close to it in both directions) changes both. A flat or narrow box
    therefore has no corners at all: if its sides sat inside each other's reach, grabbing the end of a flat box for its width would also grab its height (and the
    height collapsed to a few pixels or none). Where the reach of two edges of one axis overlaps (a narrow box), the NEARER edge is taken. Where a vertical and a horizontal edge are both in
    reach but it is not a real corner, the SHORT edge wins (the end of a flat box is its width, the end of a tall narrow box its height): the long edge is
    taken along its length.
    m: how close to an edge, in pixels, counts as being on it. */
inline int edgesAt (IRect r, int px, int py, int m = 6)
{
    if (px < r.x - m || px > r.right() + m || py < r.y - m || py > r.bottom() + m) return 0;
    const int dl = std::abs (px - r.x),   dr = std::abs (px - r.right());
    const int dt = std::abs (py - r.y),   db = std::abs (py - r.bottom());
    const int dx = std::min (dl, dr), dy = std::min (dt, db);
    const int sx = dx <= m ? (dl <= dr ? 1 : 2) : 0;
    const int sy = dy <= m ? (dt <= db ? 4 : 8) : 0;
    if (sx == 0 || sy == 0) return sx | sy;                              // on one side only (or in the middle of the box: 0)
    const int cr = std::min (m, std::min (r.w, r.h) / 3);                 // how near a corner point counts as the corner; a flat or narrow box has none
    if (cr >= 2 && dx <= cr && dy <= cr) return sx | sy;
    return r.w >= r.h ? sx : sy;                                          // near the end of a long edge but not at a corner: the short edge there (the end of the box), and only that one
}
} // namespace td::boxedit
