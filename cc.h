
#ifndef __CC_H__
#define __CC_H__

#include <expat.h>

#define CC_DEVICE "/dev/currentcost"
#define CC_TIMEOUT (60)

struct currentcost {
	void (*cb)(double temperature, unsigned sensor, unsigned watt);
	int fd;
	size_t size;
	char data[8192];
	XML_Parser expat;
};

int currentcost_open(struct currentcost *cc, const char *path);
int currentcost_read(struct currentcost *cc);
void currentcost_close(struct currentcost *cc);

#endif

