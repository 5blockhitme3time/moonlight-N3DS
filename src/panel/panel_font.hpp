/*
 * Glyph rasterisation for the dialogue panel.
 *
 * citro2d's system font has no CJK coverage, so the panel rasterises a subset
 * of Noto Sans SC with FreeType (already linked by the project). The subset
 * holds the characters galgames use - GB2312, Big5 level 1, JIS X 0208 and
 * the usual symbols, about 11 800 (console/fonts/makesubset.py) - in 3.8 MB
 * instead of 19. Anything else comes from a full font the player may put on
 * the SD card (add_fallback), or is drawn as a box.
 *
 * One PanelFont per pixel size. Cached glyphs are 8-bit coverage masks, so a
 * single entry serves every colour the panel draws in.
 *
 * Line metrics are the panel's own rather than the face's: Noto's ascender and
 * line gap are sized for mixed scripts and leave CJK text floating high with
 * too much leading at these sizes. Every size uses the same proportions, so
 * text of different sizes lines up predictably.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H

struct Glyph {
    uint8_t *coverage = nullptr;   // width * rows, 0 = clear, 255 = solid
    int width = 0;
    int rows = 0;
    int left = 0;                  // bitmap offset from the pen position
    int top = 0;                   // bitmap top above the baseline
    int advance = 0;
    bool missing = false;          // in no face: drawn as a box
};

class PanelFont {
  public:
    PanelFont() = default;
    ~PanelFont();
    PanelFont(const PanelFont &) = delete;
    PanelFont &operator=(const PanelFont &) = delete;

    // The first face: the embedded subset. The data must stay valid for
    // the font's lifetime (it is the embedded font, or a buffer the host
    // tool keeps alive).
    bool load(const void *data, size_t size, int pixel_size);
    // A fallback face for characters the ones before it lack, read from a
    // file as glyphs are needed (a full CJK font on the SD card is 15-20 MB,
    // too much to hold in memory next to the video decoder). TTF, TTC (its
    // first face) or OTF. Call after load().
    bool add_fallback(const char *path);
    int fallbacks() const { return nfaces > 0 ? nfaces - 1 : 0; }
    bool ready() const { return nfaces > 0; }

    int size() const { return px; }
    int line_height() const { return line_px; }   // baseline to baseline
    int baseline() const { return base_px; }      // from the top of a line

    // Memoised. The pointer stays valid until the next trim().
    const Glyph *glyph(uint32_t cp);
    int advance(uint32_t cp);
    int text_width(const char *utf8);

    // Drops the cache if it has grown past its budget. Called between frames,
    // never while glyph pointers are in use.
    void trim();

    // Decodes one code point and advances the pointer; returns 0 at the end.
    // Malformed bytes are skipped.
    static uint32_t next(const char **utf8);

  private:
    void clear();
    bool set_size(FT_Face f);

    static const int kMaxFaces = 3;
    FT_Library lib = nullptr;
    FT_Face faces[kMaxFaces] = {};
    int nfaces = 0;
    int px = 0;
    int line_px = 0;
    int base_px = 0;
    std::unordered_map<uint32_t, Glyph *> cache;
};
