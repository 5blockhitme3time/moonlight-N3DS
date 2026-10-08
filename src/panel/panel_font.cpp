/*
 * Glyph rasterisation for the dialogue panel - implementation.
 */
#include "panel_font.hpp"

#include <cstdlib>
#include <cstring>

namespace {
// A full chapter uses a few hundred distinct characters per size; this is a
// ceiling against pathological text, not a working-set estimate.
const size_t kCacheBudget = 3000;
}  // namespace

PanelFont::~PanelFont() {
    clear();
    for (int i = 0; i < nfaces; i++) {
        FT_Done_Face(faces[i]);
    }
    if (lib) {
        FT_Done_FreeType(lib);
    }
}

bool PanelFont::set_size(FT_Face f) {
    return FT_Set_Pixel_Sizes(f, 0, px) == 0;
}

void PanelFont::clear() {
    for (auto &kv : cache) {
        free(kv.second->coverage);
        delete kv.second;
    }
    cache.clear();
}

void PanelFont::trim() {
    if (cache.size() > kCacheBudget) {
        clear();
    }
}

bool PanelFont::load(const void *data, size_t size, int pixel_size) {
    if (nfaces > 0) {
        return false;   // the first face is set once
    }
    if (!lib && FT_Init_FreeType(&lib) != 0) {
        lib = nullptr;
        return false;
    }
    if (!data || size == 0) {
        return false;
    }
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(lib, (const FT_Byte *)data, (FT_Long)size, 0,
                           &face) != 0) {
        return false;
    }
    px = pixel_size;
    if (!set_size(face)) {
        FT_Done_Face(face);
        return false;
    }
    faces[nfaces++] = face;
    // 1.35 em leading with the baseline at 0.88 em centres CJK ink in the line
    line_px = (pixel_size * 135 + 50) / 100;
    base_px = (pixel_size * 88 + 50) / 100 + (line_px - pixel_size) / 2;
    return true;
}

uint32_t PanelFont::next(const char **utf8) {
    const unsigned char *s = (const unsigned char *)*utf8;
    while (*s) {
        uint32_t cp;
        int extra;
        if (s[0] < 0x80) {
            cp = s[0];
            extra = 0;
        } else if ((s[0] & 0xE0) == 0xC0) {
            cp = s[0] & 0x1F;
            extra = 1;
        } else if ((s[0] & 0xF0) == 0xE0) {
            cp = s[0] & 0x0F;
            extra = 2;
        } else if ((s[0] & 0xF8) == 0xF0) {
            cp = s[0] & 0x07;
            extra = 3;
        } else {
            s++;   // stray continuation byte
            continue;
        }
        bool ok = true;
        for (int i = 1; i <= extra; i++) {
            if ((s[i] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        if (!ok) {
            s++;   // truncated sequence
            continue;
        }
        *utf8 = (const char *)(s + extra + 1);
        return cp;
    }
    *utf8 = (const char *)s;
    return 0;
}

bool PanelFont::add_fallback(const char *path) {
    if (nfaces == 0 || nfaces >= kMaxFaces || !path) {
        return false;
    }
    FT_Face face = nullptr;
    if (FT_New_Face(lib, path, 0, &face) != 0) {
        return false;
    }
    if (!set_size(face)) {
        FT_Done_Face(face);
        return false;
    }
    faces[nfaces++] = face;
    // boxes drawn for characters this face may have are stale now
    clear();
    return true;
}

const Glyph *PanelFont::glyph(uint32_t cp) {
    auto it = cache.find(cp);
    if (it != cache.end()) {
        return it->second;
    }
    if (nfaces == 0) {
        return nullptr;
    }

    // the first face that has the character
    FT_Face face = faces[0];
    FT_UInt index = 0;
    for (int i = 0; i < nfaces; i++) {
        index = FT_Get_Char_Index(faces[i], cp);
        if (index != 0) {
            face = faces[i];
            break;
        }
    }

    Glyph *g = new Glyph();
    if (index == 0 && cp > 0x20) {
        // In no face. A visible box says "missing glyph" instead of
        // silently dropping a character from someone's line.
        g->missing = true;
        g->advance = px * 6 / 10;
    } else if (FT_Load_Glyph(face, index,
                             FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap &bm = slot->bitmap;
        g->advance = (int)((slot->advance.x + 32) >> 6);
        g->left = slot->bitmap_left;
        g->top = slot->bitmap_top;
        if (bm.width > 0 && bm.rows > 0 &&
            bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
            g->width = (int)bm.width;
            g->rows = (int)bm.rows;
            g->coverage = (uint8_t *)malloc((size_t)g->width * g->rows);
            if (g->coverage) {
                for (int y = 0; y < g->rows; y++) {
                    memcpy(g->coverage + (size_t)y * g->width,
                           bm.buffer + (ptrdiff_t)y * bm.pitch, g->width);
                }
            } else {
                g->width = g->rows = 0;
            }
        }
    } else {
        g->missing = true;
        g->advance = px * 6 / 10;
    }
    cache[cp] = g;
    return g;
}

int PanelFont::advance(uint32_t cp) {
    const Glyph *g = glyph(cp);
    return g ? g->advance : 0;
}

int PanelFont::text_width(const char *utf8) {
    int w = 0;
    const char *p = utf8;
    for (uint32_t cp = next(&p); cp; cp = next(&p)) {
        w += advance(cp);
    }
    return w;
}
