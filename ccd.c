
/* c */
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>

/* lib */
#include <evhttp.h>

/* unix */
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>

#include "cc.h"

#include <systemd/sd-daemon.h>


/* 2.3 for old CC, -2.2 for new from car boot */
/* #define CURRENTCOST_TEMP_OFFSET -2.2 */
#define CURRENTCOST_TEMP_OFFSET 0.0
#define CHANNELS 11
#define TIMEOUT_PRESENT 60

static int g_port = -1;
static double g_temperature = INFINITY;
static uint g_watts[CHANNELS];
static time_t g_watt_lastseen[CHANNELS];
static char *g_statsfile = "/var/log/currentcost/currentcost.csv";
static char *g_device = CC_DEVICE;

/*
 * '{ "temperature": 10.2, "watts": [ 512, null, 102 ] }'
 */
static uint count_appliances(time_t now)
{
	int i;

	for (i=CHANNELS - 1; i>=0 ; i--) {
		if (g_watt_lastseen[i] + TIMEOUT_PRESENT >= now)
			break;
	}

	return i + 1;
}

static void process_watt(struct evhttp_request *req, void *arg)
{
	int i;

	if (!isfinite(g_temperature)) {
		// nothing received yet
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Not ready");
		return;
	}

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
		evhttp_add_header(req->output_headers, "Allow", "GET");
		evhttp_send_error(req, HTTP_BADMETHOD, "Method not allowed");
		return;
	}

	struct evbuffer *buf = evbuffer_new();
	if (buf == NULL) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Out of memory");
		return;
	}

	evhttp_add_header(req->output_headers, "Content-Type", "application/json");
	evhttp_add_header(req->output_headers, "Connection", "close");


	evbuffer_add_printf(buf, "{ \"temperature\": %.1f, \"watts\": [", 
				g_temperature + CURRENTCOST_TEMP_OFFSET);

	time_t now = time(NULL);

	for (i=0; i<count_appliances(now); i++) {
		if (i)
			evbuffer_add_printf(buf, ",");

		if (g_watt_lastseen[i] + TIMEOUT_PRESENT >= now)
			evbuffer_add_printf(buf, " %u", g_watts[i]);
		else
			evbuffer_add_printf(buf, " null");
	}

	evbuffer_add_printf(buf, " ] }\n");
	evhttp_send_reply(req, HTTP_OK, "OK", buf);
	evbuffer_free(buf);
}

#include "ccd.html.c"

static void process_html(struct evhttp_request *req, void *arg)
{
	struct evbuffer *buf = evbuffer_new();
	if (buf == NULL) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Out of memory");
		return;
	}

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
		evhttp_add_header(req->output_headers, "Allow", "GET");
		evhttp_send_error(req, HTTP_BADMETHOD, "Method not allowed");
		return;
	}

	evhttp_add_header(req->output_headers, "Content-Encoding", "gzip");
	evhttp_add_header(req->output_headers, "Connection", "close");

	evbuffer_add(buf, html, sizeof(html) - 1);
	
	evhttp_send_reply(req, HTTP_OK, "OK", buf);

	evbuffer_free(buf);
}

static int create_http(struct event_base *base)
{
	struct evhttp *httpd;
	int i, n;

	httpd = evhttp_new(base);
	if (httpd == NULL)
		return ENOMEM;

	n = sd_listen_fds(0);
	for (i=0; i<n; i++) {
		int rc, flags, fd = SD_LISTEN_FDS_START + i;
		flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0) {
			rc = errno;
			syslog(LOG_WARNING, "warning: fcntl failed on systemd socket: %m");
			evhttp_free(httpd);
			return rc;
		}

		if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
			rc = errno;
			syslog(LOG_WARNING, "warning: fcntl failed on systemd socket: %m");
			evhttp_free(httpd);
			return rc;
		}

		if (evhttp_accept_socket(httpd, fd)) {
			evhttp_free(httpd);
			syslog(LOG_WARNING, "warning: failed to add systemd socket");
			return EINVAL;
		}
	}
	if (g_port > 0 && evhttp_bind_socket(httpd, "::", g_port)) {
		evhttp_free(httpd);
		return errno;
	}

	evhttp_set_cb(httpd, "/currentcost", process_watt, NULL);
	evhttp_set_cb(httpd, "/", process_html, NULL);
	
	return 0;
}

static void logit()
{
	// log it
	time_t now = time(NULL);
	char buf[100], timestr[80];
	struct tm tm;
	size_t size;
	int i;

	localtime_r(&now, &tm);
	strftime(timestr, sizeof(timestr), "%d %b %Y %T %z", &tm);

	size = snprintf(buf, sizeof(buf), "%s,%.1f", timestr, g_temperature);

	for (i=0; i<count_appliances(now); i++) {
		buf[size++] = ',';

		if (g_watt_lastseen[i] + TIMEOUT_PRESENT >= now)
			size += snprintf(buf + size, sizeof(buf) - size, "%u", 
								g_watts[i]);
	}

	buf[size++] = '\n';

	int fd = TEMP_FAILURE_RETRY(open(g_statsfile, O_APPEND|O_CREAT|O_WRONLY|O_CLOEXEC, 0644));
	if (fd == -1) {
		syslog(LOG_WARNING, "failed to open %s: %m", g_statsfile);
		return;
	}
	
	size_t off = 0;
	while (size) {
		ssize_t ret = TEMP_FAILURE_RETRY(write(fd, buf + off, size));
		if (ret == -1) {
			syslog(LOG_WARNING, "failed to write %s: %m", g_statsfile);
			close(fd);
			return;
		}
		off += ret;
		size -= ret;
	}

	close(fd);
}

static void data_cb(double temperature, uint sensor, uint watts)
{
	bool changed = false;

	if (sensor >= CHANNELS)  {
		syslog(LOG_ERR, "sensor #%d out of range", sensor);
		return;
	}

	if (temperature && g_temperature != temperature) {
		g_temperature = temperature;
		changed = true;
	}

	g_watt_lastseen[sensor] = time(NULL);
	if (g_watts[sensor] != watts) {
		g_watts[sensor] = watts;	
		changed = true;
	}

	if (changed)
		logit();
}

static void cc_data(evutil_socket_t fd, short event, void *arg)
{
	if (event & EV_READ) {
		int rc = currentcost_read(arg);
		if (rc) {
			syslog(LOG_ERR, "failed to read from %s: %s\n", 
							g_device, strerror(rc));
			exit(EXIT_FAILURE);
		}
	} else if (event & EV_TIMEOUT) {
		syslog(LOG_ERR, "timeout reading from %s after %d seconds\n", 
							g_device, CC_TIMEOUT);
	}
}

static int open_cc(struct event_base *base)
{
	struct currentcost *cc = malloc(sizeof(*cc));
	struct timeval timeout = { CC_TIMEOUT, 0 };
	int rc;

	rc = currentcost_open(cc, g_device);

	if (rc) {
		fprintf(stderr, "failed to open %s: %s\n", g_device,
								strerror(rc));
		exit(EXIT_FAILURE);
	}

	cc->cb = data_cb;
	
	struct event *event = event_new(base, cc->fd, 
				EV_TIMEOUT | EV_READ | EV_PERSIST, cc_data, cc);
	event_add(event, &timeout);

	return 0;
}

int main(int argc, char *argv[])
{
	int rc;
	bool daemonize = false;

	while ((rc = getopt(argc, argv, "hdp:s:")) != -1) {
		switch (rc) {
		case 'd':
			daemonize = true;
			break;
		case 'p':
			g_port = atoi(optarg);
			if (g_port <= 0 || g_port >= 65536) {
				fprintf(stderr, 
					"error: %s is not a valid port\n", 
					optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			printf("Usage: %s [-d] [-p port] [-h] [device]\n", 
								argv[0]);
			exit(EXIT_SUCCESS);
		case '?':
			exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		if (argv[optind][0] == '/') {
			g_device = strdup(argv[optind]);
			if (!g_device) {
				fprintf(stderr, "error: failed to malloc\n");
				exit(EXIT_FAILURE);
			}
		}
		else {
			if (asprintf(&g_device, "/dev/%s", argv[optind]) < 0) {
				fprintf(stderr, "error: failed to malloc\n");
				exit(EXIT_FAILURE);
			}
		}

		optind++;
	}

	if (optind < argc) {
		fprintf(stderr, "%s: invalid argument -- '%s'\n", argv[0], 
								argv[optind]);
		exit(EXIT_FAILURE);
	}

	struct event_base *base = event_init();

	rc = open_cc(base);
	if (rc) {
		printf("error: failed to open currentcost device: %s\n", 
							strerror(rc));
		exit(EXIT_FAILURE);
	}

	rc = create_http(base);
	if (rc) {
		printf("error: failed to create http server: %s\n", strerror(rc));
		exit(EXIT_FAILURE);
	}

	if (daemonize && daemon(0, 0)) {
		printf("error: failed to fork: %m\n");
		exit(EXIT_FAILURE);
	}

	openlog("currentcostd", LOG_ODELAY | LOG_PID, LOG_USER);
	event_base_dispatch(base);
	closelog();

	return 0;
}
