/****************************************************************************
 ** hw_usbirtoy.c **********************************************************
 ****************************************************************************
 *
 * Routines for USB Infrared Toy receiver/transmitter in sampling mode
 *
 * Copyright (C) 2011 Peter Kooiman <pkooiman@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
	#include <config.h>
#endif


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "lirc_driver.h"
#include "lirc/serial.h"


#define IRTOY_MINFWVERSION 20

#define IRTOY_UNIT 21.3333
#define IRTOY_LONGSPACE 1000000


const unsigned char IRTOY_COMMAND_TXSTART[] = {0x24, 0x25, 0x26, 0x03};
#define IRTOY_COMMAND_RESET 0
#define IRTOY_COMMAND_SMODE_ENTER 's'
#define IRTOY_COMMAND_VERSION 'v'

#define IRTOY_REPLY_XMITCOUNT 't'
#define IRTOY_REPLY_XMITSUCCESS 'C'
#define IRTOY_REPLY_VERSION 'V'
#define IRTOY_REPLY_SAMPLEMODEPROTO 'S'

#define IRTOY_LEN_XMITRES 4
#define IRTOY_LEN_VERSION 4
#define IRTOY_LEN_SAMPLEMODEPROTO 3

#define IRTOY_TIMEOUT_READYFORDATA 1000000
#define IRTOY_TIMEOUT_FLUSH 20000
#define IRTOY_TIMEOUT_SMODE_ENTER 500000
#define IRTOY_TIMEOUT_VERSION 500000


struct tag_irtoy_t {
	int hwVersion;
	int swVersion;
	int protoVersion;
	int fd;
	int awaitingNewSig;
	int pulse;
};

typedef struct tag_irtoy_t irtoy_t;

static irtoy_t *dev;

unsigned char rawSB[WBUF_SIZE * 2 + 2];

/* exported functions  */
static int init(void);
static int deinit(void);
static int send(struct ir_remote *remote, struct ir_ncode *code);
static char* receive(struct ir_remote* remotes);
static int decode(struct ir_remote* remote, ir_code* prep,
                  ir_code* codep, ir_code * postp,
		  int* repeat_flagp,
                  lirc_t* min_remaining_gapp, lirc_t* max_remaining_gapp);
static lirc_t readdata(lirc_t timeout);


const struct driver hw_usbirtoy = {
	.name           =       "irtoy",
	.device         =       "/dev/ttyACM0",
	.features       =       LIRC_CAN_REC_MODE2 | LIRC_CAN_SEND_PULSE,
	.send_mode      =       LIRC_MODE_PULSE,
	.rec_mode       =       LIRC_MODE_MODE2,
	.code_length    =       0,
	.init_func 	=	init,
	.deinit_func    =       deinit,
	.send_func      =       send,
	.rec_func       =       receive,
	.decode_func    =       decode,
	.drvctl_func    =       NULL,
	.readdata       =       readdata,
	.api_version	=	2,
	.driver_version = 	"0.9.2"
};


const struct driver* hardwares[] =
	 { &hw_usbirtoy, (const struct driver*) NULL };


static int decode(struct ir_remote* remote, ir_code* prep,
                   ir_code* codep, ir_code* postp,
		   int* repeat_flagp,
                   lirc_t* min_remaining_gapp, lirc_t* max_remaining_gapp)
{
	int res;

	LOGPRINTF(1, "decode: enter");

	res = receive_decode(remote, prep, codep, postp, repeat_flagp,
                             min_remaining_gapp, max_remaining_gapp);

	LOGPRINTF(1, "decode: %d", res);

	return res;
}


static ssize_t
read_with_timeout(int fd, void *buf, size_t count, long to_usec)
{
	ssize_t rc;
	size_t numread = 0;
	struct timeval timeout;
	fd_set fds;

	timeout.tv_sec = 0;
	timeout.tv_usec = to_usec;

	rc = read(fd, (char *) buf, count);

	if (rc > 0) {
		numread += rc;
	}

	while ((rc == -1 && errno == EAGAIN) || (rc >= 0 && numread < count)){
		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		rc = select(fd + 1, &fds, NULL, NULL, &timeout);

		if (rc == 0) {
			/* timeout */
			break;
		} else if (rc == -1) {
			/* continue for EAGAIN case */
			continue;
		}

		rc = read(fd, ((char *)buf) + numread, count - numread);

		if (rc > 0) {
			numread += rc;
		}
	}
	return(numread == 0) ? -1 : numread;
}


static int irtoy_readflush(irtoy_t * dev, long timeout)
{
	int res;
	char c;

	while ((res = read_with_timeout(dev->fd, &c, 1, timeout)) == 1)
		;
	if (res != 0)
		return -1;
	else
		return 0;
}


static lirc_t irtoy_read(irtoy_t * dev, lirc_t timeout)
{

	lirc_t data;
	int res;
	unsigned char dur[2];

	if (!waitfordata(timeout))
		return 0;

	// lircd expects a space as start of the next transmission, not
	// just at the end of the last one.
	// irrecord however likes to see a space at the end of the signal
	// We remember if we saw the 0xFFFF timeout from the usbtoy and
	// send a long space both after last signal and at start of next signal
	// From usb irtoy:
	//  <signal...><lastpulse> [usbtoy timeout duration] 0xFFFF
	//  [however long it takes before next signal]
	//  <firstpulse><signal..>
	// We return:
	// <signal><lastpulse> [usbtoy timeout duration] LONGSPACE
	// [however long it takes before next signal]
	// LONGSPACE <firstpulse><signal>AA

	if (dev->awaitingNewSig) {
		LOGPRINTF(1, "new signal after large space");
		dev->pulse = 1;
		dev->awaitingNewSig = 0;
		return IRTOY_LONGSPACE;
	}
	res = read_with_timeout(dev->fd, dur, 2, 0);
	if (res != 2) {
		logprintf(LOG_ERR, "irtoy_read: could not get 2 bytes");
		return 0;
	}
	LOGPRINTF(3, "read_raw %02x%02x", dur[0], dur[1]);
	if (dur[0] == 0xff && dur[1] == 0xff) {
		dev->awaitingNewSig = 1;
		return IRTOY_LONGSPACE;
	}
	data = (lirc_t) (IRTOY_UNIT * (double) (256 * dur[0] + dur[1]));
	LOGPRINTF(3, "read_raw %d", data);

	if (dev->pulse) {
		data = data | PULSE_BIT;
	}
	dev->pulse = ! (dev->pulse);

	return data;
}


static lirc_t readdata(lirc_t timeout)
{
	lirc_t data = irtoy_read(dev, timeout);

	if (data) {
		LOGPRINTF(1, "readdata %d %d",
                          !!(data & PULSE_BIT), data & PULSE_MASK);
	}
	return(data);
}


static int irtoy_getversion(irtoy_t *dev)
{
	int res;
	char buf[16];
	int vNum;

	irtoy_readflush(dev, IRTOY_TIMEOUT_FLUSH);


	buf[0] = IRTOY_COMMAND_VERSION;
	res = write(dev->fd, buf, 1);

	if (res != 1) {
		logprintf(LOG_ERR,
                          "irtoy_getversion: couldn't write command");
		return 0;
	}

	res = read_with_timeout(dev->fd, buf,
                                IRTOY_LEN_VERSION,
                                IRTOY_TIMEOUT_VERSION);
	if (res != IRTOY_LEN_VERSION) {
		logprintf(LOG_ERR,
                          "irtoy_getversion: couldn't read version");
		logprintf(LOG_ERR,
                          "please make sure you are using firmware v20 " \
                          "or higher");
		return 0;
	}

	buf[IRTOY_LEN_VERSION] = 0;

	LOGPRINTF(1, "irtoy_getversion: Got version %s", buf);

	if (buf[0] != IRTOY_REPLY_VERSION) {
		logprintf(LOG_ERR,
                          "irtoy_getversion: invalid response %02X", buf[0]);
		logprintf(LOG_ERR,
                          "please make sure you are using firmware v20 " \
                          "or higher");
		return 0;
	}

	vNum = atoi(buf + 1);
	dev->hwVersion = vNum / 100;
	dev->swVersion = vNum % 100;
	return 1;
}


static int irtoy_reset(irtoy_t *dev)
{
	int res;
	char buf[16];

	buf[0] = IRTOY_COMMAND_RESET;
	res = write(dev->fd, buf, 1);

	if (res != 1) {
		logprintf(LOG_ERR, "irtoy_reset: couldn't write command");
		return 0;
	}

	irtoy_readflush(dev, IRTOY_TIMEOUT_FLUSH);

	return 1;
}


static int irtoy_enter_samplemode(irtoy_t *dev)
{

	int res;
	char buf[16];

	buf[0] = IRTOY_COMMAND_SMODE_ENTER;
	res = write(dev->fd, buf, 1);

	if (res != 1) {
		logprintf(LOG_ERR,
                          "irtoy_enter_samplemode: couldn't write command");
		return 0;
	}

	res = read_with_timeout(dev->fd, buf,
                                IRTOY_LEN_SAMPLEMODEPROTO,
                                IRTOY_TIMEOUT_SMODE_ENTER);
	if (res != IRTOY_LEN_SAMPLEMODEPROTO) {
		logprintf(LOG_ERR,
                         "irtoy_enter_samplemode: Can't read command result");
		return 0;
	}

	buf[IRTOY_LEN_SAMPLEMODEPROTO] = 0;
	if (buf[0] != IRTOY_REPLY_SAMPLEMODEPROTO) {
		logprintf(LOG_ERR,
			  "irtoy_enter_samplemode: invalid response %02X",
			  buf[0]);
		return 0;
	}

	LOGPRINTF(1, "irtoy_reset: Got protocol %s", buf);
	dev->protoVersion = atoi(buf + 1);
	return 1;
}


static irtoy_t *irtoy_hw_init(int fd)
{
	irtoy_t *dev = (irtoy_t *) malloc(sizeof(irtoy_t));

	if (dev == NULL) {
		logprintf(LOG_ERR, "init: out of memory");
		return NULL;
	}

	memset(dev, 0, sizeof(irtoy_t));

	dev->awaitingNewSig = 1;
	dev->fd = fd;
	dev->pulse = 1;

	irtoy_readflush(dev, IRTOY_TIMEOUT_FLUSH);

	if (!irtoy_reset(dev) || !irtoy_getversion(dev) ||
            !irtoy_enter_samplemode(dev)) {
		free(dev);
		dev = NULL;
		return NULL;
	}
	return dev;
}


static int init(void)
{
	if (!tty_create_lock(hw.device)) {
		logprintf(LOG_ERR, "irtoy: could not create lock files");
		return(0);
	}
	if ((hw.fd = open(hw.device, O_RDWR | O_NONBLOCK | O_NOCTTY)) < 0) {
		logprintf(LOG_ERR, "irtoy: could not open %s", hw.device);
		tty_delete_lock();
		return(0);
	}
	if (!tty_reset(hw.fd)) {
		logprintf(LOG_ERR, "irtoy: could not reset tty");
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}
	if (!tty_setbaud(hw.fd, 115200)) {
		logprintf(LOG_ERR, "irtoy: could not set baud rate");
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}
	if (!tty_setcsize(hw.fd, 8)) {
		logprintf(LOG_ERR, "irtoy: could not set csize");
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}
	if (!tty_setrtscts(hw.fd, 1)) {
		logprintf(LOG_ERR, "irtoy: could not enable hardware flow");
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}
	if ((dev = irtoy_hw_init(hw.fd)) == NULL) {
		logprintf(LOG_ERR,
			  "irtoy: No USB Irtoy device found at %s",
			  hw.device);
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}
	LOGPRINTF(1, "Version hw %d, sw %d, protocol %d\n",
		  dev->hwVersion, dev->swVersion, dev->protoVersion);
	if (dev->swVersion < IRTOY_MINFWVERSION) {
		logprintf(LOG_ERR,
			  "irtoy: Need firmware V%02d or higher, " \
                          "this firmware: %02d",
			  IRTOY_MINFWVERSION,
			  dev->swVersion);
		free(dev);
		return 0;
	}
	init_rec_buffer();
	init_send_buffer();

	return(1);
}


static int deinit(void)
{
	// IMPORTANT do not remove this reset. it is vital to return the
	// irtoy to IRMAN mode.
	// If we leave the irtoy in sample mode while no-one has the
	// tty open, the linux cdc_acm driver will fail on the next open.
	// This is apparently due to data being sent while the tty is not
	// open and fairly well known
	// (google for "tty_port_close_start: tty->count = 1 port count = 0")
	// IRMAN mode will wait until a signal is actually read before
	// sending the next one, while sample mode will keep streaming
	// (and under fluorescent light it WILL stream..)
	// triggering the problem
	if (dev != NULL) {
		irtoy_reset(dev);
		free(dev);
	}
	dev = NULL;

	close(hw.fd);
	hw.fd = -1;
	tty_delete_lock();
	return 1;
}


static char *receive(struct ir_remote *remotes)
{
	LOGPRINTF(1, "irtoy_raw_rec");
	if (!clear_rec_buffer())
		return(NULL);
	return decode_all(remotes);
}


static int irtoy_send_double_buffered(unsigned char * signals, int length)
{
	int numToXmit = length;
	int numThisTime;
	int res;
	unsigned char irToyBufLen;
	unsigned char *txPtr;
	unsigned char reply[16];
	int irtoyXmit;


	res = write(dev->fd, IRTOY_COMMAND_TXSTART, sizeof(IRTOY_COMMAND_TXSTART));

	if (res != sizeof(IRTOY_COMMAND_TXSTART)) {
		logprintf(LOG_ERR, "irtoy_send: couldn't write command");
		return 0;
	}

	res = read_with_timeout(dev->fd, &irToyBufLen, 1, IRTOY_TIMEOUT_READYFORDATA);

	if (res != 1) {
		logprintf(LOG_ERR, "irtoy_send: couldn't read command result");
		return -1;
	}

	LOGPRINTF(1, "irtoy ready for %d bytes\n", irToyBufLen);

	txPtr = signals;

	while (numToXmit) {
		numThisTime = (numToXmit < irToyBufLen) ? numToXmit : irToyBufLen;
		res = write(dev->fd, txPtr, numThisTime);

		if (res != numThisTime) {
			logprintf(LOG_ERR, "irtoy_send: couldn't write command");
			return 0;
		}

		txPtr += numThisTime;
		numToXmit -= numThisTime;


		res = read_with_timeout(dev->fd, &irToyBufLen, 1, IRTOY_TIMEOUT_READYFORDATA);

		if (res != 1) {
			logprintf(LOG_ERR, "irtoy_send: couldn't read command result");
			return -1;
		}

		LOGPRINTF(1, "irtoy ready for %d bytes\n", irToyBufLen);


	}


	res = read_with_timeout(dev->fd, reply, IRTOY_LEN_XMITRES, IRTOY_TIMEOUT_READYFORDATA);

	if (res != IRTOY_LEN_XMITRES) {
		logprintf(LOG_ERR, "irtoy_send: couldn't read command result");
		return -1;
	}

	LOGPRINTF(1, "%c %02X %02X %c\n", reply[0], reply[1], reply[2], reply[3]);

	if (reply[0] != IRTOY_REPLY_XMITCOUNT) {
		logprintf(LOG_ERR, "irtoy_send: invalid byte count indicator received: %02X", reply[0]);
		return 0;
	}

	irtoyXmit = (reply[1] << 8) | reply[2];
	if (length != irtoyXmit) {
		logprintf(LOG_ERR, "irtoy_send: incorrect byte count received: %d expected: %d", irtoyXmit, length);
		return 0;
	}

	if (reply[3] != IRTOY_REPLY_XMITSUCCESS) {
		logprintf(LOG_ERR, "irtoy_send: received error status %02X", reply[3]);
		return 0;
	}

	return 1;

}


static int send(struct ir_remote *remote, struct ir_ncode *code)
{
	int length;
	lirc_t *signals;

	int numToXmit;
	int i;
	lirc_t val;


	if (!init_send(remote, code)) {
		return 0;
	}

	length = send_buffer_length();
	signals = send_buffer_data();

	for (i = 0; i < length; i++) {
		val = (lirc_t)(((double) signals[i]) / IRTOY_UNIT);
		rawSB[2*i] = val >> 8;
		rawSB[2*i + 1] = val & 0xFF;
	}

	rawSB[2 * length] = 0xFF;
	rawSB[2 * length + 1] = 0xFF;

	numToXmit = 2 * length + 2;
	return irtoy_send_double_buffered(rawSB, numToXmit);
}
