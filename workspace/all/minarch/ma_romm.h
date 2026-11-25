// Larry Boy Advanced: "RomM Saves" in the in-game menu.
//
// Lists the game's saves on the RomM server in the panel where Save and Load
// show their slots, and puts the one picked in place of the game's own, which
// larry-sync backs up first. Everything that talks to the server runs in the
// background, so the menu never stops answering.

#ifndef MA_ROMM_H
#define MA_ROMM_H

#include <SDL2/SDL.h>

// Forgets everything from the last time the menu was open.
void RommSaves_reset(void);
// Starts loading the list if it has not been, for the game at rom_path.
void RommSaves_open(const char* rom_path);
// Call every frame. Returns 1 when something changed that needs drawing.
int RommSaves_poll(void);
// Left and right move through the list.
void RommSaves_move(int delta);
// A: asks to confirm, then, confirmed, starts loading the save picked.
void RommSaves_accept(void);
// B: returns 1 when it was used (to back out of the confirmation), 0 when the
// menu should treat it as usual.
int RommSaves_back(void);
// Whether a save is being put in place, during which the menu must not move.
int RommSaves_busy(void);
// Returns 1, once, after the picked save was put in place: the caller loads it
// into the game and restarts it. message is what to tell the player.
int RommSaves_takeLoaded(char* message, int size);
// The button hints for the current state.
char** RommSaves_hints(void);
// Draws the list, a message, or the confirmation into panel.
void RommSaves_draw(SDL_Surface* screen, SDL_Rect panel);

#endif
