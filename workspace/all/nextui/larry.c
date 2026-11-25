// Larry Boy Advanced: reading the catalog larry-sync writes, and running it.
// See larry.h.

#include "larry.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils.h"

///////////////////////////////////////
// small helpers

static char* dupField(const char* s) {
	return strdup(s ? s : "");
}

// Splits a line on tabs in place, keeping empty fields, which strtok would
// merge. Returns the number of fields found, at most max.
static int splitTabs(char* line, char** fields, int max) {
	int count = 0;
	char* cursor = line;
	while (count < max) {
		fields[count++] = cursor;
		char* tab = strchr(cursor, '\t');
		if (!tab) break;
		*tab = '\0';
		cursor = tab + 1;
	}
	return count;
}

static void chomp(char* line) {
	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
}

// Compares two directory paths, ignoring trailing slashes.
static int sameDir(const char* a, const char* b) {
	size_t la = strlen(a), lb = strlen(b);
	while (la > 1 && a[la - 1] == '/') la--;
	while (lb > 1 && b[lb - 1] == '/') lb--;
	return la == lb && strncmp(a, b, la) == 0;
}

static void copyText(char* dst, size_t size, const char* src) {
	if (!size) return;
	snprintf(dst, size, "%s", src ? src : "");
}

///////////////////////////////////////
// text

static int isBreakAfter(char c) {
	return c == '/' || c == '?' || c == '&' || c == '=' || c == '-';
}

void Larry_wrapText(const char* in, char* out, size_t size, int max_width, LarryMeasure measure, void* context) {
	if (!size) return;
	out[0] = '\0';

	// positions in out
	size_t o = 0;
	size_t line_start = 0;
	size_t after_space = 0; // just past the line's last space that fits; 0 when none
	size_t after_soft = 0;  // just past its last break-after character that fits

	for (size_t i = 0; in[i] && o + 2 < size; i++) {
		char c = in[i];
		out[o++] = c;
		out[o] = '\0';

		if (c == '\n') {
			line_start = o;
			after_space = after_soft = 0;
			continue;
		}

		// measure only at the end of a character, never inside UTF-8
		if (((unsigned char)in[i + 1] & 0xC0) == 0x80) continue;

		// Break points are recorded only once the text up to them is known
		// to fit, so the character that overflows is never taken for one.
		if (measure(out + line_start, context) <= max_width) {
			if (c == ' ') after_space = o;
			else if (isBreakAfter(c)) after_soft = o;
			continue;
		}

		if (c == ' ') {
			// the space that overflowed becomes the break
			out[o - 1] = '\n';
			line_start = o;
			after_space = after_soft = 0;
			continue;
		}

		if (after_space > line_start) {
			out[after_space - 1] = '\n';
			line_start = after_space;
			after_space = 0;
			if (after_soft <= line_start) after_soft = 0;
		}
		else {
			// No space: break inside the word, after a break-after character
			// if one fits, else before the character that overflowed.
			size_t at;
			if (after_soft > line_start) {
				at = after_soft;
			}
			else {
				at = o - 1;
				while (at > line_start && ((unsigned char)out[at] & 0xC0) == 0x80) at--;
			}
			if (at <= line_start) continue; // one character wider than the line
			if (o + 1 >= size) break;

			memmove(out + at + 1, out + at, o - at + 1); // the terminator too
			out[at] = '\n';
			o++;
			line_start = at + 1;
			after_space = after_soft = 0;
		}

		// the character that overflowed now ends a shorter line
		if (isBreakAfter(c) && o > line_start) after_soft = o;
	}
}

void Larry_pairCode(const char* url, char* out, size_t size) {
	if (!size) return;
	out[0] = '\0';
	const char* key = "user_code=";
	const char* at = url ? strstr(url, key) : NULL;
	if (!at) return;
	at += strlen(key);
	size_t len = strcspn(at, "&#");
	if (len >= size) len = size - 1;
	memcpy(out, at, len);
	out[len] = '\0';
}

///////////////////////////////////////
// state on the card

int Larry_installed(void) {
	return exists(LARRY_BINARY) && exists(LARRY_WRAPPER);
}

int Larry_hasCatalog(void) {
	return exists(LARRY_PLATFORMS_FILE);
}

int Larry_isPaired(void) {
	FILE* file = fopen(LARRY_CONFIG_FILE, "r");
	if (!file) return 0;

	char* text = NULL;
	size_t size = 0;
	ssize_t len = getdelim(&text, &size, '\0', file);
	fclose(file);
	if (len <= 0 || !text) {
		free(text);
		return 0;
	}

	// The settings are written by Go's indenting encoder, so a stored token
	// reads "token": "<value>". token_name and token_expires_at must not count.
	int paired = 0;
	const char* key = "\"token\"";
	for (char* at = strstr(text, key); at && !paired; at = strstr(at + 1, key)) {
		char* p = at + strlen(key);
		while (*p == ' ' || *p == '\t') p++;
		if (*p != ':') continue;
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		if (*p == '"' && p[1] != '"' && p[1] != '\0') paired = 1;
	}
	free(text);
	return paired;
}

int Larry_sortReadyFirst(void) {
	return exists(LARRY_SORT_FILE);
}

void Larry_setSortReadyFirst(int on) {
	if (on) {
		mkdir(LARRY_HOME, 0755);
		FILE* file = fopen(LARRY_SORT_FILE, "w");
		if (file) fclose(file);
	}
	else {
		unlink(LARRY_SORT_FILE);
	}
}

int Larry_serverAddress(char* out, size_t size, int* insecure) {
	if (insecure) *insecure = 0;
	if (size) out[0] = '\0';

	FILE* file = fopen(LARRY_SERVER_FILE, "r");
	if (!file) return 0;

	char line[LARRY_PATH];
	while (fgets(line, sizeof(line), file)) {
		chomp(line);
		char* start = line;
		while (*start == ' ' || *start == '\t') start++;
		char* end = start + strlen(start);
		while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
		if (!*start || *start == '#') continue;

		if (strcasecmp(start, "insecure") == 0) {
			if (insecure) *insecure = 1;
			continue;
		}
		if (!out[0]) copyText(out, size, start);
	}
	fclose(file);
	return out[0] != '\0';
}

///////////////////////////////////////
// the catalog

static LarryPlatform* platforms = NULL;
static int platform_count = 0;
static int platforms_loaded = 0;

static void freePlatforms(void) {
	for (int i = 0; i < platform_count; i++) {
		free(platforms[i].fs_slug);
		free(platforms[i].name);
		free(platforms[i].rom_dir);
	}
	free(platforms);
	platforms = NULL;
	platform_count = 0;
}

void Larry_reload(void) {
	freePlatforms();
	platforms_loaded = 1;

	FILE* file = fopen(LARRY_PLATFORMS_FILE, "r");
	if (!file) return;

	int capacity = 0;
	char* line = NULL;
	size_t size = 0;
	while (getline(&line, &size, file) != -1) {
		chomp(line);
		if (!line[0]) continue;

		// fs_slug  name  rom_directory  games  downloaded
		char* fields[5];
		if (splitTabs(line, fields, 5) < 5) continue;

		if (platform_count == capacity) {
			capacity = capacity ? capacity * 2 : 16;
			platforms = realloc(platforms, sizeof(LarryPlatform) * capacity);
		}
		LarryPlatform* platform = &platforms[platform_count++];
		platform->fs_slug = dupField(fields[0]);
		platform->name = dupField(fields[1]);
		platform->rom_dir = dupField(fields[2]);
		platform->games = atoi(fields[3]);
		platform->downloaded = atoi(fields[4]);
	}
	free(line);
	fclose(file);
}

LarryPlatform* Larry_platformForDir(const char* rom_dir) {
	if (!platforms_loaded) Larry_reload();
	if (!rom_dir) return NULL;
	for (int i = 0; i < platform_count; i++) {
		if (platforms[i].games > 0 && sameDir(platforms[i].rom_dir, rom_dir)) return &platforms[i];
	}
	return NULL;
}

// Turns a platform slug into the name of its games file, the same way
// larry-sync names it.
static void gamesFileFor(const char* fs_slug, char* out, size_t size) {
	char safe[LARRY_TEXT];
	size_t j = 0;
	for (size_t i = 0; fs_slug[i] && j < sizeof(safe) - 1; i++) {
		char c = fs_slug[i];
		if (c == '/' || c == '\\' || c == ':' || c == '\t' || c == '\n' || c == '\r') c = '_';
		safe[j++] = c;
	}
	safe[j] = '\0';

	// leading and trailing dots are trimmed
	char* start = safe;
	while (*start == '.') start++;
	size_t len = strlen(start);
	while (len > 0 && start[len - 1] == '.') start[--len] = '\0';
	if (!*start) start = "_";

	snprintf(out, size, "%s/games/%s.tsv", LARRY_CATALOG_DIR, start);
}

int Larry_loadGames(LarryPlatform* platform, LarryGame** out) {
	*out = NULL;
	if (!platform) return 0;

	char path[LARRY_PATH];
	gamesFileFor(platform->fs_slug, path, sizeof(path));
	FILE* file = fopen(path, "r");
	if (!file) return 0;

	LarryGame* games = NULL;
	int count = 0, capacity = 0;
	char* line = NULL;
	size_t size = 0;
	while (getline(&line, &size, file) != -1) {
		chomp(line);
		if (!line[0]) continue;

		// rom_id  name  downloaded  local_path  art_path  size_bytes  file_name
		char* fields[7];
		if (splitTabs(line, fields, 7) < 7) continue;
		int rom_id = atoi(fields[0]);
		if (rom_id <= 0) continue;

		if (count == capacity) {
			capacity = capacity ? capacity * 2 : 64;
			games = realloc(games, sizeof(LarryGame) * capacity);
		}
		LarryGame* game = &games[count++];
		game->rom_id = rom_id;
		game->name = dupField(fields[1]);
		game->downloaded = fields[2][0] == '1' && fields[3][0] != '\0';
		game->local_path = dupField(fields[3]);
		game->art_path = dupField(fields[4]);
		game->file_name = dupField(fields[6]);
	}
	free(line);
	fclose(file);

	*out = games;
	return count;
}

void Larry_freeGames(LarryGame* games, int count) {
	for (int i = 0; i < count; i++) {
		free(games[i].name);
		free(games[i].local_path);
		free(games[i].art_path);
		free(games[i].file_name);
	}
	free(games);
}

///////////////////////////////////////
// running larry-sync

static void handleLine(LarryJob* job, char* line) {
	char* fields[4] = { 0 };
	int count = splitTabs(line, fields, 4);
	if (count < 1) return;
	const char* kind = fields[0];

	pthread_mutex_lock(&job->lock);
	if (strcmp(kind, "STAGE") == 0) {
		copyText(job->stage, sizeof(job->stage), count > 1 ? fields[1] : "");
		copyText(job->detail, sizeof(job->detail), count > 2 ? fields[2] : "");
		job->percent = 0;
		job->done = 0;
		job->total = 0;
	}
	else if (strcmp(kind, "PROGRESS") == 0 && count >= 4) {
		job->percent = atoi(fields[1]);
		job->done = atoll(fields[2]);
		job->total = atoll(fields[3]);
	}
	else if (strcmp(kind, "INFO") == 0 && count >= 2) {
		copyText(job->info, sizeof(job->info), fields[1]);
	}
	else if (strcmp(kind, "ERROR") == 0 && count >= 2) {
		copyText(job->error, sizeof(job->error), fields[1]);
	}
	else if (strcmp(kind, "RESULT") == 0 && count >= 3) {
		if (strcmp(fields[1], "rom_path") == 0) copyText(job->rom_path, sizeof(job->rom_path), fields[2]);
		else if (strcmp(fields[1], "pair_url") == 0) copyText(job->pair_url, sizeof(job->pair_url), fields[2]);
	}
	pthread_mutex_unlock(&job->lock);
}

static void* readJob(void* arg) {
	LarryJob* job = arg;
	FILE* stream = fdopen(job->fd, "r");
	if (stream) {
		char* line = NULL;
		size_t size = 0;
		while (getline(&line, &size, stream) != -1) {
			chomp(line);
			handleLine(job, line);
		}
		free(line);
		fclose(stream); // closes job->fd
	}
	else {
		close(job->fd);
	}
	job->fd = -1;

	int status = 0;
	int code = LARRY_EXIT_FAILED;
	while (waitpid(job->pid, &status, 0) == -1 && errno == EINTR) {}
	if (WIFEXITED(status)) code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);

	pthread_mutex_lock(&job->lock);
	if (job->cancelled) code = LARRY_EXIT_CANCELLED;
	job->exit_code = code;
	job->finished = 1;
	pthread_mutex_unlock(&job->lock);
	return NULL;
}

LarryJob* LarryJob_start(const char* const* args) {
	int pipefd[2];
	if (pipe(pipefd) != 0) return NULL;

	int argc = 0;
	while (args[argc]) argc++;
	char** argv = calloc(argc + 2, sizeof(char*));
	argv[0] = LARRY_WRAPPER;
	for (int i = 0; i < argc; i++) argv[i + 1] = (char*)args[i];

	pid_t pid = fork();
	if (pid < 0) {
		free(argv);
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		// Its own process group, so cancelling reaches the wrapper and
		// whatever it is waiting on as well as larry-sync itself.
		setpgid(0, 0);
		dup2(pipefd[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO); // larry-sync logs to its own file
		}
		for (int fd = 3; fd < 256; fd++) close(fd);
		signal(SIGPIPE, SIG_DFL);
		execv(LARRY_WRAPPER, argv);
		_exit(127);
	}

	setpgid(pid, pid); // also from here, in case the parent wins the race
	free(argv);
	close(pipefd[1]);

	LarryJob* job = calloc(1, sizeof(LarryJob));
	job->pid = pid;
	job->fd = pipefd[0];
	job->exit_code = -1;
	pthread_mutex_init(&job->lock, NULL);
	if (pthread_create(&job->thread, NULL, readJob, job) != 0) {
		kill(-pid, SIGTERM);
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		pthread_mutex_destroy(&job->lock);
		free(job);
		return NULL;
	}
	return job;
}

void LarryJob_snapshot(LarryJob* job, LarryJob* out) {
	pthread_mutex_lock(&job->lock);
	memcpy(out, job, sizeof(LarryJob));
	pthread_mutex_unlock(&job->lock);
}

void LarryJob_cancel(LarryJob* job) {
	pthread_mutex_lock(&job->lock);
	int running = !job->finished;
	if (running) job->cancelled = 1;
	pthread_mutex_unlock(&job->lock);
	if (running) kill(-job->pid, SIGTERM);
}

void LarryJob_free(LarryJob* job) {
	if (!job) return;
	pthread_join(job->thread, NULL);
	pthread_mutex_destroy(&job->lock);
	free(job);
}
