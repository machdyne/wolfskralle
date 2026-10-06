/*
 * wkhid: the Wolfskralle through Linux hidraw.
 */

#ifndef WKHID_H
#define WKHID_H

#include "wkclient.h"

struct wkhid {
	int fd;
	int gone;               /* the device disappeared (unplugged) */
	char path[300];
};

struct wkhid_found {
	char path[300];         /* /dev/hidrawN */
	char serial[40];
};

/* every Wolfskralle attached; returns how many */
int wkhid_list(struct wkhid_found *found, int max);

/* opens path, or the one Wolfskralle with serial (or the only one);
 * 0, or -1 with a message in err */
int wkhid_open(struct wkhid *h, const char *path, const char *serial, char *err, int errlen);
void wkhid_close(struct wkhid *h);

void wkhid_transport(struct wkhid *h, struct wk_transport *t);

#endif
