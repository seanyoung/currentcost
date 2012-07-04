
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

/* lib */
#include <evhttp.h>

/* unix */
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <syslog.h>

#include "cc.h"

#define CURRENTCOST_TEMP_OFFSET 2.3

static int g_port = 80;
static char *g_fqdn = NULL;
static double g_temperature;
static uint g_watts[10], g_wattcount;
static char *g_statsfile = "/var/log/currentcost/currentcost.csv";

/*
 * '{ "temperature": 10.2, "watts": [ 512 ] }'
 */

static void process_watt(struct evhttp_request *req, void *arg)
{
	int i;

	if (g_temperature == 0.0) {
		// nothing received yet
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Not ready");
		return;
	}

	evhttp_add_header(req->output_headers, "Connection", "close");

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
		evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
		return;
	}

	evhttp_add_header(req->output_headers, "Content-Type", "application/json");

	struct evbuffer *buf = evbuffer_new();
	if (buf == NULL) {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Out of memory");
		return;
	}

	evbuffer_add_printf(buf, "{ \"temperature\": %.2f, \"watts\": [", 
				g_temperature + CURRENTCOST_TEMP_OFFSET);

	for (i=0; i<g_wattcount; i++)
		evbuffer_add_printf(buf, "%s %u", (i ? "," : ""), g_watts[i]);

	evbuffer_add_printf(buf, " ] }\n");
	evhttp_send_reply(req, HTTP_OK, "OK", buf);
	evbuffer_free(buf);
}

#include "ccd.html.c"

static void process_html(struct evhttp_request *req, void *arg)
{
	evhttp_add_header(req->output_headers, "Connection", "close");

	if (g_fqdn) {
		struct evbuffer *buf = evbuffer_new();

		evbuffer_add_printf(buf, html, g_fqdn, g_port);
		
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
	} else {
		evhttp_send_error(req, HTTP_SERVUNAVAIL, 
			"Failed to find fully qualified domain name");
	}
}

static int create_http(struct event_base *base)
{
	struct evhttp *httpd;

	httpd = evhttp_new(base);
	if (httpd == NULL)
		return ENOMEM;

	if (evhttp_bind_socket(httpd, "::", g_port)) {
		evhttp_free(httpd);
		return errno;
	}

	evhttp_set_cb(httpd, "/currentcost", process_watt, NULL);
	evhttp_set_cb(httpd, "/", process_html, NULL);
	
	return 0;
}

static void get_fqdn()
{
	char host[HOST_NAME_MAX];

	if (gethostname(host, sizeof(host))) {
		printf("warning: failed to get hostname: %m\n");
		return;
	}

	struct addrinfo *res, hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_CANONNAME;

	int rc = getaddrinfo(host, NULL, &hints, &res);

	if (rc) {
		printf("warning: failed to get fully qualified domain name"
						": %s\n", gai_strerror(rc));
		return;
	}

	if (res && res->ai_canonname) {
		g_fqdn = strdup(res->ai_canonname);
	} else {
		printf("warning: failed to find fully qualified domain name\n");
	}

	freeaddrinfo(res);
}

static void logit()
{
	// log it
	char buf[100];

	size_t size = snprintf(buf, sizeof(buf), "%ld,%.2f", time(NULL), 
								g_temperature);

	for (int i=0; i<g_wattcount; i++) {
		size += snprintf(buf + size, sizeof(buf) - size, ",%u", 
								g_watts[i]);
	}

	buf[size++] = '\n';

	int fd = TEMP_FAILURE_RETRY(open(g_statsfile, O_APPEND|O_CREAT|O_WRONLY|O_CLOEXEC, 0644));
	if (fd == -1) {
		syslog(LOG_WARNING, "failed to write %s: %m", g_statsfile);
		return;
	}
	
	size_t off = 0;
	while (size) {
		ssize_t ret = TEMP_FAILURE_RETRY(write(fd, buf + off, size));
		if (ret == -1) {
			syslog(LOG_WARNING, "failed to write %s: %m", g_statsfile);
			TEMP_FAILURE_RETRY(close(fd));
			return;
		}
		off += ret;
		size -= ret;
	}

	TEMP_FAILURE_RETRY(close(fd));
}

static void data_cb(double temperature, uint count, uint *watts)
{
	if (count > 0) {
		g_temperature = temperature;
		g_wattcount = count;

		for (int i=0; i<count; i++)
			g_watts[i] = watts[i];	

		logit();
	}
}

static void cc_data(evutil_socket_t fd, short event, void *arg)
{
	int rc = currentcost_read(arg, fd);
	if (rc) {
		syslog(LOG_ERR, "failed to read from " CC_DEVICE ": %s\n", 
								strerror(rc));
		exit(EXIT_FAILURE);
	}
}

static int open_cc(struct event_base *base)
{
	struct currentcost *cc = malloc(sizeof(*cc));
	int fd, rc;

	rc = currentcost_open(&fd);

	if (rc) {
		fprintf(stderr, "failed to open " CC_DEVICE ": %s\n", 
								strerror(rc));
		exit(EXIT_FAILURE);
	}

	currentcost_init(cc);
	cc->cb = data_cb;
	
	struct event *event = event_new(base, fd, EV_READ | EV_PERSIST,
                                                        cc_data, cc);
        event_add(event, NULL);

	return 0;
}

int main(int argc, char *argv[])
{
	int rc;
	bool daemonize = true;

	while ((rc = getopt(argc, argv, "hdp:")) != -1) {
		switch (rc) {
		case 'd':
			daemonize = false;
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
			printf("Usage: %s [-d] [-p port] [-h]\n", argv[0]);
			exit(EXIT_SUCCESS);
		case '?':
			exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "%s: invalid argument -- '%s'\n", argv[0], 
								argv[optind]);
		exit(EXIT_FAILURE);
	}

	get_fqdn();

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

	free(g_fqdn);

	return 0;
}