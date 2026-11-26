#ifndef MA_MUSIC_H
#define MA_MUSIC_H

// Larry Boy Advanced: your music while you play, from the in-game menu.
//
// X in the in-game menu goes round the music music.txt sets up (Game Music,
// Johnny Music) and Off. While music plays the game's own sound is off and
// the game lets go of the speaker, which the music player takes; Off gives
// it back. Every game starts with it off.

// Whether music.txt sets up any music, so X and its hint are offered.
int GameMusic_available(void);
// Whether music is playing over the game, so the game's sound is dropped.
int GameMusic_isOn(void);
// The hint for X: what is playing now.
const char* GameMusic_label(void);
// X: on to the next music, or off.
void GameMusic_cycle(void);
// Sleep: the music stops with the screen and carries on after.
void GameMusic_beforeSleep(void);
void GameMusic_afterSleep(void);

#endif
