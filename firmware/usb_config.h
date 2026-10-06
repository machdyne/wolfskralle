#ifndef _USB_CONFIG_H
#define _USB_CONFIG_H

/* rv003usb configuration for the Wolfskralle (docs/protocol.md) */

#define ENDPOINTS 2

#define USB_PORT D      /* D+ PD3, D- PD4, the 1K5 pull-up on D- from PD5 */
#define USB_PIN_DP 3
#define USB_PIN_DM 4
#define USB_PIN_DPU 5

#define RV003USB_OPTIMIZE_FLASH          1
#define RV003USB_EVENT_DEBUGGING         0
#define RV003USB_HANDLE_IN_REQUEST       1   /* to NAK the interrupt endpoint */
#define RV003USB_OTHER_CONTROL           0
#define RV003USB_HANDLE_USER_DATA        1
#define RV003USB_HID_FEATURES            1
#define RV003USB_USER_DATA_HANDLES_TOKEN 0   /* rv003usb ACKs every data packet */
#define RV003USB_USE_REBOOT_FEATURE_REPORT 1 /* report 0xFD: into the bootloader */

#ifndef __ASSEMBLER__

#include <tinyusb_hid.h>
#include "wkproto.h"

#define WK_FW_VERSION 0x0008   /* 0.8: also bcdDevice */

/* the serial number string, filled from the chip's unique ID at start */
#define WK_SERIAL_CHARS 16
extern uint16_t wk_serial_desc[1 + WK_SERIAL_CHARS];

#ifdef INSTANCE_DESCRIPTORS

static const uint8_t device_descriptor[] = {
	18,                                 /* bLength */
	1,                                  /* bDescriptorType: device */
	0x10, 0x01,                         /* bcdUSB 1.1 */
	0x00, 0x00, 0x00,                   /* class, subclass, protocol: per interface */
	0x08,                               /* bMaxPacketSize0: 8 at low speed */
	WK_USB_VID & 0xff, WK_USB_VID >> 8,
	WK_USB_PID & 0xff, WK_USB_PID >> 8,
	WK_FW_VERSION & 0xff, WK_FW_VERSION >> 8,
	1, 2, 3,                            /* manufacturer, product, serial strings */
	1,                                  /* configurations */
};

/* one vendor feature report: ID WK_REPORT_ID, WK_REPORT_LEN bytes with the ID */
static const uint8_t special_hid_desc[] = {
	HID_USAGE_PAGE_N   ( 0xff57, 2 ),   /* vendor page 0xFF57 ("W") */
	HID_USAGE          ( 0x01 ),
	HID_COLLECTION     ( HID_COLLECTION_APPLICATION ),
		HID_REPORT_ID      ( WK_REPORT_ID )
		HID_USAGE          ( 0x01 ),
		HID_LOGICAL_MIN    ( 0x00 ),
		HID_LOGICAL_MAX_N  ( 0xff, 2 ),
		HID_REPORT_SIZE    ( 8 ),
		HID_REPORT_COUNT   ( WK_REPORT_LEN - 1 ),
		HID_FEATURE        ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
	HID_COLLECTION_END,
};

static const uint8_t config_descriptor[] = {
	9,                                  /* bLength */
	2,                                  /* bDescriptorType: configuration */
	0x22, 0x00,                         /* wTotalLength */
	0x01,                               /* bNumInterfaces */
	0x01,                               /* bConfigurationValue */
	0x00,                               /* iConfiguration */
	0x80,                               /* bmAttributes: bus powered */
	0x32,                               /* bMaxPower: 100 mA */

	9,                                  /* interface */
	4,
	0,                                  /* bInterfaceNumber */
	0,                                  /* bAlternateSetting */
	1,                                  /* bNumEndpoints */
	0x03,                               /* HID */
	0x00,                               /* no boot subclass */
	0x00,                               /* no protocol */
	0,                                  /* iInterface */

	9,                                  /* HID descriptor */
	0x21,
	0x11, 0x01,                         /* HID 1.11 */
	0x00,                               /* country */
	0x01,                               /* one class descriptor */
	0x22,                               /* report descriptor */
	sizeof(special_hid_desc), 0x00,

	7,                                  /* endpoint 1 IN, interrupt: required */
	0x05,                               /* by HID, never used: always NAKed */
	0x81,
	0x03,
	0x08, 0x00,                         /* wMaxPacketSize */
	255,                                /* bInterval: the longest, 255 ms */
};

#define STR_MANUFACTURER u"Machdyne"
#define STR_PRODUCT      u"Wolfskralle"

struct usb_string_descriptor_struct {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t wString[];
};
const static struct usb_string_descriptor_struct string0 __attribute__((section(".rodata"))) = {
	4, 3, {0x0409}
};
const static struct usb_string_descriptor_struct string1 __attribute__((section(".rodata"))) = {
	sizeof(STR_MANUFACTURER), 3, STR_MANUFACTURER
};
const static struct usb_string_descriptor_struct string2 __attribute__((section(".rodata"))) = {
	sizeof(STR_PRODUCT), 3, STR_PRODUCT
};

const static struct descriptor_list_struct {
	uint32_t      lIndexValue;
	const uint8_t *addr;
	uint8_t       length;
} descriptor_list[] = {
	{0x00000100, device_descriptor, sizeof(device_descriptor)},
	{0x00000200, config_descriptor, sizeof(config_descriptor)},
	{0x00002200, special_hid_desc, sizeof(special_hid_desc)},
	{0x00002100, config_descriptor + 18, 9},
	{0x00000300, (const uint8_t *)&string0, 4},
	{0x04090301, (const uint8_t *)&string1, sizeof(STR_MANUFACTURER)},
	{0x04090302, (const uint8_t *)&string2, sizeof(STR_PRODUCT)},
	{0x04090303, (const uint8_t *)wk_serial_desc, 2 + 2 * WK_SERIAL_CHARS},
};
#define DESCRIPTOR_LIST_ENTRIES ((sizeof(descriptor_list))/(sizeof(struct descriptor_list_struct)))

#endif /* INSTANCE_DESCRIPTORS */

#endif /* __ASSEMBLER__ */

#endif
