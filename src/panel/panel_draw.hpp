/*
 * Software drawing for the dialogue panel: an RGB565 canvas in display
 * orientation (320 x 240), plus CJK-aware line wrapping.
 *
 * The 3DS stores the bottom framebuffer rotated; PanelController rotates at
 * blit time so everything here works in ordinary screen coordinates, which
 * are also the coordinates touch input arrives in.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "panel_font.hpp"

typedef uint16_t Color;

constexpr Color rgb(int r, int g, int b) {
    return (Color)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

class Canvas {
  public:
    Canvas(uint16_t *pixels, int width, int height);

    int width() const { return w; }
    int height() const { return h; }

    // Drawing is clipped to this rectangle (and always to the canvas).
    void clip(int x, int y, int cw, int ch);
    void unclip();

    void fill(int x, int y, int fw, int fh, Color c);
    // Rounded corners of radius r, anti-aliased against what is underneath.
    void fill_round(int x, int y, int fw, int fh, int r, Color c);
    // Mixes c over a rectangle at alpha 0..255 (used for scrims and fades).
    void blend(int x, int y, int fw, int fh, Color c, int alpha);
    void hline(int x, int y, int len, Color c);
    void vline(int x, int y, int len, Color c);
    // Small filled triangle pointing down, top edge at y, centred on cx.
    void triangle_down(int cx, int y, int half_w, Color c);

    // Draws one line of text with its line box's top at y; returns the
    // advance. alpha < 255 fades the whole string.
    int text(PanelFont &f, int x, int y, const char *utf8, Color c,
             int alpha = 255);

  private:
    void put(int x, int y, Color c, int alpha);
    bool inside(int x, int y) const {
        return x >= cx0 && y >= cy0 && x < cx1 && y < cy1;
    }

    uint16_t *px;
    int w, h;
    int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
};

Color mix(Color dst, Color src, int alpha);

// Breaks text into lines no wider than `width`, with the first line limited
// to `first_width` (for a speaker name that precedes it on the same line).
//
// CJK can break between any two characters, but closing punctuation must not
// start a line and opening brackets must not end one (kinsoku). Closing
// punctuation that would overflow is allowed to hang into the margin by up to
// `hang` pixels rather than dragging a character down with it. Runs of ASCII
// letters and digits stay together when they fit on a line.
std::vector<std::string> wrap_text(PanelFont &f, const std::string &text,
                                   int width, int first_width, int hang);

// Cuts text to fit in width, adding "…" if anything was dropped.
std::string ellipsize(PanelFont &f, const std::string &text, int width);
