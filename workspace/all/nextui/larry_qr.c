// Larry Boy Advanced: QR codes on screen. See larry_qr.h.

#include "larry_qr.h"

#include "qrcodegen.h"

// Scanners need four modules of plain white around the code.
#define QUIET_ZONE 4

static uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
static uint8_t temp[qrcodegen_BUFFER_LEN_MAX];

// Encodes text into qrcode and works out the pixels per module that fit
// max_size. Returns 0 when it cannot be drawn.
static int encode(const char* text, int max_size, int* modules, int* scale) {
	// Medium error correction survives a glare or a smudge on the screen and
	// still keeps a pairing address at a size a phone reads from arm's length.
	if (!qrcodegen_encodeText(text, temp, qrcode, qrcodegen_Ecc_MEDIUM,
			qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
		return 0;

	*modules = qrcodegen_getSize(qrcode);
	*scale = max_size / (*modules + QUIET_ZONE * 2); // whole pixels keep edges sharp
	return *scale >= 1;
}

int LarryQR_size(const char* text, int max_size) {
	int modules, scale;
	if (!encode(text, max_size, &modules, &scale)) return 0;
	return (modules + QUIET_ZONE * 2) * scale;
}

int LarryQR_draw(SDL_Surface* dst, const char* text, int center_x, int top, int max_size) {
	int modules, scale;
	if (!encode(text, max_size, &modules, &scale)) return 0;
	int span = modules + QUIET_ZONE * 2;

	int size = span * scale;
	int left = center_x - size / 2;
	Uint32 white = SDL_MapRGB(dst->format, 255, 255, 255);
	Uint32 black = SDL_MapRGB(dst->format, 0, 0, 0);

	SDL_FillRect(dst, &(SDL_Rect){ left, top, size, size }, white);
	for (int y = 0; y < modules; y++) {
		for (int x = 0; x < modules; x++) {
			if (!qrcodegen_getModule(qrcode, x, y)) continue;
			SDL_FillRect(dst, &(SDL_Rect){
				left + (x + QUIET_ZONE) * scale,
				top + (y + QUIET_ZONE) * scale,
				scale, scale,
			}, black);
		}
	}
	return size;
}
