#pragma once

#include <lvgl.h>

LV_FONT_DECLARE(NerdFonts_Regular_20);
LV_FONT_DECLARE(NerdFonts_Regular_40);

// Rounded, heavy-weight face for the small amount of text left on screen.
// SIL OFL 1.1 - see src/fonts/Fredoka-OFL.txt.
LV_FONT_DECLARE(Fredoka_SemiBold_20);

// Punctuation only, for the symbols drifting behind the symbol layer. A label's
// size is its font, so a second size is the only way to draw them larger.
LV_FONT_DECLARE(Fredoka_SemiBold_40);