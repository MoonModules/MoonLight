#pragma once

#include "light/effects/EffectBase.h"

#include "light/powerfunctions/fonts.h"             // fonts::kAll: the selectable bitmap fonts

namespace mm {

// Author: MoonLight original, on the predecessor's Scrolling Text, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Effect rendering a scrolling multi-line string in a bitmap font.
/// @card TextEffect.gif
///
/// The text is static by default, laid out from the top left and clipped at the grid's edge.
/// `scroll` marches the whole block leftward as a marquee instead, wrapping as it goes.
/// Each newline drops a row, so several lines render at once.
///
/// Prior art: MoonLight's Scrolling Text, whose font set and scroll this carries.
class TextEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Draws on the z=0 plane, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The string to show, where the text area preserves its newlines.
    char    text_[128] = "MoonLight";
    /// March the block leftward as a marquee rather than holding it still.
    bool    scroll = false;
    /// Which bitmap font renders it.
    uint8_t font   = 1;
    /// How fast the marquee runs, when scrolling.
    uint8_t speed  = 30;
    /// The palette index the text takes, mid-palette since some palettes start black.
    uint8_t hue    = 128;

    /// Publish the string, the scroll, the font and the color.
    void defineControls() override {
        controls_.addTextArea("text", text_, sizeof(text_));
        controls_.addControl("scroll", scroll);
        static constexpr const char* kFontOptions[] = {"4x6", "6x8"};
        controls_.addSelect("font", font, kFontOptions, fonts::kCount);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("hue", hue, 0, 255);
    }

    /// Draw the text, either in place or marched along its marquee cycle.
    void tick() MM_NONBLOCKING override {
        const int w = width();

        const draw::Canvas cv = canvas();
        const fonts::Font& f = fonts::kAll[font < fonts::kCount ? font : 0];
        const RGB color = colorFromPalette(*Palettes::active(), hue);

        draw::fill(cv, {0, 0, 0});   // the text is redrawn whole each frame

        if (!scroll) {
            // From the top left, where each newline drops a row.
            draw::text(cv, f, text_, 0, 0, color);
            return;
        }

        // The offset wraps over the block plus the grid, so the text re-enters seamlessly.
        const int blockW = pixelWidth(text_, f);
        const int span = blockW + w;                          // one cycle: it clears, then re-enters
        const int off = span > 0 ? static_cast<int>((elapsed() * static_cast<uint32_t>(speed) / 1000u) % span) : 0;
        // Running from off the right edge to fully off the left.
        const lengthType startX = static_cast<lengthType>(w - off);
        draw::text(cv, f, text_, startX, 0, color);
    }

private:
    /// The widest line's pixel width, which sizes the marquee's wrap cycle.
    static int pixelWidth(const char* str, const fonts::Font& f) {
        int widest = 0, cur = 0;
        for (const char* p = str; *p; p++) {
            if (*p == '\n') { if (cur > widest) widest = cur; cur = 0; }
            else cur += f.width;
        }
        return cur > widest ? cur : widest;
    }
};

}  // namespace mm
