/*
 * This file is part of the Advance project.
 *
 * Copyright (C) 2002 Kari Hautio
 * Copyright (C) 1999-2003 Andrea Mazzoleni
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * In addition, as a special exception, Andrea Mazzoleni
 * gives permission to link the code of this program with
 * the MAME library (or with modified versions of MAME that use the
 * same license as MAME), and distribute linked combinations including
 * the two.  You must obey the GNU General Public License in all
 * respects for all of the code used other than MAME.  If you modify
 * this file, you may extend this exception to your version of the
 * file, but you are not obligated to do so.  If you do not wish to
 * do so, delete this exception statement from your version.
 */

#include "kraw.h"
#include "log.h"
#include "error.h"
#include "oslinux.h"

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/kd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/vt.h>
#include <signal.h>
#include <errno.h>

#define LOW_INVALID ((unsigned)0xFFFFFFFF)

struct keyb_raw_context {
	struct termios oldkbdtermios;
	struct termios newkbdtermios;
	int oldkbmode;
	int f; /**< Handle. */
	unsigned map_up_to_low[KEYB_MAX]; /**< Key mapping. */
	unsigned char state[256]; /**< Key state. */
	adv_bool disable_special_flag; /**< Disable special hotkeys. */
	adv_bool graphics_flag; /**< Set the terminal in graphics mode. */
	adv_bool passive_flag; /**< Be passive on some actions. Required for compatibility with other libs. */
};

static struct keyb_pair {
	unsigned up_code;
	unsigned low_code;
} KEYS[] = {
{ KEYB_ESC, 1 },
{ KEYB_1, 2 },
{ KEYB_2, 3 },
{ KEYB_3, 4 },
{ KEYB_4, 5 },
{ KEYB_5, 6 },
{ KEYB_6, 7 },
{ KEYB_7, 8 },
{ KEYB_8, 9 },
{ KEYB_9, 10 },
{ KEYB_0, 11 },
{ KEYB_MINUS, 12 },
{ KEYB_EQUALS, 13 },
{ KEYB_BACKSPACE, 14 },
{ KEYB_TAB, 15 },
{ KEYB_Q, 16 },
{ KEYB_W, 17 },
{ KEYB_E, 18 },
{ KEYB_R, 19 },
{ KEYB_T, 20 },
{ KEYB_Y, 21 },
{ KEYB_U, 22 },
{ KEYB_I, 23 },
{ KEYB_O, 24 },
{ KEYB_P, 25 },
{ KEYB_OPENBRACE, 26 },
{ KEYB_CLOSEBRACE, 27 },
{ KEYB_ENTER, 28 },
{ KEYB_LCONTROL, 29 },
{ KEYB_A, 30 },
{ KEYB_S, 31 },
{ KEYB_D, 32 },
{ KEYB_F, 33 },
{ KEYB_G, 34 },
{ KEYB_H, 35 },
{ KEYB_J, 36 },
{ KEYB_K, 37 },
{ KEYB_L, 38 },
{ KEYB_SEMICOLON, 39 },
{ KEYB_QUOTE, 40 },
{ KEYB_BACKQUOTE, 41 },
{ KEYB_LSHIFT, 42 },
{ KEYB_BACKSLASH, 43 },
{ KEYB_Z, 44 },
{ KEYB_X, 45 },
{ KEYB_C, 46 },
{ KEYB_V, 47 },
{ KEYB_B, 48 },
{ KEYB_N, 49 },
{ KEYB_M, 50 },
{ KEYB_COMMA, 51 },
{ KEYB_PERIOD, 52 },
{ KEYB_SLASH, 53 },
{ KEYB_RSHIFT, 54 },
{ KEYB_ASTERISK, 55 },
{ KEYB_ALT, 56 },
{ KEYB_SPACE, 57 },
{ KEYB_CAPSLOCK, 58 },
{ KEYB_F1, 59 },
{ KEYB_F2, 60 },
{ KEYB_F3, 61 },
{ KEYB_F4, 62 },
{ KEYB_F5, 63 },
{ KEYB_F6, 64 },
{ KEYB_F7, 65 },
{ KEYB_F8, 66 },
{ KEYB_F9, 67 },
{ KEYB_F10, 68 },
{ KEYB_NUMLOCK, 69 },
{ KEYB_SCRLOCK, 70 },
{ KEYB_7_PAD, 71 },
{ KEYB_8_PAD, 72 },
{ KEYB_9_PAD, 73 },
{ KEYB_MINUS_PAD, 74 },
{ KEYB_4_PAD, 75 },
{ KEYB_5_PAD, 76 },
{ KEYB_6_PAD, 77 },
{ KEYB_PLUS_PAD, 78 },
{ KEYB_1_PAD, 79 },
{ KEYB_2_PAD, 80 },
{ KEYB_3_PAD, 81 },
{ KEYB_0_PAD, 82 },
{ KEYB_PERIOD_PAD, 83 },
{ KEYB_LESS, 86 },
{ KEYB_F11, 87 },
{ KEYB_F12, 88 },
{ KEYB_ENTER_PAD, 96 },
{ KEYB_RCONTROL, 97 },
{ KEYB_SLASH_PAD, 98 },
{ KEYB_PRTSCR, 99 },
{ KEYB_ALTGR, 100 },
{ KEYB_PAUSE, 101 },
{ KEYB_HOME, 102 },
{ KEYB_UP, 103 },
{ KEYB_PGUP, 104 },
{ KEYB_LEFT, 105 },
{ KEYB_RIGHT, 106 },
{ KEYB_END, 107 },
{ KEYB_DOWN, 108 },
{ KEYB_PGDN, 109 },
{ KEYB_INSERT, 110 },
{ KEYB_DEL, 111 },
{ KEYB_PAUSE, 119 },
{ KEYB_LWIN, 125 },
{ KEYB_RWIN, 126 },
{ KEYB_MENU, 127 },
{ KEYB_MAX, 0 }
};

static adv_device DEVICE[] = {
{ "auto", -1, "RAW keyboard" },
{ 0, 0, 0 }
};

static struct keyb_raw_context raw_state;

static void keyb_raw_clear(void)
{
	unsigned i;

	for(i=0;i<256;++i) {
		raw_state.state[i] = 0;
	}
}

adv_error keyb_raw_init(int keyb_id, adv_bool disable_special)
{
	struct keyb_pair* i;
	unsigned j;

	log_std(("keyb:raw: keyb_raw_init(id:%d, disable_special:%d)\n", keyb_id, (int)disable_special));

	if (getenv("DISPLAY")) {
		error_set("Unsupported in X.\n");
		return -1;
	}

	for(j=0;j<KEYB_MAX;++j) {
		raw_state.map_up_to_low[j] = LOW_INVALID;
	}
	for(i=KEYS;i->up_code != KEYB_MAX;++i) {
		raw_state.map_up_to_low[i->up_code] = i->low_code;
	}

	raw_state.disable_special_flag = disable_special;

	return 0;
}

void keyb_raw_done(void)
{
	log_std(("keyb:raw: keyb_raw_done()\n"));
}

adv_error keyb_raw_enable(adv_bool graphics)
{
	log_std(("keyb:raw: keyb_raw_enable()\n"));

#if defined(USE_VIDEO_SDL)
	if (os_internal_sdl_is_video_active()) {
		error_set("The raw keyboard driver cannot be used with the SDL video driver.\n");
		return -1;
	}
#endif

	raw_state.passive_flag = 0;
#ifdef USE_VIDEO_SVGALIB
	/* SVGALIB already set the terminal in KD_GRAPHICS mode and */
	/* it waits on a signal on a vt switch */
	if (os_internal_svgalib_is_video_mode_active()) {
		raw_state.passive_flag = 1;
	}
#endif

	raw_state.graphics_flag = graphics;

	raw_state.f = open("/dev/tty", O_RDONLY);
	if (raw_state.f == -1) {
		error_set("Error enabling the raw keyboard driver. Function open(/dev/tty) failed.\n");
		goto err;
	}

	if (ioctl(raw_state.f, KDGKBMODE, &raw_state.oldkbmode) != 0) {
		error_set("Error enabling the raw keyboard driver. Function ioctl(KDGKBMODE) failed.\n");
		goto err_close;
	}

	if (tcgetattr(raw_state.f, &raw_state.oldkbdtermios) != 0) {
		error_set("Error enabling the raw keyboard driver. Function tcgetattr() failed.\n");
		goto err_close;
	}

	raw_state.newkbdtermios = raw_state.oldkbdtermios;

	/* setting taken from SVGALIB */
	raw_state.newkbdtermios.c_lflag &= ~(ICANON | ECHO | ISIG);
	raw_state.newkbdtermios.c_iflag &= ~(ISTRIP | IGNCR | ICRNL | INLCR | IXOFF | IXON);
	raw_state.newkbdtermios.c_cc[VMIN] = 0;
	raw_state.newkbdtermios.c_cc[VTIME] = 0;

	if (tcsetattr(raw_state.f, TCSAFLUSH, &raw_state.newkbdtermios) != 0) {
		error_set("Error enabling the raw keyboard driver. Function tcsetattr(TCSAFLUSH) failed.\n");
		goto err_close;
	}

	if (ioctl(raw_state.f, KDSKBMODE, K_MEDIUMRAW) != 0) {
		error_set("Error enabling the raw keyboard driver. Function ioctl(KDSKBMODE) failed.\n");
		goto err_term;
	}

	if (!raw_state.passive_flag && raw_state.graphics_flag) {
		/* set the console in graphics mode, it only disable the cursor and the echo */
		log_std(("keyb:raw: ioctl(KDSETMODE, KD_GRAPHICS)\n"));
		if (ioctl(raw_state.f, KDSETMODE, KD_GRAPHICS) < 0) {
			log_std(("keyb:raw: ioctl(KDSETMODE, KD_GRAPHICS) failed\n"));
			error_set("Error setting the tty in graphics mode.\n");
			goto err_mode;
		}
	}

	keyb_raw_clear();

	return 0;

err_mode:
	if (ioctl(raw_state.f, KDSKBMODE, raw_state.oldkbmode) < 0) {
		/* ignore error */
		log_std(("keyb:raw: ioctl(KDSKBMODE,old) failed\n"));
	}
err_term:
	if (tcsetattr(raw_state.f, TCSAFLUSH, &raw_state.oldkbdtermios) != 0) {
		/* ignore error */
		log_std(("keyb:raw: tcsetattr(TCSAFLUSH) failed\n"));
	}
err_close:
	close(raw_state.f);
err:
	return -1;
}

void keyb_raw_disable(void)
{
	log_std(("keyb:raw: keyb_raw_disable()\n"));

	if (!raw_state.passive_flag && raw_state.graphics_flag) {
		if (ioctl(raw_state.f, KDSETMODE, KD_TEXT) < 0) {
			/* ignore error */
			log_std(("keyb:raw: ioctl(KDSETMODE, KD_TEXT) failed\n"));
		}
	}

	if (ioctl(raw_state.f, KDSKBMODE, raw_state.oldkbmode) < 0) {
		/* ignore error */
		log_std(("keyb:raw: ioctl(KDSKBMODE,old) failed\n"));
	}

	if (tcsetattr(raw_state.f, TCSAFLUSH, &raw_state.oldkbdtermios) != 0) {
		/* ignore error */
		log_std(("keyb:raw: tcsetattr(TCSAFLUSH) failed\n"));
	}

	close(raw_state.f);
}

unsigned keyb_raw_count_get(void)
{
	log_debug(("keyb:raw: keyb_raw_count_get(void)\n"));

	return 1;
}

adv_bool keyb_raw_has(unsigned keyboard, unsigned code)
{
	log_debug(("keyb:raw: keyb_raw_has()\n"));

	assert(code < KEYB_MAX);

	return raw_state.map_up_to_low[code] != LOW_INVALID;
}

unsigned keyb_raw_get(unsigned keyboard, unsigned code)
{
	unsigned low_code;

	assert(keyboard < keyb_raw_count_get());
	assert(code < KEYB_MAX);

	log_debug(("keyb:raw: keyb_raw_get(keyboard:%d,code:%d)\n", keyboard, code));

	/* disable the pause key */
	if (code == KEYB_PAUSE)
		return 0;

	low_code = raw_state.map_up_to_low[code];
	if (low_code == LOW_INVALID)
		return 0;

	return raw_state.state[low_code];
}

void keyb_raw_all_get(unsigned keyboard, unsigned char* code_map)
{
	unsigned i;

	assert(keyboard < keyb_raw_count_get());

	log_debug(("keyb:raw: keyb_raw_all_get(keyboard:%d)\n", keyboard));

	for(i=0;i<KEYB_MAX;++i) {
		unsigned low_code = raw_state.map_up_to_low[i];
		if (low_code == LOW_INVALID)
			code_map[i] = 0;
		else
			code_map[i] = raw_state.state[low_code];
	}

	/* disable the pause key */
	code_map[KEYB_PAUSE] = 0;
}

#define SCANCODE_LCTRL 29
#define SCANCODE_RCTRL 97
#define SCANCODE_C 46
#define SCANCODE_ALT 56
#define SCANCODE_ALTGR 100
#define SCANCODE_F1 59
#define SCANCODE_F2 60
#define SCANCODE_F3 61
#define SCANCODE_F4 62
#define SCANCODE_F5 63
#define SCANCODE_F6 64
#define SCANCODE_F7 65
#define SCANCODE_F8 66
#define SCANCODE_F9 67
#define SCANCODE_F10 68

static void keyb_raw_vt_switch(unsigned char code)
{
	unsigned i;
	struct vt_stat vts;
	int vt;

	/* check only if required */
	if (raw_state.disable_special_flag)
		return;

	/* check only if a switch key is pressed/released */
	switch (code) {
	case SCANCODE_LCTRL :
	case SCANCODE_RCTRL :
	case SCANCODE_C :
	case SCANCODE_ALT :
	case SCANCODE_ALTGR :
	case SCANCODE_F1 :
	case SCANCODE_F2 :
	case SCANCODE_F3 :
	case SCANCODE_F4 :
	case SCANCODE_F5 :
	case SCANCODE_F6 :
	case SCANCODE_F7 :
	case SCANCODE_F8 :
	case SCANCODE_F9 :
	case SCANCODE_F10 :
		break;
	default:
		return;
	}

	/* check for CTRL+C */
	if ((raw_state.state[SCANCODE_LCTRL] || raw_state.state[SCANCODE_RCTRL])
		&& raw_state.state[SCANCODE_C]) {
		raise(SIGINT);
		return;
	}

	/* check for ALT+Fx */

	if (!raw_state.state[SCANCODE_ALT] && !raw_state.state[SCANCODE_ALTGR]) {
		return;
	}
	vt = 0;
	for(i=SCANCODE_F1;i<=SCANCODE_F10;++i) {
		if (raw_state.state[i]) {
			vt = i + 1 - SCANCODE_F1;
			break;
		}
	}
	if (!vt)
		return;

	/* get active vt */
	if (ioctl(raw_state.f, VT_GETSTATE, &vts) < 0) {
		log_std(("keyb:raw: ioctl(VT_GETSTATE) failed\n"));
		return;
	}

	/* do not switch vt's if need not to */
	if (vt == vts.v_active)
		return;

	if (!raw_state.passive_flag && raw_state.graphics_flag) {
		if (ioctl(raw_state.f, KDSETMODE, KD_TEXT) < 0) {
			log_std(("keyb:raw: ioctl(KDSETMODE, KD_TEXT) failed\n"));
			return;
		}
	}

	/* change vt */
	if (ioctl(raw_state.f, VT_ACTIVATE, vt) == 0) {
		if (!raw_state.passive_flag) {
			log_std(("keyb:raw: waiting vt change\n"));

			/* wait for new console to be activated */
			ioctl(raw_state.f, VT_WAITACTIVE, vt);

			log_std(("keyb:raw: waiting vt return\n"));

			/* wait for original console to be actived */
			while (ioctl(raw_state.f, VT_WAITACTIVE, vts.v_active) < 0 ) {

				if ((errno != EINTR) && (errno != EAGAIN)) {
					log_std(("keyb:raw: ioctl(VT_WAITACTIVE) failed, %d\n", errno));
					/* unknown VT error - cancel this without blocking */
					break;
				}

				sleep(1);
			}
		}
	}

	if (!raw_state.passive_flag && raw_state.graphics_flag) {
		if (ioctl(raw_state.f, KDSETMODE, KD_GRAPHICS) < 0) {
			/* ignore error */
			log_std(("keyb:raw: ioctl(KDSETMODE, KD_GRAPHICS) failed\n"));
		}
	}

	keyb_raw_clear();
}

void keyb_raw_poll(void)
{
	unsigned char c;

	log_debug(("keyb:raw: keyb_raw_poll()\n"));

	while ((1 == read(raw_state.f, &c, 1)) && (c)) {
		unsigned char code = c & 0x7f;
		adv_bool pressed = (c & 0x80) == 0;

		raw_state.state[code] = pressed;

		keyb_raw_vt_switch(code);
	}
}

unsigned keyb_raw_flags(void)
{
	return 0;
}

adv_error keyb_raw_load(adv_conf* context)
{
	return 0;
}

void keyb_raw_reg(adv_conf* context)
{
}

/***************************************************************************/
/* Driver */

keyb_driver keyb_raw_driver = {
	"raw",
	DEVICE,
	keyb_raw_load,
	keyb_raw_reg,
	keyb_raw_init,
	keyb_raw_done,
	keyb_raw_enable,
	keyb_raw_disable,
	keyb_raw_flags,
	keyb_raw_count_get,
	keyb_raw_has,
	keyb_raw_get,
	keyb_raw_all_get,
	keyb_raw_poll
};

