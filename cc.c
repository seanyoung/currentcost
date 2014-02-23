
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

	if (strcmp(el, "msg") == 0) {
		parser->cc->cb(parser->temperature, parser->sensor, parser->watt);
	}
}

static void currentcost_parse(struct currentcost *cc, char *data, size_t size)
{
	struct parser parser;
	memset(&parser, 0, sizeof(parser));
	parser.cc = cc;
	
	XML_Parser p = XML_ParserCreate("US-ASCII");
	XML_SetUserData(p, &parser);
	XML_SetElementHandler(p, start, end);
	XML_SetCharacterDataHandler(p, char_data);

	if (!XML_Parse(p, data, size, true)) {
		enum XML_Error rc = XML_GetErrorCode(p);

		syslog(LOG_WARNING, "XML parse error: %s while parsing '%.*s'", 
				XML_ErrorString(rc), size, data);
	}

	XML_ParserFree(p);
}

int currentcost_read(struct currentcost *cc, int fd)
{
	while (true) {
		ssize_t rc;

		size_t len = cc->size;

		rc = read(fd, cc->data + cc->size, sizeof(cc->data) - cc->size);
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

int currentcost_open(int *fd_ret, const char *path)
{
	int rc, fd;

	fd = TEMP_FAILURE_RETRY(open(path, O_RDWR | O_NOCTTY | O_NDELAY | O_CLOEXEC));
	if (fd == -1)
		return errno;

	struct termios termios;

	rc = TEMP_FAILURE_RETRY(tcgetattr(fd, &termios));
	if (rc == -1) {
		rc = errno;
		TEMP_FAILURE_RETRY(close(fd));
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
		TEMP_FAILURE_RETRY(close(fd));
		return rc;
	}

	*fd_ret = fd;

	return 0;
}

#ifdef TEST
static void cb(double temperature, unsigned channels, unsigned *watts)
{
	printf("temp: %.2f", temperature);

	for (unsigned i=0; i<channels; i++) printf(",%d", watts[i]);

	putchar('\n');
}

int main(int argc, char *argv[])
{
	struct currentcost cc;

	currentcost_init(&cc);

	int fd = open("cc.txt", O_RDONLY| O_NDELAY | O_CLOEXEC);
	cc.cb = cb;

	currentcost_read(&cc, fd);

	return 0;
}
#endif
