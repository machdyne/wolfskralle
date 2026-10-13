/*
 * i2cm: a bit-banged I2C master for the Sechs bus. See i2cm.h.
 */

#include "i2cm.h"

/* releases SCL and waits until it is high (a slave may stretch) */
static int scl_high(void)
{
	i2cm_scl(1);
	for (uint32_t t = 0; !i2cm_scl_in(); t += 10) {
		if (t >= I2CM_STRETCH_US) return -1;
		i2cm_delay_us(10);
	}
	return 0;
}

void i2cm_release(void)
{
	i2cm_sda(1);
	i2cm_scl(1);
}

/* a free bus: both lines high. A slave left in the middle of a read holds
 * SDA low: up to nine clocks let it finish its byte (then the master's
 * missing acknowledge ends the read), and a stop resets it. */
static int bus_free(void)
{
	i2cm_release();
	i2cm_delay();
	if (!i2cm_scl_in()) {
		if (scl_high()) return -1;
	}
	for (int i = 0; i < 9 && !i2cm_sda_in(); i++) {
		i2cm_scl(0);
		i2cm_delay();
		i2cm_delay();
		if (scl_high()) return -1;
		i2cm_delay();
		i2cm_delay();
	}
	if (!i2cm_sda_in()) return -1;
	/* a stop: whatever a slave was doing ends here */
	i2cm_scl(0);
	i2cm_delay();
	i2cm_sda(0);
	i2cm_delay();
	if (scl_high()) return -1;
	i2cm_delay();
	i2cm_sda(1);
	i2cm_delay();
	return i2cm_sda_in() && i2cm_scl_in() ? 0 : -1;
}

/* SCL high, SDA released: SDA falls (a start, or a repeated start) */
static int start(void)
{
	i2cm_sda(1);
	i2cm_delay();
	if (scl_high()) return -1;
	i2cm_delay();
	i2cm_sda(0);
	i2cm_delay();
	i2cm_scl(0);
	i2cm_delay();
	return 0;
}

static int stop(void)
{
	i2cm_sda(0);
	i2cm_delay();
	int r = scl_high();
	i2cm_delay();
	i2cm_sda(1);
	i2cm_delay();
	return r;
}

/* one bit; SCL is low on entry and on return. *in: SDA while SCL was high */
static int bit(int out, int *in)
{
	i2cm_sda(out);
	i2cm_delay();
	if (scl_high()) return -1;
	i2cm_delay();
	*in = i2cm_sda_in();
	i2cm_delay();
	i2cm_scl(0);
	i2cm_delay();
	return 0;
}

/* a byte out; 0 acknowledged, 1 not, -1 stretched too long */
static int write_byte(uint8_t b)
{
	int in;
	for (int i = 7; i >= 0; i--)
		if (bit((b >> i) & 1, &in)) return -1;
	if (bit(1, &in)) return -1;     /* release SDA: the slave acknowledges */
	return in ? 1 : 0;
}

static int read_byte(uint8_t *b, int ack)
{
	int in;
	uint8_t v = 0;
	for (int i = 0; i < 8; i++) {
		if (bit(1, &in)) return -1;
		v = (uint8_t)(v << 1 | (in ? 1 : 0));
	}
	if (bit(ack ? 0 : 1, &in)) return -1;
	*b = v;
	return 0;
}

int i2cm_xfer(uint8_t addr, const uint8_t *w, int wlen, uint8_t *r, int rlen)
{
	int res = I2CM_OK, k;

	if (bus_free()) {
		i2cm_release();
		return I2CM_STUCK;
	}
	if (start()) goto stretched;

	if (wlen || !rlen) {
		k = write_byte((uint8_t)(addr << 1));
		if (k < 0) goto stretched;
		if (k) { res = I2CM_NAK_ADDR; goto end; }
		for (int i = 0; i < wlen; i++) {
			k = write_byte(w[i]);
			if (k < 0) goto stretched;
			if (k) { res = I2CM_NAK_DATA; goto end; }
		}
		if (rlen && start()) goto stretched;
	}
	if (rlen) {
		k = write_byte((uint8_t)(addr << 1 | 1));
		if (k < 0) goto stretched;
		if (k) { res = I2CM_NAK_ADDR; goto end; }
		for (int i = 0; i < rlen; i++)
			if (read_byte(&r[i], i < rlen - 1)) goto stretched;
	}
end:
	if (stop()) res = I2CM_STRETCH;
	i2cm_release();
	return res;
stretched:
	stop();
	i2cm_release();
	return I2CM_STRETCH;
}
