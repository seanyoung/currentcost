
#ifndef __CC_H__
#define __CC_H__

#define CC_DEVICE "/dev/ttyUSB0"
#define CC_TIMEOUT (60*30)

struct currentcost {
	void (*cb)(double temperature, unsigned sensor, unsigned watt);
	int fd;
	size_t size;
	char data[8192];
};

int currentcost_open(int*, const char *path);
int currentcost_read(struct currentcost *cc, int fd);

static void inline currentcost_init(struct currentcost *cc)
{
	cc->size = 0;
}

#endif

