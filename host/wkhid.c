/*
 * wkhid: the Wolfskralle through Linux hidraw (no libraries needed).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/hidraw.h>

#include "wkproto.h"
#include "wkhid.h"

/* reads /sys/class/hidraw/<name>/device/uevent */
static int read_uevent(const char *name, char *buf, int size)
{
	char path[300];
	snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", name);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int n = (int)fread(buf, 1, size - 1, f);
	fclose(f);
	buf[n < 0 ? 0 : n] = 0;
	return 0;
}

static void uevent_field(const char *ue, const char *key, char *out, int size)
{
	out[0] = 0;
	const char *p = strstr(ue, key);
	if (!p) return;
	p += strlen(key);
	int i = 0;
	while (*p && *p != '\n' && i < size - 1) out[i++] = *p++;
	out[i] = 0;
}

int wkhid_list(struct wkhid_found *found, int max)
{
	DIR *d = opendir("/sys/class/hidraw");
	if (!d) return 0;
	char want[64];
	snprintf(want, sizeof(want), "HID_ID=0003:%08X:%08X\n", WK_USB_VID, WK_USB_PID);
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		if (strncmp(e->d_name, "hidraw", 6)) continue;
		char ue[1024];
		if (read_uevent(e->d_name, ue, sizeof(ue))) continue;
		if (!strstr(ue, want)) continue;
		char name[128];
		uevent_field(ue, "HID_NAME=", name, sizeof(name));
		if (!strstr(name, "Wolfskralle")) continue;  /* the test PID is shared */
		snprintf(found[n].path, sizeof(found[n].path), "/dev/%s", e->d_name);
		uevent_field(ue, "HID_UNIQ=", found[n].serial, sizeof(found[n].serial));
		n++;
	}
	closedir(d);
	return n;
}

int wkhid_open(struct wkhid *h, const char *path, const char *serial, char *err, int errlen)
{
	struct wkhid_found f[16];
	char chosen[300];

	h->fd = -1;
	if (path) {
		snprintf(chosen, sizeof(chosen), "%s", path);
	} else {
		int n = wkhid_list(f, 16), pick = -1;
		for (int i = 0; i < n; i++) {
			if (serial && strcmp(serial, f[i].serial)) continue;
			if (pick >= 0) {
				snprintf(err, errlen, "more than one Wolfskralle: choose one with -s "
				         "(wolfctl list)");
				return -1;
			}
			pick = i;
		}
		if (pick < 0) {
			snprintf(err, errlen, serial ? "no Wolfskralle with serial %s"
			                             : "no Wolfskralle found%s", serial ? serial : "");
			return -1;
		}
		snprintf(chosen, sizeof(chosen), "%s", f[pick].path);
	}

	h->fd = open(chosen, O_RDWR | O_CLOEXEC);
	if (h->fd < 0) {
		snprintf(err, errlen, "%s: %s%s", chosen, strerror(errno),
		         errno == EACCES ? " (install host/99-wolfskralle.rules)" : "");
		return -1;
	}
	/* one wolfctl at a time per Wolfskralle */
	if (flock(h->fd, LOCK_EX | LOCK_NB) < 0) {
		snprintf(err, errlen, "%s is in use by another wolfctl", chosen);
		close(h->fd);
		h->fd = -1;
		return -1;
	}
	snprintf(h->path, sizeof(h->path), "%s", chosen);
	return 0;
}

void wkhid_close(struct wkhid *h)
{
	if (h->fd >= 0) close(h->fd);
	h->fd = -1;
}

static int hid_set(void *ctx, const uint8_t *buf, int len)
{
	struct wkhid *h = ctx;
	uint8_t tmp[WK_REPORT_LEN];
	memcpy(tmp, buf, len);
	int r = ioctl(h->fd, HIDIOCSFEATURE(len), tmp);
	if (r < 0 && errno == ENODEV) h->gone = 1;
	return r == len ? 0 : -1;
}

static int hid_get(void *ctx, uint8_t *buf, int len)
{
	struct wkhid *h = ctx;
	buf[0] = WK_REPORT_ID;
	int r = ioctl(h->fd, HIDIOCGFEATURE(len), buf);
	if (r < 0 && errno == ENODEV) h->gone = 1;
	return r;
}

static void hid_sleep(void *ctx, unsigned us)
{
	(void)ctx;
	usleep(us);
}

static uint64_t hid_now(void *ctx)
{
	(void)ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000;
}

void wkhid_transport(struct wkhid *h, struct wk_transport *t)
{
	t->set = hid_set;
	t->get = hid_get;
	t->sleep_us = hid_sleep;
	t->now_us = hid_now;
	t->ctx = h;
}
