// Larry Boy Advanced: the RomM library behind the game lists.
//
// larry-sync (built from the larry-sync fork of grout) keeps a catalog of the
// whole RomM library on the card and does every download and save transfer.
// This file reads the catalog it writes and runs it as a child process. It
// draws nothing; the screens that use it live in nextui.c.

#ifndef LARRY_H
#define LARRY_H

#include <pthread.h>
#include <sys/types.h>

#include "defines.h"

#define LARRY_HOME SHARED_USERDATA_PATH "/larry"
#define LARRY_CATALOG_DIR LARRY_HOME "/catalog"
#define LARRY_PLATFORMS_FILE LARRY_CATALOG_DIR "/platforms.tsv"
#define LARRY_CONFIG_FILE LARRY_HOME "/config.json"
#define LARRY_SORT_FILE LARRY_HOME "/sort-ready-first"
#define LARRY_WRAPPER SYSTEM_PATH "/bin/larry-sync.sh"
#define LARRY_BINARY SYSTEM_PATH "/bin/larry-sync"
#define LARRY_SERVER_FILE SDCARD_PATH "/romm-server.txt"

// Settings the player can write on the card before first boot. Each is
// applied when its contents change; a fingerprint of what was applied last is
// kept beside the RomM settings.
#define LARRY_WIFI_FILE SDCARD_PATH "/wifi.txt"
#define LARRY_RA_FILE SDCARD_PATH "/retroachievements.txt"
#define LARRY_WIFI_APPLIED LARRY_HOME "/wifi.applied"
#define LARRY_WIFI_LOG LARRY_HOME "/logs/wifi.log"
#define LARRY_RA_APPLIED LARRY_HOME "/retroachievements.applied"

// How NextUI reaches wpa_supplicant on the Brick family.
#define LARRY_WPA_SOCKETS "/etc/wifi/sockets"
#define LARRY_WPA_INTERFACE "wlan0"

// Stands in for a path in the root list: opening it refreshes the library.
#define LARRY_SYNC_PATH SDCARD_PATH "/.larry-sync"
#define LARRY_SYNC_NAME "Sync RomM Library"
// the main-menu item that updates the firmware from its GitHub releases
#define LARRY_UPDATE_PATH SDCARD_PATH "/.larry-update"
#define LARRY_UPDATE_NAME "Check for Updates"
// the main-menu item that makes the device a Bluetooth controller for a TV
#define LARRY_PAD_PATH SDCARD_PATH "/.larry-pad"
#define LARRY_PAD_NAME "Controller Mode"
#define LARRY_PAD_FIFO "/tmp/larry-pad.fifo"
#define LARRY_PAD_ACTIVE "/tmp/larry-pad.active"
#define LARRY_PAD_HOST LARRY_HOME "/pad-host"
// the main-menu item that opens the card to a phone over Wi-Fi
#define LARRY_FILES_PATH SDCARD_PATH "/.larry-files"
#define LARRY_FILES_NAME "File Access"

// Menu music: set up in music.txt, chosen from the main menu, played by a
// player of its own while the menus are up.
#define LARRY_MUSIC_FILE SDCARD_PATH "/music.txt"
#define LARRY_MUSIC_CHOICE LARRY_HOME "/music-choice"
#define LARRY_MUSIC_PLAYER SYSTEM_PATH "/bin/larry-music.elf"
#define LARRY_MUSIC_PID "/tmp/larry-music.pid"
#define LARRY_MUSIC_PAUSED "/tmp/larry-music.paused"
#define LARRY_MUSIC_SOURCE "/tmp/larry-music.source"
#define LARRY_MUSIC_PATH SDCARD_PATH "/.larry-music"

// Exit codes larry-sync uses, so a caller can branch without reading text.
#define LARRY_EXIT_OK 0
#define LARRY_EXIT_FAILED 1
#define LARRY_EXIT_USAGE 2
#define LARRY_EXIT_NOT_PAIRED 3
#define LARRY_EXIT_UNREACHABLE 4
#define LARRY_EXIT_CANCELLED 130

#define LARRY_TEXT 256
#define LARRY_PATH 512

typedef struct LarryPlatform {
	char* fs_slug;
	char* name;
	char* rom_dir;
	int games;
	int downloaded;
} LarryPlatform;

typedef struct LarryGame {
	int rom_id;
	char* name;
	int downloaded;
	char* local_path; // what to launch; "" until downloaded
	char* art_path;   // cover from the RomM cache; "" when there is none
	char* file_name;  // the name the game will have once downloaded
} LarryGame;

// Whether larry-sync is part of this firmware at all. A build made without
// it is plain NextUI.
int Larry_installed(void);
// Whether a server token is stored. Says nothing about whether it still works.
int Larry_isPaired(void);
// Whether a catalog has been written, i.e. the library has synced once.
int Larry_hasCatalog(void);

// Re-reads platforms.tsv. Call after anything that rewrites the catalog.
void Larry_reload(void);
// The platform whose games belong in rom_dir, or NULL.
LarryPlatform* Larry_platformForDir(const char* rom_dir);

// Reads a platform's games. Returns how many; *out is NULL when there are
// none. Free with Larry_freeGames.
int Larry_loadGames(LarryPlatform* platform, LarryGame** out);
void Larry_freeGames(LarryGame* games, int count);

// Whether lists put games that are on the card first.
int Larry_sortReadyFirst(void);
void Larry_setSortReadyFirst(int on);

// Reads the server address from romm-server.txt: the first line that is not
// blank or a comment. Returns 0 when there is none. *insecure is set when a
// line says "insecure".
int Larry_serverAddress(char* out, size_t size, int* insecure);

// Measures how wide a line of text would be drawn, in pixels.
typedef int (*LarryMeasure)(const char* text, void* context);

// Wraps in into out so no line measures wider than max_width. Lines break at
// spaces. A word too long for a line on its own, an address mostly, breaks
// after its last '/', '?', '&', '=' or '-' that fits, and mid-word only when
// it has none, so a split address still reads as two pieces with no gap in
// between to mistake for a space. Line breaks already in the text stay.
void Larry_wrapText(const char* in, char* out, size_t size, int max_width, LarryMeasure measure, void* context);

// The user code in a pairing address (its user_code parameter), or "" if it
// has none.
void Larry_pairCode(const char* url, char* out, size_t size);

// A Wi-Fi network from wifi.txt.
#define LARRY_SSID_MAX 32
#define LARRY_PSK_MAX 64
typedef struct LarryWifiNetwork {
	char ssid[LARRY_SSID_MAX + 1];
	char password[LARRY_PSK_MAX + 1]; // "" for an open network
} LarryWifiNetwork;

// Reads wifi.txt: "network:" lines, each followed by its "password:" line.
// Returns how many networks it holds, up to max. A network it cannot use (a
// password of the wrong length, say) is left out and described in problem.
int Larry_readWifiFile(const char* path, LarryWifiNetwork* out, int max, char* problem, size_t problem_size);

// Reads retroachievements.txt. Returns 1 when it has both a username and a
// password.
int Larry_readRAFile(const char* path, char* username, size_t username_size, char* password, size_t password_size);

// After a game, launch.sh sends its save to RomM in the background and leaves
// what larry-sync said here once it is done.
#define LARRY_PUSH_RESULT "/tmp/larry-push.done"
// Reads that result into a line to show, and clears it. Returns 1 when there
// is something to show.
int Larry_readPushResult(const char* path, char* message, size_t size);

// Which music music.txt sets up: game soundtracks, and Johnny Music.
void Larry_readMusicFile(const char* path, int* game, int* johnny);
// The music chosen to play in the menus: "game", "johnny" or "off".
void Larry_musicChoice(char* out, size_t size);
void Larry_setMusicChoice(const char* choice);
// Plays the chosen music if it is set up: starts the player, or has it carry
// on after a pause. Anything else stops it.
void Larry_musicPlay(void);
// Plays source ("game" or "johnny") if music.txt sets it up, whatever was
// chosen for the menus: the in-game menu's music. Returns 1 if it plays.
int Larry_musicPlaySource(const char* source);
// Pauses the player so a game can have the speaker, waiting up to wait_ms
// for it to let go.
void Larry_musicPause(int wait_ms);
// Stops the player.
void Larry_musicStop(void);

// Whether path's contents are what was last marked applied in stamp. A file
// that does not exist counts as applied, so deleting one changes nothing.
int Larry_isApplied(const char* path, const char* stamp);
void Larry_markApplied(const char* path, const char* stamp);

// Waits up to timeout_ms for wpa_supplicant to answer. Returns 1 if it did.
int Larry_wifiReady(int timeout_ms);
// Saves networks in wpa_supplicant's configuration, replacing any saved under
// the same name, and lets it join whichever is in range. Returns how many were
// saved, or -1 if wpa_supplicant could not be reached.
int Larry_wifiSave(const LarryWifiNetwork* networks, int count);

// What came of connecting after networks were saved.
typedef enum {
	LARRY_WIFI_CONNECTED,      // joined a network; its name is in ssid
	LARRY_WIFI_WRONG_PASSWORD, // a network turned the password down; its name is in ssid
	LARRY_WIFI_NONE_IN_RANGE,  // never got as far as trying a password
} LarryWifiOutcome;

// Watches wpa_supplicant for up to timeout_ms, or until should_stop returns
// non-zero, and says how connecting went. should_stop may be NULL; it is
// called between checks, about every half second.
LarryWifiOutcome Larry_wifiWatch(int timeout_ms, char* ssid, size_t ssid_size, int (*should_stop)(void));

// One of a game's saves on the server, from list-saves.
#define LARRY_SAVES_MAX 48
typedef struct LarrySave {
	int id;
	char slot[32];
	long long updated; // unix seconds
	char age[32];      // "9 days ago", worked out by larry-sync
	long long bytes;
	int newest;        // the newest in its slot
	int on_device;     // the one this device has
	char devices[96];  // devices that hold it
	char emulator[32];
	char file[160];
	int has_shot;          // RomM has a picture of it
	char shot[LARRY_PATH]; // where that picture is on the card, once fetched
} LarrySave;

// A running larry-sync command and what it has reported so far. The fields
// below lock are written by a reader thread; read them through
// LarryJob_snapshot.
typedef struct LarryJob {
	pid_t pid;
	int fd;
	pthread_t thread;
	pthread_mutex_t lock;

	char stage[64];
	char detail[LARRY_TEXT];
	char info[LARRY_TEXT];
	char error[LARRY_TEXT];
	int percent;
	long long done;
	long long total;
	char rom_path[LARRY_PATH];
	char pair_url[LARRY_PATH];
	char files_url[LARRY_PATH]; // File Access: the page's address, with its key
	char pad_state[16];         // controller mode: waiting, connected
	char pad_host[32];          // the TV's Bluetooth address
	// what the save check found: -1 when it did not report
	int saves_downloaded;
	int saves_conflicts;
	int saves_errors;
	char saves_problem[LARRY_TEXT]; // why saves could not be checked
	// list-saves: the saves listed, newest first
	LarrySave saves[LARRY_SAVES_MAX];
	int save_count;
	int saves_listed; // the list is whole; pictures may still be coming
	// choose-save: where the chosen save went
	char save_path[LARRY_PATH];
	int promoted;
	// update check / download
	char update_current[96];
	char update_latest[96];
	char update_date[32];
	long long update_size;
	int update_available;
	char update_ready[LARRY_PATH];
	int finished;
	int exit_code;
	int cancelled;
} LarryJob;

// Starts larry-sync with the given arguments (NULL-terminated). Returns NULL
// if it could not be started.
LarryJob* LarryJob_start(const char* const* args);
// Copies the reported state, safe to read while the job runs.
void LarryJob_snapshot(LarryJob* job, LarryJob* out);
// Asks the command to stop. It removes any partial download on the way out.
void LarryJob_cancel(LarryJob* job);
// Waits for the command to end and frees the job.
void LarryJob_free(LarryJob* job);

#endif
