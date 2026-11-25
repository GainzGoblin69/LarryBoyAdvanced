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
// settings files on the card

// Splits "key: value" into a lowercased key and a trimmed value, in place.
// Returns 0 for a line that is blank, a comment, or not of that form.
static int keyValue(char* line, char** key, char** value) {
	chomp(line);
	char* start = line;
	while (*start == ' ' || *start == '\t') start++;
	if (!*start || *start == '#') return 0;

	char* colon = strchr(start, ':');
	if (!colon) return 0;
	*colon = '\0';

	char* end = colon;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
	for (char* c = start; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';

	char* v = colon + 1;
	while (*v == ' ' || *v == '\t') v++;
	end = v + strlen(v);
	while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

	// "a value in quotes" is taken as what is between them
	size_t len = strlen(v);
	if (len >= 2 && ((v[0] == '"' && v[len - 1] == '"') || (v[0] == '\'' && v[len - 1] == '\''))) {
		v[len - 1] = '\0';
		v++;
	}

	*key = start;
	*value = v;
	return 1;
}

static int isHex64(const char* s) {
	if (strlen(s) != 64) return 0;
	for (; *s; s++) {
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return 0;
	}
	return 1;
}

static void addProblem(char* problem, size_t size, const char* text) {
	if (!problem || !size || problem[0]) return; // the first one is enough
	copyText(problem, size, text);
}

// Adds the network being read, if it can be used. A later entry with the same
// name replaces an earlier one.
static int finishNetwork(LarryWifiNetwork* current, int have, LarryWifiNetwork* out, int count, int max, char* problem, size_t problem_size) {
	if (!have || !current->ssid[0]) return count;

	size_t len = strlen(current->password);
	if (len && (len < 8 || len > 63) && !isHex64(current->password)) {
		char text[LARRY_TEXT];
		snprintf(text, sizeof(text), "The password for %s must be 8 to 63 characters.", current->ssid);
		addProblem(problem, problem_size, text);
		return count;
	}

	for (int i = 0; i < count; i++) {
		if (strcmp(out[i].ssid, current->ssid) == 0) {
			out[i] = *current;
			return count;
		}
	}
	if (count >= max) return count;
	out[count] = *current;
	return count + 1;
}

int Larry_readWifiFile(const char* path, LarryWifiNetwork* out, int max, char* problem, size_t problem_size) {
	if (problem && problem_size) problem[0] = '\0';
	FILE* file = fopen(path, "r");
	if (!file) return 0;

	int count = 0;
	int have = 0;         // reading a network
	int has_password = 0; // and its password line has been read
	LarryWifiNetwork current;
	memset(&current, 0, sizeof(current));

	char line[LARRY_TEXT];
	while (fgets(line, sizeof(line), file)) {
		char *key, *value;
		if (!keyValue(line, &key, &value)) continue;

		if (!strcmp(key, "network") || !strcmp(key, "name") || !strcmp(key, "ssid")) {
			count = finishNetwork(&current, have, out, count, max, problem, problem_size);
			memset(&current, 0, sizeof(current));
			have = 1;
			has_password = 0;
			if (strlen(value) > LARRY_SSID_MAX) {
				char text[LARRY_TEXT];
				snprintf(text, sizeof(text), "The network name %.40s is longer than Wi-Fi allows.", value);
				addProblem(problem, problem_size, text);
				have = 0;
				continue;
			}
			copyText(current.ssid, sizeof(current.ssid), value);
		}
		else if (!strcmp(key, "password") || !strcmp(key, "pass")) {
			// Each network takes the first password after its name. Another,
			// with no network of its own, is a slip and changes nothing.
			if (!have || has_password) continue;
			has_password = 1;
			if (strlen(value) > LARRY_PSK_MAX) {
				char text[LARRY_TEXT];
				snprintf(text, sizeof(text), "The password for %s must be 8 to 63 characters.", current.ssid);
				addProblem(problem, problem_size, text);
				have = 0;
				continue;
			}
			copyText(current.password, sizeof(current.password), value);
		}
	}
	count = finishNetwork(&current, have, out, count, max, problem, problem_size);
	fclose(file);
	return count;
}

int Larry_readRAFile(const char* path, char* username, size_t username_size, char* password, size_t password_size) {
	copyText(username, username_size, "");
	copyText(password, password_size, "");
	FILE* file = fopen(path, "r");
	if (!file) return 0;

	char line[LARRY_TEXT];
	while (fgets(line, sizeof(line), file)) {
		char *key, *value;
		if (!keyValue(line, &key, &value)) continue;
		if (!strcmp(key, "username") || !strcmp(key, "user")) copyText(username, username_size, value);
		else if (!strcmp(key, "password") || !strcmp(key, "pass")) copyText(password, password_size, value);
	}
	fclose(file);
	return username[0] && password[0];
}

// FNV-1a over the file's bytes, as 16 hex digits after a format number; ""
// if it cannot be read. Raising the number has every file applied once more
// on cards updated to the new firmware: 2 brought reporting on Wi-Fi.
static void fingerprint(const char* path, char* out, size_t size) {
	copyText(out, size, "");
	FILE* file = fopen(path, "rb");
	if (!file) return;
	unsigned long long hash = 1469598103934665603ULL;
	int c;
	while ((c = fgetc(file)) != EOF) {
		hash ^= (unsigned char)c;
		hash *= 1099511628211ULL;
	}
	fclose(file);
	snprintf(out, size, "2-%016llx", hash);
}

int Larry_isApplied(const char* path, const char* stamp) {
	char now[32];
	fingerprint(path, now, sizeof(now));
	if (!now[0]) return 1;

	char applied[32] = "";
	FILE* file = fopen(stamp, "r");
	if (file) {
		if (fgets(applied, sizeof(applied), file)) chomp(applied);
		fclose(file);
	}
	return strcmp(now, applied) == 0;
}

void Larry_markApplied(const char* path, const char* stamp) {
	char now[32];
	fingerprint(path, now, sizeof(now));
	mkdir(LARRY_HOME, 0755);
	FILE* file = fopen(stamp, "w");
	if (!file) return;
	fprintf(file, "%s\n", now);
	fclose(file);
}

///////////////////////////////////////
// Wi-Fi, through wpa_cli

// The Wi-Fi log on the card, for working out a network that will not
// connect. Passwords never go in it.
static FILE* wifiLog(void) {
	static FILE* log = NULL;
	if (!log) {
		mkdir(LARRY_HOME, 0755);
		mkdir(LARRY_HOME "/logs", 0755);
		// start afresh past 256 KB so it cannot grow without end
		struct stat info;
		int fresh = stat(LARRY_WIFI_LOG, &info) == 0 && info.st_size > 256 * 1024;
		log = fopen(LARRY_WIFI_LOG, fresh ? "w" : "a");
		if (log) setvbuf(log, NULL, _IOLBF, 0);
	}
	return log;
}

static void logLine(const char* text) {
	FILE* log = wifiLog();
	if (log) fprintf(log, "%s\n", text);
}

// Describes a password without giving it away: its length, and the slips
// that most often stop one working.
static void describePassword(const char* psk, char* out, size_t size) {
	char inner[LARRY_PSK_MAX + 1];
	size_t len = strlen(psk);
	if (len >= 2 && psk[0] == '"' && psk[len - 1] == '"') {
		snprintf(inner, sizeof(inner), "%.*s", (int)(len - 2), psk + 1);
	}
	else {
		snprintf(out, size, "<a %zu-digit key>", len);
		return;
	}
	len = strlen(inner);
	snprintf(out, size, "<%zu characters%s%s%s%s>", len,
		strchr(inner, ' ') ? ", with spaces" : "",
		strchr(inner, '"') || strchr(inner, '\'') ? ", with quote marks" : "",
		len && (inner[0] == ' ' || inner[len - 1] == ' ') ? ", space at an end" : "",
		strchr(inner, '\\') ? ", with a backslash" : "");
}

// Runs wpa_cli with the given command words, no shell in between, so names
// and passwords need no quoting. Returns its exit status, -1 if it could not
// run; its output goes to out. Each command and its answer go in the log,
// except the status checks Larry_wifiWatch makes, which it logs as changes.
static int wpaCli(char* out, size_t size, const char* const* words) {
	if (size) out[0] = '\0';
	int quiet = words[0] && (!strcmp(words[0], "status") || !strcmp(words[0], "ping"));

	const char* argv[16] = { "wpa_cli", "-p", LARRY_WPA_SOCKETS, "-i", LARRY_WPA_INTERFACE };
	int argc = 5;
	for (int i = 0; words[i] && argc < 15; i++) argv[argc++] = words[i];
	argv[argc] = NULL;

	int pipefd[2];
	if (pipe(pipefd) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		for (int fd = 3; fd < 256; fd++) close(fd);
		execvp("wpa_cli", (char* const*)argv);
		_exit(127);
	}
	close(pipefd[1]);

	size_t used = 0;
	char buffer[512];
	ssize_t n;
	while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0 || (n < 0 && errno == EINTR)) {
		if (n <= 0) continue;
		if (used + 1 < size) {
			size_t take = (size_t)n;
			if (take > size - 1 - used) take = size - 1 - used;
			memcpy(out + used, buffer, take);
			used += take;
			out[used] = '\0';
		}
	}
	close(pipefd[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

	if (!quiet) {
		char line[LARRY_TEXT * 2] = "wpa_cli";
		for (int i = 0; words[i]; i++) {
			char word[LARRY_TEXT];
			if (i == 3 && !strcmp(words[0], "set_network") && !strcmp(words[2], "psk")) describePassword(words[i], word, sizeof(word));
			else snprintf(word, sizeof(word), "%s", words[i]);
			size_t used = strlen(line);
			snprintf(line + used, sizeof(line) - used, " %s", word);
		}
		char answer[128];
		snprintf(answer, sizeof(answer), "%.*s", (int)strcspn(out, "\n"), out);
		size_t used = strlen(line);
		snprintf(line + used, sizeof(line) - used, "  ->  %s (exit %d)", strchr(out, '\n') && strchr(out, '\n')[1] ? "(several lines)" : answer, code);
		logLine(line);
	}
	return code;
}

static int wpaOK(const char* out) {
	return strncmp(out, "OK", 2) == 0;
}

int Larry_wifiReady(int timeout_ms) {
	char out[64];
	for (int waited = 0; ; waited += 250) {
		const char* ping[] = { "ping", NULL };
		if (wpaCli(out, sizeof(out), ping) == 0 && strncmp(out, "PONG", 4) == 0) return 1;
		if (waited >= timeout_ms) return 0;
		usleep(250 * 1000);
	}
}

// The ids of saved networks called ssid; returns how many, up to max.
static int savedIds(const char* ssid, int* ids, int max) {
	char list[8192];
	const char* words[] = { "list_networks", NULL };
	if (wpaCli(list, sizeof(list), words) != 0) return 0;

	int count = 0;
	char* line = strchr(list, '\n'); // the first line is a header
	while (line && count < max) {
		line++;
		char* next = strchr(line, '\n');
		if (next) *next = '\0';

		// id \t ssid \t bssid \t flags
		char* fields[4];
		if (splitTabs(line, fields, 4) >= 2 && fields[0][0] >= '0' && fields[0][0] <= '9' && !strcmp(fields[1], ssid)) {
			ids[count++] = atoi(fields[0]);
		}
		line = next;
	}
	return count;
}

// Adds one network, after removing any saved under its name. Returns 1 if
// wpa_supplicant took it.
static int saveNetwork(const LarryWifiNetwork* network) {
	char out[256];
	char id[16];

	int ids[8];
	int existing = savedIds(network->ssid, ids, 8);
	for (int i = 0; i < existing; i++) {
		snprintf(id, sizeof(id), "%d", ids[i]);
		const char* remove[] = { "remove_network", id, NULL };
		wpaCli(out, sizeof(out), remove);
	}

	const char* add[] = { "add_network", NULL };
	if (wpaCli(out, sizeof(out), add) != 0 || out[0] < '0' || out[0] > '9') return 0;
	snprintf(id, sizeof(id), "%d", atoi(out));

	// The name goes in as hex, which wpa_supplicant takes for any characters.
	char ssid[LARRY_SSID_MAX * 2 + 1];
	size_t len = strlen(network->ssid);
	for (size_t i = 0; i < len; i++) snprintf(ssid + i * 2, 3, "%02x", (unsigned char)network->ssid[i]);
	const char* set_ssid[] = { "set_network", id, "ssid", ssid, NULL };
	int ok = wpaCli(out, sizeof(out), set_ssid) == 0 && wpaOK(out);

	if (ok && network->password[0]) {
		// a passphrase in quotes; 64 hex digits is a key and goes in bare
		char psk[LARRY_PSK_MAX + 3];
		if (isHex64(network->password)) snprintf(psk, sizeof(psk), "%s", network->password);
		else snprintf(psk, sizeof(psk), "\"%s\"", network->password);
		const char* set_psk[] = { "set_network", id, "psk", psk, NULL };
		ok = wpaCli(out, sizeof(out), set_psk) == 0 && wpaOK(out);
	}
	else if (ok) {
		const char* set_open[] = { "set_network", id, "key_mgmt", "NONE", NULL };
		ok = wpaCli(out, sizeof(out), set_open) == 0 && wpaOK(out);
	}

	if (ok) {
		const char* enable[] = { "enable_network", id, NULL };
		ok = wpaCli(out, sizeof(out), enable) == 0 && wpaOK(out);
	}
	if (!ok) {
		const char* remove[] = { "remove_network", id, NULL };
		wpaCli(out, sizeof(out), remove);
	}
	return ok;
}

int Larry_wifiSave(const LarryWifiNetwork* networks, int count) {
	logLine("--- saving networks from wifi.txt");
	for (int i = 0; i < count; i++) {
		char line[LARRY_TEXT * 2], quoted[LARRY_PSK_MAX + 3], described[LARRY_TEXT];
		snprintf(quoted, sizeof(quoted), "\"%s\"", networks[i].password);
		if (networks[i].password[0]) describePassword(isHex64(networks[i].password) ? networks[i].password : quoted, described, sizeof(described));
		else snprintf(described, sizeof(described), "<none: an open network>");
		snprintf(line, sizeof(line), "network \"%s\" (%zu bytes), password %s", networks[i].ssid, strlen(networks[i].ssid), described);
		logLine(line);
	}
	if (!Larry_wifiReady(0)) {
		logLine("wpa_supplicant is not answering");
		return -1;
	}

	int saved = 0;
	for (int i = 0; i < count; i++) saved += saveNetwork(&networks[i]);

	char out[64];
	const char* save[] = { "save_config", NULL };
	if (wpaCli(out, sizeof(out), save) != 0 || !wpaOK(out)) return -1;

	// Only acts when not already connected: then it looks for any saved
	// network in range, new ones included.
	const char* reconnect[] = { "reconnect", NULL };
	wpaCli(out, sizeof(out), reconnect);

	char list[4096];
	const char* words[] = { "list_networks", NULL };
	wpaCli(list, sizeof(list), words);
	logLine("saved networks now:");
	logLine(list);
	return saved;
}

// Reads a "key=value" line from wpa_cli status output.
static void statusField(const char* status, const char* key, char* out, size_t size) {
	copyText(out, size, "");
	size_t klen = strlen(key);
	for (const char* line = status; line && *line; ) {
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			const char* v = line + klen + 1;
			snprintf(out, size, "%.*s", (int)strcspn(v, "\n"), v);
			return;
		}
		line = strchr(line, '\n');
		if (line) line++;
	}
}

LarryWifiOutcome Larry_wifiWatch(int timeout_ms, char* ssid, size_t ssid_size, int (*should_stop)(void)) {
	copyText(ssid, ssid_size, "");
	char last_state[64] = "";
	char tried[LARRY_SSID_MAX + 1] = ""; // a network that got as far as the password
	int failures = 0;
	int handshaking = 0;

	for (int waited = 0; waited <= timeout_ms; waited += 500) {
		char status[2048], state[64], name[LARRY_SSID_MAX * 4 + 1];
		const char* words[] = { "status", NULL };
		wpaCli(status, sizeof(status), words);
		statusField(status, "wpa_state", state, sizeof(state));
		statusField(status, "ssid", name, sizeof(name));

		if (strcmp(state, last_state)) {
			char line[LARRY_TEXT];
			snprintf(line, sizeof(line), "after %d.%d s: %s %s", waited / 1000, (waited % 1000) / 100, state[0] ? state : "(no answer)", name);
			logLine(line);
			copyText(last_state, sizeof(last_state), state);
		}

		if (!strcmp(state, "COMPLETED")) {
			copyText(ssid, ssid_size, name);
			logLine("connected");
			return LARRY_WIFI_CONNECTED;
		}

		// The password is checked in the 4-way handshake. Falling back from
		// it to looking for a network, twice, means it was turned down.
		if (!strcmp(state, "4WAY_HANDSHAKE") || !strcmp(state, "GROUP_HANDSHAKE")) {
			handshaking = 1;
			if (name[0]) copyText(tried, sizeof(tried), name);
		}
		else if (handshaking && (!strcmp(state, "DISCONNECTED") || !strcmp(state, "SCANNING") || !strcmp(state, "INACTIVE"))) {
			handshaking = 0;
			if (++failures >= 2) break;
		}

		if (should_stop && should_stop()) break;
		usleep(500 * 1000);
	}

	if (failures > 0) {
		copyText(ssid, ssid_size, tried);
		logLine("a network turned the password down");
		return LARRY_WIFI_WRONG_PASSWORD;
	}
	logLine("no saved network connected");
	return LARRY_WIFI_NONE_IN_RANGE;
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
		else if (strcmp(fields[1], "saves_downloaded") == 0) job->saves_downloaded = atoi(fields[2]);
		else if (strcmp(fields[1], "saves_conflicts") == 0) job->saves_conflicts = atoi(fields[2]);
		else if (strcmp(fields[1], "saves_errors") == 0) job->saves_errors = atoi(fields[2]);
		else if (strcmp(fields[1], "saves_problem") == 0) copyText(job->saves_problem, sizeof(job->saves_problem), fields[2]);
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
	job->saves_downloaded = job->saves_conflicts = job->saves_errors = -1;
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
