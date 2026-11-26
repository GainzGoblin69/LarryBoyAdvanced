// Larry Boy Advanced: menu music.
//
//   larry-music.elf game|johnny
//
// Plays songs while the menus are up, one after another, from the folder
// named: game soundtracks or Johnny Music, both on the player's Copyparty
// server and set in music.txt. larry-sync brings each song down while the one
// before it plays; this decodes it (MP3, FLAC, Ogg Vorbis or WAV) and plays
// it through SDL, as the emulator plays a game.
//
// The launcher starts it and talks to it with signals:
//   SIGUSR1  pause: stop and let go of the speaker, for a game or sleep.
//            /tmp/larry-music.paused appears once it has.
//   SIGUSR2  carry on from where it paused.
//   SIGTERM  stop for good.
// Only one runs at a time; its PID is in /tmp/larry-music.pid.

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#ifndef LARRY_WRAPPER
#define LARRY_WRAPPER "/mnt/SDCARD/.system/tg5040/bin/larry-sync.sh"
#endif
#define PID_PATH "/tmp/larry-music.pid"
#define PAUSED_PATH "/tmp/larry-music.paused"
#define NOW_PATH "/tmp/larry-music.now"

// music sits under whatever else is playing; the system volume still applies
#define MUSIC_GAIN 0.55f
#define FADE_MS 1500
// how much is handed to SDL ahead of what is heard
#define QUEUE_MS 300

static volatile sig_atomic_t want_pause = 0;
static volatile sig_atomic_t want_resume = 0;
static volatile sig_atomic_t want_quit = 0;

static void onSignal(int sig) {
	if (sig == SIGUSR1) want_pause = 1;
	else if (sig == SIGUSR2) want_resume = 1;
	else want_quit = 1;
}

static void logLine(const char* format, ...) {
	va_list args;
	va_start(args, format);
	fprintf(stderr, "larry-music: ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	va_end(args);
}

///////////////////////////////////////
// songs coming down

typedef struct {
	char path[1024];
	char title[256];
	char album[256];
} Song;

static struct {
	pthread_mutex_t lock;
	const char* source;
	Song next;     // ready to play, when path is set
	int waiting;   // the player wants another song
	int failures;  // in a row, for waiting longer between tries
} fetch = { PTHREAD_MUTEX_INITIALIZER, NULL, { "", "", "" }, 1, 0 };

static void copyField(char* out, size_t size, const char* value) {
	snprintf(out, size, "%s", value);
	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
}

// Runs larry-sync for one song. Returns 1 with song filled in.
static int fetchOne(Song* song) {
	int pipefd[2];
	if (pipe(pipefd) != 0) return 0;
	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		signal(SIGUSR1, SIG_DFL);
		signal(SIGUSR2, SIG_DFL);
		execl("/bin/sh", "sh", LARRY_WRAPPER, "music", "next", "--source", fetch.source, (char*)NULL);
		_exit(127);
	}
	close(pipefd[1]);

	memset(song, 0, sizeof(*song));
	FILE* out = fdopen(pipefd[0], "r");
	if (out) {
		char line[2048];
		while (fgets(line, sizeof(line), out)) {
			if (strncmp(line, "RESULT\t", 7)) continue;
			char* key = line + 7;
			char* value = strchr(key, '\t');
			if (!value) continue;
			*value++ = '\0';
			if (!strcmp(key, "path")) copyField(song->path, sizeof(song->path), value);
			else if (!strcmp(key, "title")) copyField(song->title, sizeof(song->title), value);
			else if (!strcmp(key, "album")) copyField(song->album, sizeof(song->album), value);
		}
		fclose(out);
	}
	else close(pipefd[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 && song->path[0];
}

static void* fetcher(void* unused) {
	(void)unused;
	while (!want_quit) {
		pthread_mutex_lock(&fetch.lock);
		int need = fetch.waiting && !fetch.next.path[0];
		pthread_mutex_unlock(&fetch.lock);
		if (!need) {
			SDL_Delay(200);
			continue;
		}

		Song song;
		if (fetchOne(&song)) {
			pthread_mutex_lock(&fetch.lock);
			fetch.next = song;
			fetch.failures = 0;
			pthread_mutex_unlock(&fetch.lock);
			continue;
		}

		// offline, or the server is down: try again later, longer each time
		pthread_mutex_lock(&fetch.lock);
		int failures = ++fetch.failures;
		pthread_mutex_unlock(&fetch.lock);
		int wait_ms = failures < 3 ? 5000 : failures < 6 ? 30000 : 120000;
		logLine("no song this time; trying again in %d s", wait_ms / 1000);
		for (int waited = 0; waited < wait_ms && !want_quit; waited += 200) SDL_Delay(200);
	}
	return NULL;
}

// Takes the song that is ready, if there is one, and asks for the one after.
static int takeSong(Song* song) {
	pthread_mutex_lock(&fetch.lock);
	int ready = fetch.next.path[0] != '\0';
	if (ready) {
		*song = fetch.next;
		fetch.next.path[0] = '\0';
	}
	fetch.waiting = 1;
	pthread_mutex_unlock(&fetch.lock);
	return ready;
}

///////////////////////////////////////
// decoding

enum { DEC_NONE, DEC_MP3, DEC_FLAC, DEC_WAV, DEC_OGG };

typedef struct {
	int kind;
	int channels;
	int rate;
	drmp3 mp3;
	drflac* flac;
	drwav wav;
	stb_vorbis* ogg;
} Decoder;

static const char* extension(const char* path) {
	const char* dot = strrchr(path, '.');
	return dot ? dot + 1 : "";
}

static int decoderOpen(Decoder* d, const char* path) {
	memset(d, 0, sizeof(*d));
	const char* ext = extension(path);
	if (!strcasecmp(ext, "mp3") && drmp3_init_file(&d->mp3, path, NULL)) {
		d->kind = DEC_MP3;
		d->channels = d->mp3.channels;
		d->rate = d->mp3.sampleRate;
	}
	else if (!strcasecmp(ext, "flac") && (d->flac = drflac_open_file(path, NULL))) {
		d->kind = DEC_FLAC;
		d->channels = d->flac->channels;
		d->rate = d->flac->sampleRate;
	}
	else if (!strcasecmp(ext, "wav") && drwav_init_file(&d->wav, path, NULL)) {
		d->kind = DEC_WAV;
		d->channels = d->wav.channels;
		d->rate = d->wav.sampleRate;
	}
	else if (!strcasecmp(ext, "ogg")) {
		int error = 0;
		d->ogg = stb_vorbis_open_filename(path, &error, NULL);
		if (d->ogg) {
			stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
			d->kind = DEC_OGG;
			d->channels = info.channels;
			d->rate = info.sample_rate;
		}
	}
	if (d->kind == DEC_NONE) return 0;
	if (d->channels < 1 || d->channels > 8 || d->rate < 8000 || d->rate > 192000) {
		logLine("unplayable format in %s", path);
		return 0;
	}
	return 1;
}

// Reads up to frames frames of 16-bit samples. Returns how many; 0 at the end.
static int decoderRead(Decoder* d, int16_t* out, int frames) {
	switch (d->kind) {
		case DEC_MP3: return (int)drmp3_read_pcm_frames_s16(&d->mp3, frames, out);
		case DEC_FLAC: return (int)drflac_read_pcm_frames_s16(d->flac, frames, out);
		case DEC_WAV: return (int)drwav_read_pcm_frames_s16(&d->wav, frames, out);
		case DEC_OGG: return stb_vorbis_get_samples_short_interleaved(d->ogg, d->channels, out, frames * d->channels);
	}
	return 0;
}

static void decoderClose(Decoder* d) {
	switch (d->kind) {
		case DEC_MP3: drmp3_uninit(&d->mp3); break;
		case DEC_FLAC: drflac_close(d->flac); break;
		case DEC_WAV: drwav_uninit(&d->wav); break;
		case DEC_OGG: stb_vorbis_close(d->ogg); break;
	}
	d->kind = DEC_NONE;
}

///////////////////////////////////////
// playing

static SDL_AudioDeviceID device = 0;
static SDL_AudioSpec have;

static int openDevice(void) {
	SDL_AudioSpec want;
	SDL_zero(want);
	want.freq = 44100;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 2048;
	device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!device) {
		logLine("cannot open the speaker: %s", SDL_GetError());
		return 0;
	}
	SDL_PauseAudioDevice(device, 0);
	return 1;
}

static void closeDevice(void) {
	if (!device) return;
	SDL_PauseAudioDevice(device, 1);
	SDL_ClearQueuedAudio(device);
	SDL_CloseAudioDevice(device);
	device = 0;
}

static void writeNow(const Song* song) {
	FILE* file = fopen(NOW_PATH ".part", "w");
	if (!file) return;
	fprintf(file, "%s\n%s\n", song->title, song->album);
	fclose(file);
	rename(NOW_PATH ".part", NOW_PATH);
}

static void touch(const char* path) {
	FILE* file = fopen(path, "w");
	if (file) fclose(file);
}

// Another player left running is told to stop; this one takes over.
static void takeOver(void) {
	FILE* file = fopen(PID_PATH, "r");
	if (file) {
		int other = 0;
		if (fscanf(file, "%d", &other) == 1 && other > 1 && other != getpid() && kill(other, 0) == 0) {
			kill(other, SIGTERM);
			for (int i = 0; i < 20 && kill(other, 0) == 0; i++) usleep(100000);
		}
		fclose(file);
	}
	file = fopen(PID_PATH, "w");
	if (file) {
		fprintf(file, "%d\n", getpid());
		fclose(file);
	}
}

int main(int argc, char* argv[]) {
	if (argc < 2 || (strcmp(argv[1], "game") && strcmp(argv[1], "johnny"))) {
		fprintf(stderr, "usage: larry-music.elf game|johnny\n");
		return 2;
	}
	fetch.source = argv[1];

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = onSignal;
	sigaction(SIGUSR1, &action, NULL);
	sigaction(SIGUSR2, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	signal(SIGPIPE, SIG_IGN);

	takeOver();
	unlink(PAUSED_PATH);

	if (SDL_Init(SDL_INIT_AUDIO) != 0) {
		logLine("no audio: %s", SDL_GetError());
		return 1;
	}

	pthread_t thread;
	pthread_create(&thread, NULL, fetcher, NULL);

	int paused = 0;
	if (!openDevice()) paused = 1; // try again on the next resume

	Decoder decoder = { 0 };
	SDL_AudioStream* stream = NULL;
	float fade = 0.0f;           // 0 silent .. 1 full
	int16_t decoded[4096 * 8];   // up to 4096 frames of up to 8 channels
	int16_t converted[4096 * 4];

	while (!want_quit) {
		if (want_pause) {
			want_pause = 0;
			if (!paused) {
				closeDevice();
				paused = 1;
			}
			// said every time, so a launcher waiting on it is never left waiting
			touch(PAUSED_PATH);
		}
		if (want_resume) {
			want_resume = 0;
			if (paused && openDevice()) {
				paused = 0;
				fade = 0.0f;
				unlink(PAUSED_PATH);
				// what was queued went with the old device; the song picks up
				// where decoding got to, which is a moment further on
				if (stream) SDL_AudioStreamClear(stream);
			}
		}
		if (paused) {
			SDL_Delay(50);
			continue;
		}

		// a song to play
		if (decoder.kind == DEC_NONE) {
			Song song;
			if (!takeSong(&song)) {
				SDL_Delay(100);
				continue;
			}
			if (!decoderOpen(&decoder, song.path)) {
				logLine("cannot play %s", song.path);
				unlink(song.path);
				continue;
			}
			if (stream) SDL_FreeAudioStream(stream);
			stream = SDL_NewAudioStream(AUDIO_S16SYS, decoder.channels, decoder.rate, have.format, have.channels, have.freq);
			if (!stream) {
				decoderClose(&decoder);
				continue;
			}
			writeNow(&song);
			logLine("playing %s (%s)", song.title, song.album);
		}

		// keep a little queued ahead
		int bytes_per_ms = have.freq * have.channels * (int)sizeof(int16_t) / 1000;
		while (decoder.kind != DEC_NONE && (int)SDL_GetQueuedAudioSize(device) < QUEUE_MS * bytes_per_ms) {
			int frames = decoderRead(&decoder, decoded, 4096);
			if (frames <= 0) {
				SDL_AudioStreamFlush(stream);
				decoderClose(&decoder);
			}
			else SDL_AudioStreamPut(stream, decoded, frames * decoder.channels * (int)sizeof(int16_t));

			int got;
			while ((got = SDL_AudioStreamGet(stream, converted, sizeof(converted))) > 0) {
				int samples = got / (int)sizeof(int16_t);
				float step = 1.0f / ((float)FADE_MS * have.freq * have.channels / 1000.0f);
				for (int i = 0; i < samples; i++) {
					if (fade < 1.0f) fade += step;
					float gain = MUSIC_GAIN * (fade < 1.0f ? fade : 1.0f);
					converted[i] = (int16_t)(converted[i] * gain);
				}
				SDL_QueueAudio(device, converted, got);
			}
		}
		SDL_Delay(15);
	}

	closeDevice();
	if (decoder.kind != DEC_NONE) decoderClose(&decoder);
	if (stream) SDL_FreeAudioStream(stream);
	unlink(NOW_PATH);
	unlink(PAUSED_PATH);
	// only the player that owns the PID file removes it
	FILE* file = fopen(PID_PATH, "r");
	if (file) {
		int owner = 0;
		if (fscanf(file, "%d", &owner) == 1 && owner == getpid()) unlink(PID_PATH);
		fclose(file);
	}
	SDL_Quit();
	_exit(0); // the fetcher may be waiting on larry-sync; nothing of it needs keeping
}
