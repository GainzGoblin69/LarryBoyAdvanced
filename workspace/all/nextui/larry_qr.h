// Larry Boy Advanced: QR codes on screen, for the RomM pairing address.
//
// Encoding is Project Nayuki's QR Code generator (qrcodegen.c, MIT licence),
// vendored unchanged from https://github.com/nayuki/QR-Code-generator at
// commit 9a217d443937f60cc56c1458c39a211d7f2d3af5.

#ifndef LARRY_QR_H
#define LARRY_QR_H

#include <SDL2/SDL.h>

// Draws text as a QR code, black on white with the quiet border scanners
// need, as large as fits in a max_size square, centred on x = center_x with
// its top at y = top. Returns the height drawn, or 0 if the text is too long
// to encode.
int LarryQR_draw(SDL_Surface* dst, const char* text, int center_x, int top, int max_size);

// The height LarryQR_draw would draw text at, without drawing it: 0 if it
// cannot be drawn in max_size.
int LarryQR_size(const char* text, int max_size);

#endif
