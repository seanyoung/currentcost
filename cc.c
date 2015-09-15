
/* */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <expat.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>

#include "cc.h"

struct parser {
	struct currentcost *cc;
	double temperature;
	unsigned sensor;
	unsigned watt;
	bool is_history;
	enum {
		TAG_WATTS,
		TAG_TMPR,
		TAG_SENSOR,
		TAG_NONE
	} last_tag;
};

static void start(void *data, const char *el, const char **attr)
{
	struct parser *parser = data;

	parser->last_tag = TAG_NONE;

	if (strcmp(el, "watts") == 0) {
		parser->last_tag = TAG_WATTS;
	} else if (strcmp(el, "tmpr") == 0) {
		parser->last_tag = TAG_TMPR;
	} else if (strcmp(el, "sensor") == 0) {
		parser->last_tag = TAG_SENSOR;
	} else if (strcmp(el, "hist") == 0) {
		parser->is_history = true;
	}
}

static void char_data(void *data, const char *s, int len)
{
	struct parser *parser = data;

	if (parser->last_tag == TAG_WATTS) {
		parser->watt = atoi(s);
	} else if (parser->last_tag == TAG_TMPR) {
		parser->temperature = atof(s);
	} else if (parser->last_tag == TAG_SENSOR) {
		parser->sensor = atoi(s);
	}
}

static void end(void *data, const char *el)
{
	struct parser *parser = data;

	parser->last_tag = TAG_NONE;

	if (strcmp(el, "msg") == 0 && !parser->is_history) {
		parser->cc->cb(parser->temperature, parser->sensor, parser->watt);
	}
}

static void currentcost_parse(struct currentcost *cc, char *data, size_t size)
{
	struct parser parser;
	memset(&parser, 0, sizeof(parser));
	parser.cc = cc;

	XML_SetUserData(cc->expat, &parser);
	XML_SetElementHandler(cc->expat, start, end);
	XML_SetCharacterDataHandler(cc->expat, char_data);

	if (!XML_Parse(cc->expat, data, size, true)) {
		enum XML_Error rc = XML_GetErrorCode(cc->expat);

		syslog(LOG_WARNING, "XML parse error: %s while parsing '%.*s'",
				XML_ErrorString(rc), (int)size, data);
	}

	XML_ParserReset(cc->expat, "US-ASCII");
}

/* currentcost produces zero bytes in its output */
unsigned strcpy_ignore_zeros(char *dst, const char *src, unsigned len)
{
	unsigned i, newlen = len;

	for (i=0; i<len; i++) {
		if (src[i])
			*dst++ = src[i];
		else
			newlen--;
	}

	return newlen;
}

int currentcost_read(struct currentcost *cc)
{
	char buf[sizeof(cc->data)];

	while (true) {
		ssize_t rc;

		size_t len = cc->size;

		rc = read(cc->fd, buf, sizeof(cc->data) - cc->size);

		if (rc == 0)
			return EIO;

		if (rc < 0) {
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				return 0;
			default:
				return errno;
			}
		}

		rc = strcpy_ignore_zeros(cc->data + cc->size, buf, rc);

		cc->size += rc;

		while (len < cc->size) {
			if (cc->data[len++] == '\n') {
				currentcost_parse(cc, cc->data, len);
				if (len < cc->size) {
					memmove(cc->data, cc->data + len,
							cc->size - len);
					cc->size -= len;
					len = 0;
				} else {
					cc->size = 0;
				}
			}
		}

		if (cc->size == sizeof(cc->data)) {
			syslog(LOG_WARNING, "missing newline, skipping data");
			cc->size = 0;
		}
	}
}

int currentcost_open(struct currentcost *cc, const char *path)
{
	int rc, fd;

	fd = TEMP_FAILURE_RETRY(open(path, O_RDWR | O_NOCTTY | O_NDELAY | O_CLOEXEC));
	if (fd == -1)
		return errno;

	struct termios termios;

	rc = TEMP_FAILURE_RETRY(tcgetattr(fd, &termios));
	if (rc == -1) {
		rc = errno;
		close(fd);
		return rc;
	}

	// 57600 8N1 no flow control (hardware or software)
	cfmakeraw(&termios);
	cfsetspeed(&termios, B57600);
	termios.c_iflag &= ~(INPCK | IXON | IXOFF);
	termios.c_cflag &= ~(CSTOPB | CRTSCTS);

	rc = TEMP_FAILURE_RETRY(tcsetattr(fd, TCSAFLUSH, &termios));
	if (rc == -1) {
		rc = errno;
		close(fd);
		return rc;
	}

	cc->expat = XML_ParserCreate("US-ASCII");
	if (!cc->expat) {
		close(fd);
		return ENOMEM;
	}

	cc->fd = fd;
	cc->size = 0;

	return 0;
}

void currentcost_close(struct currentcost *cc)
{
	close(cc->fd);
	XML_ParserFree(cc->expat);
}
