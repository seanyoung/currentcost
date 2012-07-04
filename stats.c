/* collect stats */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/queue.h>

//#define STATS_DIR "/var/log/currentcost"
#define STATS_DIR "/home/sean/dev/currentcost/stats"
// this is small for testing purposes
#define CHUNK 0x10000

struct watt {
	time_t epoch;
	uint watt;
};

#define STRIDE_SIZE 100000

struct wattstride {
	LIST_ENTRY(wattstride) entries;
	uint count, size;
	struct watt e[0];
};

static LIST_HEAD(watthead, wattstride) head = LIST_HEAD_INITIALIZER(head);

static void process_line(const char *line, size_t len)
{
	time_t epoch;
	uint watt;

	epoch = atol(line);
	watt = atoi(strchr(strchr(line, ',') + 1, ',') + 1);
	
	static struct wattstride *stride;

	if (LIST_EMPTY(&head) || stride->count >= stride->size) {
		struct wattstride *new;

		new = malloc(sizeof(*stride) + sizeof(watt) * STRIDE_SIZE);
		new->count = 0;
		new->size = STRIDE_SIZE;

		if (LIST_EMPTY(&head)) {
			LIST_INSERT_HEAD(&head, new, entries);
		} else {
			/* should be sorted!! */
			LIST_INSERT_AFTER(stride, new, entries);
		}

		stride = new;
	}

	stride->e[stride->count].epoch = epoch;
	stride->e[stride->count].watt = watt;

	stride->count++;
}

static int process_file(const char *name)
{
	char *pathname;
	asprintf(&pathname, STATS_DIR "/%s", name);

	int fd = TEMP_FAILURE_RETRY(open(pathname, O_RDONLY| O_CLOEXEC));
	if (fd == -1) {
		fprintf(stderr, "error: cannot open %s: %m\n", pathname);
		free(pathname);
		return errno;
	}

	struct stat st;
	if (TEMP_FAILURE_RETRY(fstat(fd, &st))) {
		fprintf(stderr, "error: cannot open %s: %m\n", pathname);
		TEMP_FAILURE_RETRY(close(fd));
		free(pathname);
		return errno;
	}

	char *str = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

	if (str == MAP_FAILED) {
		fprintf(stderr, "error: cannot mmap %s: %m\n", pathname);
		TEMP_FAILURE_RETRY(close(fd));
		free(pathname);
		return errno;
	}

	// do stuff
	char *q, *p = str;
	size_t size = st.st_size;
	char *map_start = str;

	while (size) {
		for (q = p; *q != '\n'; q++) {
			if (--size == 0) 
				break;
		}

		/* include \n */
		size--;
		size_t len = q - p + 1;

		process_line(p, len);
		p += len;

		while (map_start + CHUNK < p) {
			if (madvise(map_start, CHUNK, MADV_DONTNEED)) {
				fprintf(stderr, "error: madvise failed: %m\n");
				TEMP_FAILURE_RETRY(close(fd));
				free(pathname);
				return errno;
			}
			map_start += CHUNK;
		}
	}

	munmap(str, st.st_size);
	TEMP_FAILURE_RETRY(close(fd));

	return 0;
}

int collect_stats()
{
	struct dirent *dp;
	DIR *dir = opendir(STATS_DIR);

	if (!dir) {
		fprintf(stderr, "error: cannot open " STATS_DIR ": %m\n");
		return errno;
	}

	while ((dp = readdir(dir))) {
		if (dp->d_type != DT_REG)
			continue;
		
		process_file(dp->d_name);
	}

	closedir(dir);

	return 0;
}

void free_stats()
{
	struct wattstride *stride, *next;

	for (stride=LIST_FIRST(&head); stride; stride = next) {
		next = LIST_NEXT(stride, entries);
		free(stride);
	}
}

/*
on x = years, months, weeks, days, hours, minutes, seconds
on y = sum/avr/mean/medium(watt)
*/

int stats()
{
}

int main(int argc, char *argv[])
{
	collect_stats();
	free_stats();

	return 0;
}

