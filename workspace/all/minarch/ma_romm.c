// Larry Boy Advanced: "RomM Saves" in the in-game menu. See ma_romm.h.

#include "ma_romm.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "../nextui/larry.h"

typedef enum {
	ROMM_IDLE,     // not opened yet this time the menu is up
	ROMM_LOADING,  // list-saves running
	ROMM_LIST,     // a list to pick from
	ROMM_EMPTY,    // the server has none for the game
	ROMM_ERROR,    // something went wrong; message says what
	ROMM_CONFIRM,  // asking before replacing the game's save
	ROMM_WORKING,  // choose-save running
	ROMM_LOADED,   // the save is in place; waiting for the caller to take it
} RommState;

static struct {
	RommState state;
	char rom_path[LARRY_PATH];
	LarryJob* job;
	LarrySave saves[LARRY_SAVES_MAX];
	int count;
	int selected;
	char message[LARRY_TEXT * 2];
	char loaded_message[LARRY_TEXT];
	int taken;
	int listing; // the job running is list-saves, still fetching pictures
} romm;

// The selected save's picture, fitted to the card, kept between frames.
static struct {
	SDL_Surface* image;
	char path[LARRY_PATH];
	int w, h; // the card it was fitted to
	int failed;
} picture;

static void dropPicture(void) {
	if (picture.image) SDL_FreeSurface(picture.image);
	memset(&picture, 0, sizeof(picture));
}

static void stopJob(void) {
	if (!romm.job) return;
	LarryJob_cancel(romm.job);
	LarryJob_free(romm.job);
	romm.job = NULL;
	romm.listing = 0;
}

void RommSaves_reset(void) {
	stopJob();
	dropPicture();
	memset(&romm, 0, sizeof(romm));
	romm.state = ROMM_IDLE;
}

// What a command that failed means, in words fit for the panel.
static void describeFailure(LarryJob* result, const char* doing) {
	switch (result->exit_code) {
		case LARRY_EXIT_UNREACHABLE:
			snprintf(romm.message, sizeof(romm.message), "Could not reach RomM to %s. Check Wi-Fi, then press A to try again.", doing);
			break;
		case LARRY_EXIT_NOT_PAIRED:
			snprintf(romm.message, sizeof(romm.message), "This device is not linked to RomM. Link it from Sync RomM Library on the main menu.");
			break;
		default:
			if (strstr(result->error, "not on this device"))
				snprintf(romm.message, sizeof(romm.message), "Could not match this game's file to RomM. Delete the game, download it again from the library, then try again.");
			else if (result->error[0])
				snprintf(romm.message, sizeof(romm.message), "Could not %s: %s", doing, result->error);
			else
				snprintf(romm.message, sizeof(romm.message), "Could not %s (code %d). Press A to try again.", doing, result->exit_code);
	}
	romm.state = ROMM_ERROR;
}

static void startList(void) {
	stopJob();
	romm.count = 0;
	romm.selected = 0;
	if (!Larry_installed()) {
		snprintf(romm.message, sizeof(romm.message), "RomM is not set up on this device.");
		romm.state = ROMM_ERROR;
		return;
	}
	const char* args[] = { "list-saves", "--path", romm.rom_path, NULL };
	romm.job = LarryJob_start(args);
	if (!romm.job) {
		snprintf(romm.message, sizeof(romm.message), "Could not start larry-sync.");
		romm.state = ROMM_ERROR;
		return;
	}
	romm.state = ROMM_LOADING;
	romm.listing = 1;
}

void RommSaves_open(const char* rom_path) {
	if (romm.state != ROMM_IDLE) return;
	snprintf(romm.rom_path, sizeof(romm.rom_path), "%s", rom_path);
	startList();
}

// Takes the list from the job, starting on the save the device has, so left
// and right are relative to what is being played.
static void takeList(LarryJob* now) {
	romm.count = now->save_count;
	memcpy(romm.saves, now->saves, sizeof(LarrySave) * romm.count);
	romm.state = romm.count ? ROMM_LIST : ROMM_EMPTY;
	romm.selected = 0;
	for (int i = 0; i < romm.count; i++) {
		if (romm.saves[i].on_device) {
			romm.selected = i;
			break;
		}
	}
}

int RommSaves_poll(void) {
	if (!romm.job) return 0;

	// LarryJob is large; one copy, kept off the stack.
	static LarryJob now;
	LarryJob_snapshot(romm.job, &now);
	int changed = 0;

	// The list comes first and can be chosen from while pictures follow.
	if (romm.state == ROMM_LOADING && now.saves_listed) {
		takeList(&now);
		changed = 1;
	}
	if (romm.listing && romm.state != ROMM_LOADING) {
		for (int i = 0; i < romm.count && i < now.save_count; i++) {
			if (now.saves[i].id == romm.saves[i].id && strcmp(now.saves[i].shot, romm.saves[i].shot)) {
				memcpy(romm.saves[i].shot, now.saves[i].shot, sizeof(romm.saves[i].shot));
				changed = 1;
			}
		}
	}
	if (!now.finished) return changed;

	int was_listing = romm.listing;
	LarryJob_free(romm.job);
	romm.job = NULL;
	romm.listing = 0;

	if (romm.state == ROMM_LOADING) {
		if (now.exit_code != LARRY_EXIT_OK) describeFailure(&now, "list the saves");
		else takeList(&now);
		return 1;
	}
	if (was_listing) return 1; // pictures done; a picture that failed just stays missing

	if (romm.state == ROMM_WORKING) {
		if (now.exit_code != LARRY_EXIT_OK) {
			describeFailure(&now, "load that save");
			return 1;
		}
		snprintf(romm.loaded_message, sizeof(romm.loaded_message), "Loaded the save from %s", romm.saves[romm.selected].age);
		romm.state = ROMM_LOADED;
		romm.taken = 0;
		return 1;
	}
	return changed;
}

void RommSaves_move(int delta) {
	if (romm.state != ROMM_LIST || romm.count == 0) return;
	// like the save slots: stops at the ends rather than wrapping
	int next = romm.selected + delta;
	if (next < 0 || next >= romm.count) return;
	romm.selected = next;
}

void RommSaves_keepShot(SDL_Surface* frame, const char* tag, const char* name) {
	if (!frame || !tag || !tag[0] || !name || !name[0] || !Larry_installed()) return;

	char dir[LARRY_PATH];
	snprintf(dir, sizeof(dir), SHARED_USERDATA_PATH "/larry");
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), SHARED_USERDATA_PATH "/larry/shots");
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), SHARED_USERDATA_PATH "/larry/shots/%s", tag);
	mkdir(dir, 0755);

	// half size: plenty for a card, and quick to send
	int w = frame->w / 2, h = frame->h / 2;
	if (w < 1 || h < 1) return;
	SDL_Surface* small = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!small) return;
	SDL_BlendMode mode;
	SDL_GetSurfaceBlendMode(frame, &mode);
	SDL_SetSurfaceBlendMode(frame, SDL_BLENDMODE_NONE);
	SDL_BlitScaled(frame, NULL, small, NULL);
	SDL_SetSurfaceBlendMode(frame, mode);

	char path[LARRY_PATH * 2], partial[LARRY_PATH * 2 + 8];
	snprintf(path, sizeof(path), "%s/%s.png", dir, name);
	snprintf(partial, sizeof(partial), "%s.part", path);
	if (IMG_SavePNG(small, partial) == 0) rename(partial, path);
	else remove(partial);
	SDL_FreeSurface(small);
}

void RommSaves_accept(void) {
	switch (romm.state) {
		case ROMM_LIST:
			romm.state = ROMM_CONFIRM;
			break;
		case ROMM_CONFIRM: {
			char save_id[16];
			snprintf(save_id, sizeof(save_id), "%d", romm.saves[romm.selected].id);
			const char* args[] = { "choose-save", "--path", romm.rom_path, "--save", save_id, NULL };
			stopJob();
			romm.job = LarryJob_start(args);
			if (!romm.job) {
				snprintf(romm.message, sizeof(romm.message), "Could not start larry-sync.");
				romm.state = ROMM_ERROR;
				break;
			}
			romm.state = ROMM_WORKING;
			break;
		}
		case ROMM_ERROR:
		case ROMM_EMPTY:
			startList(); // try again
			break;
		default:
			break;
	}
}

int RommSaves_back(void) {
	if (romm.state == ROMM_CONFIRM) {
		romm.state = ROMM_LIST;
		return 1;
	}
	return 0;
}

int RommSaves_busy(void) {
	return romm.state == ROMM_WORKING;
}

int RommSaves_takeLoaded(char* message, int size) {
	if (romm.state != ROMM_LOADED || romm.taken) return 0;
	romm.taken = 1;
	snprintf(message, size, "%s", romm.loaded_message);
	return 1;
}

char** RommSaves_hints(void) {
	static char* list[] = { "B", "BACK", "A", "LOAD", NULL };
	static char* confirm[] = { "B", "CANCEL", "A", "LOAD", NULL };
	static char* retry[] = { "B", "BACK", "A", "RETRY", NULL };
	static char* wait[] = { "B", "BACK", NULL };
	static char* none[] = { NULL };
	switch (romm.state) {
		case ROMM_LIST: return list;
		case ROMM_CONFIRM: return confirm;
		case ROMM_ERROR:
		case ROMM_EMPTY: return retry;
		case ROMM_WORKING: return none;
		default: return wait;
	}
}

///////////////////////////////////////
// drawing

static int measure(const char* text, void* context) {
	char line[512];
	size_t len = strcspn(text, "\n");
	if (len >= sizeof(line)) len = sizeof(line) - 1;
	memcpy(line, text, len);
	line[len] = '\0';
	int w = 0;
	TTF_SizeUTF8((TTF_Font*)context, line, &w, NULL);
	return w;
}

// Draws text wrapped to width, from y, each line centred in the panel.
// Returns the y below it.
static int drawWrapped(SDL_Surface* screen, TTF_Font* ttf, const char* text, SDL_Color color, SDL_Rect panel, int y) {
	char wrapped[1024];
	Larry_wrapText(text, wrapped, sizeof(wrapped), panel.w - SCALE1(PADDING * 2), measure, ttf);
	int line_h = TTF_FontHeight(ttf);
	for (char* line = wrapped; line && *line; ) {
		char* next = strchr(line, '\n');
		if (next) *next = '\0';
		if (*line) {
			SDL_Surface* rendered = TTF_RenderUTF8_Blended(ttf, line, color);
			if (rendered) {
				SDL_BlitSurface(rendered, NULL, screen, &(SDL_Rect){ panel.x + (panel.w - rendered->w) / 2, y });
				SDL_FreeSurface(rendered);
			}
		}
		y += line_h;
		line = next ? next + 1 : NULL;
	}
	return y;
}

// Draws text centred in the panel, as a block.
static void drawMessage(SDL_Surface* screen, const char* text, SDL_Rect panel) {
	char wrapped[1024];
	Larry_wrapText(text, wrapped, sizeof(wrapped), panel.w - SCALE1(PADDING * 2), measure, font.small);
	int lines = 1;
	for (char* c = wrapped; *c; c++) if (*c == '\n') lines++;
	int y = panel.y + (panel.h - lines * TTF_FontHeight(font.small)) / 2;
	drawWrapped(screen, font.small, text, COLOR_WHITE, panel, y);
}

static void formatSize(long long bytes, char* out, size_t size) {
	if (bytes >= 1024 * 1024) snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024) snprintf(out, size, "%lld KB", (bytes + 512) / 1024);
	else snprintf(out, size, "%lld B", bytes);
}

static void blitText(SDL_Surface* screen, TTF_Font* ttf, const char* text, SDL_Color color, int x, int y, int max_w, int align_right) {
	char fitted[256];
	GFX_truncateText(ttf, text, fitted, max_w, 0);
	SDL_Surface* rendered = TTF_RenderUTF8_Blended(ttf, fitted, color);
	if (!rendered) return;
	int w = rendered->w < max_w ? rendered->w : max_w;
	SDL_BlitSurface(rendered, &(SDL_Rect){ 0, 0, w, rendered->h }, screen, &(SDL_Rect){ align_right ? x + max_w - w : x, y });
	SDL_FreeSurface(rendered);
}

// The save's picture fitted inside w by h, keeping its shape, loaded once
// and kept while the same save stays selected. NULL when there is none.
static SDL_Surface* pictureFor(LarrySave* save, int w, int h, SDL_PixelFormat* format) {
	if (!save->shot[0]) return NULL;
	if (!strcmp(picture.path, save->shot) && picture.w == w && picture.h == h)
		return picture.failed ? NULL : picture.image;

	dropPicture();
	snprintf(picture.path, sizeof(picture.path), "%s", save->shot);
	picture.w = w;
	picture.h = h;

	SDL_Surface* loaded = IMG_Load(save->shot);
	if (!loaded || loaded->w < 1 || loaded->h < 1) {
		if (loaded) SDL_FreeSurface(loaded);
		picture.failed = 1;
		return NULL;
	}
	SDL_Surface* converted = SDL_ConvertSurface(loaded, format, 0);
	SDL_FreeSurface(loaded);
	if (!converted) {
		picture.failed = 1;
		return NULL;
	}

	// fit, keeping the picture's shape: a widescreen one from a PC is
	// letterboxed rather than squashed
	int fit_w = w, fit_h = converted->h * w / converted->w;
	if (fit_h > h) {
		fit_h = h;
		fit_w = converted->w * h / converted->h;
	}
	if (fit_w < 1) fit_w = 1;
	if (fit_h < 1) fit_h = 1;
	picture.image = SDL_CreateRGBSurfaceWithFormat(0, fit_w, fit_h, format->BitsPerPixel, format->format);
	if (picture.image) {
		SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_NONE);
		SDL_BlitScaled(converted, NULL, picture.image, NULL);
	}
	SDL_FreeSurface(converted);
	if (!picture.image) picture.failed = 1;
	return picture.image;
}

// Darkens a band of the screen so text over a picture can be read.
static void shade(SDL_Surface* screen, SDL_Rect band) {
	SDL_Surface* veil = SDL_CreateRGBSurfaceWithFormat(0, band.w, band.h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!veil) return;
	SDL_FillRect(veil, NULL, SDL_MapRGBA(veil->format, 0, 0, 0, 180));
	SDL_SetSurfaceBlendMode(veil, SDL_BLENDMODE_BLEND);
	SDL_BlitSurface(veil, NULL, screen, &band);
	SDL_FreeSurface(veil);
}

// One save at a time, as a card the size of a save slot's preview: its
// picture, when it has one, with when it is from along the bottom, and dots
// below for where it is among the game's saves, as the slots have.
static void drawCard(SDL_Surface* screen, SDL_Rect panel) {
	SDL_Color dim = { 190, 190, 190, 255 };
	LarrySave* save = &romm.saves[romm.selected];
	int pad = SCALE1(PADDING);
	// the menu gives a panel the size of a slot preview plus the row of dots
	SDL_Rect card = { panel.x, panel.y, panel.w, panel.h - SCALE1(10) };

	// the picture, or what stands in for it
	SDL_Surface* image = pictureFor(save, card.w, card.h, screen->format);
	if (image) {
		SDL_BlitSurface(image, NULL, screen, &(SDL_Rect){ card.x + (card.w - image->w) / 2, card.y + (card.h - image->h) / 2 });
	}
	else {
		const char* stand_in = save->has_shot && !picture.failed && romm.listing ? "Loading Picture..." : "No Picture";
		SDL_Rect above = { card.x, card.y, card.w, card.h - SCALE1(40) };
		GFX_blitMessage(font.large, (char*)stand_in, screen, &above);
	}

	// when it is from, and how big, along the bottom
	int line1 = TTF_FontHeight(font.small);
	int line2 = TTF_FontHeight(font.tiny);
	SDL_Rect band = { card.x, card.y + card.h - line1 - line2 - pad, card.w, line1 + line2 + pad };
	shade(screen, band);

	int x = card.x + pad;
	int w = card.w - pad * 2;
	int y = band.y + pad / 2;
	char size[32];
	formatSize(save->bytes, size, sizeof(size));
	int size_w = 0;
	TTF_SizeUTF8(font.tiny, size, &size_w, NULL);
	blitText(screen, font.small, save->age, COLOR_WHITE, x, y, w - size_w - pad, 0);
	blitText(screen, font.tiny, size, dim, x + w - size_w, y + (line1 - line2) / 2, size_w, 0);
	y += line1;

	char from[160];
	if (save->devices[0]) snprintf(from, sizeof(from), "%s", save->devices);
	else if (save->emulator[0]) snprintf(from, sizeof(from), "%s", save->emulator);
	else snprintf(from, sizeof(from), "RomM");
	char details[256];
	snprintf(details, sizeof(details), "%s  |  %s%s", from, save->slot, save->newest ? ", newest" : "");
	blitText(screen, font.tiny, details, dim, x, y, w, 0);

	// the one the game has now, marked in the corner
	if (save->on_device) {
		const char* mark = "On this device";
		int mark_w = 0;
		TTF_SizeUTF8(font.tiny, mark, &mark_w, NULL);
		SDL_Rect badge = { card.x, card.y, mark_w + pad * 2, line2 + pad };
		shade(screen, badge);
		blitText(screen, font.tiny, mark, COLOR_WHITE, badge.x + pad, badge.y + pad / 2, mark_w, 0);
	}

	// where this one is among them: dots like the slots, or "3 / 14" when
	// there are too many to fit
	int dots_y = card.y + card.h + SCALE1(4);
	int step = SCALE1(15);
	if (romm.count * step <= card.w - pad * 2) {
		int dx = card.x + (card.w - step * romm.count) / 2;
		for (int i = 0; i < romm.count; i++) {
			if (i == romm.selected) GFX_blitAsset(ASSET_PAGE, NULL, screen, &(SDL_Rect){ dx + i * step, dots_y });
			else GFX_blitAsset(ASSET_DOT, NULL, screen, &(SDL_Rect){ dx + i * step + 4, dots_y + SCALE1(2) });
		}
	}
	else {
		char where[32];
		snprintf(where, sizeof(where), "%d / %d", romm.selected + 1, romm.count);
		int where_w = 0;
		TTF_SizeUTF8(font.tiny, where, &where_w, NULL);
		int where_y = card.y + card.h + (panel.y + panel.h - card.y - card.h - line2) / 2;
		blitText(screen, font.tiny, where, dim, card.x + (card.w - where_w) / 2, where_y, where_w, 0);
	}
}

void RommSaves_draw(SDL_Surface* screen, SDL_Rect panel) {
	SDL_FillRect(screen, &panel, SDL_MapRGBA(screen->format, 0, 0, 0, 255));

	switch (romm.state) {
		case ROMM_IDLE:
		case ROMM_LOADING:
			drawMessage(screen, "Loading saves from RomM...", panel);
			break;
		case ROMM_EMPTY:
			drawMessage(screen, "RomM has no saves for this game yet.", panel);
			break;
		case ROMM_ERROR:
			drawMessage(screen, romm.message, panel);
			break;
		case ROMM_LIST:
			drawCard(screen, panel);
			break;
		case ROMM_CONFIRM: {
			char text[LARRY_TEXT * 2];
			LarrySave* save = &romm.saves[romm.selected];
			snprintf(text, sizeof(text),
				"Load the save from %s?\n\nThe game restarts with it. Your current save is backed up on this card first.",
				save->age);
			drawMessage(screen, text, panel);
			break;
		}
		case ROMM_WORKING:
		case ROMM_LOADED:
			drawMessage(screen, "Loading the save...", panel);
			break;
	}
}
