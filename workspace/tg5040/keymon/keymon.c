#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>

#include <msettings.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

// #include "defines.h"

#define VOLUME_MIN 		0
#define VOLUME_MAX 		20
#define BRIGHTNESS_MIN 	0
#define BRIGHTNESS_MAX 	10
#define COLORTEMP_MIN 	0
#define COLORTEMP_MAX 	40

#define CODE_MENU0		314
#define CODE_MENU1		315
#define CODE_MENU2		316
#define CODE_PLUS		115
#define CODE_MINUS		114
#define CODE_MUTE		1
#define CODE_JACK		2

// keymon and api might need different codes

//	for ev.value
#define RELEASED	0
#define PRESSED		1
#define REPEAT		2

#define MUTE_STATE_PATH "/sys/class/gpio/gpio243/value"



#define INPUT_COUNT 5
static int inputs[INPUT_COUNT] = {};
static struct input_event ev;

static volatile int quit = 0;
static void on_term(int sig) { quit = 1; }

static int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}

// Larry Boy Advanced: the menu music player, told to change song by signal
#define MUSIC_PID_PATH "/tmp/larry-music.pid"
#define MUSIC_PAUSED_PATH "/tmp/larry-music.paused"
#define MUSIC_NEXT_SIGNAL (SIGRTMIN + 1)
#define MUSIC_PREVIOUS_SIGNAL (SIGRTMIN + 2)

// Changes the song if music is playing: not while it is paused for a game
// that has the speaker, nor when there is none.
static void musicSkip(int sig) {
	if (access(MUSIC_PAUSED_PATH, F_OK) == 0) return;
	int pid = getInt(MUSIC_PID_PATH);
	if (pid > 1 && kill(pid, 0) == 0) kill(pid, sig);
}

int stringEquals(const char* str1, const char* str2) {
	if (!str1 || !str2) return 0; // NULL isn't safe here
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}

static pthread_t mute_pt;
static void* watchMute(void *arg) {
	int is_muted,was_muted;
	int is_brickpro = stringEquals(getenv("DEVICE"), "brickpro");
	
	is_muted = was_muted = getInt(MUTE_STATE_PATH);
	SetMute(is_muted);
	
	while(!quit) {
		usleep(200000); // 5 times per second
		
		is_muted = getInt(MUTE_STATE_PATH);
		// swallow mute val -1 on shutdown
		if (is_muted >= 0 && was_muted!=is_muted) {
			was_muted = is_muted;
			SetMute(is_muted);
			if (GetMute()) {
				// tmp solution
				if (is_brickpro)
					system("echo 900000 > /sys/class/motor/voltage");
				else
					system("echo 1500000 > /sys/class/motor/voltage");
				system("echo 1 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 0 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 1 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 0 > /sys/class/gpio/gpio227/value");
			}
		}
	}
	
	return NULL;
}

int main (int argc, char *argv[]) {
	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);

	InitSettings();
	pthread_create(&mute_pt, NULL, &watchMute, NULL);

	char path[32];
	for (int i=0; i<INPUT_COUNT; i++) {
		sprintf(path, "/dev/input/event%i", i);
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	
	uint32_t input;
	uint32_t val;
	uint32_t menu_pressed = 0;
	uint32_t menu2_pressed = 0;
	
	uint32_t up_pressed = 0;
	uint32_t up_just_pressed = 0;
	uint32_t up_repeat_at = 0;
	
	uint32_t down_pressed = 0;
	uint32_t down_just_pressed = 0;
	uint32_t down_repeat_at = 0;

	// Larry Boy Advanced: the D-pad, for Select + up/down (brightness) and
	// Select + left/right (the music's song)
	int hat_x = 0, hat_y = 0;
	uint32_t dpad_up_pressed = 0, dpad_up_just_pressed = 0, dpad_up_repeat_at = 0;
	uint32_t dpad_down_pressed = 0, dpad_down_just_pressed = 0, dpad_down_repeat_at = 0;
	int skip = 0; // +1 next song, -1 previous, once per press
	
	uint8_t ignore;
	uint32_t then;
	uint32_t now;
	struct timeval tod;
	
	gettimeofday(&tod, NULL);
	then = tod.tv_sec * 1000 + tod.tv_usec / 1000; // essential SDL_GetTicks()
	ignore = 0;
	
	while (!quit) {
		gettimeofday(&tod, NULL);
		now = tod.tv_sec * 1000 + tod.tv_usec / 1000;
		if (now-then>1000) ignore = 1; // ignore input that arrived during sleep
		
		for (int i=0; i<INPUT_COUNT; i++) {
			input = inputs[i];
			while(read(input, &ev, sizeof(ev))==sizeof(ev)) {
				if (ignore) continue;
				val = ev.value;
				if (ev.type==EV_SW) {
					//printf("switch: %i\n", ev.code);
					if (ev.code==CODE_JACK) {
						//printf("jack: %i\n", val);
						SetJack(val);
					}
				}
				if (ev.type==EV_ABS && (ev.code==ABS_HAT0X || ev.code==ABS_HAT0Y)) {
					int value = ev.value < 0 ? -1 : ev.value > 0 ? 1 : 0;
					if (ev.code==ABS_HAT0Y) {
						if (value != hat_y) {
							dpad_up_pressed = dpad_up_just_pressed = value < 0;
							dpad_down_pressed = dpad_down_just_pressed = value > 0;
							if (value < 0) dpad_up_repeat_at = now + 300;
							if (value > 0) dpad_down_repeat_at = now + 300;
						}
						hat_y = value;
					}
					else {
						if (value != hat_x && value != 0) skip = value;
						hat_x = value;
					}
					continue;
				}
				if (( ev.type != EV_KEY ) || ( val > REPEAT )) continue;
				//printf("code: %i (%i)\n", ev.code, val); fflush(stdout);
				switch (ev.code) {
					case CODE_MENU2:
						menu_pressed = val;
					break;
					case CODE_MENU0:
						menu2_pressed = val;
					break;
					case CODE_PLUS:
						up_pressed = up_just_pressed = val;
						if (val) up_repeat_at = now + 300;
					break;
					case CODE_MINUS:
						down_pressed = down_just_pressed = val;
						if (val) down_repeat_at = now + 300;
					break;
					default:
					break;
				}
			}
		}
		
		if (ignore) {
			hat_x = hat_y = 0;
			dpad_up_pressed = dpad_up_just_pressed = dpad_down_pressed = dpad_down_just_pressed = 0;
			skip = 0;
			menu_pressed = 0;
			menu2_pressed = 0;
			up_pressed = up_just_pressed = 0;
			down_pressed = down_just_pressed = 0;
			up_repeat_at = 0;
			down_repeat_at = 0;
		}
		
		if (up_just_pressed || (up_pressed && now>=up_repeat_at)) {
			if (menu_pressed) {
				val = GetBrightness();
				if (val<BRIGHTNESS_MAX) SetBrightness(++val);
			}
			else if (menu2_pressed) {
				val = GetColortemp();
				if (val<COLORTEMP_MAX) {
					SetColortemp(++val);
				}
			}
			else {
				val = GetVolume();
				if (val<VOLUME_MAX) SetVolume(++val);
			}
			
			if (up_just_pressed) up_just_pressed = 0;
			else up_repeat_at += 100;
		}
		
		if (down_just_pressed || (down_pressed && now>=down_repeat_at)) {
			if (menu_pressed) {
				val = GetBrightness();
				if (val>BRIGHTNESS_MIN) SetBrightness(--val);
			}
			else if (menu2_pressed) {
				val = GetColortemp();
				if (val>COLORTEMP_MIN) {
					SetColortemp(--val);
				}
			}
			else {
				val = GetVolume();
				if (val>VOLUME_MIN) SetVolume(--val);
			}
			
			if (down_just_pressed) down_just_pressed = 0;
			else down_repeat_at += 100;
		}
		
		// Larry Boy Advanced: Select + D-pad. menu2_pressed is Select.
		if (menu2_pressed && !menu_pressed) {
			if (dpad_up_just_pressed || (dpad_up_pressed && now>=dpad_up_repeat_at)) {
				val = GetBrightness();
				if (val<BRIGHTNESS_MAX) SetBrightness(++val);
				if (dpad_up_just_pressed) dpad_up_just_pressed = 0;
				else dpad_up_repeat_at += 100;
			}
			if (dpad_down_just_pressed || (dpad_down_pressed && now>=dpad_down_repeat_at)) {
				val = GetBrightness();
				if (val>BRIGHTNESS_MIN) SetBrightness(--val);
				if (dpad_down_just_pressed) dpad_down_just_pressed = 0;
				else dpad_down_repeat_at += 100;
			}
			if (skip) musicSkip(skip > 0 ? MUSIC_NEXT_SIGNAL : MUSIC_PREVIOUS_SIGNAL);
		}
		else {
			// pressed without Select: not a combo, and not one later either
			dpad_up_just_pressed = dpad_down_just_pressed = 0;
			dpad_up_pressed = dpad_down_pressed = 0;
		}
		skip = 0;

		then = now;
		ignore = 0;
		
		usleep(16666); // 60fps
	}

	for (int i=0; i<INPUT_COUNT; i++)
		close(inputs[i]);
	
	pthread_cancel(mute_pt);
	pthread_join(mute_pt, NULL);
}
