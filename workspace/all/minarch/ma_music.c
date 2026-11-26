#include "ma_music.h"

#include <string.h>

#include "ma_internal.h"
#include "../nextui/larry.h"

enum { MUSIC_OFF, MUSIC_GAME, MUSIC_JOHNNY };
static int playing = MUSIC_OFF;

static const char* sourceName(int music) {
	return music == MUSIC_JOHNNY ? "johnny" : "game";
}

int GameMusic_available(void) {
	if (!Larry_installed()) return 0;
	int game, johnny;
	Larry_readMusicFile(LARRY_MUSIC_FILE, &game, &johnny);
	return game || johnny;
}

int GameMusic_isOn(void) {
	return playing != MUSIC_OFF;
}

const char* GameMusic_label(void) {
	switch (playing) {
		case MUSIC_GAME: return "GAME MUSIC";
		case MUSIC_JOHNNY: return "JOHNNY MUSIC";
	}
	return "MUSIC OFF";
}

// The game gives up the speaker, and drops its sound until it has it back.
static void gameSoundOff(void) {
	SND_quit();
}

static void gameSoundOn(void) {
	SND_resetAudio(core.sample_rate, core.fps);
}

void GameMusic_cycle(void) {
	int game, johnny;
	Larry_readMusicFile(LARRY_MUSIC_FILE, &game, &johnny);

	// Off, then each music set up, in order
	int next = playing;
	for (int step = 0; step < 3; step++) {
		next = (next + 1) % 3;
		if (next == MUSIC_OFF || (next == MUSIC_GAME && game) || (next == MUSIC_JOHNNY && johnny)) break;
	}
	if (next == playing) return;

	if (next == MUSIC_OFF) {
		Larry_musicPause(1000); // it lets go of the speaker before the game takes it
		playing = MUSIC_OFF;
		gameSoundOn();
		return;
	}
	if (playing == MUSIC_OFF) gameSoundOff();
	playing = next;
	if (!Larry_musicPlaySource(sourceName(next))) {
		// no player, or the music is no longer set up: the game's sound comes back
		playing = MUSIC_OFF;
		gameSoundOn();
	}
}

void GameMusic_beforeSleep(void) {
	if (playing != MUSIC_OFF) Larry_musicPause(500);
}

void GameMusic_afterSleep(void) {
	if (playing != MUSIC_OFF) Larry_musicPlaySource(sourceName(playing));
}
