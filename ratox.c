/* See LICENSE file for copyright and license details. */
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <tox/toxencryptsave.h>

#include "arg.h"
#include "queue.h"
#include "readpassphrase.h"
#include "util.h"

#define TOX_CLIENT_ID_SIZE TOX_PUBLIC_KEY_SIZE

#define TOX_FRIEND_ADDRESS_SIZE TOX_ADDRESS_SIZE

#define TOX_USER_STATUS_INVALID 0

const char *reqerr[] = {
	[TOX_ERR_FRIEND_ADD_OK]      = "No errors, completed successfully",
	[TOX_ERR_FRIEND_ADD_NULL]      = "Unexpected argument, NULL error",
	[TOX_ERR_FRIEND_ADD_TOO_LONG]      = "Message is too long",
	[TOX_ERR_FRIEND_ADD_NO_MESSAGE]    = "Please add a message to your request",
	[TOX_ERR_FRIEND_ADD_OWN_KEY]       = "That appears to be your own ID",
	[TOX_ERR_FRIEND_ADD_ALREADY_SENT]  = "Friend request already sent",
//	[TOX_ERR_FRIEND_ADD_UNKNOWN]      = "Unknown error while sending your request",
	[TOX_ERR_FRIEND_ADD_BAD_CHECKSUM]  = "Bad checksum while verifying address",
	[TOX_ERR_FRIEND_ADD_SET_NEW_NOSPAM] = "Friend already added but invalid nospam",
	[TOX_ERR_FRIEND_ADD_MALLOC]        = "Error increasing the friend list size"
};

const char *newerr[] = {
	[TOX_ERR_NEW_OK]		= "No Errors, completed successfully",
	[TOX_ERR_NEW_NULL]		= "Unexpected argument, NULL error",
	[TOX_ERR_NEW_MALLOC]		= "Unable to allocate memory for Tox data",
	[TOX_ERR_NEW_PORT_ALLOC]	= "Unable to bind port, check your permissions",
	[TOX_ERR_NEW_PROXY_BAD_TYPE]	= "Bad proxy type",
	[TOX_ERR_NEW_PROXY_BAD_HOST]	= "Proxy host has invalid format or does not exist",
	[TOX_ERR_NEW_PROXY_BAD_PORT]	= "Proxy port is invalid",
	[TOX_ERR_NEW_PROXY_NOT_FOUND]	= "Unable to resolve proxy",
	[TOX_ERR_NEW_LOAD_ENCRYPTED]	= "Unable to load encrypted data file",
	[TOX_ERR_NEW_LOAD_BAD_FORMAT]	= "Data file format error"
};

struct node {
	char    *addr4;
	char    *addr6;
	uint16_t port;
	char    *idstr;
};

#include "config.h"

struct file {
	int         type;
	const char *name;
	int         flags;
};

enum { NONE, FIFO, STATIC, FOLDER };
enum { IN, OUT, ERR };

static struct file gfiles[] = {
	[IN]  = { .type = FIFO,	  .name = "in",	 .flags = O_RDONLY | O_NONBLOCK	       },
	[OUT] = { .type = NONE,	  .name = "out", .flags = O_WRONLY | O_TRUNC | O_CREAT },
	[ERR] = { .type = STATIC, .name = "err", .flags = O_WRONLY | O_TRUNC | O_CREAT },
};

static int idfd = -1;

struct slot {
	const char *name;
	void      (*cb)(void *);
	int         outisfolder;
	int         dirfd;
	int         fd[LEN(gfiles)];
};

static void setname(void *);
static void setstatus(void *);
static void setuserstate(void *);
static void sendfriendreq(void *);
static void setnospam(void *);

enum { NAME, STATUS, STATE, REQUEST, NOSPAM };

static struct slot gslots[] = {
	[NAME]    = { .name = "name",	 .cb = setname,	      .outisfolder = 0, .dirfd = -1, .fd = {-1, -1, -1} },
	[STATUS]  = { .name = "status",	 .cb = setstatus,     .outisfolder = 0, .dirfd = -1, .fd = {-1, -1, -1} },
	[STATE]   = { .name = "state",	 .cb = setuserstate,  .outisfolder = 0, .dirfd = -1, .fd = {-1, -1, -1} },
	[REQUEST] = { .name = "request", .cb = sendfriendreq, .outisfolder = 1, .dirfd = -1, .fd = {-1, -1, -1} },
	[NOSPAM]  = { .name = "nospam",	 .cb = setnospam,     .outisfolder = 0, .dirfd = -1, .fd = {-1, -1, -1} },
};

enum { FTEXT_IN,
	FFILE_IN,
	FCALL_IN,
	FTEXT_OUT,
	FFILE_OUT,
	//FCALL_OUT,
	FREMOVE,
	FONLINE,
	FNAME,
	FSTATUS,
	FSTATE,
	FFILE_STATE,
	//FCALL_STATE 
};

static struct file ffiles[] = {
	[FTEXT_IN]    = { .type = FIFO,	  .name = "text_in",	  .flags = O_RDONLY | O_NONBLOCK	 },
	[FFILE_IN]    = { .type = FIFO,	  .name = "file_in",	  .flags = O_RDONLY | O_NONBLOCK	 },
	[FCALL_IN]    = { .type = FIFO,	  .name = "call_in",	  .flags = O_RDONLY | O_NONBLOCK	 },
	[FTEXT_OUT]   = { .type = STATIC, .name = "text_out",	  .flags = O_WRONLY | O_APPEND | O_CREAT },
	[FFILE_OUT]   = { .type = FIFO,	  .name = "file_out",	  .flags = O_WRONLY | O_NONBLOCK	 },
	//[FCALL_OUT]   = { .type = FIFO,	  .name = "call_out",	  .flags = O_WRONLY | O_NONBLOCK	 },
	[FREMOVE]     = { .type = FIFO,	  .name = "remove",	  .flags = O_RDONLY | O_NONBLOCK	 },
	[FONLINE]     = { .type = STATIC, .name = "online",	  .flags = O_WRONLY | O_TRUNC  | O_CREAT },
	[FNAME]	      = { .type = STATIC, .name = "name",	  .flags = O_WRONLY | O_TRUNC  | O_CREAT },
	[FSTATUS]     = { .type = STATIC, .name = "status",	  .flags = O_WRONLY | O_TRUNC  | O_CREAT },
	[FSTATE]      = { .type = STATIC, .name = "state",	  .flags = O_WRONLY | O_TRUNC  | O_CREAT },
	[FFILE_STATE] = { .type = STATIC, .name = "file_pending", .flags = O_WRONLY | O_TRUNC  | O_CREAT },
	//[FCALL_STATE] = { .type = STATIC, .name = "call_state",	  .flags = O_WRONLY | O_TRUNC  | O_CREAT },
};

static char *ustate[] = {
	[TOX_USER_STATUS_NONE]    = "none",
	[TOX_USER_STATUS_AWAY]    = "away",
	[TOX_USER_STATUS_BUSY]    = "busy"	//,
	//[TOX_USER_STATUS_INVALID] = "invalid"
};

enum { TRANSFER_NONE, TRANSFER_INITIATED, TRANSFER_PENDING, TRANSFER_INPROGRESS, TRANSFER_PAUSED };

struct transfer {
	uint8_t  fnum;
	uint8_t *buf;
	int      chunksz;
	ssize_t  n;
	int      pendingbuf;
	int      state;
	struct   timespec lastblock;
	int      cooldown;
};

enum {
	OUTGOING     = 1 << 0,
	INCOMING     = 1 << 1,
	TRANSMITTING = 1 << 2,
	INCOMPLETE   = 1 << 3,
};

/* struct call {
	int      num;
	int      state;
	uint8_t *frame;
	uint8_t  payload[RTP_PAYLOAD_SIZE];
	ssize_t  n;
	struct   timespec lastsent;
}; */

struct friend {
	char    name[TOX_MAX_NAME_LENGTH + 1];
	int32_t num;
	uint8_t id[TOX_CLIENT_ID_SIZE];
	char    idstr[2 * TOX_CLIENT_ID_SIZE + 1];
	int     dirfd;
	int     fd[LEN(ffiles)];
	struct  transfer tx;
	int     rxstate;
//	struct  call av;
	TAILQ_ENTRY(friend) entry;
};

struct request {
	uint8_t id[TOX_CLIENT_ID_SIZE];
	char    idstr[2 * TOX_CLIENT_ID_SIZE + 1];
	char   *msg;
	int     fd;
	TAILQ_ENTRY(request) entry;
};

static TAILQ_HEAD(friendhead, friend) friendhead = TAILQ_HEAD_INITIALIZER(friendhead);
static TAILQ_HEAD(reqhead, request) reqhead = TAILQ_HEAD_INITIALIZER(reqhead);

static Tox *tox;
static struct Tox_Options toxopt;

//static ToxAv *toxav;
//static ToxAvCSettings toxavconfig;
//static int    framesize;

static uint8_t *passphrase;
static uint32_t pplen;

static volatile sig_atomic_t running = 1;

static struct timespec timediff(struct timespec, struct timespec);
static void printrat(void);
static void logmsg(const char *, ...);
static int fifoopen(int, struct file);
static void fiforeset(int, int *, struct file);
static ssize_t fiforead(int, int *, struct file, void *, size_t);
//static uint32_t interval(Tox *, ToxAv *);
static uint32_t interval(Tox *);
/*static void cbcallinvite(void *, int32_t, void *);
static void cbcallstart(void *, int32_t, void *);
static void cbcallterminate(void *, int32_t, void *);
static void cbcalltypechange(void *, int32_t, void *);
static void cbcalldata(void *, int32_t, const int16_t *, uint16_t, void *);
static void cancelcall(struct friend *, char *);
static void sendfriendcalldata(struct friend *);*/
/**/
static void cbfriendrequest(Tox *, const uint8_t *, const uint8_t *, size_t,  void *);
static void cbconnstatus(Tox *, uint32_t, enum TOX_CONNECTION, void *udata);
static void cbfriendmessage(Tox *, uint32_t,  enum TOX_MESSAGE_TYPE,  const uint8_t *, size_t, void *);
static void cbnamechange(Tox *, uint32_t,  const uint8_t *, size_t,  void *);
static void cbstatusmessage(Tox *, uint32_t,  const uint8_t *, size_t,  void *);
static void cbuserstate(Tox *, uint32_t,  enum TOX_USER_STATUS,  void *);
static void cbfilecontrol(Tox *, uint32_t,  uint32_t,  enum TOX_FILE_CONTROL,  void *);
static void cbfilesendreq(Tox *, uint32_t,  uint32_t,  uint64_t,  size_t,  void *);
static void cbfiledata(Tox *, uint32_t,  uint32_t, uint32_t,  uint64_t, const uint8_t *, size_t, void *);
/**/

/*
static void cbconnstatus(Tox *, int32_t, uint8_t, void *);
static void cbfriendmessage(Tox *, int32_t, const uint8_t *, uint16_t, void *);
static void cbfriendrequest(Tox *, const uint8_t *, const uint8_t *, uint16_t, void *);
static void cbnamechange(Tox *, int32_t, const uint8_t *, uint16_t, void *);
static void cbstatusmessage(Tox *, int32_t, const uint8_t *, uint16_t, void *);
static void cbuserstate(Tox *, int32_t, uint8_t, void *);
static void cbfilecontrol(Tox *, int32_t, uint8_t, uint8_t, uint8_t, const uint8_t *, uint16_t, void *);
static void cbfilesendreq(Tox *, int32_t, uint8_t, uint64_t, const uint8_t *, uint16_t, void *);
static void cbfiledata(Tox *, int32_t, uint8_t, const uint8_t *, uint16_t, void *);
*/
static void canceltxtransfer(struct friend *);
static void cancelrxtransfer(struct friend *);
static void sendfriendfile(struct friend *);
static void sendfriendtext(struct friend *);
static void removefriend(struct friend *);
static int readpass(const char *, uint8_t **, uint32_t *);
static int tox_load(Tox *, uint8_t*, off_t);
static int tox_encrypted_load(Tox *, uint8_t*, off_t);
static int tox_save(Tox *, uint8_t *);
static int tox_encrypted_save(Tox *, uint8_t *, off_t);
static void dataload(void);
static void datasave(void);
static int localinit(void);
static int toxinit(void);
static int toxconnect(void);
static void id2str(uint8_t *, char *);
static void str2id(char *, uint8_t *);
static struct friend *friendcreate(int32_t);
static void friendload(void);
static void frienddestroy(struct friend *);
static void loop(void);
static void initshutdown(int);
static void shutdown(void);
static void usage(void);

#define FD_APPEND(fd) do {	\
	FD_SET((fd), &rfds);	\
	if ((fd) > fdmax)	\
		fdmax = (fd);	\
} while (0)

#undef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define MAX_CRYPTO_PACKET_SIZE 1400

#define crypto_box_MACBYTES 16

#define CRYPTO_DATA_PACKET_MIN_SIZE (1 + sizeof(uint16_t) + (sizeof(uint32_t) + sizeof(uint32_t)) + crypto_box_MACBYTES)

#define MAX_CRYPTO_DATA_SIZE (MAX_CRYPTO_PACKET_SIZE - CRYPTO_DATA_PACKET_MIN_SIZE)
static int tox_file_data_size(const Tox *tox, int32_t friendnumber)
{
    return MAX_CRYPTO_DATA_SIZE - 2;
}

#define TOX_ENC_SAVE_MAGIC_NUMBER "toxEsave"
#define TOX_ENC_SAVE_MAGIC_LENGTH 8

/* Load the messenger from data of size length. */
static int tox_load(Tox *tox, uint8_t* data, off_t sz)
{
	toxopt.savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
	toxopt.savedata_data = (uint8_t *) data;
	toxopt.savedata_length = sz;
    return 0;
}

off_t tox_size(const Tox *tox){
	off_t sz = tox_get_savedata_size(tox);
	return sz;
}

off_t tox_encrypted_size(const Tox * tox){
	off_t sz = tox_size(tox) + TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
	return sz;
}

/* Load the messenger from encrypted data of size length.
 *
 * returns 0 on success
 * returns -1 or less on failure
 */
static int tox_encrypted_load(Tox *tox, uint8_t * data, off_t sz)
{
	TOX_ERR_DECRYPTION pwerr;
	off_t plainsz = sz - TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
	uint8_t plain[plainsz];
	int r =	tox_pass_decrypt(data, sz, passphrase, pplen, plain, &pwerr );
	if (pwerr == TOX_ERR_DECRYPTION_OK){
		r = tox_load(tox, plain, sz);
	}
    return r;
}

/* Save the messenger in data (must be allocated memory of size Messenger_size()). */
static int tox_save(Tox *tox, uint8_t *data)
{
	tox_get_savedata(tox, data);
	if( data == NULL)
		return -1;
	return 0;
}

/* Save the messenger data encrypted with the given password.
 * data must be at least tox_encrypted_size().
 *
 * returns 0 on success
 * returns -1 on failure
 */
static int tox_encrypted_save(Tox *tox, uint8_t * data, off_t sz)
{
	TOX_ERR_ENCRYPTION pwerr;
	uint8_t enc[sz]; 
	int r =	tox_pass_encrypt(data, sz, passphrase, pplen, enc, &pwerr );
	if (pwerr == TOX_ERR_ENCRYPTION_OK){
		r = tox_save(tox, enc);
	}
	return r;
}

static struct timespec
timediff(struct timespec t1, struct timespec t2)
{
	struct timespec tmp;

	tmp.tv_sec = t2.tv_sec - t1.tv_sec;

	if ((t2.tv_nsec - t1.tv_nsec) > 0) {
		tmp.tv_nsec = (t2.tv_nsec - t1.tv_nsec);
	} else {
		tmp.tv_nsec = 1E9 - (t1.tv_nsec - t2.tv_nsec);
		tmp.tv_sec--;
	}

	return tmp;
}

static void
printrat(void)
{
	printf(	"\033[31m"
		"                /y\\            /y\\\n"
		"               /ver\\          /"VERSION"\\\n"
		"               yyyyyy\\      /yyyyyy\n"
		"               \\yyyyyyyyyyyyyyyyyy/\n"
		"                yyyyyyyyyyyyyyyyyy\n"
		"                yyyyyyyyyyyyyyyyyy\n"
		"                yyy'yyyyyyyyyy'yyy\n"
		"                \\yy  yyyyyyyy  yy/\n"
		"                 \\yy.yyyyyyyy.yy/\n"
		"                  \\yyyyyyyyyyyy/\n"
		"                    \\yyyyyyyy/\n"
		"              -------yyyyyyyy-------\n"
		"                 ..---yyyyyy---..\n"
		"                   ..--yyyy--..\n"
		"\033[0m\n");
}

static void
logmsg(const char *fmt, ...)
{
	time_t  t;
	va_list ap;
	char    buft[64];

	va_start(ap, fmt);
	t = time(NULL);
	strftime(buft, sizeof(buft), "%F %R", localtime(&t));
	printf("%s ", buft);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
}

static int
fifoopen(int dirfd, struct file f)
{
	int fd;

	fd = openat(dirfd, f.name, f.flags, 0666);
	if (fd < 0 && errno != ENXIO)
		eprintf("openat %s:", f.name);
	return fd;
}

static void
fiforeset(int dirfd, int *fd, struct file f)
{
	ssize_t r;

	r = unlinkat(dirfd, f.name, 0);
	if (r < 0 && errno != ENOENT)
		eprintf("unlinkat %s:", f.name);
	if (*fd != -1)
		close(*fd);
	r = mkfifoat(dirfd, f.name, 0666);
	if (r < 0 && errno != EEXIST)
		eprintf("mkfifoat %s:", f.name);
	*fd = fifoopen(dirfd, f);
}

static ssize_t
fiforead(int dirfd, int *fd, struct file f, void *buf, size_t sz)
{
	ssize_t r;

again:
	r = read(*fd, buf, sz);
	if (r == 0) {
		fiforeset(dirfd, fd, f);
		return 0;
	} else if (r < 0) {
		if (errno == EINTR)
			goto again;
		if (errno == EWOULDBLOCK)
			return -1;
		eprintf("read %s:", f.name);
	}
	return r;
}

/*static uint32_t
interval(Tox *m, ToxAv *av)
{
	return MIN(tox_do_interval(m), toxav_do_interval(av));
}*/

static uint32_t
interval(Tox *m)
{
	return tox_iteration_interval(m);
}

/*static void
cbcallinvite(void *av, int32_t cnum, void *udata)
{
	struct  friend *f;
	ToxAvCSettings avconfig;
	int32_t fnum, r;

	fnum = toxav_get_peer_id(toxav, cnum, 0);
	if (fnum < 0) {
		weprintf("Failed to determine peer-id from call-id\n");
		r = toxav_reject(toxav, cnum, NULL);
		if (r < 0)
			weprintf("Failed to reject call\n");
		return;
	}
	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->num == fnum)
			break;
	if (!f)
		return;

	f->av.num = cnum;
	r = toxav_get_peer_csettings(toxav, cnum, 0, &avconfig);
	if (r < 0) {
		weprintf("Failed to determine peer call type\n");
		r = toxav_reject(toxav, f->av.num, NULL);
		if (r < 0)
			weprintf("Failed to reject call\n");
		return;
	}

	logmsg(": %s : Audio : Rx > Inviting (%luHz/%luch)\n",
	       f->name, avconfig.audio_sample_rate, avconfig.audio_channels);

	ftruncate(f->fd[FCALL_STATE], 0);
	lseek(f->fd[FCALL_STATE], 0, SEEK_SET);
	dprintf(f->fd[FCALL_STATE], "pending\n");
}

static void
cbcallstart(void *av, int32_t cnum, void *udata)
{
	struct friend *f;
	int    r;

	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->av.num == cnum)
			break;
	if (!f)
		return;

	f->av.frame = malloc(sizeof(int16_t) * framesize);
	if (!f->av.frame)
		eprintf("malloc:");

	f->av.n = 0;
	f->av.lastsent.tv_sec = 0;
	f->av.lastsent.tv_nsec = 0;

	r = toxav_prepare_transmission(toxav, f->av.num, 0);
	if (r < 0) {
		weprintf("Failed to prepare Rx/Tx AV transmission\n");
		r = toxav_hangup(toxav, f->av.num);
		if (r < 0)
			weprintf("Failed to hang up\n");
		return;
	}
	f->av.state |= TRANSMITTING;

	ftruncate(f->fd[FCALL_STATE], 0);
	lseek(f->fd[FCALL_STATE], 0, SEEK_SET);
	dprintf(f->fd[FCALL_STATE], "active\n");

	logmsg(": %s : Audio > Started\n", f->name);
}

static void
cbcallterminate(void *av, int32_t cnum, void *udata)
{
	struct friend *f;
	int    r;

	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->av.num == cnum)
			break;
	if (!f)
		return;

	if (!strcmp(udata, "Peer timeout")) {
		r = toxav_stop_call(toxav, cnum);
		if (r < 0)
			weprintf("Failed to stop call\n");
	}
	cancelcall(f, udata);
}

static void
cbcalltypechange(void *av, int32_t cnum, void *udata)
{
	printf("Entered %s\n", __func__);
}

static void
cbcalldata(void *av, int32_t cnum, const int16_t *data, uint16_t len, void *udata)
{
	struct   friend *f;
	ssize_t  n, wrote;
	int      fd;
	uint8_t *buf;

	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->av.num == cnum)
			break;
	if (!f)
		return;
	if (!(f->av.state & INCOMING)) {*/
		/* try to open call_out for writing */
		/*fd = fifoopen(f->dirfd, ffiles[FCALL_OUT]);
		if (fd < 0) {
			close (fd);
			return;
		}
		if (f->fd[FCALL_OUT] < 0) {
			f->fd[FCALL_OUT] = fd;
			f->av.state |= INCOMING;
		}
	}

	buf = (uint8_t *)data;
	len *= 2;
	wrote = 0;
	while (len > 0) {
		n = write(f->fd[FCALL_OUT], &buf[wrote], len);
		if (n < 0) {
			if (errno == EPIPE)
				f->av.state &= ~INCOMING;
			break;
		} else if (n == 0) {
			break;
		}
		wrote += n;
		len -= n;
	}
}

static void
cancelcall(struct friend *f, char *action)
{
	int r;

	logmsg(": %s : Audio > %s\n", f->name, action);

	if (f->av.num != -1 && f->av.state & TRANSMITTING) {
		r = toxav_kill_transmission(toxav, f->av.num);
		if (r < 0)
			weprintf("Failed to kill transmission\n");
	}
	f->av.state = 0;
	f->av.num = -1;
*/
	/* Cancel Rx side of the call */
	/*if (f->fd[FCALL_OUT] != -1) {
		close(f->fd[FCALL_OUT]);
		f->fd[FCALL_OUT] = -1;
	}
	ftruncate(f->fd[FCALL_STATE], 0);
	lseek(f->fd[FCALL_STATE], 0, SEEK_SET);
	dprintf(f->fd[FCALL_STATE], "none\n");
*/
	/* Cancel Tx side of the call */
/*	free(f->av.frame);
	f->av.frame = NULL;
	fiforeset(f->dirfd, &f->fd[FCALL_IN], ffiles[FCALL_IN]);
}

static void
sendfriendcalldata(struct friend *f)
{
	struct  timespec now, diff;
	ssize_t n, payloadsize;
	int     r;

	n = fiforead(f->dirfd, &f->fd[FCALL_IN], ffiles[FCALL_IN],
		     f->av.frame + (f->av.state & INCOMPLETE) * f->av.n,
		     framesize * sizeof(int16_t) - (f->av.state & INCOMPLETE) * f->av.n);
	if (n == 0) {
		f->av.state &= ~OUTGOING;
		return;
	} else if (n < 0) {
		return;
	} else if (n == (framesize * sizeof(int16_t) - (f->av.state & INCOMPLETE) * f->av.n)) {
		f->av.state &= ~INCOMPLETE;
		f->av.n = 0;
	} else {
		f->av.state |= INCOMPLETE;
		f->av.n += n;
		return;
	}

	payloadsize = toxav_prepare_audio_frame(toxav, f->av.num,
						f->av.payload, sizeof(f->av.payload),
						(int16_t *)f->av.frame, framesize);
	if (payloadsize < 0) {
		weprintf("Failed to encode payload\n");
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	diff = timediff(f->av.lastsent, now);
	if (diff.tv_sec == 0 && diff.tv_nsec < toxavconfig.audio_frame_duration * 1E6) {
		diff.tv_nsec = toxavconfig.audio_frame_duration * 1E6 - diff.tv_nsec;
		nanosleep(&diff, NULL);
	}
	clock_gettime(CLOCK_MONOTONIC, &f->av.lastsent);
	r = toxav_send_audio(toxav, f->av.num, f->av.payload, payloadsize);
	if (r < 0)
		weprintf("Failed to send audio frame\n");
}
*/
static void
cbconnstatus(Tox *m, uint32_t frnum,  enum TOX_CONNECTION status,  void *udata)
{
	struct friend *f;
	struct request *req, *rtmp;
	int    r;
	char   name[TOX_MAX_NAME_LENGTH + 1];
	r = tox_friend_get_name_size(tox, frnum, NULL); //(uint8_t *)name);
	if (r < 0) {
		weprintf("Failed to get name for friend number %ld\n", (long)frnum);
		weprintf("Failed to get name for friend number %ld\n", r);
		return;
	} else if (r == 0) {
		snprintf(name, sizeof(name), "Anonymous");
	} else {
		name[r] = '\0';
	}

	switch(status){
		case TOX_CONNECTION_NONE:
			logmsg(": %s > %s\n", name, "Offline" );
		case TOX_CONNECTION_TCP:
			logmsg(": %s > %s\n", name, "Online using TCP" );
		case TOX_CONNECTION_UDP:
			logmsg(": %s > %s\n", name, "Online using UDP" );
	}	   

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->num == frnum) {
			ftruncate(f->fd[FONLINE], 0);
			lseek(f->fd[FONLINE], 0, SEEK_SET);
			dprintf(f->fd[FONLINE], "%d\n", status);
			break;
		}
	}

	/* Remove the pending request-FIFO if it exists */
	for (req = TAILQ_FIRST(&reqhead); req; req = rtmp) {
		rtmp = TAILQ_NEXT(req, entry);

		if (memcmp(f->id, req->id, TOX_CLIENT_ID_SIZE))
			continue;
		unlinkat(gslots[REQUEST].fd[OUT], req->idstr, 0);
		close(req->fd);
		TAILQ_REMOVE(&reqhead, req, entry);
		free(req->msg);
		free(req);
	}
}

static void
cbfriendmessage(Tox *m, uint32_t frnum,  enum TOX_MESSAGE_TYPE type,  const uint8_t * data, size_t len,  void *udata)
{
	struct  friend *f;
	time_t  t;
	uint8_t msg[len + 1];
	char    buft[64];

	memcpy(msg, data, len);
	msg[len] = '\0';

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->num == frnum) {
			t = time(NULL);
			strftime(buft, sizeof(buft), "%F %R", localtime(&t));
			dprintf(f->fd[FTEXT_OUT], "%s %s\n", buft, msg);
			logmsg(": %s > %s\n", f->name, msg);
			break;
		}
	}
}

static void
cbfriendrequest(Tox *tox, const uint8_t *id, const uint8_t *data, size_t len, void *udata)
{
	struct file reqfifo;
	struct request *req;

	req = calloc(1, sizeof(*req));
	if (!req)
		eprintf("calloc:");
	req->fd = -1;

	memcpy(req->id, id, TOX_CLIENT_ID_SIZE);
	id2str(req->id, req->idstr);

	if (len > 0) {
		req->msg = malloc(len + 1);
		if (!req->msg)
			eprintf("malloc:");
		memcpy(req->msg, data, len);
		req->msg[len] = '\0';
	}

	reqfifo.name = req->idstr;
	reqfifo.flags = O_RDONLY | O_NONBLOCK;
	fiforeset(gslots[REQUEST].fd[OUT], &req->fd, reqfifo);

	TAILQ_INSERT_TAIL(&reqhead, req, entry);

	logmsg("Request : %s > %s\n",
	       req->idstr, req->msg);
}

static void
cbnamechange(Tox *m, uint32_t frnum,  const uint8_t *data, size_t len,  void *udata)
{
	struct  friend *f;
	uint8_t name[len + 1];

	memcpy(name, data, len);
	name[len] = '\0';

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->num == frnum) {
			if (memcmp(f->name, name, len + 1) == 0)
				break;
			ftruncate(f->fd[FNAME], 0);
			lseek(f->fd[FNAME], 0, SEEK_SET);
			dprintf(f->fd[FNAME], "%s\n", name);
			logmsg(": %s : Name > %s\n", f->name, name);
			memcpy(f->name, name, len + 1);
			break;
		}
	}
	datasave();
}

static void
cbstatusmessage(Tox *m, uint32_t frnum,  const uint8_t *data, size_t len,  void *udata)
{
	struct friend *f;
	uint8_t status[len + 1];

	memcpy(status, data, len);
	status[len] = '\0';

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->num == frnum) {
			ftruncate(f->fd[FSTATUS], 0);
			lseek(f->fd[FSTATUS], 0, SEEK_SET);
			dprintf(f->fd[FSTATUS], "%s\n", status);
			logmsg(": %s : Status > %s\n", f->name, status);
			break;
		}
	}
	datasave();
}

static void
cbuserstate(Tox *m, uint32_t frnum,  enum TOX_USER_STATUS state,  void *udata)
{
	struct friend *f;

	if (state >= LEN(ustate)) {
		weprintf("Received invalid user status: %d\n", state);
		return;
	}

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->num == frnum) {
			ftruncate(f->fd[FSTATE], 0);
			lseek(f->fd[FSTATE], 0, SEEK_SET);
			dprintf(f->fd[FSTATE], "%s\n", ustate[state]);
			logmsg(": %s : State > %s\n", f->name, ustate[state]);
			break;
		}
	}
	datasave();
}

static void
cbfilecontrol(Tox *m, uint32_t frnum,  uint32_t fnum,  enum TOX_FILE_CONTROL ctrltype,  void *udata)
{
	struct friend *f;
	int rec_sen = (fnum == 0 && ctrltype == TOX_FILE_CONTROL_RESUME) ? 1 : 0;
	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->num == frnum)
			break;
	if (!f)
		return;

	switch (ctrltype) {
	case TOX_FILE_CONTROL_RESUME:
		if (rec_sen == 1) {
			if (f->tx.state == TRANSFER_PAUSED) {
				logmsg(": %s : Tx > Resumed\n", f->name);
				f->tx.state = TRANSFER_INPROGRESS;
			} else {
				f->tx.fnum = fnum;
				f->tx.chunksz = tox_file_data_size(tox, fnum);
				f->tx.buf = malloc(f->tx.chunksz);
				if (!f->tx.buf)
					eprintf("malloc:");
				f->tx.n = 0;
				f->tx.pendingbuf = 0;
				f->tx.state = TRANSFER_INPROGRESS;
				logmsg(": %s : Tx > In Progress\n", f->name);
			}
		}
		break;
	case TOX_FILE_CONTROL_PAUSE:
		if (rec_sen == 1) {
			if (f->tx.state == TRANSFER_INPROGRESS) {
				logmsg(": %s : Tx > Paused\n", f->name);
				f->tx.state = TRANSFER_PAUSED;
			}
		}
		break;
	case TOX_FILE_CONTROL_CANCEL:
		if (rec_sen == 1) {
			logmsg(": %s : Tx > Rejected\n", f->name);
			f->tx.state = TRANSFER_NONE;
			free(f->tx.buf);
			f->tx.buf = NULL;
			f->tx.lastblock.tv_sec = 0;
			f->tx.lastblock.tv_nsec = 0;
			f->tx.cooldown = 0;
			fiforeset(f->dirfd, &f->fd[FFILE_IN], ffiles[FFILE_IN]);
		} else {
			logmsg(": %s : Rx > Cancelled by Sender\n", f->name);
			cancelrxtransfer(f);
		}
		break;
	default:
		weprintf("Unhandled file control type: %d\n", ctrltype);
		break;
	};
}

static void
cbfilesendreq(Tox *m, uint32_t frnum,  uint32_t fnum,  uint64_t fsz,  size_t flen,  void *udata)
{
	struct  friend *f;
	uint8_t filename[flen + 1];

	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->num == frnum)
			break;
	if (!f)
		return;

	//memcpy(filename, fname, flen);
	filename[flen] = '\0';

	/* We only support a single transfer at a time */
	if (f->rxstate == TRANSFER_INPROGRESS) {
		logmsg(": %s : Rx > Rejected %s, already one in progress\n",
		       f->name, filename);
		if (tox_file_control(tox, f->num, fnum, TOX_FILE_CONTROL_CANCEL, NULL) < 0)
			weprintf("Failed to kill new Rx transfer\n");
		return;
	}

	ftruncate(f->fd[FFILE_STATE], 0);
	lseek(f->fd[FFILE_STATE], 0, SEEK_SET);
	dprintf(f->fd[FFILE_STATE], "%s\n", filename);
	f->rxstate = TRANSFER_PENDING;
	logmsg(": %s : Rx > Pending %s\n", f->name, filename);
}

static void
cbfiledata(Tox *m, uint32_t frnum,  uint32_t fnum,  uint32_t fileid,  uint64_t fsz, 
				const uint8_t *data, size_t len,  void *udata)
{
	struct   friend *f;
	ssize_t  n;
	uint16_t wrote = 0;

	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->num == frnum)
			break;
	if (!f)
		return;

	while (len > 0) {
		n = write(f->fd[FFILE_OUT], &data[wrote], len);
		if (n < 0) {
			if (errno == EPIPE) {
				cancelrxtransfer(f);
				break;
			} else if (errno == EWOULDBLOCK) {
				continue;
			}
			break;
		} else if (n == 0) {
			break;
		}
		wrote += n;
		len -= n;
	}
}

static void
canceltxtransfer(struct friend *f)
{
	if (f->tx.state == TRANSFER_NONE)
		return;
	logmsg(": %s : Tx > Cancelling\n", f->name);
	if (tox_file_control(tox, f->num, 0, TOX_FILE_CONTROL_CANCEL, NULL) < 0)
		weprintf("Failed to kill Tx transfer\n");
	f->tx.state = TRANSFER_NONE;
	free(f->tx.buf);
	f->tx.buf = NULL;
	f->tx.lastblock.tv_sec = 0;
	f->tx.lastblock.tv_nsec = 0;
	f->tx.cooldown = 0;
	fiforeset(f->dirfd, &f->fd[FFILE_IN], ffiles[FFILE_IN]);
}

static void
cancelrxtransfer(struct friend *f)
{
	if (f->rxstate == TRANSFER_NONE)
		return;
	logmsg(": %s : Rx > Cancelling\n", f->name);
	if (tox_file_control(tox, f->num, 0, TOX_FILE_CONTROL_CANCEL, NULL) < 0)
		weprintf("Failed to kill Rx transfer\n");
	if (f->fd[FFILE_OUT] != -1) {
		close(f->fd[FFILE_OUT]);
		f->fd[FFILE_OUT] = -1;
	}
	ftruncate(f->fd[FFILE_STATE], 0);
	lseek(f->fd[FFILE_STATE], 0, SEEK_SET);
	f->rxstate = TRANSFER_NONE;
}

static void
sendfriendfile(struct friend *f)
{
	struct  timespec start, now, diff = {0, 0};
	ssize_t n;

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (diff.tv_sec == 0 && diff.tv_nsec < interval(tox) * 1E6) {
		/* Attempt to transmit the pending buffer */
		if (f->tx.pendingbuf) {
			if (tox_file_send(tox, 
					f->num, 
					0,
					UINT64_MAX,
					&f->tx.fnum, 
					f->tx.buf, 				
					f->tx.n, 
					NULL) < 0) {
				clock_gettime(CLOCK_MONOTONIC, &f->tx.lastblock);
				f->tx.cooldown = 1;
				break;
			}
			f->tx.pendingbuf = 0;
		}
		/* Grab another buffer from the FIFO */
		n = fiforead(f->dirfd, &f->fd[FFILE_IN], ffiles[FFILE_IN], f->tx.buf,
			     f->tx.chunksz);
		if (n == 0) {
			/* Signal transfer completion to other end */
			if (tox_file_control(tox, f->num, f->tx.fnum,
						  TOX_FILE_CONTROL_CANCEL, NULL) < 0)
				weprintf("Failed to signal transfer completion to the receiver\n");
			f->tx.state = TRANSFER_NONE;
			break;
		}
		if (n < 0) {
			if (errno != EWOULDBLOCK)
				weprintf("fiforead:");
			break;
		}
		/* Store transfer size in case we can't send it right now */
		f->tx.n = n;
		if (tox_file_send(tox, 
				f->num, 
				0,
				UINT64_MAX,
				&f->tx.fnum, 
				f->tx.buf, 				
				f->tx.n, 
				NULL) < 0) {
			clock_gettime(CLOCK_MONOTONIC, &f->tx.lastblock);
			f->tx.cooldown = 1;
			f->tx.pendingbuf = 1;
			return;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		diff = timediff(start, now);
	}
}

static void
sendfriendtext(struct friend *f)
{
	ssize_t n;
	int     r;
	uint8_t buf[TOX_MAX_MESSAGE_LENGTH];

	n = fiforead(f->dirfd, &f->fd[FTEXT_IN], ffiles[FTEXT_IN], buf, sizeof(buf));
	if (n <= 0)
		return;
	if (buf[n - 1] == '\n')
		n--;
	r = tox_friend_send_message(tox, f->num, TOX_MESSAGE_TYPE_ACTION, buf, n, NULL);
	if (r < 0)
		weprintf("Failed to send message\n");
}

static void
removefriend(struct friend *f)
{
	char c;

	if (fiforead(f->dirfd, &f->fd[FREMOVE], ffiles[FREMOVE], &c, 1) != 1 || c != '1')
		return;
	tox_friend_delete(tox, f->num, NULL);
	datasave();
	logmsg(": %s > Removed\n", f->name);
	frienddestroy(f);
}

static int
readpass(const char *prompt, uint8_t **target, uint32_t *len)
{
	char pass[BUFSIZ], *p;

	p = readpassphrase(prompt, pass, sizeof(pass), RPP_ECHO_OFF);
	if (!p) {
		weprintf("readpassphrase:");
		return -1;
	}
	if (p[0] == '\0')
		return -1;
	*target = realloc(*target, strlen(p)); /* not null-terminated */
	if (!*target)
		eprintf("malloc:");
	memcpy(*target, p, strlen(p));
	*len = strlen(p);
	return 0;
}

static void
dataload(void)
{
	off_t    sz;
	uint32_t pp2len = 0;
	int      fd;
	uint8_t *data, *passphrase2 = NULL;

	fd = open(savefile, O_RDONLY);
	if (fd < 0) {
		if (encryptsavefile) {
reprompt1:
			while (readpass("Data : New passphrase > ", &passphrase, &pplen) < 0);
			while (readpass("Data : Re-enter passphrase > ", &passphrase2, &pp2len) < 0);

			if (pplen != pp2len || memcmp(passphrase, passphrase2, pplen)) {
				weprintf("Data : Passphrase mismatch\n");
				goto reprompt1;
			}
			free(passphrase2);
		}
		return;
	}

	sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	if (sz == 0) {
		weprintf("Data : %s > Empty\n", savefile);
		return;
	}

	data = malloc(sz);
	if (!data)
		eprintf("malloc:");

	if (read(fd, data, sz) != sz)
		eprintf("read %s:", savefile);

	if (tox_is_data_encrypted(data)) {
		if (!encryptsavefile)
			logmsg("Data : %s > Encrypted, but saving unencrypted\n", savefile);
		while (readpass("Data : Passphrase > ", &passphrase, &pplen) < 0 ||
		       tox_encrypted_load(tox, data, sz) < 0);
	} else {
		if (tox_load(tox, data, sz) < 0)
			eprintf("Data : %s > Failed to load\n", savefile);
		if (encryptsavefile) {
			logmsg("Data : %s > Not encrypted, but saving encrypted\n", savefile);
reprompt2:
			while (readpass("Data : New passphrase > ", &passphrase, &pplen) < 0);
			while (readpass("Data : Re-enter passphrase > ", &passphrase2, &pp2len) < 0);

			if (pplen != pp2len || memcmp(passphrase, passphrase2, pplen)) {
				weprintf("Data : Passphrase mismatch\n");
				goto reprompt2;
			}
			free(passphrase2);
		}
	}
	free(data);
	close(fd);
	
}

static void
datasave(void)
{
	off_t    sz;
	int      fd;
	uint8_t *data;

	fd = open(savefile, O_WRONLY | O_TRUNC | O_CREAT , 0666);
	if (fd < 0)
		eprintf("open %s:", savefile);

	sz = encryptsavefile ? tox_encrypted_size(tox) : tox_size(tox);
	data = malloc(sz);
	if (!data)
		eprintf("malloc:");
	if (encryptsavefile)
		tox_encrypted_save(tox, data, sz);
	else
		tox_save(tox, data);
	if (write(fd, data, sz) != sz)
		eprintf("write %s:", savefile);
	fsync(fd);

	free(data);
	close(fd);
}

static int
localinit(void)
{
	DIR    *d;
	size_t  i, m;
	int     r;
	uint8_t name[TOX_MAX_NAME_LENGTH + 1];
	uint8_t address[TOX_FRIEND_ADDRESS_SIZE];
	uint8_t status[TOX_MAX_STATUS_MESSAGE_LENGTH + 1];


	for (i = 0; i < LEN(gslots); i++) {
		r = mkdir(gslots[i].name, 0777);
		if (r < 0 && errno != EEXIST)
			eprintf("mkdir %s:", gslots[i].name);
		d = opendir(gslots[i].name);
		if (!d)
			eprintf("opendir %s:", gslots[i].name);
		r = dirfd(d);
		if (r < 0)
			eprintf("dirfd %s:", gslots[i].name);
		gslots[i].dirfd = r;

		for (m = 0; m < LEN(gfiles); m++) {
			if (gfiles[m].type == FIFO) {
				fiforeset(gslots[i].dirfd, &gslots[i].fd[m], gfiles[m]);
			} else if (gfiles[m].type == STATIC || (gfiles[m].type == NONE && !gslots[i].outisfolder)) {
				gslots[i].fd[m] = fifoopen(gslots[i].dirfd, gfiles[m]);
			} else if (gfiles[m].type == NONE && gslots[i].outisfolder) {
				r = mkdirat(gslots[i].dirfd, gfiles[m].name, 0777);
				if (r < 0 && errno != EEXIST)
					eprintf("mkdirat %s:", gfiles[m].name);
				r = openat(gslots[i].dirfd, gfiles[m].name, O_RDONLY | O_DIRECTORY);
				if (r < 0)
					eprintf("openat %s:", gfiles[m].name);
				gslots[i].fd[m] = r;
			}
		}
	}

	/* Dump current name */
	r = tox_self_get_name_size(tox);
	tox_self_get_name(tox, name);
	if (r == 0) {
		weprintf("Name : Empty\n");
	} else if (r > sizeof(name) - 1) {
		r = sizeof(name) - 1;
	}
	name[r] = '\0';
	ftruncate(gslots[NAME].fd[OUT], 0);
	dprintf(gslots[NAME].fd[OUT], "%s\n", name);

	/* Dump status */
	r = tox_self_get_status_message_size(tox);
	tox_self_get_status_message(tox, status);
	if (r == 0) {
		weprintf("Status : Empty\n");
	} else if (r > sizeof(status) - 1) {
		r = sizeof(status) - 1;
	}
	status[r] = '\0';
	ftruncate(gslots[STATUS].fd[OUT], 0);
	dprintf(gslots[STATUS].fd[OUT], "%s\n", status);

	/* Dump user state */
	r = tox_self_get_status(tox);
	if (r < 0) {
		weprintf("State : Empty\n");
	} else if (r >= LEN(ustate)) {
		ftruncate(gslots[STATE].fd[ERR], 0);
		dprintf(gslots[STATE].fd[ERR], "invalid\n");
		weprintf("State : %d > Invalid\n", r);
	} else {
		ftruncate(gslots[STATE].fd[OUT], 0);
		dprintf(gslots[STATE].fd[OUT], "%s\n", ustate[r]);
	}

	/* Dump ID */
	idfd = open("id", O_WRONLY | O_CREAT, 0666);
	if (idfd < 0)
		eprintf("open %s:", "id");
	tox_self_get_address(tox, address);
	for (i = 0; i < TOX_FRIEND_ADDRESS_SIZE; i++)
		dprintf(idfd, "%02X", address[i]);
	dprintf(idfd, "\n");

	/* Dump Nospam */
	ftruncate(gslots[NOSPAM].fd[OUT], 0);
	dprintf(gslots[NOSPAM].fd[OUT], "%08X\n", tox_self_get_nospam(tox));

	return 0;
}

static int
toxinit(void)
{
	toxopt.ipv6_enabled = ipv6;
	toxopt.udp_enabled = udp;
	if (proxy) {
		udp = 0;
		toxopt.udp_enabled = udp;
		logmsg("Net > Forcing TCP mode\n");
		size_t psz = sizeof(toxopt.proxy_host);
		snprintf((char *)toxopt.proxy_host, psz,
			 "%s", proxyaddr);
		toxopt.proxy_port = proxyport;
		toxopt.proxy_type = proxytype;
		logmsg("Net > Using proxy %s:%hu\n", proxyaddr, proxyport);
	}

	tox = tox_new(&toxopt, NULL);
	if (!tox)
		eprintf("Core : Tox > Initialization failed\n");

	dataload();
	datasave();

	/*toxav = toxav_new(tox, MAXCALLS);
	if (!toxav)
		eprintf("Core : ToxAV > Initialization failed\n");

	toxavconfig = av_DefaultSettings;
	framesize = (toxavconfig.audio_sample_rate * toxavconfig.audio_frame_duration *
		     toxavconfig.audio_channels) / 1000;*/

	tox_callback_friend_connection_status(tox, cbconnstatus, NULL);
	tox_callback_friend_message(tox, cbfriendmessage, NULL);
	tox_callback_friend_request(tox, cbfriendrequest, NULL);
	tox_callback_friend_name(tox, cbnamechange, NULL);
	tox_callback_friend_status_message(tox, cbstatusmessage, NULL);
	tox_callback_friend_status(tox, cbuserstate, NULL);
	tox_callback_file_recv_control(tox, cbfilecontrol, NULL);
	tox_callback_file_chunk_request(tox, cbfilesendreq, NULL);
	tox_callback_file_recv(tox, cbfiledata, NULL);

	/*toxav_register_callstate_callback(toxav, cbcallinvite, av_OnInvite, NULL);
	toxav_register_callstate_callback(toxav, cbcallstart, av_OnStart, NULL);
	toxav_register_callstate_callback(toxav, cbcallterminate, av_OnEnd, "Ended");
	toxav_register_callstate_callback(toxav, cbcallterminate, av_OnCancel, "Cancelled");
	toxav_register_callstate_callback(toxav, cbcallterminate, av_OnReject, "Rejected");

	toxav_register_callstate_callback(toxav, cbcallterminate, av_OnRequestTimeout, "Request timeout");
	toxav_register_callstate_callback(toxav, cbcallterminate, av_OnPeerTimeout, "Peer timeout");
	toxav_register_callstate_callback(toxav, cbcalltypechange, av_OnPeerCSChange, NULL);
	toxav_register_callstate_callback(toxav, cbcalltypechange, av_OnSelfCSChange, NULL);

	toxav_register_audio_callback(toxav, cbcalldata, NULL);*/

	return 0;
}

static int
toxconnect(void)
{
	struct  node *n;
	struct  node tmp;
	size_t  i, j;
	int     r;
	uint8_t id[TOX_CLIENT_ID_SIZE];

	srand(time(NULL));

	/* shuffle it to minimize load on nodes */
	for (i = LEN(nodes) - 1; i > 0; i--) {
		j = rand() % LEN(nodes);
		tmp = nodes[j];
		nodes[j] = nodes[i];
		nodes[i] = tmp;
	}

	for (i = 0; i < LEN(nodes); i++) {
		n = &nodes[i];
		if (ipv6 && !n->addr6)
			continue;
		str2id(n->idstr, id);
		r = tox_bootstrap(tox, ipv6 ? n->addr6 : n->addr4, n->port, id, NULL);
		if (r == 0)
			weprintf("Net : %s > Bootstrap failed\n", ipv6 ? n->addr6 : n->addr4);
	}
	return 0;
}

/* Caller has to ensure `idstr' is big enough */
static void
id2str(uint8_t *id, char *idstr)
{
	int  i;
	char hex[] = "0123456789ABCDEF";

	for (i = 0; i < TOX_CLIENT_ID_SIZE; i++) {
		*idstr++ = hex[(id[i] >> 4) & 0xf];
		*idstr++ = hex[id[i] & 0xf];
	}
	*idstr = '\0';
}

/* Caller has to ensure that `id' is big enough */
static void
str2id(char *idstr, uint8_t *id)
{
	size_t i, len = strlen(idstr) / 2;
	char  *p = idstr;

	for (i = 0; i < len; ++i, p += 2)
		sscanf(p, "%2hhx", &id[i]);
}

static struct friend *
friendcreate(int32_t frnum)
{
	struct  friend *f;
	DIR    *d;
	size_t  i;
	size_t     r;
	uint8_t status[TOX_MAX_STATUS_MESSAGE_LENGTH + 1];

	f = calloc(1, sizeof(*f));
	if (!f)
		eprintf("calloc:");

	r = tox_friend_get_name_size(tox, frnum, NULL) ;//(uint8_t *)f->name);
	if (r < 0) {
		weprintf(": %ld : Name : Failed to get\n", (long)frnum);
		return NULL;
	} else if (r == 0) {
		snprintf(f->name, sizeof(f->name), "Anonymous");
	} else {
		f->name[r] = '\0';
	}

	f->num = frnum;
	tox_friend_get_public_key(tox, f->num, f->id, NULL);
	id2str(f->id, f->idstr);

	r = mkdir(f->idstr, 0777);
	if (r < 0 && errno != EEXIST)
		eprintf("mkdir %s:", f->idstr);

	d = opendir(f->idstr);
	if (!d)
		eprintf("opendir %s:", f->idstr);

	r = dirfd(d);
	if (r < 0)
		eprintf("dirfd %s:", f->idstr);
	f->dirfd = r;

	for (i = 0; i < LEN(ffiles); i++) {
		f->fd[i] = -1;
		if (ffiles[i].type == FIFO) {
			fiforeset(f->dirfd, &f->fd[i], ffiles[i]);
		} else if (ffiles[i].type == STATIC) {
			f->fd[i] = fifoopen(f->dirfd, ffiles[i]);
		}
	}

	/* Dump name */
	ftruncate(f->fd[FNAME], 0);
	dprintf(f->fd[FNAME], "%s\n", f->name);

	/* Dump online state */
	ftruncate(f->fd[FONLINE], 0);
	dprintf(f->fd[FONLINE], "%d\n",
		tox_friend_get_status(tox, frnum, NULL));

	/* Dump status */
	r = tox_friend_get_status_message_size(tox, frnum, NULL);
	tox_friend_get_status_message(tox, frnum, status, NULL);
	if (r < 0) {
		weprintf(": %s : Status : Failed to get\n", f->name);
		r = 0;
	} else if (r > sizeof(status) - 1) {
		r = sizeof(status) - 1;
	}
	status[r] = '\0';
	ftruncate(f->fd[FSTATUS], 0);
	dprintf(f->fd[FSTATUS], "%s\n", status);

	/* Dump user state */
	r = tox_friend_get_status(tox, frnum, NULL);
	if (r < 0) {
		weprintf(": %s : State : Failed to get\n", f->name);
	} else if (r >= LEN(ustate)) {
		weprintf(": %s : State : %d > Invalid\n", f->name, r);
	} else {
		ftruncate(f->fd[FSTATE], 0);
		dprintf(f->fd[FSTATE], "%s\n", ustate[r]);
	}

	/* Dump file pending state */
	ftruncate(f->fd[FFILE_STATE], 0);

	/* Dump call pending state */
//	ftruncate(f->fd[FCALL_STATE], 0);
//	dprintf(f->fd[FCALL_STATE], "none\n");

//	f->av.state = 0;
//	f->av.num = -1;

	TAILQ_INSERT_TAIL(&friendhead, f, entry);

	return f;
}

static void
frienddestroy(struct friend *f)
{
	int i;

	canceltxtransfer(f);
	cancelrxtransfer(f);
	//if (f->av.num != -1 && toxav_get_call_state(toxav, f->av.num) != av_CallNonExistent)
		//cancelcall(f, "Destroying"); /* todo: check state */
	for (i = 0; i < LEN(ffiles); i++) {
		if (f->dirfd != -1) {
			unlinkat(f->dirfd, ffiles[i].name, 0);
			if (f->fd[i] != -1)
				close(f->fd[i]);
		}
	}
	rmdir(f->idstr);
	TAILQ_REMOVE(&friendhead, f, entry);
}

static void
friendload(void)
{
	uint32_t sz;
	uint32_t i;
	uint32_t *frnums;

	sz = tox_self_get_friend_list_size(tox);
	frnums = malloc(sz * sizeof(*frnums));
	if (!frnums)
		eprintf("malloc:");

	tox_self_get_friend_list(tox, frnums);

	for (i = 0; i < sz; i++)
		friendcreate(frnums[i]);

	free(frnums);
}

static void
setname(void *data)
{
	ssize_t n;
	int     r;
	char    name[TOX_MAX_NAME_LENGTH + 1];

	n = fiforead(gslots[NAME].dirfd, &gslots[NAME].fd[IN],
		     gfiles[IN], name, sizeof(name) - 1);
	if (n <= 0)
		return;
	if (name[n - 1] == '\n')
		n--;
	name[n] = '\0';
	tox_self_set_name(tox, (uint8_t *)name, n, NULL);
	r = tox_self_get_name_size(tox);
	if (r < 0) {
		weprintf("Failed to set name to \"%s\"\n", name);
		return;
	}
	datasave();
	logmsg("Name > %s\n", name);
	ftruncate(gslots[NAME].fd[OUT], 0);
	lseek(gslots[NAME].fd[OUT], 0, SEEK_SET);
	dprintf(gslots[NAME].fd[OUT], "%s\n", name);
}

static void
setstatus(void *data)
{
	ssize_t n;
	int     r;
	uint8_t status[TOX_MAX_STATUS_MESSAGE_LENGTH + 1];

	n = fiforead(gslots[STATUS].dirfd, &gslots[STATUS].fd[IN], gfiles[IN],
		     status, sizeof(status) - 1);
	if (n <= 0)
		return;
	if (status[n - 1] == '\n')
		n--;
	status[n] = '\0';
	tox_self_set_status_message(tox, status, n, NULL);
	r = tox_self_get_status_message_size(tox);
	if (r < 0) {
		weprintf("Failed to set status message to \"%s\"\n");
		return;
	}
	datasave();
	logmsg("Status > %s\n", status);
	ftruncate(gslots[STATUS].fd[OUT], 0);
	lseek(gslots[STATUS].fd[OUT], 0, SEEK_SET);
	dprintf(gslots[STATUS].fd[OUT], "%s\n", status);
}

static void
setuserstate(void *data)
{
	size_t  i;
	ssize_t n;
	char    buf[PIPE_BUF];

	n = fiforead(gslots[STATE].dirfd, &gslots[STATE].fd[IN], gfiles[IN],
		     buf, sizeof(buf) - 1);
	if (n <= 0)
		return;
	if (buf[n - 1] == '\n')
		n--;
	buf[n] = '\0';
	for (i = 0; i < LEN(ustate); i++) {
		if (i != TOX_USER_STATUS_INVALID && strcmp(buf, ustate[i]) == 0) {
			tox_self_set_status(tox, i);
			break;
		}
	}
	if (i == LEN(ustate)) {
		ftruncate(gslots[STATE].fd[ERR], 0);
		lseek(gslots[STATE].fd[ERR], 0, SEEK_SET);
		dprintf(gslots[STATE].fd[ERR], "invalid\n");
		weprintf("Invalid state: %s\n", buf);
		return;
	}
	ftruncate(gslots[STATE].fd[OUT], 0);
	lseek(gslots[STATE].fd[OUT], 0, SEEK_SET);
	dprintf(gslots[STATE].fd[OUT], "%s\n", buf);
	datasave();
	logmsg(": State > %s\n", buf);
}

static void
sendfriendreq(void *data)
{
	ssize_t n;
	int     r;
	char    buf[PIPE_BUF], *p;
	char   *msg = "ratox is awesome!";
	uint8_t id[TOX_FRIEND_ADDRESS_SIZE];

	n = fiforead(gslots[REQUEST].dirfd, &gslots[REQUEST].fd[IN], gfiles[IN],
		     buf, sizeof(buf) - 1);
	if (n <= 0)
		return;
	buf[n] = '\0';

	/* locate start of msg */
	for (p = buf; *p && !isspace(*p); p++)
		;
	if (*p == '\0')
		goto out; /* no msg */
	*p++ = '\0';
	if (*p == '\0') {
		goto out; /* no msg */
	} else {
		msg = p;
		if (msg[strlen(msg) - 1] == '\n')
			msg[strlen(msg) - 1] = '\0';
	}
out:
	if (strlen(buf) != sizeof(id) * 2) {
		ftruncate(gslots[REQUEST].fd[ERR], 0);
		lseek(gslots[REQUEST].fd[ERR], 0, SEEK_SET);
		dprintf(gslots[REQUEST].fd[ERR], "Invalid friend ID\n");
		return;
	}
	str2id(buf, id);

	r = tox_friend_add(tox, id, (uint8_t *)msg, strlen(msg), NULL);
	ftruncate(gslots[REQUEST].fd[ERR], 0);
	lseek(gslots[REQUEST].fd[ERR], 0, SEEK_SET);

	if (r < 0) {
		dprintf(gslots[REQUEST].fd[ERR], "%s\n", reqerr[-r]);
		return;
	}
	friendcreate(r);
	datasave();
	logmsg("Request > Sent\n");
}

static void
setnospam(void *data)
{
	ssize_t  n, i;
	uint32_t nsval;
	uint8_t  nospam[2 * sizeof(uint32_t) + 1];
	uint8_t  address[TOX_FRIEND_ADDRESS_SIZE];

	n = fiforead(gslots[NOSPAM].dirfd, &gslots[NOSPAM].fd[IN], gfiles[IN],
		     nospam, sizeof(nospam) - 1);
	if (n <= 0)
		return;
	if (nospam[n - 1] == '\n')
		n--;
	nospam[n] = '\0';

	for (i = 0; i < n; i++) {
		if (nospam[i] < '0' || (nospam[i] > '9' && nospam[i] < 'A') || nospam[i] > 'F') {
			dprintf(gslots[NOSPAM].fd[ERR], "Input contains invalid characters ![0-9, A-F]\n");
			goto end;
		}
	}

	nsval = strtoul((char *)nospam, NULL, 16);
	tox_self_set_nospam(tox, nsval);
	datasave();
	logmsg("Nospam > %08X\n", nsval);
	ftruncate(gslots[NOSPAM].fd[OUT], 0);
	lseek(gslots[NOSPAM].fd[OUT], 0, SEEK_SET);
	dprintf(gslots[NOSPAM].fd[OUT], "%08X\n", nsval);

	tox_self_get_address(tox, address);
	ftruncate(idfd, 0);
	lseek(idfd, 0, SEEK_SET);
	for (i = 0; i < TOX_FRIEND_ADDRESS_SIZE; i++)
		dprintf(idfd, "%02X", address[i]);
	dprintf(idfd, "\n");
end:
	fiforeset(gslots[NOSPAM].dirfd, &gslots[NOSPAM].fd[IN], gfiles[IN]);
}

static void
loop(void)
{
	struct file reqfifo;
	struct friend *f, *ftmp;
	struct request *req, *rtmp;
	struct timespec curtime, diff;
	struct timeval tv;
	fd_set rfds;
	time_t t0, t1;
	int    connected = 0, i, n, r, fd, fdmax;
	char   tstamp[64], c;

	t0 = time(NULL);
	logmsg("DHT > Connecting\n");
	toxconnect();
	int ctest = 0;
	while (running) {
		/* Handle connection states */
		if (tox_self_get_connection_status(tox)) {
			if (!connected) {
				logmsg("DHT > Connected\n");
				TAILQ_FOREACH(f, &friendhead, entry) {
					canceltxtransfer(f);
					cancelrxtransfer(f);
				}
				connected = 1;
			}
			ctest++;
			if(ctest>3600){
				connected = 0;
				ctest = 0;
			}
		} else {
			if (connected) {
				logmsg("DHT > Disconnected\n");
				connected = 0;
			}
			t1 = time(NULL);
			if (t1 > t0 + CONNECTDELAY) {
				t0 = time(NULL);
				logmsg("DHT > Connecting\n");
				toxconnect();
			}
		}
		tox_iterate(tox);

		/* Prepare select-fd-set */
		FD_ZERO(&rfds);
		fdmax = -1;

		for (i = 0; i < LEN(gslots); i++)
			FD_APPEND(gslots[i].fd[IN]);

		TAILQ_FOREACH(req, &reqhead, entry)
			FD_APPEND(req->fd);

		TAILQ_FOREACH(f, &friendhead, entry) {
			/* File transfer cooldown */
			if (f->tx.cooldown) {
				clock_gettime(CLOCK_MONOTONIC, &curtime);
				diff = timediff(f->tx.lastblock, curtime);

				if (diff.tv_sec > 0 || diff.tv_nsec > interval(tox) * 3 * 1E6) {
					f->tx.lastblock.tv_sec = 0;
					f->tx.lastblock.tv_nsec = 0;
					f->tx.cooldown = 0;
				}
			}

			/* Only monitor friends that are online */
			if (tox_friend_get_connection_status(tox, f->num, NULL) == 1) {
				FD_APPEND(f->fd[FTEXT_IN]);

				if (f->tx.state == TRANSFER_NONE ||
				    (f->tx.state == TRANSFER_INPROGRESS && !f->tx.cooldown))
					FD_APPEND(f->fd[FFILE_IN]);
			}
			FD_APPEND(f->fd[FREMOVE]);
		}

		tv.tv_sec = 0;
		tv.tv_usec = interval(tox) * 1000;
		n = select(fdmax + 1, &rfds, NULL, NULL, &tv);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			eprintf("select:");
		}

		/* Check for broken transfers (friend went offline, file_out was closed) */
		TAILQ_FOREACH(f, &friendhead, entry) {
			if (tox_friend_get_connection_status(tox, f->num, NULL) == 0) {
				canceltxtransfer(f);
				cancelrxtransfer(f);
			}
			if (f->rxstate != TRANSFER_INPROGRESS)
				continue;
			fd = fifoopen(f->dirfd, ffiles[FFILE_OUT]);
			if (fd < 0) {
				cancelrxtransfer(f);
			} else {
				close(fd);
			}
		}

		/* If we hit the receiver too hard, we will run out of
		 * local buffer slots.	In that case tox_file_send()
		 * will return -1 and we will have to queue the buffer to
		 * send it later.  If this is the last buffer read from
		 * the FIFO, then select() won't make the fd readable again
		 * so we have to check if there's anything pending to be
		 * sent.
		 */
		TAILQ_FOREACH(f, &friendhead, entry) {
			if (tox_friend_get_connection_status(tox, f->num, NULL) == 0)
				continue;
			if (f->tx.state != TRANSFER_INPROGRESS)
				continue;
			if (f->tx.pendingbuf)
				sendfriendfile(f);
			if (f->tx.state == TRANSFER_NONE)
				FD_CLR(f->fd[FFILE_IN], &rfds);
		}

		/* Accept pending transfers if any */
		TAILQ_FOREACH(f, &friendhead, entry) {
			if (tox_friend_get_connection_status(tox, f->num, NULL) == 0)
				continue;
			if (f->rxstate == TRANSFER_NONE)
				continue;
			if (f->fd[FFILE_OUT] >= 0)
				continue;
			r = fifoopen(f->dirfd, ffiles[FFILE_OUT]);
			if (r < 0)
				continue;
			f->fd[FFILE_OUT] = r;
			if (tox_file_control(tox, f->num, 0, TOX_FILE_CONTROL_RESUME, NULL) < 0) {
				weprintf("Failed to accept transfer from receiver\n");
				cancelrxtransfer(f);
			} else {
				logmsg(": %s : Rx > Accepted\n", f->name);
				f->rxstate = TRANSFER_INPROGRESS;
			}
		}

		if (n == 0)
			continue;

		for (i = 0; i < LEN(gslots); i++) {
			if (FD_ISSET(gslots[i].fd[IN], &rfds) == 0)
				continue;
			(*gslots[i].cb)(NULL);
		}

		for (req = TAILQ_FIRST(&reqhead); req; req = rtmp) {
			rtmp = TAILQ_NEXT(req, entry);
			if (FD_ISSET(req->fd, &rfds) == 0)
				continue;
			reqfifo.name = req->idstr;
			reqfifo.flags = O_RDONLY | O_NONBLOCK;
			if (fiforead(gslots[REQUEST].fd[OUT], &req->fd, reqfifo,
				     &c, 1) != 1)
				continue;
			if (c != '0' && c != '1')
				continue;
			r = tox_friend_add_norequest(tox, req->id, NULL);
			if (r < 0) {
				weprintf("Failed to add friend %s\n", req->idstr);
				fiforeset(gslots[REQUEST].fd[OUT], &req->fd, reqfifo);
				continue;
			}
			if (c == '1') {
				friendcreate(r);
				logmsg("Request : %s > Accepted\n", req->idstr);
				datasave();
			} else {
				tox_friend_delete(tox, r, NULL);
				logmsg("Request : %s > Rejected\n", req->idstr);
			}
			unlinkat(gslots[REQUEST].fd[OUT], req->idstr, 0);
			close(req->fd);
			TAILQ_REMOVE(&reqhead, req, entry);
			free(req->msg);
			free(req);
		}

		for (f = TAILQ_FIRST(&friendhead); f; f = ftmp) {
			ftmp = TAILQ_NEXT(f, entry);
			if (FD_ISSET(f->fd[FTEXT_IN], &rfds))
				sendfriendtext(f);
			if (FD_ISSET(f->fd[FFILE_IN], &rfds)) {
				switch (f->tx.state) {
				case TRANSFER_NONE:
					/* Prepare a new transfer */
					snprintf(tstamp, sizeof(tstamp), "%lu", (unsigned long)time(NULL));
					if (tox_file_send(tox, 
							f->num, 
							0,
							UINT64_MAX,
							&f->tx.fnum, 
							f->tx.buf, 				
							f->tx.n, 
							NULL) < 0){
						//(tox_new_file_sender(tox, f->num,
						//	0, (uint8_t *)tstamp, strlen(tstamp)) < 0) {
						weprintf("Failed to initiate new transfer\n");
						fiforeset(f->dirfd, &f->fd[FFILE_IN], ffiles[FFILE_IN]);
					} else {
						f->tx.state = TRANSFER_INITIATED;
						logmsg(": %s : Tx > Initiated\n", f->name);
					}
					break;
				case TRANSFER_INPROGRESS:
					sendfriendfile(f);
					break;
				}
			}
			if (FD_ISSET(f->fd[FREMOVE], &rfds))
				removefriend(f);
		}
	}
}

static void
initshutdown(int sig)
{
	running = 0;
}

static void
shutdown(void)
{
	struct friend *f, *ftmp;
	struct request *r, *rtmp;
	int    i, m;

	logmsg("Shutdown\n");

	datasave();

	/* Friends */
	for (f = TAILQ_FIRST(&friendhead); f; f = ftmp) {
		ftmp = TAILQ_NEXT(f, entry);
		frienddestroy(f);
	}

	/* Requests */
	for (r = TAILQ_FIRST(&reqhead); r; r = rtmp) {
		rtmp = TAILQ_NEXT(r, entry);

		if (gslots[REQUEST].fd[OUT] != -1) {
			unlinkat(gslots[REQUEST].fd[OUT], r->idstr, 0);
			if (r->fd != -1)
				close(r->fd);
		}
		TAILQ_REMOVE(&reqhead, r, entry);
		free(r->msg);
		free(r);
	}

	/* Global files and slots */
	for (i = 0; i < LEN(gslots); i++) {
		for (m = 0; m < LEN(gfiles); m++) {
			if (gslots[i].dirfd != -1) {
				unlinkat(gslots[i].dirfd, gfiles[m].name,
					 (gslots[i].outisfolder && m == OUT)
					 ? AT_REMOVEDIR : 0);
				if (gslots[i].fd[m] != -1)
					close(gslots[i].fd[m]);
			}
		}
		rmdir(gslots[i].name);
	}
	unlink("id");
	if (idfd != -1)
		close(idfd);

//	toxav_kill(toxav);
	tox_kill(tox);
}

static void
usage(void)
{
	eprintf("usage: %s [-4|-6] [-E|-e] [-T|-t] [-P|-p] [savefile]\n", argv0);
}

int
main(int argc, char *argv[])
{
	ARGBEGIN {
	case '4':
		ipv6 = 0;
		break;
	case '6':
		ipv6 = 1;
		break;
	case 'E':
		encryptsavefile = 1;
		break;
	case 'e':
		encryptsavefile = 0;
		break;
	case 'T':
		udp = 0;
		break;
	case 't':
		udp = 1;
		break;
	case 'P':
		proxy = 1;
		break;
	case 'p':
		proxy = 0;
		break;
	default:
		usage();
	} ARGEND;

	if (argc > 1)
		usage();
	if (argc == 1)
		savefile = *argv;

	setbuf(stdout, NULL);

	signal(SIGHUP, initshutdown);
	signal(SIGINT, initshutdown);
	signal(SIGQUIT, initshutdown);
	signal(SIGTERM, initshutdown);
	signal(SIGPIPE, SIG_IGN);

	printrat();
	toxinit();
	localinit();
	friendload();
	loop();
	shutdown();
	return 0;
}
