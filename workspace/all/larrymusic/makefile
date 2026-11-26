###########################################################
# Larry Boy Advanced: menu music player

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env

###########################################################

TARGET = larry-music
PRODUCT = $(TARGET).elf

CC = $(CROSS_COMPILE)gcc
CFLAGS = -O2 -Wall -Wno-unused-function -Wno-unused-value -Wno-unused-but-set-variable
CFLAGS += -DLARRY_WRAPPER=\"/mnt/SDCARD/.system/$(PLATFORM)/bin/larry-sync.sh\"
LDFLAGS = -lSDL2 -lpthread -lm -ldl -lrt -Wl,--gc-sections -s

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(CFLAGS) larrymusic.c -o build/$(PLATFORM)/$(PRODUCT) $(LDFLAGS)

clean:
	rm -rf build/$(PLATFORM)/$(PRODUCT)
