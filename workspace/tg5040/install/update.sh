#!/bin/sh

SDCARD_PATH=/mnt/SDCARD

# --------------------------------------
# remove old brick system folder
BRICK_PATH=${SDCARD_PATH}/.system/tg3040
echo "check for $BRICK_PATH"
# this might always exist so we can pull up old cards
if [ -d $BRICK_PATH ]; then
	echo "deleting brick system folder $BRICK_PATH"
	rm -rf "$BRICK_PATH"
	
	# copy brick configs from userdata
	SRC_PATH=${SDCARD_PATH}/.userdata/tg3040
	if [ -d $SRC_PATH ]; then
		DST_PATH=${SDCARD_PATH}/.userdata/tg5040
		mkdir -p $DST_PATH # just in case
	
		for SUB_PATH in $SRC_PATH/*; do
			if [ -d $SUB_PATH ]; then
				SUB_NAME=$(basename $SUB_PATH)
				NEW_PATH=$DST_PATH/$SUB_NAME
			
				if [ ! -d $NEW_PATH ]; then
					echo "creating new path $NEW_PATH"
					mkdir -p $NEW_PATH
				fi
			
				for CFG_PATH in $SUB_PATH/*.cfg; do
					if [ -f $CFG_PATH ]; then
						CFG_NAME=$(basename $CFG_PATH .cfg)
						echo "copying $CFG_PATH to $NEW_PATH/$CFG_NAME-brick.cfg"
						cp $CFG_PATH $NEW_PATH/$CFG_NAME-brick.cfg
					fi
				done
			fi
		done
		echo "deleting brick userdata $SRC_PATH"
		rm -rf $SRC_PATH
		
		UPDATE_PATH=${SDCARD_PATH}/.tmp_update/tg3040
		rm -rf $UPDATE_PATH.sh
		rm -rf $UPDATE_PATH
		
		reboot
		# we need to sleep until reboot otherwise 
		# it will poweroff without rebooting
		while :; do
			sleep 1
		done
	fi
fi
# --------------------------------------

# --------------------------------------
# Larry Boy Advanced: its boot logo, put in place once. The logo lives in
# the device's own boot partition, not on the card, so this is done by the
# install rather than shipped. The logo that was there is kept on the card
# first. Choosing another in Tools > Bootlogo afterwards sticks: this only
# runs again if the logo itself changes (LOGO_VERSION).
LOGO_VERSION=1
LARRY_HOME=${SDCARD_PATH}/.userdata/shared/larry
LOGO_STAMP=${LARRY_HOME}/bootlogo-applied
LOGO_PATH="${SDCARD_PATH}/Tools/tg5040/Bootlogo.pak/brick/Larry Boy Advanced.bmp"
TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if { [ "$TRIMUI_MODEL" = "Trimui Brick" ] || [ "$TRIMUI_MODEL" = "Trimui Brick Pro" ]; } \
	&& [ -f "$LOGO_PATH" ] && [ "$(cat "$LOGO_STAMP" 2>/dev/null)" != "$LOGO_VERSION" ]; then
	echo "TEXT:Setting the boot logo" > /tmp/show2.fifo
	BOOT_PATH=/mnt/boot
	mkdir -p "$BOOT_PATH" "$LARRY_HOME"
	if mount -t vfat /dev/mmcblk0p1 "$BOOT_PATH"; then
		if [ -f "$BOOT_PATH/bootlogo.bmp" ] && [ ! -f "$LARRY_HOME/bootlogo-before.bmp" ]; then
			cp "$BOOT_PATH/bootlogo.bmp" "$LARRY_HOME/bootlogo-before.bmp"
		fi
		# written beside it, then renamed, so a cut-off copy never becomes the logo
		if cp "$LOGO_PATH" "$BOOT_PATH/bootlogo.bmp.new" && sync \
			&& mv "$BOOT_PATH/bootlogo.bmp.new" "$BOOT_PATH/bootlogo.bmp" && sync; then
			echo "$LOGO_VERSION" > "$LOGO_STAMP"
		else
			rm -f "$BOOT_PATH/bootlogo.bmp.new"
		fi
		umount "$BOOT_PATH"
	fi
fi

# --------------------------------------
# Larry Boy Advanced: the menu music settings file, for cards set up before
# it existed. One already there is never touched.
MUSIC_FILE=${SDCARD_PATH}/music.txt
if [ ! -f "$MUSIC_FILE" ]; then
	cat > "$MUSIC_FILE" <<'MUSIC'
# Larry Boy Advanced: menu music
#
# Music plays while you browse the menus, and pauses while you play. It
# streams from your Copyparty music server: fill in the password for your
# Game Music folder below. Choose what plays with "Music" on the main menu.
#
# This file is left as you write it, so the password stays here in plain
# text.

game music: https://music.alice99.box.ca/music/
game music password:
MUSIC
fi
