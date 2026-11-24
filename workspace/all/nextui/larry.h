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

// Stands in for a path in the root list: opening it refreshes the library.
#define LARRY_SYNC_PATH SDCARD_PATH "/.larry-sync"
#define LARRY_SYNC_NAME "Sync RomM Library"

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
