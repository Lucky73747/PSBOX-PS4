// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// PSBOX screen-and-four-buttons artwork; see assets/ps4/icon0.svg.
#pragma once

#include "ui/canvas.h"
#include "ui/font.h"

namespace xc::ui {

constexpr Color kBrandGreen = rgba(168, 196, 207);

// Original monochrome screen-and-controls symbol.
void drawBrandMark(Canvas& c, float cx, float cy, float r, Color color = rgba(255, 255, 255));

// The square app icon on a charcoal field.
void drawAppIcon(Canvas& c, const Fonts& fonts);

// PS5 home-screen artwork, any 16:9 size (3840x2160 for sce_sys):
// `launch` false: pic0, the selected-app background (mark on the right, the
// left kept dark for the Shell's title and Play button);
// `launch` true: pic1, the launch transition (mark and name centred).
void drawHomeArt(Canvas& c, const Fonts& fonts, bool launch);

}  // namespace xc::ui
