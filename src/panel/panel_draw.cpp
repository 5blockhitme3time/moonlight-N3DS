/*
 * Software drawing for the dialogue panel - implementation.
 */
#include "panel_draw.hpp"

#include <cmath>
#include <cstring>

namespace {

// Kinsoku: may not begin a line.
const char *kNoStart = "、。，．！？：；）」』】〉》〕”’…‥ー～・,.!?:;)]}%";
// May not end a line.
const char *kNoEnd = "（「『【〈《〔“‘([{";

bool in_set(const char *set, uint32_t cp) {
    const char *p = set;
    for (uint32_t c = PanelFont::next(&p); c; c = PanelFont::next(&p)) {
        if (c == cp) {
            return true;
        }
    }
    return false;
}

bool is_word_char(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z') || cp == '\'' || cp == '-' || cp == '_';
}

struct Unit {
    std::string bytes;
    int width;
    uint32_t first;
    uint32_t last;
    bool space;
};

// Splits text into breakable units: one CJK character, one space, or one
// whole ASCII word.
std::vector<Unit> units_of(PanelFont &f, const std::string &text) {
    std::vector<Unit> out;
    const char *p = text.c_str();
    while (*p) {
        const char *start = p;
        uint32_t cp = PanelFont::next(&p);
        if (!cp) {
            break;
        }
        Unit u;
        u.first = u.last = cp;
        u.width = f.advance(cp);
        u.space = (cp == ' ' || cp == 0x3000);
        if (is_word_char(cp)) {
            const char *q = p;
            for (;;) {
                const char *before = q;
                uint32_t c2 = PanelFont::next(&q);
                if (!c2 || !is_word_char(c2)) {
                    q = before;
                    break;
                }
                u.width += f.advance(c2);
                u.last = c2;
            }
            p = q;
        }
        u.bytes.assign(start, p - start);
        out.push_back(u);
    }
    return out;
}

// Splits a unit too wide for any line into single characters.
void explode(PanelFont &f, const Unit &u, std::vector<Unit> &out) {
    const char *p = u.bytes.c_str();
    while (*p) {
        const char *start = p;
        uint32_t cp = PanelFont::next(&p);
        if (!cp) {
            break;
        }
        Unit c;
        c.bytes.assign(start, p - start);
        c.width = f.advance(cp);
        c.first = c.last = cp;
        c.space = false;
        out.push_back(c);
    }
}

}  // namespace

// ------------------------------------------------------------- colours ----

Color mix(Color dst, Color src, int a) {
    if (a >= 255) {
        return src;
    }
    if (a <= 0) {
        return dst;
    }
    int dr = dst >> 11, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    int sr = src >> 11, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    int r = dr + ((sr - dr) * a + 127) / 255;
    int g = dg + ((sg - dg) * a + 127) / 255;
    int b = db + ((sb - db) * a + 127) / 255;
    return (Color)((r << 11) | (g << 5) | b);
}

// -------------------------------------------------------------- canvas ----

Canvas::Canvas(uint16_t *pixels, int width, int height)
    : px(pixels), w(width), h(height) {
    unclip();
}

void Canvas::clip(int x, int y, int cw, int ch) {
    cx0 = x < 0 ? 0 : x;
    cy0 = y < 0 ? 0 : y;
    cx1 = x + cw > w ? w : x + cw;
    cy1 = y + ch > h ? h : y + ch;
}

void Canvas::unclip() {
    cx0 = cy0 = 0;
    cx1 = w;
    cy1 = h;
}

void Canvas::put(int x, int y, Color c, int alpha) {
    if (!inside(x, y)) {
        return;
    }
    uint16_t &d = px[y * w + x];
    d = mix(d, c, alpha);
}

void Canvas::fill(int x, int y, int fw, int fh, Color c) {
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
    int x1 = x + fw > cx1 ? cx1 : x + fw, y1 = y + fh > cy1 ? cy1 : y + fh;
    for (int j = y0; j < y1; j++) {
        uint16_t *row = px + j * w;
        for (int i = x0; i < x1; i++) {
            row[i] = c;
        }
    }
}

void Canvas::blend(int x, int y, int fw, int fh, Color c, int alpha) {
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
    int x1 = x + fw > cx1 ? cx1 : x + fw, y1 = y + fh > cy1 ? cy1 : y + fh;
    for (int j = y0; j < y1; j++) {
        uint16_t *row = px + j * w;
        for (int i = x0; i < x1; i++) {
            row[i] = mix(row[i], c, alpha);
        }
    }
}

void Canvas::fill_round(int x, int y, int fw, int fh, int r, Color c) {
    if (r * 2 > fh) {
        r = fh / 2;
    }
    if (r * 2 > fw) {
        r = fw / 2;
    }
    if (r <= 0) {
        fill(x, y, fw, fh, c);
        return;
    }
    // middle band, then the rows that carry corners
    fill(x, y + r, fw, fh - 2 * r, c);
    for (int j = 0; j < r; j++) {
        // distance from the corner circle's centre for this row
        float dy = r - j - 0.5f;
        float half = sqrtf((float)r * r - dy * dy);
        float edge = r - half;              // fractional inset of the edge
        int solid = (int)ceilf(edge);
        int frac_a = (int)((solid - edge) * 255.0f);
        int top = y + j, bot = y + fh - 1 - j;
        fill(x + solid, top, fw - 2 * solid, 1, c);
        fill(x + solid, bot, fw - 2 * solid, 1, c);
        if (solid > 0) {
            put(x + solid - 1, top, c, frac_a);
            put(x + fw - solid, top, c, frac_a);
            put(x + solid - 1, bot, c, frac_a);
            put(x + fw - solid, bot, c, frac_a);
        }
    }
}

void Canvas::hline(int x, int y, int len, Color c) { fill(x, y, len, 1, c); }

void Canvas::vline(int x, int y, int len, Color c) { fill(x, y, 1, len, c); }

void Canvas::triangle_down(int cx, int y, int half_w, Color c) {
    for (int j = 0; j <= half_w; j++) {
        int hw = half_w - j;
        fill(cx - hw, y + j, hw * 2 + 1, 1, c);
    }
}

int Canvas::text(PanelFont &f, int x, int y, const char *utf8, Color c,
                 int alpha) {
    int pen = x;
    int base = y + f.baseline();
    const char *p = utf8;
    for (uint32_t cp = PanelFont::next(&p); cp; cp = PanelFont::next(&p)) {
        const Glyph *g = f.glyph(cp);
        if (!g) {
            continue;
        }
        if (g->missing) {
            // hollow box in the glyph's place
            int s = f.size() * 7 / 10, gx = pen + 1, gy = base - s;
            for (int i = 0; i < s; i++) {
                put(gx + i, gy, c, alpha / 2);
                put(gx + i, gy + s - 1, c, alpha / 2);
                put(gx, gy + i, c, alpha / 2);
                put(gx + s - 1, gy + i, c, alpha / 2);
            }
        } else if (g->coverage) {
            int gx = pen + g->left, gy = base - g->top;
            // skip glyphs entirely outside the clip before the inner loop
            if (gx < cx1 && gy < cy1 && gx + g->width > cx0 &&
                gy + g->rows > cy0) {
                for (int j = 0; j < g->rows; j++) {
                    int yy = gy + j;
                    if (yy < cy0 || yy >= cy1) {
                        continue;
                    }
                    const uint8_t *src = g->coverage + j * g->width;
                    uint16_t *row = px + yy * w;
                    for (int i = 0; i < g->width; i++) {
                        int xx = gx + i;
                        int a = src[i];
                        if (a == 0 || xx < cx0 || xx >= cx1) {
                            continue;
                        }
                        if (alpha < 255) {
                            a = a * alpha / 255;
                        }
                        row[xx] = mix(row[xx], c, a);
                    }
                }
            }
        }
        pen += g->advance;
    }
    return pen - x;
}

// ------------------------------------------------------------ wrapping ----

std::vector<std::string> wrap_text(PanelFont &f, const std::string &text,
                                   int width, int first_width, int hang) {
    std::vector<Unit> raw = units_of(f, text);
    std::vector<Unit> units;
    units.reserve(raw.size());
    for (const Unit &u : raw) {
        if (u.width > width && u.bytes.size() > 1) {
            explode(f, u, units);   // a word longer than a whole line
        } else {
            units.push_back(u);
        }
    }

    std::vector<std::string> lines;
    std::vector<const Unit *> cur;
    int cur_w = 0;
    auto limit = [&]() { return lines.empty() ? first_width : width; };
    auto flush = [&]() {
        std::string s;
        for (const Unit *u : cur) {
            s += u->bytes;
        }
        // trailing spaces carry no meaning at a line end
        while (!s.empty() && s.back() == ' ') {
            s.pop_back();
        }
        lines.push_back(s);
        cur.clear();
        cur_w = 0;
    };

    for (size_t i = 0; i < units.size(); i++) {
        const Unit &u = units[i];
        if (cur.empty() && u.space && !lines.empty()) {
            continue;   // no leading space on a continuation line
        }
        if (cur_w + u.width <= limit()) {
            cur.push_back(&u);
            cur_w += u.width;
            continue;
        }
        // Overflow. Closing punctuation may hang into the margin.
        if (in_set(kNoStart, u.first) && cur_w + u.width <= limit() + hang &&
            !cur.empty()) {
            cur.push_back(&u);
            cur_w += u.width;
            continue;
        }
        if (u.space) {
            flush();    // break at the space and drop it
            continue;
        }
        if (cur.empty()) {
            if (lines.empty() && first_width < width) {
                // the first line is too short for anything (a long speaker
                // name): leave it to the name and start the text below
                flush();
                i--;
                continue;
            }
            cur.push_back(&u);   // nothing fits anywhere; take it anyway
            cur_w = u.width;
            flush();
            continue;
        }
        // Do not start the next line with closing punctuation, and do not end
        // this one on an opening bracket: carry units down until both hold.
        std::vector<const Unit *> carry;
        bool bad_start = in_set(kNoStart, u.first);
        while (cur.size() > 1) {
            const Unit *last = cur.back();
            if (!bad_start && !in_set(kNoEnd, last->last)) {
                break;
            }
            carry.insert(carry.begin(), last);
            cur_w -= last->width;
            cur.pop_back();
            bad_start = in_set(kNoStart, last->first);
        }
        flush();
        for (const Unit *c : carry) {
            cur.push_back(c);
            cur_w += c->width;
        }
        cur.push_back(&u);
        cur_w += u.width;
    }
    if (!cur.empty() || lines.empty()) {
        flush();
    }
    return lines;
}

std::string ellipsize(PanelFont &f, const std::string &text, int width) {
    if (f.text_width(text.c_str()) <= width) {
        return text;
    }
    const char *dots = "…";
    int budget = width - f.text_width(dots);
    std::string out;
    int w = 0;
    const char *p = text.c_str();
    while (*p) {
        const char *start = p;
        uint32_t cp = PanelFont::next(&p);
        if (!cp) {
            break;
        }
        int a = f.advance(cp);
        if (w + a > budget) {
            break;
        }
        out.append(start, p - start);
        w += a;
    }
    return out + dots;
}
