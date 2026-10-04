/*
 * USB HID: keyboards, mice and tablets, as input devices.
 *
 * Keyboards use the boot protocol (an 8-byte report every keyboard knows:
 * modifier bits and up to six keys held), translated to Linux key codes;
 * they don't repeat keys themselves, so the kernel does. Mice and tablets
 * are read with their report descriptor, which says where the buttons, the
 * motion (relative for a mouse, absolute for a tablet) and the wheel are in
 * their reports.
 */
#include <vexa/input.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/usb.h>

#define REQ_SET_IDLE 0x0a
#define REQ_SET_PROTOCOL 0x0b

/* HID usage IDs (the Keyboard page) to Linux key codes. */
static const uint8_t keycodes[256] = {
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26,
    27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
    72, 73, 82, 83, 86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
    115, 114, 0, 0, 0, 121, 0, 89, 93, 124, 92, 94, 95, 0, 0, 0,
    122, 123, 90, 91, 85, 0, 0, 0, 0, 0, 0, 0, 111, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    29, 42, 56, 125, 97, 54, 100, 126, 164, 166, 165, 163, 161, 115, 114, 113,
    150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140, 0, 0, 0, 0,
};

/* Where something is in a report (from the report descriptor). */
struct field {
    bool present;
    uint8_t report_id;
    uint16_t offset, size; /* In bits, after the report ID. */
    bool is_signed, absolute;
    int32_t minimum, maximum;
};

struct hid {
    struct usb_device *usb;
    struct input_device *input;
    bool keyboard;
    uint8_t last[8];          /* Keyboards: the last report. */
    bool report_ids;
    struct field buttons[5], x, y, wheel, pan;
    uint8_t last_buttons;
};

/* ---- Report descriptors ---- */

static uint32_t item_value(const uint8_t *p, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) {
        value |= (uint32_t)p[i] << (8 * i);
    }
    return value;
}

static int32_t signed_value(uint32_t value, int size) {
    if (size == 1) return (int8_t)value;
    if (size == 2) return (int16_t)value;
    return (int32_t)value;
}

static void parse_report_descriptor(struct hid *hid, const uint8_t *p, int length) {
    uint32_t usage_page = 0, report_size = 0, report_count = 0;
    int32_t logical_min = 0, logical_max = 0;
    uint8_t report_id = 0;
    uint32_t usages[16];
    int usage_count = 0;
    uint32_t usage_min = 0, usage_max = 0;
    uint16_t offsets[256] = {0}; /* Bits used so far, per report ID. */
    /* Push and Pop: a stack of the global items. */
    struct globals {
        uint32_t usage_page, report_size, report_count;
        int32_t logical_min, logical_max;
        uint8_t report_id;
    } stack[4];
    int depth = 0;
    const uint8_t *end = p + length;
    while (p < end) {
        uint8_t prefix = *p++;
        if (prefix == 0xfe) { /* A long item: skipped. */
            if (p + 2 > end) {
                break;
            }
            p += 2 + p[0];
            continue;
        }
        int size = (prefix & 3) == 3 ? 4 : prefix & 3;
        if (p + size > end) {
            break;
        }
        uint32_t value = item_value(p, size);
        p += size;
        uint8_t tag = prefix & 0xfc;
        switch (tag) {
        case 0x04: usage_page = value; break;                         /* Usage Page */
        case 0x14: logical_min = signed_value(value, size); break;     /* Logical Minimum */
        case 0x24:                                                     /* Logical Maximum */
            logical_max = logical_min < 0 ? signed_value(value, size) : (int32_t)value;
            break;
        case 0x74: report_size = value; break;                         /* Report Size */
        case 0x94: report_count = value; break;                        /* Report Count */
        case 0x84:                                                     /* Report ID */
            report_id = (uint8_t)value;
            hid->report_ids = true;
            break;
        case 0x08:                                                     /* Usage */
            if (usage_count < 16) {
                usages[usage_count++] = size == 4 ? value : (usage_page << 16 | value);
            }
            break;
        case 0x18: usage_min = size == 4 ? value : (usage_page << 16 | value); break;
        case 0x28: usage_max = size == 4 ? value : (usage_page << 16 | value); break;
        case 0x80: { /* Input */
            bool constant = value & 1, variable = value & 2, relative = value & 4;
            for (uint32_t i = 0; i < report_count; i++) {
                uint32_t usage = (int)i < usage_count ? usages[i]
                                 : usage_count ? usages[usage_count - 1]
                                 : usage_min + i <= usage_max ? usage_min + i : 0;
                struct field f = {
                    .present = true, .report_id = report_id,
                    .offset = offsets[report_id], .size = (uint16_t)report_size,
                    .is_signed = logical_min < 0, .absolute = !relative,
                    .minimum = logical_min, .maximum = logical_max,
                };
                offsets[report_id] = (uint16_t)(offsets[report_id] + report_size);
                if (constant || !variable) {
                    continue;
                }
                uint16_t page = usage >> 16, id = usage & 0xffff;
                if (page == 0x09 && id >= 1 && id <= 5 && !hid->buttons[id - 1].present) {
                    hid->buttons[id - 1] = f;
                } else if (page == 0x01 && id == 0x30 && !hid->x.present) {
                    hid->x = f;
                } else if (page == 0x01 && id == 0x31 && !hid->y.present) {
                    hid->y = f;
                } else if (page == 0x01 && id == 0x38 && !hid->wheel.present) {
                    hid->wheel = f;
                } else if (page == 0x0c && id == 0x238 && !hid->pan.present) {
                    hid->pan = f;
                }
            }
            usage_count = 0;
            usage_min = usage_max = 0;
            break;
        }
        case 0x90: /* Output */
        case 0xb0: /* Feature: (their bits are in other reports) */
            usage_count = 0;
            usage_min = usage_max = 0;
            break;
        case 0xa4: /* Push */
            if (depth < 4) {
                stack[depth++] = (struct globals){usage_page, report_size, report_count,
                                                  logical_min, logical_max, report_id};
            }
            break;
        case 0xb4: /* Pop */
            if (depth > 0) {
                struct globals *g = &stack[--depth];
                usage_page = g->usage_page;
                report_size = g->report_size;
                report_count = g->report_count;
                logical_min = g->logical_min;
                logical_max = g->logical_max;
                report_id = g->report_id;
            }
            break;
        case 0xa0: /* Collection */
        case 0xc0: /* End Collection */
            usage_count = 0;
            usage_min = usage_max = 0;
            break;
        default:
            break;
        }
    }
}

static int32_t field_value(const struct field *f, const uint8_t *data, int length) {
    uint32_t value = 0;
    for (int i = 0; i < f->size && i < 32; i++) {
        int bit = f->offset + i;
        if (bit / 8 < length && (data[bit / 8] >> (bit % 8)) & 1) {
            value |= 1U << i;
        }
    }
    if (f->is_signed && f->size < 32 && (value & (1U << (f->size - 1)))) {
        value |= ~0U << f->size;
    }
    return (int32_t)value;
}

/* ---- Reports ---- */

static void keyboard_report(void *arg, const uint8_t *data, int length) {
    struct hid *hid = arg;
    if (length < 8 || (data[2] == 1 && data[3] == 1)) {
        return; /* (Too many keys at once: "phantom" state.) */
    }
    /* Modifiers: one bit each. */
    uint8_t changed = data[0] ^ hid->last[0];
    for (int bit = 0; bit < 8; bit++) {
        if (changed & (1 << bit)) {
            keyboard_key(hid->input, keycodes[0xe0 + bit], (data[0] >> bit) & 1);
        }
    }
    /* Keys released, then keys pressed. */
    for (int i = 2; i < 8; i++) {
        uint8_t key = hid->last[i];
        if (key > 3 && !memchr(data + 2, key, 6)) {
            keyboard_key(hid->input, keycodes[key], 0);
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t key = data[i];
        if (key > 3 && !memchr(hid->last + 2, key, 6)) {
            keyboard_key(hid->input, keycodes[key], 1);
        }
    }
    memcpy(hid->last, data, 8);
    keyboard_still_held(hid->input);
}

static int32_t scaled(const struct field *f, int32_t value) {
    int64_t range = (int64_t)f->maximum - f->minimum;
    if (range <= 0) {
        return 0;
    }
    int64_t v = value < f->minimum ? f->minimum : value > f->maximum ? f->maximum : value;
    return (int32_t)((v - f->minimum) * VX_ABS_MAX / range);
}

static void pointer_report(void *arg, const uint8_t *data, int length) {
    struct hid *hid = arg;
    uint8_t id = 0;
    if (hid->report_ids && length > 0) {
        id = data[0];
        data++;
        length--;
    }
    struct input_device *input = hid->input;
    bool any = false;
    if (hid->x.present && hid->x.report_id == id) {
        int32_t x = field_value(&hid->x, data, length);
        int32_t y = hid->y.present ? field_value(&hid->y, data, length) : 0;
        if (hid->x.absolute) {
            input_report(input, VX_EV_ABS, VX_ABS_X, scaled(&hid->x, x));
            input_report(input, VX_EV_ABS, VX_ABS_Y, scaled(&hid->y, y));
            any = true;
        } else if (x || y) {
            if (x) {
                input_report(input, VX_EV_REL, VX_REL_X, x);
            }
            if (y) {
                input_report(input, VX_EV_REL, VX_REL_Y, y);
            }
            any = true;
        }
    }
    if (hid->wheel.present && hid->wheel.report_id == id) {
        int32_t wheel = field_value(&hid->wheel, data, length);
        if (wheel) {
            input_report(input, VX_EV_REL, VX_REL_WHEEL, wheel);
            any = true;
        }
    }
    if (hid->pan.present && hid->pan.report_id == id) {
        int32_t pan = field_value(&hid->pan, data, length);
        if (pan) {
            input_report(input, VX_EV_REL, VX_REL_HWHEEL, pan);
            any = true;
        }
    }
    static const uint16_t codes[5] = {VX_BTN_LEFT, VX_BTN_RIGHT, VX_BTN_MIDDLE, 0x113, 0x114};
    for (int i = 0; i < 5; i++) {
        if (hid->buttons[i].present && hid->buttons[i].report_id == id) {
            bool down = field_value(&hid->buttons[i], data, length) != 0;
            if (down != ((hid->last_buttons >> i) & 1)) {
                hid->last_buttons ^= (uint8_t)(1 << i);
                input_report(input, VX_EV_KEY, codes[i], down);
                any = true;
            }
        }
    }
    if (any) {
        input_sync(input);
    }
}

/* ---- The driver ---- */

static const struct usb_endpoint *interrupt_in(struct usb_interface *interface) {
    for (int i = 0; i < interface->endpoint_count; i++) {
        const struct usb_endpoint *e = &interface->endpoints[i];
        if (e->type == USB_ENDPOINT_INTERRUPT && (e->address & USB_DIR_IN)) {
            return e;
        }
    }
    return NULL;
}

static uint16_t report_descriptor_length(struct usb_interface *interface) {
    const uint8_t *p = interface->extra, *end = p + interface->extra_length;
    while (p + 2 <= end && p[0] >= 2) {
        if (p[1] == USB_DESC_HID && p[0] >= 9 && p + 9 <= end) {
            return (uint16_t)(p[7] | p[8] << 8);
        }
        p += p[0];
    }
    return 0;
}

static bool hid_probe(struct usb_interface *interface) {
    if (interface->interface_class != USB_CLASS_HID) {
        return false;
    }
    struct usb_device *usb = interface->device;
    const struct usb_endpoint *endpoint = interrupt_in(interface);
    if (!endpoint) {
        return false;
    }
    bool keyboard = interface->subclass == 1 && interface->protocol == 1;
    struct hid *hid = kzalloc(sizeof(*hid));
    if (!hid) {
        return false;
    }
    hid->usb = usb;
    hid->keyboard = keyboard;
    uint8_t request = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    if (keyboard) {
        usb_control(usb, request, REQ_SET_PROTOCOL, 0, interface->number, NULL, 0); /* Boot. */
    } else {
        /* The report protocol, which its report descriptor describes: the
         * firmware may have left a mouse in the boot protocol (3 or 4 bytes,
         * another layout) for its own use. */
        if (interface->subclass == 1) {
            usb_control(usb, request, REQ_SET_PROTOCOL, 1, interface->number, NULL, 0);
        }
        uint16_t length = report_descriptor_length(interface);
        if (length == 0 || length > 1024) {
            kfree(hid);
            return false;
        }
        uint8_t *descriptor = kzalloc(length);
        int n = descriptor ? usb_control(usb, USB_DIR_IN | USB_RECIP_INTERFACE,
                                         USB_REQ_GET_DESCRIPTOR, USB_DESC_REPORT << 8,
                                         interface->number, descriptor, length)
                           : -1;
        if (n > 0) {
            parse_report_descriptor(hid, descriptor, n);
        }
        kfree(descriptor);
        if (!hid->x.present || !hid->buttons[0].present) {
            kfree(hid); /* Not a pointer (a consumer control, a game pad...). */
            return false;
        }
    }
    /* Keyboards: their report again every 100 ms while keys are held (so key
     * repeat knows they still are); pointers: only changes. */
    bool held_reports = keyboard && usb_control(usb, request, REQ_SET_IDLE, 25 << 8,
                                                interface->number, NULL, 0) >= 0;
    if (!keyboard) {
        usb_control(usb, request, REQ_SET_IDLE, 0, interface->number, NULL, 0);
    }

    uint32_t capabilities = keyboard ? VX_INPUT_KEYS
                            : hid->x.absolute ? VX_INPUT_POINTER | VX_INPUT_ABSOLUTE
                                              : VX_INPUT_POINTER;
    struct input_device *input = input_reuse(capabilities);
    if (!input) {
        input = kzalloc(sizeof(*input));
    }
    if (!input) {
        kfree(hid);
        return false;
    }
    usb_device_name(usb, input->name, sizeof(input->name));
    input->capabilities = capabilities;
    input->parent = usb->node;
    if (keyboard) {
        input->set_repeat = NULL;
        input->held_reports = held_reports;
        keyboard_soft_repeat(input);
    }
    hid->input = input;
    input_register(input);
    interface->driver_data = hid;
    uint16_t size = endpoint->max_packet & 0x7ff;
    if (usb_interrupt_in(usb, endpoint->address, size < 8 && keyboard ? 8 : size,
                         keyboard ? keyboard_report : pointer_report, hid) < 0) {
        kprintf("[usb-hid] %s: can't read its reports\n", input->name);
    }
    kprintf("[usb-hid] %s: %s\n", input->name,
            keyboard ? "keyboard" : hid->x.absolute ? "tablet (absolute)" : "mouse");
    return true;
}

static void hid_disconnect(struct usb_interface *interface) {
    struct hid *hid = interface->driver_data;
    if (!hid) {
        return;
    }
    /* Keys held when it went are let go of. */
    if (hid->keyboard) {
        uint8_t none[8] = {0};
        keyboard_report(hid, none, 8);
    }
    input_unregister(hid->input); /* (It stays, for the next one: input_reuse.) */
    kfree(hid);
    interface->driver_data = NULL;
}

const struct usb_driver usb_hid_driver = {
    .name = "usb-hid",
    .probe = hid_probe,
    .disconnect = hid_disconnect,
};
