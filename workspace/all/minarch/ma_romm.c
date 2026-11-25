// Larry Boy Advanced: "RomM Saves" in the in-game menu. See ma_romm.h.

#include "ma_romm.h"

#include <stdio.h>
#include <string.h>

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
} romm;

static void stopJob(void) {
	if (!romm.job) return;
	LarryJob_cancel(romm.job);
	LarryJob_free(romm.job);
	romm.job = NULL;
}

void RommSaves_reset(void) {
	stopJob();
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
				snprintf(romm.message, sizeof(romm.message), "This game is not in your RomM library, so it has no saves there.");
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
}

void RommSaves_open(const char* rom_path) {
	if (romm.state != ROMM_IDLE) return;
	snprintf(romm.rom_path, sizeof(romm.rom_path), "%s", rom_path);
	startList();
}

int RommSaves_poll(void) {
	if (!romm.job) return 0;

	// LarryJob is large; one copy, kept off the stack.
	static LarryJob now;
	LarryJob_snapshot(romm.job, &now);
	if (!now.finished) return 0;

	LarryJob_free(romm.job);
	romm.job = NULL;

	if (romm.state == ROMM_LOADING) {
		if (now.exit_code != LARRY_EXIT_OK) {
			describeFailure(&now, "list the saves");
			return 1;
		}
		romm.count = now.save_count;
		memcpy(romm.saves, now.saves, sizeof(LarrySave) * romm.count);
		romm.state = romm.count ? ROMM_LIST : ROMM_EMPTY;
		// start on the save the device has, so left and right are relative
		// to what is being played
		for (int i = 0; i < romm.count; i++) {
			if (romm.saves[i].on_device) {
				romm.selected = i;
				break;
			}
		}
		return 1;
	}

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
	return 0;
}

void RommSaves_move(int delta) {
	if (romm.state != ROMM_LIST || romm.count == 0) return;
	romm.selected = (romm.selected + delta + romm.count) % romm.count;
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

static void drawList(SDL_Surface* screen, SDL_Rect panel) {
	SDL_Color dim = { 160, 160, 160, 255 };
	SDL_Color accent = uintToColour(THEME_COLOR5_255);
	int pad = SCALE1(PADDING);
	int x = panel.x + pad;
	int w = panel.w - pad * 2;
	int y = panel.y + pad / 2;

	// header
	char header[64];
	snprintf(header, sizeof(header), "%d save%s on RomM", romm.count, romm.count == 1 ? "" : "s");
	blitText(screen, font.tiny, header, dim, x, y, w, 0);
	y += TTF_FontHeight(font.tiny) + pad / 2;

	// the selected save's details take the bottom
	int detail_h = TTF_FontHeight(font.tiny) * 2 + pad;
	int row_h = TTF_FontHeight(font.small) + SCALE1(4);
	int rows = (panel.y + panel.h - detail_h - y) / row_h;
	if (rows < 1) rows = 1;

	int first = romm.selected - rows / 2;
	if (first > romm.count - rows) first = romm.count - rows;
	if (first < 0) first = 0;

	for (int i = first; i < romm.count && i < first + rows; i++) {
		LarrySave* save = &romm.saves[i];
		int selected = i == romm.selected;
		if (selected) {
			SDL_FillRect(screen, &(SDL_Rect){ panel.x + pad / 2, y, panel.w - pad, row_h },
				SDL_MapRGB(screen->format, accent.r, accent.g, accent.b));
		}
		SDL_Color color = selected ? (SDL_Color){ 0, 0, 0, 255 } : COLOR_WHITE;

		char size[32], right[96];
		formatSize(save->bytes, size, sizeof(size));
		snprintf(right, sizeof(right), "%s  %s", save->slot, size);
		int right_w = 0;
		TTF_SizeUTF8(font.tiny, right, &right_w, NULL);
		int text_y = y + (row_h - TTF_FontHeight(font.small)) / 2;

		char left[64];
		snprintf(left, sizeof(left), "%s%s", save->on_device ? "* " : "", save->age);
		blitText(screen, font.small, left, color, x, text_y, w - right_w - pad, 0);
		blitText(screen, font.tiny, right, selected ? color : dim, x + w - right_w, text_y + (TTF_FontHeight(font.small) - TTF_FontHeight(font.tiny)) / 2, right_w, 0);
		y += row_h;
	}

	// details of the selected save
	LarrySave* save = &romm.saves[romm.selected];
	int dy = panel.y + panel.h - detail_h + pad / 2;
	char line[256];
	if (save->devices[0]) snprintf(line, sizeof(line), "From %s", save->devices);
	else if (save->emulator[0]) snprintf(line, sizeof(line), "From %s", save->emulator);
	else snprintf(line, sizeof(line), "%s", save->file);
	blitText(screen, font.tiny, line, dim, x, dy, w, 0);
	dy += TTF_FontHeight(font.tiny);
	snprintf(line, sizeof(line), "%s%s%s",
		save->on_device ? "* On this device" : "",
		save->on_device && save->newest ? "  " : "",
		save->newest ? "Newest in its slot" : "");
	if (!line[0]) snprintf(line, sizeof(line), "Older version");
	blitText(screen, font.tiny, line, dim, x, dy, w, 0);
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
			drawList(screen, panel);
			break;
		case ROMM_CONFIRM: {
			char text[LARRY_TEXT * 2];
			LarrySave* save = &romm.saves[romm.selected];
			snprintf(text, sizeof(text),
				"Load the save from %s?\n\nThe game restarts with it. Its save now is backed up first, on this card.",
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
