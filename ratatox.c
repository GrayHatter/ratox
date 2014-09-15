/* See LICENSE file for copyright and license details. */
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tox/tox.h>

#include "queue.h"

#define LEN(x) (sizeof (x) / sizeof *(x))

struct bootstrapnode {
	char *addr;
	uint16_t port;
	uint8_t key[TOX_CLIENT_ID_SIZE];
};

#include "config.h"

enum {
	TEXT_IN_FIFO,
	NR_FIFOS
};

static struct fifo {
	const char *name;
	int flags;
	mode_t mode;
} fifos[] = {
	{ .name = "text_in", .flags = O_RDONLY | O_NONBLOCK, .mode = 0644 },
};

struct friend {
	/* null terminated name */
	uint8_t namestr[TOX_MAX_NAME_LENGTH + 1];
	int fid;
	uint8_t id[TOX_CLIENT_ID_SIZE];
	/* null terminated id */
	uint8_t idstr[2 * TOX_CLIENT_ID_SIZE + 1];
	int fd[NR_FIFOS];
	TAILQ_ENTRY(friend) entry;
};

struct request {
	uint8_t id[TOX_CLIENT_ID_SIZE];
	/* null terminated id */
	uint8_t idstr[2 * TOX_CLIENT_ID_SIZE + 1];
	/* null terminated friend request message */
	uint8_t *msgstr;
	TAILQ_ENTRY(request) entry;
};

static TAILQ_HEAD(friendhead, friend) friendhead = TAILQ_HEAD_INITIALIZER(friendhead);
static TAILQ_HEAD(reqhead, request) reqhead = TAILQ_HEAD_INITIALIZER(reqhead);

static Tox *tox;

static void cb_conn_status(Tox *, int32_t, uint8_t, void *);
static void cb_friend_message(Tox *, int32_t, const uint8_t *, uint16_t, void *);
static void cb_friend_request(Tox *, const uint8_t *, const uint8_t *, uint16_t, void *);
static void cb_name_change(Tox *, int32_t, const uint8_t *, uint16_t, void *);
static void cb_status_message(Tox *, int32_t, const uint8_t *, uint16_t, void *);
static void send_friend_text(struct friend *);
static void dataload(void);
static void datasave(void);
static void toxrestore(void);
static int toxinit(void);
static int toxconnect(void);
static void id2str(uint8_t *, uint8_t *);
static void str2id(uint8_t *, uint8_t *);
static void friendcreate(int32_t);
static void friendload(void);
static int cmdrun(void);
static int doaccept(char *, size_t);
static int dofriend(char *, size_t);
static int dohelp(char *, size_t);
static void loop(void);

static char qsep[] = " \t\r\n";

/* tokenization routines taken from Plan9 */
static char *
qtoken(char *s, char *sep)
{
	int quoting;
	char *t;

	quoting = 0;
	t = s;	/* s is output string, t is input string */
	while(*t!='\0' && (quoting || strchr(sep, *t)==NULL)) {
		if(*t != '\'') {
			*s++ = *t++;
			continue;
		}
		/* *t is a quote */
		if(!quoting) {
			quoting = 1;
			t++;
			continue;
		}
		/* quoting and we're on a quote */
		if(t[1] != '\'') {
			/* end of quoted section; absorb closing quote */
			t++;
			quoting = 0;
			continue;
		}
		/* doubled quote; fold one quote into two */
		t++;
		*s++ = *t++;
	}
	if(*s != '\0') {
		*s = '\0';
		if(t == s)
			t++;
	}
	return t;
}

static char *
etoken(char *t, char *sep)
{
	int quoting;

	/* move to end of next token */
	quoting = 0;
	while(*t!='\0' && (quoting || strchr(sep, *t)==NULL)) {
		if(*t != '\'') {
			t++;
			continue;
		}
		/* *t is a quote */
		if(!quoting) {
			quoting = 1;
			t++;
			continue;
		}
		/* quoting and we're on a quote */
		if(t[1] != '\'') {
			/* end of quoted section; absorb closing quote */
			t++;
			quoting = 0;
			continue;
		}
		/* doubled quote; fold one quote into two */
		t += 2;
	}
	return t;
}

static int
gettokens(char *s, char **args, int maxargs, char *sep)
{
	int nargs;

	for(nargs=0; nargs<maxargs; nargs++) {
		while(*s!='\0' && strchr(sep, *s)!=NULL)
			*s++ = '\0';
		if(*s == '\0')
			break;
		args[nargs] = s;
		s = etoken(s, sep);
	}

	return nargs;
}

static int
tokenize(char *s, char **args, int maxargs)
{
	int nargs;

	for(nargs=0; nargs<maxargs; nargs++) {
		while(*s!='\0' && strchr(qsep, *s)!=NULL)
			s++;
		if(*s == '\0')
			break;
		args[nargs] = s;
		s = qtoken(s, qsep);
	}

	return nargs;
}

static void
cb_conn_status(Tox *tox, int32_t fid, uint8_t status, void *udata)
{
	struct friend *f;
	uint8_t name[TOX_MAX_NAME_LENGTH + 1];
	uint8_t *nick;
	int n;

	n = tox_get_name(tox, fid, name);
	if (n < 0) {
		fprintf(stderr, "tox_get_name() on fid %d failed\n", fid);
		exit(1);
	}
	name[n] = '\0';

	if (n == 0) {
		if (status == 0)
			printf("Anonymous went offline\n");
		else
			printf("Anonymous came online\n");
	} else {
		if (status == 0)
			printf("%s went offline\n", name);
		else
			printf("%s came online\n", name);
	}

	TAILQ_FOREACH(f, &friendhead, entry)
		if (f->fid == fid)
			return;

	friendcreate(fid);
}

static void
cb_friend_message(Tox *tox, int32_t fid, const uint8_t *data, uint16_t len, void *udata)
{
	FILE *fp;
	struct friend *f;
	uint8_t msg[len + 1];
	char path[PATH_MAX];

	memcpy(msg, data, len);
	msg[len] = '\0';

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->fid == fid) {
			snprintf(path, sizeof(path), "%s/text_out", f->idstr);
			fp = fopen(path, "a");
			if (!fp) {
				perror("fopen");
				exit(1);
			}
			fputs(msg, fp);
			fputc('\n', fp);
			fclose(fp);
			break;
		}
	}
}

static void
cb_friend_request(Tox *tox, const uint8_t *id, const uint8_t *data, uint16_t len, void *udata)
{
	struct request *req;

	req = calloc(1, sizeof(*req));
	if (!req) {
		perror("calloc");
		exit(1);
	}
	memcpy(req->id, id, TOX_CLIENT_ID_SIZE);
	id2str(req->id, req->idstr);

	if (len > 0) {
		req->msgstr = malloc(len + 1);
		if (!req->msgstr) {
			perror("malloc");
			exit(1);
		}
		memcpy(req->msgstr, data, len);
		req->msgstr[len] = '\0';
	}

	TAILQ_INSERT_TAIL(&reqhead, req, entry);

	printf("Pending request from %s with message: %s\n",
	       req->idstr, req->msgstr);
}

static void
cb_name_change(Tox *m, int32_t fid, const uint8_t *data, uint16_t len, void *user)
{
	FILE *fp;
	char path[PATH_MAX];
	struct friend *f;
	uint8_t name[len + 1];

	memcpy(name, data, len);
	name[len] = '\0';

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->fid == fid) {
			snprintf(path, sizeof(path), "%s/name", f->idstr);
			fp = fopen(path, "w");
			if (!fp) {
				perror("fopen");
				exit(1);
			}
			fputs(name, fp);
			fputc('\n', fp);
			fclose(fp);
			if (memcmp(f->namestr, name, len + 1) == 0)
				break;
			if (f->namestr[0] == '\0') {
				printf("%s -> %s\n", "Anonymous", name);
			} else {
				printf("%s -> %s\n", f->namestr, name);
			}
			memcpy(f->namestr, name, len + 1);
			break;
		}
	}
	datasave();
}

static void
cb_status_message(Tox *m, int32_t fid, const uint8_t *data, uint16_t len, void *udata)
{
	FILE *fp;
	char path[PATH_MAX];
	struct friend *f;
	uint8_t status[len + 1];

	memcpy(status, data, len);
	status[len] = '\0';

	TAILQ_FOREACH(f, &friendhead, entry) {
		if (f->fid == fid) {
			snprintf(path, sizeof(path), "%s/status", f->idstr);
			fp = fopen(path, "w");
			if (!fp) {
				perror("fopen");
				exit(1);
			}
			fputs(status, fp);
			fputc('\n', fp);
			fclose(fp);
			printf("%s current status to %s\n", f->namestr, status);
			break;
		}
	}
	datasave();
}

static void
send_friend_text(struct friend *f)
{
	uint8_t buf[TOX_MAX_MESSAGE_LENGTH];
	ssize_t n;

again:
	n = read(f->fd[TEXT_IN_FIFO], buf, sizeof(buf));
	if (n < 0) {
		if (errno == EINTR)
			goto again;
		perror("read");
		exit(1);
	}
	tox_send_message(tox, f->fid, buf, n);
}

static void
dataload(void)
{
	FILE *fp;
	size_t sz;
	uint8_t *data;

	fp = fopen("ratatox.data", "r");
	if (!fp)
		return;

	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	rewind(fp);

	data = malloc(sz);
	if (!data) {
		perror("malloc");
		exit(1);
	}

	if (fread(data, 1, sz, fp) != sz) {
		fprintf(stderr, "failed to read ratatox.data\n");
		exit(1);
	}
	tox_load(tox, data, sz);

	free(data);
	fclose(fp);
}

static void
datasave(void)
{
	FILE *fp;
	size_t sz;
	uint8_t *data;

	fp = fopen("ratatox.data", "w");
	if (!fp) {
		fprintf(stderr, "can't open ratatox.data for writing\n");
		exit(1);
	}

	sz = tox_size(tox);
	data = malloc(sz);
	if (!data) {
		perror("malloc");
		exit(1);
	}

	tox_save(tox, data);
	if (fwrite(data, 1, sz, fp) != sz) {
		fprintf(stderr, "failed to write ratatox.data\n");
		exit(1);
	}

	free(data);
	fclose(fp);
}

static void
toxrestore(void)
{
	dataload();
	datasave();
}

static int
toxinit(void)
{
	uint8_t address[TOX_FRIEND_ADDRESS_SIZE];
	int i;

	tox = tox_new(0);
	toxrestore();
	tox_callback_connection_status(tox, cb_conn_status, NULL);
	tox_callback_friend_message(tox, cb_friend_message, NULL);
	tox_callback_friend_request(tox, cb_friend_request, NULL);
	tox_callback_name_change(tox, cb_name_change, NULL);
	tox_callback_status_message(tox, cb_status_message, NULL);
	tox_set_name(tox, "TLH", strlen("TLH"));
	tox_set_user_status(tox, TOX_USERSTATUS_NONE);

	tox_get_address(tox, address);
	printf("ID: ");
	for (i = 0; i < TOX_FRIEND_ADDRESS_SIZE; i++)
		printf("%02x", address[i]);
	printf("\n");

	return 0;
}

static int
toxconnect(void)
{
	struct bootstrapnode *bn;
	size_t i;

	for (i = 0; i < LEN(bootstrapnodes); i++) {
		bn = &bootstrapnodes[i];
		tox_bootstrap_from_address(tox, bn->addr, bn->port, bn->key);
	}
	return 0;
}

static void
id2str(uint8_t *id, uint8_t *idstr)
{
	uint8_t hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < TOX_CLIENT_ID_SIZE; i++) {
		*idstr++ = hex[(id[i] >> 4) & 0xf];
		*idstr++ = hex[id[i] & 0xf];
	}
	*idstr = '\0';
}

static void
str2id(uint8_t *idstr, uint8_t *id)
{
	size_t i, len = strlen(idstr) / 2;
	char *p = idstr;

	for (i = 0; i < len; ++i, p += 2)
		sscanf(p, "%2hhx", &id[i]);
}

static void
friendcreate(int32_t fid)
{
	struct friend *f;
	char path[PATH_MAX];
	int i;
	int r;

	f = calloc(1, sizeof(*f));
	if (!f) {
		perror("calloc");
		exit(1);
	}

	r = tox_get_name(tox, fid, f->namestr);
	if (r < 0) {
		fprintf(stderr, "tox_get_name() on fid %d failed\n", fid);
		exit(1);
	}
	f->namestr[r] = '\0';

	f->fid = fid;
	tox_get_client_id(tox, f->fid, f->id);
	id2str(f->id, f->idstr);

	r = mkdir(f->idstr, 0755);
	if (r < 0 && errno != EEXIST) {
		perror("mkdir");
		exit(1);
	}

	for (i = 0; i < LEN(fifos); i++) {
		snprintf(path, sizeof(path), "%s/%s", f->idstr,
			 fifos[i].name);
		r = mkfifo(path, fifos[i].mode);
		if (r < 0 && errno != EEXIST) {
			perror("mkfifo");
			exit(1);
		}
		r = open(path, fifos[i].flags, 0);
		if (r < 0) {
			perror("open");
			exit(1);
		}
		f->fd[i] = r;
	}
	TAILQ_INSERT_TAIL(&friendhead, f, entry);
}

static void
friendload(void)
{
	int32_t *fids;
	uint32_t sz;
	uint32_t i, j;
	int n;
	char name[TOX_MAX_NAME_LENGTH + 1];

	sz = tox_count_friendlist(tox);
	fids = malloc(sz);
	if (!fids) {
		perror("malloc");
		exit(1);
	}

	tox_get_friendlist(tox, fids, sz);

	for (i = 0; i < sz; i++)
		friendcreate(fids[i]);
}

struct cmd {
	const char *cmd;
	int (*cb)(char *, size_t);
	const char *helpstr;
} cmds[] = {
	{ .cmd = "a", .cb = doaccept, .helpstr = "usage: a [ID]\tAccept or list pending requests\n" },
	{ .cmd = "f", .cb = dofriend, .helpstr = "usage: f ID\tSend friend request to ID\n" },
	{ .cmd = "h", .cb = dohelp,   .helpstr = NULL },
};

static int
doaccept(char *cmd, size_t sz)
{
	struct request *req, *tmp;
	char *args[2];
	int n;
	int found = 0;

	n = tokenize(cmd, args, 2);

	if (n == 1) {
		TAILQ_FOREACH(req, &reqhead, entry) {
			printf("Pending request from %s with message: %s\n",
			       req->idstr, req->msgstr);
			found = 1;
		}
		if (found == 0)
			printf("No pending requests\n");
	} else {
		for (req = TAILQ_FIRST(&reqhead); req; req = tmp) {
			tmp = TAILQ_NEXT(req, entry);
			if (strcmp(req->idstr, args[1]) == 0) {
				tox_add_friend_norequest(tox, req->id);
				printf("Accepted friend request for %s\n", req->idstr);
				TAILQ_REMOVE(&reqhead, req, entry);
				free(req->msgstr);
				free(req);
				break;
			}
		}
	}

	return 0;
}

static int
dofriend(char *cmd, size_t sz)
{
	char *args[2];
	uint8_t id[TOX_FRIEND_ADDRESS_SIZE];
	uint8_t idstr[2 * TOX_FRIEND_ADDRESS_SIZE + 1];
	char *msgstr = "ratatox is awesome!";
	int r;

	r = tokenize(cmd, args, 2);
	if (r != 2) {
		printf("Command error, type h for help\n");
		return -1;
	}
	str2id(args[1], id);

	r = tox_add_friend(tox, id, msgstr, strlen(msgstr));
	switch (r) {
	case TOX_FAERR_TOOLONG:
		printf("Message is too long\n");
		break;
	case TOX_FAERR_NOMESSAGE:
		printf("Please add a message to your request\n");
		break;
	case TOX_FAERR_OWNKEY:
		printf("That appears to be your own ID\n");
		break;
	case TOX_FAERR_ALREADYSENT:
		printf("Friend request already sent\n");
		break;
	case TOX_FAERR_UNKNOWN:
		printf("Unknown error while sending your request\n");
		break;
	default:
		printf("Friend request sent\n");
		break;
	}
	return 0;
}

static int
dohelp(char *cmd, size_t sz)
{
	size_t i;

	for (i = 0; i < LEN(cmds); i++)
		if (cmds[i].helpstr)
			printf("%s", cmds[i].helpstr);
	return 0;
}

static int
cmdrun(void)
{
	char cmd[BUFSIZ];
	ssize_t n;
	size_t i;

again:
	n = read(STDIN_FILENO, cmd, sizeof(cmd) - 1);
	if (n < 0) {
		if (errno == EINTR)
			goto again;
		perror("read");
		exit(1);
	}
	if (n == 0)
		return 0;
	cmd[n] = '\0';
	if (cmd[strlen(cmd) - 1] == '\n')
		cmd[strlen(cmd) - 1] = '\0';
	if (cmd[0] == '\0')
		return 0;

	for (i = 0; i < LEN(cmds); i++)
		if (cmd[0] == cmds[i].cmd[0])
			if (cmd[1] == '\0' || isspace((int)cmd[1]))
				return (*cmds[i].cb)(cmd, strlen(cmd));

	fprintf(stderr, "unknown command: %s\n", cmd);
	return -1;
}

static void
loop(void)
{
	struct friend *f;
	time_t t0, t1;
	int connected = 0;
	int i, n;
	int fdmax;
	fd_set rfds;
	struct timeval tv;

	t0 = time(NULL);
	printf("Connecting to DHT...\n");
	toxconnect();
	while (1) {
		if (tox_isconnected(tox) == 1) {
			if (connected == 0) {
				printf("Connected to DHT\n");
				connected = 1;
			}
		} else {
			t1 = time(NULL);
			if (t1 > t0 + 5) {
				t0 = time(NULL);
				printf("Connecting to DHT...\n");
				toxconnect();
			}
		}
		tox_do(tox);

		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		fdmax = STDIN_FILENO;

		TAILQ_FOREACH(f, &friendhead, entry) {
			for (i = 0; i < NR_FIFOS; i++) {
				FD_SET(f->fd[i], &rfds);
				if (f->fd[i] > fdmax)
					fdmax = f->fd[i];
			}
		}

		tv.tv_sec = 0;
		tv.tv_usec = tox_do_interval(tox) * 1000;
		n = select(fdmax + 1, &rfds, NULL, NULL, &tv);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			exit(1);
		}
		if (n == 0)
			continue;

		if (FD_ISSET(STDIN_FILENO, &rfds) != 0)
			cmdrun();

		TAILQ_FOREACH(f, &friendhead, entry) {
			for (i = 0; i < NR_FIFOS; i++) {
				if (FD_ISSET(f->fd[i], &rfds) == 0)
					continue;
				switch (i) {
				case TEXT_IN_FIFO:
					send_friend_text(f);
					break;
				default:
					fputs("Unhandled FIFO read\n", stderr);
				}
			}
		}
	}
}

int
main(void)
{
	toxinit();
	friendload();
	loop();
	return 0;
}
