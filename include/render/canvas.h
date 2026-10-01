#pragma once
// 1-bit drawing with Pillow-identical rasterization (spec §3.2): the server drew with
// ImageDraw on an "L" image, so porting Pillow's rasterizers keeps the look pixel for pixel.
// Pure; host-tested against Pillow's own output (test/reference/primitives).
//
// The ports follow Pillow 12.3.0: src/PIL/ImageDraw.py (the Python layer: ink/outline rules,
// rounded_rectangle, the masked wide polygon outline), src/_imaging.c (`_draw_*`: every float
// coordinate is truncated with a C (int) cast) and src/libImaging/Draw.c (the rasterizers).
// Pillow's own types are kept on purpose: the polygon filler works in float, the wide-line
// geometry and ellipse clipping in double, and the ROUND_* macros mix the two exactly as below.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace ink {

constexpr int FRAME_W = 800, FRAME_H = 480, FRAME_BYTES = 48000;
constexpr int NONE = -1, BLACK = 0, WHITE = 255;
constexpr int INK_THRESHOLD = 140;  // frame.py: a value > 140 is white

struct Box {
  int x, y, w, h;
};
struct Pt {
  double x, y;
};

class Bitmap {  // 1-bit, rows of (w + 7) / 8 bytes, bit 1 = white
 public:
  Bitmap(uint8_t* bits, int w, int h) : bits_(bits), w_(w), h_(h), stride_((w + 7) / 8) {}
  int w() const { return w_; }
  int h() const { return h_; }
  int stride() const { return stride_; }
  uint8_t* bits() { return bits_; }
  const uint8_t* bits() const { return bits_; }
  void fill(int ink) { memset(bits_, ink <= INK_THRESHOLD ? 0x00 : 0xFF, static_cast<size_t>(stride_) * h_); }
  bool is_black(int x, int y) const { return (bits_[y * stride_ + x / 8] & (0x80 >> (x & 7))) == 0; }
  void set(int x, int y, int ink) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t& b = bits_[y * stride_ + x / 8];
    const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
    b = ink <= INK_THRESHOLD ? static_cast<uint8_t>(b & ~m) : static_cast<uint8_t>(b | m);
  }
  // Pixels x0..x1 (inclusive, already inside the bitmap) of row y.
  void span(int x0, int x1, int y, int ink) {
    uint8_t* row = bits_ + y * stride_;
    const bool black = ink <= INK_THRESHOLD;
    while (x0 <= x1 && (x0 & 7)) set_bit(row, x0++, black);
    while (x0 + 7 <= x1) {
      row[x0 / 8] = black ? 0x00 : 0xFF;
      x0 += 8;
    }
    while (x0 <= x1) set_bit(row, x0++, black);
  }

 private:
  static void set_bit(uint8_t* row, int x, bool black) {
    const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
    row[x / 8] = black ? static_cast<uint8_t>(row[x / 8] & ~m) : static_cast<uint8_t>(row[x / 8] | m);
  }

  uint8_t* bits_;
  int w_, h_, stride_;
};

// ---- Ports of Pillow's rasterizers (Draw.c). Coordinates here are bitmap pixels. ----
namespace pil {

constexpr double PI = 3.14159265358979323846;

// Draw.c ROUND_UP / ROUND_DOWN. With a float argument C adds 0.5F in float but fabs()
// promotes to double, so the two signs round at different precisions; keep that.
inline int round_up(double f) { return static_cast<int>(f >= 0.0 ? floor(f + 0.5) : -floor(fabs(f) + 0.5)); }
inline int round_down(double f) { return static_cast<int>(f >= 0.0 ? ceil(f - 0.5) : -ceil(fabs(f) - 0.5)); }
inline int round_up(float f) {
  return static_cast<int>(f >= 0.0f ? floor(static_cast<double>(f + 0.5f)) : -floor(fabs(static_cast<double>(f)) + 0.5));
}
inline int round_down(float f) {
  return static_cast<int>(f >= 0.0f ? ceil(static_cast<double>(f - 0.5f)) : -ceil(fabs(static_cast<double>(f)) - 0.5));
}

// Python's round(): half to even, independent of the FPU rounding mode.
inline int py_round(double x) {
  const double f = floor(x), d = x - f;
  if (d > 0.5) return static_cast<int>(f) + 1;
  if (d < 0.5) return static_cast<int>(f);
  return fmod(f, 2.0) == 0.0 ? static_cast<int>(f) : static_cast<int>(f) + 1;
}

struct Edge {
  int d;
  int x0, y0;
  int xmin, ymin, xmax, ymax;
  float dx;
};

inline void add_edge(Edge* e, int x0, int y0, int x1, int y1) {
  if (x0 <= x1) e->xmin = x0, e->xmax = x1;
  else e->xmin = x1, e->xmax = x0;
  if (y0 <= y1) e->ymin = y0, e->ymax = y1;
  else e->ymin = y1, e->ymax = y0;
  if (y0 == y1) {
    e->d = 0;
    e->dx = 0.0f;
  } else {
    e->dx = static_cast<float>(x1 - x0) / static_cast<float>(y1 - y0);
    e->d = y0 == e->ymin ? 1 : -1;
  }
  e->x0 = x0;
  e->y0 = y0;
}

// polygon_generic (non-alpha path), split into a setup pass and one call per scanline so the
// masked polygon outline can ask for the fill's spans of a single row.
struct PolyScan {
  Edge** table;  // non-horizontal edges
  int count;
  float* xx;     // 2 * count scratch intersections
  int ymin, ymax;
};

// Edge x at row y, in float like Pillow (int * float + int).
inline float edge_x(const Edge* e, int y) { return static_cast<float>(y - e->y0) * e->dx + static_cast<float>(e->x0); }

template <class Hline>
inline void poly_setup(PolyScan& s, Edge* e, int n, Edge** table, float* xx, int ysize, Hline& hline) {
  s.table = table;
  s.xx = xx;
  s.count = 0;
  s.ymin = ysize - 1;
  s.ymax = 0;
  for (int i = 0; i < n; i++) {
    if (s.ymin > e[i].ymin) s.ymin = e[i].ymin;
    if (s.ymax < e[i].ymax) s.ymax = e[i].ymax;
    if (e[i].ymin == e[i].ymax) {
      hline(e[i].xmin, e[i].ymin, e[i].xmax);
      continue;
    }
    table[s.count++] = e + i;
  }
  if (s.ymin < 0) s.ymin = 0;
  if (s.ymax > ysize) s.ymax = ysize;
}

template <class Hline>
inline void poly_row(const PolyScan& s, int y, Hline& hline) {
  float* xx = s.xx;
  int j = 0;
  for (int i = 0; i < s.count; i++) {
    const Edge* current = s.table[i];
    if (y >= current->ymin && y <= current->ymax) {
      xx[j++] = edge_x(current, y);
      if (y == current->ymax && y < s.ymax) {
        // Needed to draw consistent polygons
        xx[j] = xx[j - 1];
        j++;
      } else if ((y == current->ymin || y == current->ymax) && current->dx != 0) {
        // Connect discontiguous corners
        for (int k = 0; k < i; k++) {
          const Edge* other = s.table[k];
          if ((y != other->ymin && y != other->ymax) || other->dx == 0) continue;
          if (roundf(xx[j - 1]) == roundf(edge_x(other, y))) {
            const int offset = y == current->ymax ? -1 : 1;
            const float adj = edge_x(current, y + offset);
            if (y + offset >= other->ymin && y + offset <= other->ymax) {
              const float adj_other = edge_x(other, y + offset);
              if (xx[j - 1] > adj + 1 && xx[j - 1] > adj_other + 1) {
                xx[j - 1] = roundf(static_cast<float>(fmax(static_cast<double>(adj), static_cast<double>(adj_other)))) + 1;
              } else if (xx[j - 1] < adj - 1 && xx[j - 1] < adj_other - 1) {
                xx[j - 1] = roundf(static_cast<float>(fmin(static_cast<double>(adj), static_cast<double>(adj_other)))) - 1;
              }
              break;
            }
          }
        }
      }
    }
  }
  // qsort in Pillow; equal keys are indistinguishable, so any correct sort gives the same spans.
  for (int a = 1; a < j; a++) {
    const float v = xx[a];
    int b = a - 1;
    while (b >= 0 && xx[b] > v) {
      xx[b + 1] = xx[b];
      b--;
    }
    xx[b + 1] = v;
  }
  for (int i = 1; i < j; i += 2) hline(round_up(xx[i - 1]), y, round_down(xx[i]));
}

template <class Hline>
inline void polygon_generic(Edge* e, int n, Edge** table, float* xx, int ysize, Hline& hline) {
  if (n <= 0) return;
  PolyScan s;
  poly_setup(s, e, n, table, xx, ysize, hline);
  for (int y = s.ymin; y <= s.ymax; y++) poly_row(s, y, hline);
}

// ImagingDrawPolygon's fill edge list (merges consecutive horizontal runs). Returns the count.
inline int polygon_edges(Edge* e, int count, const int* xy) {
  int i, n;
  for (i = n = 0; i < count - 1; i++) {
    const int x0 = xy[i * 2], y0 = xy[i * 2 + 1], x1 = xy[i * 2 + 2], y1 = xy[i * 2 + 3];
    if (y0 == y1 && i != 0 && y0 == xy[i * 2 - 1]) {
      Edge* last_e = &e[n - 1];
      if (x1 > x0 && x0 > xy[i * 2 - 2]) {
        last_e->xmax = x1;
        continue;
      } else if (x1 < x0 && x0 < xy[i * 2 - 2]) {
        last_e->xmin = x1;
        continue;
      }
    }
    add_edge(&e[n++], x0, y0, x1, y1);
  }
  if (xy[i * 2] != xy[0] || xy[i * 2 + 1] != xy[1]) add_edge(&e[n++], xy[i * 2], xy[i * 2 + 1], xy[0], xy[1]);
  return n;
}

// ---- Ellipses: quarter_* / ellipse_* iterators ----

struct QuarterState {
  int32_t a, b, cx, cy, ex, ey;
  int64_t a2, b2, a2b2;
  int8_t finished;
};

inline void quarter_init(QuarterState* s, int32_t a, int32_t b) {
  if (a < 0 || b < 0) {
    s->finished = 1;
  } else {
    s->a = a;
    s->b = b;
    s->cx = a;
    s->cy = b % 2;
    s->ex = a % 2;
    s->ey = b;
    s->a2 = static_cast<int64_t>(a) * a;
    s->b2 = static_cast<int64_t>(b) * b;
    s->a2b2 = s->a2 * s->b2;
    s->finished = 0;
  }
}

inline int64_t quarter_delta(const QuarterState* s, int64_t x, int64_t y) {
  const int64_t v = s->a2 * y * y + s->b2 * x * x - s->a2b2;
  return v < 0 ? -v : v;
}

inline int8_t quarter_next(QuarterState* s, int32_t* ret_x, int32_t* ret_y) {
  if (s->finished) return -1;
  *ret_x = s->cx;
  *ret_y = s->cy;
  if (s->cx == s->ex && s->cy == s->ey) {
    s->finished = 1;
  } else {
    int32_t nx = s->cx;
    int32_t ny = s->cy + 2;
    int64_t ndelta = quarter_delta(s, nx, ny);
    if (nx > 1) {
      int64_t newdelta = quarter_delta(s, s->cx - 2, s->cy + 2);
      if (ndelta > newdelta) {
        nx = s->cx - 2;
        ny = s->cy + 2;
        ndelta = newdelta;
      }
      newdelta = quarter_delta(s, s->cx - 2, s->cy);
      if (ndelta > newdelta) {
        nx = s->cx - 2;
        ny = s->cy;
      }
    }
    s->cx = nx;
    s->cy = ny;
  }
  return 0;
}

struct EllipseState {
  QuarterState st_o, st_i;
  int32_t py, pl, pr;
  int32_t cy[4], cl[4], cr[4];
  int8_t bufcnt;
  int8_t finished;
  int8_t leftmost;
};

inline void ellipse_init(EllipseState* s, int32_t a, int32_t b, int32_t w) {
  s->bufcnt = 0;
  s->leftmost = static_cast<int8_t>(a % 2);
  quarter_init(&s->st_o, a, b);
  if (w < 1 || quarter_next(&s->st_o, &s->pr, &s->py) == -1) {
    s->finished = 1;
  } else {
    s->finished = 0;
    quarter_init(&s->st_i, a - 2 * (w - 1), b - 2 * (w - 1));
    s->pl = s->leftmost;
  }
}

inline int8_t ellipse_next(EllipseState* s, int32_t* ret_x0, int32_t* ret_y, int32_t* ret_x1) {
  if (s->bufcnt == 0) {
    if (s->finished) return -1;
    const int32_t y = s->py;
    int32_t l = s->pl;
    const int32_t r = s->pr;
    int32_t cx = 0, cy = 0;
    int8_t next_ret;
    while ((next_ret = quarter_next(&s->st_o, &cx, &cy)) != -1 && cy <= y) {
    }
    if (next_ret == -1) {
      s->finished = 1;
    } else {
      s->pr = cx;
      s->py = cy;
    }
    while ((next_ret = quarter_next(&s->st_i, &cx, &cy)) != -1 && cy <= y) l = cx;
    s->pl = next_ret == -1 ? s->leftmost : cx;

    if ((l > 0 || l < r) && y > 0) {
      s->cl[s->bufcnt] = l == 0 ? 2 : l;
      s->cy[s->bufcnt] = y;
      s->cr[s->bufcnt] = r;
      ++s->bufcnt;
    }
    if (y > 0) {
      s->cl[s->bufcnt] = -r;
      s->cy[s->bufcnt] = y;
      s->cr[s->bufcnt] = -l;
      ++s->bufcnt;
    }
    if (l > 0 || l < r) {
      s->cl[s->bufcnt] = l == 0 ? 2 : l;
      s->cy[s->bufcnt] = -y;
      s->cr[s->bufcnt] = r;
      ++s->bufcnt;
    }
    s->cl[s->bufcnt] = -r;
    s->cy[s->bufcnt] = -y;
    s->cr[s->bufcnt] = -l;
    ++s->bufcnt;
  }
  --s->bufcnt;
  *ret_x0 = s->cl[s->bufcnt];
  *ret_y = s->cy[s->bufcnt];
  *ret_x1 = s->cr[s->bufcnt];
  return 0;
}

// ---- Clipping tree for pieslices and arcs (malloc'd event lists become fixed arrays) ----

enum ClipType { CT_AND, CT_OR, CT_CLIP };

struct ClipNode {
  ClipType type;
  double a, b, c;
  ClipNode* l;
  ClipNode* r;
};

struct Events {  // sorted segment ends: type 1 = left end, -1 = right end
  enum { CAP = 16 };  // a tree of <= 7 nodes has <= 4 leaves, each one segment
  int n;
  int32_t x[CAP];
  int8_t type[CAP];
  void push(int32_t px, int8_t pt) {
    if (n < CAP) x[n] = px, type[n] = pt, ++n;
  }
};

inline void clip_tree_transpose(ClipNode* root) {
  if (root != nullptr) {
    if (root->type == CT_CLIP) {
      const double t = root->a;
      root->a = root->b;
      root->b = t;
    }
    clip_tree_transpose(root->l);
    clip_tree_transpose(root->r);
  }
}

inline void clip_tree_do_clip(const ClipNode* root, int32_t x0, int32_t y, int32_t x1, Events& ret) {
  ret.n = 0;
  if (root == nullptr) {
    ret.push(x0, 1);
    ret.push(x1, -1);
    return;
  }
  if (root->type == CT_CLIP) {
    const double eps = 1e-9;
    const double A = root->a, B = root->b, C = root->c;
    if (fabs(A) < eps) {
      if (B * y + C < -eps) {
        x0 = 1;
        x1 = 0;
      }
    } else {
      const double ix = -(B * y + C) / A;
      if (A * x0 + B * y + C < eps) x0 = static_cast<int32_t>(lround(fmax(x0, ix)));
      if (A * x1 + B * y + C < eps) x1 = static_cast<int32_t>(lround(fmin(x1, ix)));
    }
    if (x0 <= x1) {
      ret.push(x0, 1);
      ret.push(x1, -1);
    }
    return;
  }
  Events l1, l2;
  clip_tree_do_clip(root->l, x0, y, x1, l1);
  clip_tree_do_clip(root->r, x0, y, x1, l2);
  int i1 = 0, i2 = 0, k1 = 0, k2 = 0;
  int8_t tail = 0;  // type of the last kept event, 0 = none yet
  while (i1 < l1.n || i2 < l2.n) {
    int32_t tx;
    int8_t tt;
    if (i2 >= l2.n ||
        (i1 < l1.n && (l1.x[i1] < l2.x[i2] || (l1.x[i1] == l2.x[i2] && l1.type[i1] > l2.type[i2])))) {
      tx = l1.x[i1], tt = l1.type[i1++];
      k1 += tt;
    } else {
      tx = l2.x[i2], tt = l2.type[i2++];
      k2 += tt;
    }
    if ((root->type == CT_OR && ((tt == 1 && (tail == 0 || tail == -1)) || (tt == -1 && k1 == 0 && k2 == 0))) ||
        (root->type == CT_AND && ((tt == 1 && (tail == 0 || tail == -1) && k1 > 0 && k2 > 0) ||
                                  (tt == -1 && tail == 1 && (k1 == 0 || k2 == 0))))) {
      ret.push(tx, tt);
      tail = tt;
    }
  }
}

struct ClipEllipseState {
  EllipseState st;
  ClipNode* root;
  ClipNode nodes[7];
  int32_t node_count;
};

// Resulting angles satisfy 0 <= al < 360, al <= ar <= al + 360. C's fmod works in double.
inline void normalize_angles(float* al, float* ar) {
  if (*ar - *al >= 360) {
    *al = 0;
    *ar = 360;
  } else {
    *al = static_cast<float>(fmod(*al < 0 ? 360 - fmod(-static_cast<double>(*al), 360) : static_cast<double>(*al), 360));
    *ar = static_cast<float>(*al + fmod(*ar < *al ? 360 - fmod(static_cast<double>(*al - *ar), 360)
                                                    : static_cast<double>(*ar - *al),
                                        360));
  }
}

inline void arc_init(ClipEllipseState* s, int32_t a, int32_t b, int32_t w, float al, float ar) {
  if (a < b) {
    // transpose the coordinate system
    arc_init(s, b, a, w, 90 - ar, 90 - al);
    ellipse_init(&s->st, a, b, w);
    clip_tree_transpose(s->root);
    return;
  }
  ellipse_init(&s->st, a, b, w);
  s->node_count = 0;
  normalize_angles(&al, &ar);
  if (ar == al + 360) {
    s->root = nullptr;
    return;
  }
  ClipNode* lc = s->nodes + s->node_count++;
  ClipNode* rc = s->nodes + s->node_count++;
  lc->l = lc->r = rc->l = rc->r = nullptr;
  lc->type = rc->type = CT_CLIP;
  lc->a = -a * sin(al * PI / 180.0);
  lc->b = b * cos(al * PI / 180.0);
  lc->c = (a * a - b * b) * sin(al * PI / 90.0) / 2.0;
  rc->a = a * sin(ar * PI / 180.0);
  rc->b = -b * cos(ar * PI / 180.0);
  rc->c = (b * b - a * a) * sin(ar * PI / 90.0) / 2.0;
  if (fmod(static_cast<double>(al), 180) == 0 || fmod(static_cast<double>(ar), 180) == 0) {
    s->root = s->nodes + s->node_count++;
    s->root->l = lc;
    s->root->r = rc;
    s->root->type = ar - al < 180 ? CT_AND : CT_OR;
  } else if ((static_cast<int>(al / 180) + static_cast<int>(ar / 180)) % 2 == 1) {
    s->root = s->nodes + s->node_count++;
    s->root->l = s->nodes + s->node_count++;
    s->root->l->l = s->nodes + s->node_count++;
    s->root->l->r = lc;
    s->root->r = s->nodes + s->node_count++;
    s->root->r->l = s->nodes + s->node_count++;
    s->root->r->r = rc;
    s->root->type = CT_OR;
    s->root->l->type = CT_AND;
    s->root->r->type = CT_AND;
    s->root->l->l->type = CT_CLIP;
    s->root->r->l->type = CT_CLIP;
    s->root->l->l->l = s->root->l->l->r = nullptr;
    s->root->r->l->l = s->root->r->l->r = nullptr;
    s->root->l->l->a = s->root->l->l->c = 0;
    s->root->r->l->a = s->root->r->l->c = 0;
    s->root->l->l->b = static_cast<int>(al / 180) % 2 == 0 ? 1 : -1;
    s->root->r->l->b = static_cast<int>(ar / 180) % 2 == 0 ? 1 : -1;
  } else {
    s->root = s->nodes + s->node_count++;
    s->root->l = s->nodes + s->node_count++;
    s->root->r = s->nodes + s->node_count++;
    s->root->type = s->root->l->type = ar - al < 180 ? CT_AND : CT_OR;
    s->root->l->l = lc;
    s->root->l->r = rc;
    s->root->r->type = CT_CLIP;
    s->root->r->l = s->root->r->r = nullptr;
    s->root->r->a = s->root->r->c = 0;
    s->root->r->b = ar < 180 || ar > 540 ? 1 : -1;
  }
}

inline void pie_init(ClipEllipseState* s, int32_t a, int32_t b, int32_t w, float al, float ar) {
  ellipse_init(&s->st, a, b, w);
  s->node_count = 0;
  const double xl = a * cos(al * PI / 180.0), xr = a * cos(ar * PI / 180.0);
  const double yl = b * sin(al * PI / 180.0), yr = b * sin(ar * PI / 180.0);
  ClipNode* lc = s->nodes + s->node_count++;
  ClipNode* rc = s->nodes + s->node_count++;
  lc->l = lc->r = rc->l = rc->r = nullptr;
  lc->type = rc->type = CT_CLIP;
  lc->a = -yl;
  lc->b = xl;
  lc->c = 0;
  rc->a = yr;
  rc->b = -xr;
  rc->c = 0;
  s->root = s->nodes + s->node_count++;
  s->root->l = lc;
  s->root->r = rc;
  s->root->type = ar - al < 180 ? CT_AND : CT_OR;
  if (ar - al < 90) {  // one more half-plane against spikes
    ClipNode* old_root = s->root;
    ClipNode* spike = s->nodes + s->node_count++;
    s->root = s->nodes + s->node_count++;
    s->root->l = old_root;
    s->root->r = spike;
    s->root->type = CT_AND;
    spike->l = spike->r = nullptr;
    spike->type = CT_CLIP;
    spike->a = (xl + xr) / 2.0;
    spike->b = (yl + yr) / 2.0;
    spike->c = 0;
  }
}

}  // namespace pil

struct PolyMask;

class View {  // a box of a Bitmap: local coordinates, clipped
 public:
  View(Bitmap& bm, Box box) : bm_(bm), box_(clip_to(box, Box{0, 0, bm.w(), bm.h()})), ox_(box.x), oy_(box.y) {}
  int w() const { return box_.x + box_.w - ox_; }
  int h() const { return box_.y + box_.h - oy_; }
  View sub(Box local) const {
    View v(bm_, Box{ox_ + local.x, oy_ + local.y, local.w, local.h});
    v.box_ = clip_to(v.box_, box_);
    return v;
  }
  void px(int x, int y, int ink) { put(x + ox_, y + oy_, ink); }
  void fill(int ink) {  // Image.paste(ink, box)
    for (int y = box_.y; y < box_.y + box_.h; ++y) hline(box_.x, y, box_.x + box_.w - 1, ink);
  }

  void point(double x, double y, int ink);
  void line(const Pt* pts, int n, int ink, int width = 1);
  void line(double x0, double y0, double x1, double y1, int ink, int width = 1) {
    const Pt p[2] = {{x0, y0}, {x1, y1}};
    line(p, 2, ink, width);
  }
  void rectangle(double x0, double y0, double x1, double y1, int fill, int outline = NONE, int width = 1);
  void ellipse(double x0, double y0, double x1, double y1, int fill, int outline = NONE, int width = 1);
  void polygon(const Pt* pts, int n, int fill, int outline = NONE, int width = 1);
  void rounded_rectangle(double x0, double y0, double x1, double y1, double radius, int fill,
                         int outline = NONE, int width = 1);

 private:
  static Box clip_to(Box a, Box b) {
    const int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    const int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    const int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return Box{x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
  }

  // The rasterizers run in bitmap coordinates, as the server's did in frame coordinates:
  // Python's round() (half to even), the float edge math and the image-height clamps are not
  // invariant under a translation, so local inputs are offset before any of them happen.
  // Pillow clips to the image; here the writes clip to the view's box (inside the bitmap).
  int ax(double x) const { return static_cast<int>(x + ox_); }  // _imaging.c: (int)xy[i]
  int ay(double y) const { return static_cast<int>(y + oy_); }

  void put(int x, int y, int ink) {  // point8
    if (x < box_.x || y < box_.y || x >= box_.x + box_.w || y >= box_.y + box_.h) return;
    bm_.set(x, y, ink);
  }
  void hline(int x0, int y, int x1, int ink) {  // hline8
    if (y < box_.y || y >= box_.y + box_.h) return;
    if (x0 < box_.x) x0 = box_.x;
    if (x1 > box_.x + box_.w - 1) x1 = box_.x + box_.w - 1;
    if (x0 <= x1) bm_.span(x0, x1, y, ink);
  }
  void line8(int x0, int y0, int x1, int y1, int ink);
  void wide_line(int x0, int y0, int x1, int y1, int ink, int width, PolyMask* mask);
  void rectangle_abs(double x0, double y0, double x1, double y1, int fill, int outline, int width);
  void ellipse_abs(double x0, double y0, double x1, double y1, int fill, int outline, int width);
  void rect(int x0, int y0, int x1, int y1, int ink, bool fill, int width);
  void ellipse_new(int x0, int y0, int x1, int y1, int ink, bool fill, int width);
  void clip_ellipse(int x0, int y0, int x1, int y1, pil::ClipEllipseState& st, int ink);
  void draw_arc(int x0, int y0, int x1, int y1, float start, float end, int ink, int width);
  void pieslice_fill(int x0, int y0, int x1, int y1, float start, float end, int ink);
  void polygon_fill(const int* xy, int count, int ink);

  Bitmap& bm_;
  Box box_;      // absolute, already clipped
  int ox_, oy_;  // origin of local coordinates (unclipped box.x/box.y)
};

// The fill of a polygon as a mask, one row at a time: ImageDraw.polygon draws a wide outline
// through an "1" image filled with the polygon, so only the inner half of the stroke remains.
struct PolyMask {
  pil::Edge* e;
  int n;
  pil::PolyScan scan;
  int* spans;  // x0, x1 pairs for row `y`
  int nspans;
  int y;

  struct Collect {
    PolyMask* m;
    int only_y;
    void operator()(int x0, int y, int x1) {
      if (y != only_y) return;
      m->spans[2 * m->nspans] = x0;
      m->spans[2 * m->nspans + 1] = x1;
      ++m->nspans;
    }
  };
  bool covers(int x, int y) {
    if (y != this->y) {
      this->y = y;
      nspans = 0;
      Collect c{this, y};
      for (int i = 0; i < n; i++)
        if (e[i].ymin == e[i].ymax) c(e[i].xmin, e[i].ymin, e[i].xmax);
      if (y >= scan.ymin && y <= scan.ymax) pil::poly_row(scan, y, c);
    }
    for (int i = 0; i < nspans; i++)
      if (x >= spans[2 * i] && x <= spans[2 * i + 1]) return true;
    return false;
  }
};

inline void View::line8(int x0, int y0, int x1, int y1, int ink) {
  int i, n, e, dx, dy, xs, ys;
  dx = x1 - x0;
  if (dx < 0) dx = -dx, xs = -1;
  else xs = 1;
  dy = y1 - y0;
  if (dy < 0) dy = -dy, ys = -1;
  else ys = 1;
  if (dx == 0) {
    for (i = 0; i < dy; i++) {
      put(x0, y0, ink);
      y0 += ys;
    }
  } else if (dy == 0) {
    for (i = 0; i < dx; i++) {
      put(x0, y0, ink);
      x0 += xs;
    }
  } else if (dx > dy) {
    n = dx;
    dy += dy;
    e = dy - dx;
    dx += dx;
    for (i = 0; i < n; i++) {
      put(x0, y0, ink);
      if (e >= 0) {
        y0 += ys;
        e -= dx;
      }
      e += dy;
      x0 += xs;
    }
  } else {
    n = dy;
    dx += dx;
    e = dx - dy;
    dy += dy;
    for (i = 0; i < n; i++) {
      put(x0, y0, ink);
      if (e >= 0) {
        x0 += xs;
        e -= dy;
      }
      e += dx;
      y0 += ys;
    }
  }
}

// ImagingDrawWideLine: the stroke is a 4-edge polygon.
inline void View::wide_line(int x0, int y0, int x1, int y1, int ink, int width, PolyMask* m) {
  const int dx = x1 - x0, dy = y1 - y0;
  if (dx == 0 && dy == 0) {
    put(x0, y0, ink);  // draw->point ignores the mask
    return;
  }
  const double big_hypotenuse = hypot(dx, dy);
  const double small_hypotenuse = (width - 1) / 2.0;
  const double ratio_max = pil::round_up(small_hypotenuse) / big_hypotenuse;
  const double ratio_min = pil::round_down(small_hypotenuse) / big_hypotenuse;
  const int dxmin = pil::round_down(ratio_min * dy), dxmax = pil::round_down(ratio_max * dy);
  const int dymin = pil::round_down(ratio_min * dx), dymax = pil::round_down(ratio_max * dx);
  const int v[4][2] = {{x0 - dxmin, y0 + dymax}, {x1 - dxmin, y1 + dymax}, {x1 + dxmax, y1 - dymin}, {x0 + dxmax, y0 - dymin}};
  pil::Edge e[4];
  for (int i = 0; i < 4; i++) pil::add_edge(e + i, v[i][0], v[i][1], v[(i + 1) % 4][0], v[(i + 1) % 4][1]);
  pil::Edge* table[4];
  float xx[8];
  auto hl = [this, ink, m](int a, int y, int b) {
    if (!m) {
      hline(a, y, b, ink);
      return;
    }
    if (y < box_.y || y >= box_.y + box_.h) return;
    if (a < box_.x) a = box_.x;
    if (b > box_.x + box_.w - 1) b = box_.x + box_.w - 1;
    for (int x = a; x <= b; x++)
      if (m->covers(x, y)) bm_.set(x, y, ink);
  };
  pil::polygon_generic(e, 4, table, xx, bm_.h(), hl);
}

// ImagingDrawRectangle
inline void View::rect(int x0, int y0, int x1, int y1, int ink, bool fill, int width) {
  if (y0 > y1) {
    const int t = y0;
    y0 = y1;
    y1 = t;
  }
  if (fill) {
    if (y0 < 0) y0 = 0;
    else if (y0 >= bm_.h()) return;
    if (y1 < 0) return;
    else if (y1 > bm_.h()) y1 = bm_.h();
    for (int y = y0; y <= y1; y++) hline(x0, y, x1, ink);
  } else {
    if (width == 0) width = 1;
    for (int i = 0; i < width; i++) {
      hline(x0, y0 + i, x1, ink);
      hline(x0, y1 - i, x1, ink);
      line8(x1 - i, y0 + width, x1 - i, y1 - width + 1, ink);
      line8(x0 + i, y0 + width, x0 + i, y1 - width + 1, ink);
    }
  }
}

// ellipseNew
inline void View::ellipse_new(int x0, int y0, int x1, int y1, int ink, bool fill, int width) {
  const int a = x1 - x0, b = y1 - y0;
  if (a < 0 || b < 0) return;
  if (fill) width = a + b;
  pil::EllipseState st;
  pil::ellipse_init(&st, a, b, width);
  int32_t X0, Y, X1;
  while (pil::ellipse_next(&st, &X0, &Y, &X1) != -1)
    hline(x0 + (X0 + a) / 2, y0 + (Y + b) / 2, x0 + (X1 + a) / 2, ink);
}

// clipEllipseNew, after its init callback has run
inline void View::clip_ellipse(int x0, int y0, int x1, int y1, pil::ClipEllipseState& st, int ink) {
  const int a = x1 - x0, b = y1 - y0;
  int32_t X0, Y, X1;
  pil::Events ev;
  while (pil::ellipse_next(&st.st, &X0, &Y, &X1) >= 0) {
    pil::clip_tree_do_clip(st.root, X0, Y, X1, ev);
    for (int i = 0; i + 1 < ev.n; i += 2) hline(x0 + (ev.x[i] + a) / 2, y0 + (Y + b) / 2, x0 + (ev.x[i + 1] + a) / 2, ink);
  }
}

// ImagingDrawArc
inline void View::draw_arc(int x0, int y0, int x1, int y1, float start, float end, int ink, int width) {
  pil::normalize_angles(&start, &end);
  if (start + 360 == end) {
    ellipse_new(x0, y0, x1, y1, ink, false, width);
    return;
  }
  if (start == end) return;
  const int a = x1 - x0, b = y1 - y0;
  if (a < 0 || b < 0) return;
  pil::ClipEllipseState st;
  pil::arc_init(&st, a, b, width, start, end);
  clip_ellipse(x0, y0, x1, y1, st, ink);
}

// ImagingDrawPieslice, fill path (the only one rounded_rectangle uses)
inline void View::pieslice_fill(int x0, int y0, int x1, int y1, float start, float end, int ink) {
  pil::normalize_angles(&start, &end);
  if (start + 360 == end) {
    ellipse_new(x0, y0, x1, y1, ink, true, 0);
    return;
  }
  if (start == end) return;
  const int a = x1 - x0, b = y1 - y0;
  if (a < 0 || b < 0) return;
  pil::ClipEllipseState st;
  pil::pie_init(&st, a, b, x1 + y1 - x0 - y0, start, end);
  clip_ellipse(x0, y0, x1, y1, st, ink);
}

// ImagingDrawPolygon, fill path
inline void View::polygon_fill(const int* xy, int count, int ink) {
  pil::Edge* e = static_cast<pil::Edge*>(calloc(count, sizeof(pil::Edge)));
  pil::Edge** table = static_cast<pil::Edge**>(calloc(count, sizeof(pil::Edge*)));
  float* xx = static_cast<float*>(calloc(2 * count, sizeof(float)));
  if (e && table && xx) {
    const int n = pil::polygon_edges(e, count, xy);
    auto hl = [this, ink](int a, int y, int b) { hline(a, y, b, ink); };
    pil::polygon_generic(e, n, table, xx, bm_.h(), hl);
  }
  free(xx);
  free(table);
  free(e);
}

// ---- ImageDraw (Python) layer ----

inline void View::point(double x, double y, int ink) {
  if (ink == NONE) return;
  put(ax(x), ay(y), ink);
}

inline void View::line(const Pt* pts, int n, int ink, int width) {
  if (ink == NONE || width == 0 || n < 1) return;
  if (width == 1) {
    for (int i = 0; i < n - 1; i++) line8(ax(pts[i].x), ay(pts[i].y), ax(pts[i + 1].x), ay(pts[i + 1].y), ink);
    if (n > 1) put(ax(pts[n - 1].x), ay(pts[n - 1].y), ink);  // draw last point
  } else {
    for (int i = 0; i < n - 1; i++)
      wide_line(ax(pts[i].x), ay(pts[i].y), ax(pts[i + 1].x), ay(pts[i + 1].y), ink, width, nullptr);
  }
}

inline void View::rectangle(double x0, double y0, double x1, double y1, int fill, int outline, int width) {
  rectangle_abs(x0 + ox_, y0 + oy_, x1 + ox_, y1 + oy_, fill, outline, width);
}

inline void View::rectangle_abs(double x0, double y0, double x1, double y1, int fill, int outline, int width) {
  if (x1 < x0 || y1 < y0) return;  // Pillow raises ValueError
  if (fill != NONE) rect(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1), fill, true, 0);
  if (outline != NONE && outline != fill && width != 0)
    rect(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1), outline, false, width);
}

inline void View::ellipse(double x0, double y0, double x1, double y1, int fill, int outline, int width) {
  ellipse_abs(x0 + ox_, y0 + oy_, x1 + ox_, y1 + oy_, fill, outline, width);
}

inline void View::ellipse_abs(double x0, double y0, double x1, double y1, int fill, int outline, int width) {
  if (x1 < x0 || y1 < y0) return;  // Pillow raises ValueError
  if (fill != NONE)
    ellipse_new(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1), fill, true, 0);
  if (outline != NONE && outline != fill && width != 0)
    ellipse_new(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1), outline, false, width);
}

inline void View::polygon(const Pt* pts, int n, int fill, int outline, int width) {
  if (n < 2) return;  // Pillow raises TypeError
  int* xy = static_cast<int*>(malloc(sizeof(int) * 2 * n));
  if (!xy) return;
  for (int i = 0; i < n; i++) xy[2 * i] = ax(pts[i].x), xy[2 * i + 1] = ay(pts[i].y);
  if (fill != NONE) polygon_fill(xy, n, fill);
  if (outline != NONE && outline != fill && width != 0) {
    if (width == 1) {
      int i;
      for (i = 0; i < n - 1; i++) line8(xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], outline);
      line8(xy[i * 2], xy[i * 2 + 1], xy[0], xy[1], outline);
    } else {
      PolyMask m;
      m.e = static_cast<pil::Edge*>(calloc(n, sizeof(pil::Edge)));
      pil::Edge** table = static_cast<pil::Edge**>(calloc(n, sizeof(pil::Edge*)));
      float* xx = static_cast<float*>(calloc(2 * n, sizeof(float)));
      m.spans = static_cast<int*>(calloc(4 * n, sizeof(int)));  // <= n row spans + n horizontal edges
      if (m.e && table && xx && m.spans) {
        m.n = pil::polygon_edges(m.e, n, xy);
        auto none = [](int, int, int) {};  // the setup pass's horizontal edges are replayed per row
        pil::poly_setup(m.scan, m.e, m.n, table, xx, bm_.h(), none);
        m.nspans = 0;
        m.y = -1 - bm_.h();  // no row cached yet
        const int w2 = width * 2 - 1;
        int i;
        for (i = 0; i < n - 1; i++) wide_line(xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], outline, w2, &m);
        wide_line(xy[i * 2], xy[i * 2 + 1], xy[0], xy[1], outline, w2, &m);
      }
      free(m.spans);
      free(xx);
      free(table);
      free(m.e);
    }
  }
  free(xy);
}

inline void View::rounded_rectangle(double x0, double y0, double x1, double y1, double radius, int fill,
                                    int outline, int width) {
  x0 += ox_, x1 += ox_, y0 += oy_, y1 += oy_;
  if (x1 < x0 || y1 < y0) return;  // Pillow raises ValueError
  // Python min() over (x1 - x0, y1 - y0, radius * 2), still in floats.
  double d = x1 - x0;
  if (y1 - y0 < d) d = y1 - y0;
  if (radius * 2 < d) d = radius * 2;
  const int rx0 = pil::py_round(x0), ry0 = pil::py_round(y0), rx1 = pil::py_round(x1), ry1 = pil::py_round(y1);
  const bool full_x = d >= rx1 - rx0 - 1;
  if (full_x) d = rx1 - rx0;  // the two left and two right corners are joined
  const bool full_y = d >= ry1 - ry0 - 1;
  if (full_y) d = ry1 - ry0;
  if (full_x && full_y) return ellipse_abs(x0, y0, x1, y1, fill, outline, width);  // a circle
  if (d == 0) return rectangle_abs(x0, y0, x1, y1, fill, outline, width);
  // Every corner box is inverted when d < 0, so _draw_pieslice / _draw_arc raise before drawing.
  if (d < 0 || (fill == NONE && (outline == NONE || width == 0))) return;

  const int r = static_cast<int>(floor(d / 2));  // int(d // 2)
  // _draw_pieslice / _draw_arc truncate the part's coordinates; d may still be a float.
  const int dx0 = static_cast<int>(rx0 + d), dy0 = static_cast<int>(ry0 + d);
  const int dx1 = static_cast<int>(rx1 - d), dy1 = static_cast<int>(ry1 - d);
  struct Part {
    int x0, y0, x1, y1;
    float start, end;
  };
  Part parts[4];
  int nparts;
  if (full_x) {
    parts[0] = Part{rx0, ry0, dx0, dy0, 180, 360};
    parts[1] = Part{rx0, dy1, dx0, ry1, 0, 180};
    nparts = 2;
  } else if (full_y) {
    parts[0] = Part{rx0, ry0, dx0, dy0, 90, 270};
    parts[1] = Part{dx1, ry0, rx1, dy0, 270, 90};
    nparts = 2;
  } else {
    parts[0] = Part{rx0, ry0, dx0, dy0, 180, 270};
    parts[1] = Part{dx1, ry0, rx1, dy0, 270, 360};
    parts[2] = Part{dx1, dy1, rx1, ry1, 0, 90};
    parts[3] = Part{rx0, dy1, dx0, ry1, 90, 180};
    nparts = 4;
  }
  // _draw_rectangle raises on an inverted box, which ends the Python call.
  auto filled = [this](int a, int b, int c, int e, int ink) {
    if (c < a || e < b) return false;
    rect(a, b, c, e, ink, true, 0);
    return true;
  };
  if (fill != NONE) {
    for (int i = 0; i < nparts; i++) pieslice_fill(parts[i].x0, parts[i].y0, parts[i].x1, parts[i].y1, parts[i].start, parts[i].end, fill);
    if (full_x) {
      if (!filled(rx0, ry0 + r + 1, rx1, ry1 - r - 1, fill)) return;
    } else if (rx1 - r - 1 >= rx0 + r + 1) {
      if (!filled(rx0 + r + 1, ry0, rx1 - r - 1, ry1, fill)) return;
    }
    if (!full_x && !full_y) {
      if (!filled(rx0, ry0 + r + 1, rx0 + r, ry1 - r - 1, fill)) return;
      if (!filled(rx1 - r, ry0 + r + 1, rx1, ry1 - r - 1, fill)) return;
    }
  }
  if (outline != NONE && outline != fill && width != 0) {
    for (int i = 0; i < nparts; i++) draw_arc(parts[i].x0, parts[i].y0, parts[i].x1, parts[i].y1, parts[i].start, parts[i].end, outline, width);
    if (!full_x) {
      if (!filled(rx0 + r + 1, ry0, rx1 - r - 1, ry0 + width - 1, outline)) return;
      if (!filled(rx0 + r + 1, ry1 - width + 1, rx1 - r - 1, ry1, outline)) return;
    }
    if (!full_y) {
      if (!filled(rx0, ry0 + r + 1, rx0 + width - 1, ry1 - r - 1, outline)) return;
      if (!filled(rx1 - width + 1, ry0 + r + 1, rx1, ry1 - r - 1, outline)) return;
    }
  }
}

}  // namespace ink
