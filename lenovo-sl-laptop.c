/*
 *  lenovo-sl-laptop.c - Lenovo ThinkPad SL Series Extras Driver
 *
 *
 *  Copyright (C) 2008-2009 Alexandre Rostovtsev
 *
 *  Based on thinkpad_acpi.c, eeepc-laptop.c and video.c which are copyright
 *  their respective authors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 *
 */

#define LENSL_LAPTOP_VERSION "0.01"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/rfkill.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>

#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/freezer.h>

#include <linux/proc_fs.h>

#define LENSL_MODULE_DESC "Lenovo ThinkPad SL Series Extras Driver"
#define LENSL_MODULE_NAME "lenovo_sl_laptop"

MODULE_AUTHOR("Alexandre Rostovtsev");
MODULE_DESCRIPTION(LENSL_MODULE_DESC);
MODULE_LICENSE("GPL");

#define CONFIG_LENSL_DEBUG 1

#ifdef CONFIG_LENSL_DEBUG
#define DEFAULT_MESSAGE_LOGLEVEL KERN_DEBUG
#endif

#define LENSL_ERR KERN_ERR LENSL_MODULE_NAME ": "
#define LENSL_NOTICE KERN_NOTICE LENSL_MODULE_NAME ": "
#define LENSL_INFO KERN_INFO LENSL_MODULE_NAME ": "
#define LENSL_DEBUG KERN_DEBUG LENSL_MODULE_NAME ": "

#define LENSL_HKEY_FILE LENSL_MODULE_NAME
#define LENSL_DRVR_NAME LENSL_MODULE_NAME
#define LENSL_BACKLIGHT_NAME LENSL_MODULE_NAME

#define LENSL_HKEY_POLL_KTHREAD_NAME "klensl_hkeyd"

#define LENSL_EC0 "\\_SB.PCI0.SBRG.EC0"
#define LENSL_HKEY LENSL_EC0 ".HKEY"
#define LENSL_LCDD "\\_SB.PCI0.VGA.LCDD"

/* parameters */

static int debug_ec = 0; /* present EC debugging interface in procfs */
static int control_backlight = 0; /* control the backlight (NB this may conflict with video.c) */
module_param(debug_ec, bool, S_IRUGO);
module_param(control_backlight, bool, S_IRUGO);

/* general */

enum {
	LENSL_RFK_BLUETOOTH_SW_ID = 0,
	LENSL_RFK_WWAN_SW_ID,
};

acpi_handle hkey_handle;
static struct platform_device *lensl_pdev;

static int parse_strtoul(const char *buf,
		unsigned long max, unsigned long *value)
{
	char *endp;

	while (*buf && isspace(*buf))
		buf++;
	*value = simple_strtoul(buf, &endp, 0);
	while (*endp && isspace(*endp))
		endp++;
	if (*endp || *value > max)
		return -EINVAL;

	return 0;
}

static struct input_dev *hkey_inputdev;

/*************************************************************************
    bluetooth - copied nearly verbatim from thinkpad_acpi.c
 *************************************************************************/

enum {
	/* ACPI GBDC/SBDC bits */
	TP_ACPI_BLUETOOTH_HWPRESENT	= 0x01,	/* Bluetooth hw available */
	TP_ACPI_BLUETOOTH_RADIOSSW	= 0x02,	/* Bluetooth radio enabled */
	TP_ACPI_BLUETOOTH_UNK		= 0x04,	/* unknown function */
};

static struct rfkill *bluetooth_rfkill;
static int bluetooth_present;

static int lensl_get_acpi_int(acpi_handle handle, char *pathname, int *value)
{
	acpi_status status;
	unsigned long long ullval;

	if (!handle)
		return -EINVAL;
	status = acpi_evaluate_integer(handle, pathname, NULL, &ullval);
	if (ACPI_FAILURE(status))
		return -EIO;
	*value = (int)ullval;
	printk(LENSL_DEBUG "ACPI : %s == %d\n", pathname, *value);
	return 0;
}

static int lensl_set_acpi_int(acpi_handle handle, char *pathname, int value)
{
	acpi_status status;
	struct acpi_object_list params;
	union acpi_object in_obj;

	if (!handle)
		return -EINVAL;
	in_obj.integer.value = value;
	in_obj.type = ACPI_TYPE_INTEGER;
	params.count = 1;
	params.pointer = &in_obj;
	status = acpi_evaluate_object(handle, pathname, &params, NULL);
	if (ACPI_FAILURE(status))
		return -EIO;
	printk(LENSL_DEBUG "ACPI : set %s = %d\n", pathname, value);
	return 0;
}

static inline int get_wlsw(int *value)
{
	return lensl_get_acpi_int(hkey_handle, "WLSW", value);
}

static inline int get_gbdc(int *value)
{
	return lensl_get_acpi_int(hkey_handle, "GBDC", value);
}

static inline int set_sbdc(int value)
{
	return lensl_set_acpi_int(hkey_handle, "SBDC", value);
}

static int bluetooth_get_radiosw(void)
{
	int value = 0;

	if (!bluetooth_present)
		return -ENODEV;

	/* WLSW overrides bluetooth in firmware/hardware, reflect that */
	if (get_wlsw(&value) && !value)
		return RFKILL_STATE_HARD_BLOCKED;

	if (get_gbdc(&value))
		return -EIO;

	return ((value & TP_ACPI_BLUETOOTH_RADIOSSW) != 0) ?
		RFKILL_STATE_UNBLOCKED : RFKILL_STATE_SOFT_BLOCKED;
}

static void bluetooth_update_rfk(void)
{
	int result;

	if (!bluetooth_rfkill)
		return;

	result = bluetooth_get_radiosw();
	if (result < 0)
		return;
	rfkill_force_state(bluetooth_rfkill, result);
}

static int bluetooth_set_radiosw(int radio_on, int update_rfk)
{
	int value;

	if (!bluetooth_present)
		return -ENODEV;

	/* WLSW overrides bluetooth in firmware/hardware, but there is no
	 * reason to risk weird behaviour. */
	if (get_wlsw(&value) && !value && radio_on)
		return -EPERM;

	if (get_gbdc(&value))
		return -EIO;
	if (radio_on)
		value |= TP_ACPI_BLUETOOTH_RADIOSSW;
	else
		value &= ~TP_ACPI_BLUETOOTH_RADIOSSW;
	if (!set_sbdc(value))
		return -EIO;

	if (update_rfk)
		bluetooth_update_rfk();

	return 0;
}

/*************************************************************************
    bluetooth sysfs - copied nearly verbatim from thinkpad_acpi.c
 *************************************************************************/

static ssize_t bluetooth_enable_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int status;

	status = bluetooth_get_radiosw();
	if (status < 0)
		return status;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			(status == RFKILL_STATE_UNBLOCKED) ? 1 : 0);
}

static ssize_t bluetooth_enable_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;
	int res;

	if (parse_strtoul(buf, 1, &t))
		return -EINVAL;

	res = bluetooth_set_radiosw(t, 1);

	return (res) ? res : count;
}

static struct device_attribute dev_attr_bluetooth_enable =
	__ATTR(bluetooth_enable, S_IWUSR | S_IRUGO,
		bluetooth_enable_show, bluetooth_enable_store);

static struct attribute *bluetooth_attributes[] = {
	&dev_attr_bluetooth_enable.attr,
	NULL
};

static const struct attribute_group bluetooth_attr_group = {
	.attrs = bluetooth_attributes,
};

static int bluetooth_rfk_get(void *data, enum rfkill_state *state)
{
	int bts = bluetooth_get_radiosw();

	if (bts < 0)
		return bts;

	*state = bts;
	return 0;
}

static int bluetooth_rfk_set(void *data, enum rfkill_state state)
{
	return bluetooth_set_radiosw((state == RFKILL_STATE_UNBLOCKED), 0);
}

static int lensl_new_rfkill(const unsigned int id,
			struct rfkill **rfk,
			const enum rfkill_type rfktype,
			const char *name,
			int (*toggle_radio)(void *, enum rfkill_state),
			int (*get_state)(void *, enum rfkill_state *))
{
	int res;
	enum rfkill_state initial_state;

	*rfk = rfkill_allocate(&lensl_pdev->dev, rfktype);
	if (!*rfk) {
		printk(LENSL_ERR
			"failed to allocate memory for rfkill class\n");
		return -ENOMEM;
	}

	(*rfk)->name = name;
	(*rfk)->get_state = get_state;
	(*rfk)->toggle_radio = toggle_radio;

	if (!get_state(NULL, &initial_state))
		(*rfk)->state = initial_state;

	res = rfkill_register(*rfk);
	if (res < 0) {
		printk(LENSL_ERR
			"failed to register %s rfkill switch: %d\n",
			name, res);
		rfkill_free(*rfk);
		*rfk = NULL;
		return res;
	}

	return 0;
}

static void bluetooth_exit(void)
{
	if (bluetooth_rfkill)
		rfkill_unregister(bluetooth_rfkill);

	sysfs_remove_group(&lensl_pdev->dev.kobj,
			&bluetooth_attr_group);
}

static int bluetooth_init(void)
{
	int value, res;
	bluetooth_present = 0;
	if (!hkey_handle)
		return -ENODEV;
	if (get_gbdc(&value))
		return -EIO;
	if (!(value & TP_ACPI_BLUETOOTH_HWPRESENT))
		return -ENODEV;
	bluetooth_present = 1;

	res = sysfs_create_group(&lensl_pdev->dev.kobj,
				&bluetooth_attr_group);
	if (res)
		return res;

	res = lensl_new_rfkill(LENSL_RFK_BLUETOOTH_SW_ID,
				&bluetooth_rfkill,
				RFKILL_TYPE_BLUETOOTH,
				"lensl_bluetooth_sw",
				bluetooth_rfk_set,
				bluetooth_rfk_get);
	if (res) {
		bluetooth_exit();
		return res;
	}

	return 0;
}

/*************************************************************************
    backlight control - based on video.c
 *************************************************************************/

/* NB: the reason why this needs to be implemented here is that the SL series
   uses the ACPI interface for controlling the backlight in a non-standard
   manner. See http://bugzilla.kernel.org/show_bug.cgi?id=12249  */

#define	ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS	0x86
#define ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS	0x87

acpi_handle lcdd_handle;
struct backlight_device *backlight;
static struct lensl_vector {
	int count;
	int *values;
} backlight_levels;

static int get_bcl(struct lensl_vector *levels)
{
	int i, status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *o, *obj;

	if (!levels)
		return -EINVAL;
	if (levels->count) {
		levels->count = 0;
		kfree(levels->values);
	}

	/* _BCL returns an array sorted from high to low; the first two values
	   are *not* special (non-standard behavior) */
	status = acpi_evaluate_object(lcdd_handle, "_BCL", NULL, &buffer);
	if (!ACPI_SUCCESS(status))
		return status;
	obj = (union acpi_object *)buffer.pointer;
	if (!obj || (obj->type != ACPI_TYPE_PACKAGE)) {
		printk(LENSL_ERR "Invalid _BCL data\n");
		status = -EFAULT;
		goto out;
	}

	levels->count = obj->package.count;
	if (!levels->count)
		goto out;
	levels->values = kmalloc(levels->count * sizeof(int), GFP_KERNEL);
	if (!levels->values) {
		printk(KERN_ERR "can't allocate memory\n");
		status = -ENOMEM;
		goto out;
	}

	for (i = 0; i < obj->package.count; i++) {
		o = (union acpi_object *)&obj->package.elements[i];
		if (o->type != ACPI_TYPE_INTEGER) {
			printk(LENSL_ERR "Invalid data\n");
			goto err;
		}
		levels->values[i] = (int) o->integer.value;
	}
	goto out;

err:
	levels->count = 0;
	kfree(levels->values);

out:
	kfree(buffer.pointer);

	return status;
}

static inline int set_bcm(int level)
{
	/* standard behavior */
	return lensl_set_acpi_int(lcdd_handle, "_BCM", level);
}

static inline int get_bqc(int *level)
{
	/* returns an index from the bottom into the _BCL package
	   (non-standard behavior) */
	return lensl_get_acpi_int(lcdd_handle, "_BQC", level);
}

/* backlight device sysfs support */
static int lensl_bd_get_brightness(struct backlight_device *bd)
{
	int level = 0;

	if (get_bqc(&level))
		return 0;

	return level;
}

static int lensl_bd_set_brightness_int(int request_level)
{
	int n;
	n = backlight_levels.count - request_level - 1;
	if (n >= 0 && n < backlight_levels.count) {
		return set_bcm(backlight_levels.values[n]);
	}

	return -EINVAL;
}

static int lensl_bd_set_brightness(struct backlight_device *bd)
{
	if (!bd)
		return -EINVAL;

	return lensl_bd_set_brightness_int(bd->props.brightness);
}

static struct backlight_ops lensl_backlight_ops = {
	.get_brightness = lensl_bd_get_brightness,
	.update_status  = lensl_bd_set_brightness,
};

static void backlight_exit(void)
{
	backlight_device_unregister(backlight);
	backlight = NULL;
	if (backlight_levels.count) {
		kfree(backlight_levels.values);
		backlight_levels.count = 0;
	}
}

static int
backlight_init(void)
{
	int status = 0;

	lcdd_handle = NULL;
	backlight = NULL;
	backlight_levels.count = 0;
	backlight_levels.values = NULL;

	status = acpi_get_handle(NULL, LENSL_LCDD, &lcdd_handle);
	if (ACPI_FAILURE(status)) {
		printk(LENSL_ERR "Failed to get ACPI handle for %s\n", LENSL_LCDD);
		return -EIO;
	}

	status = get_bcl(&backlight_levels);
	if (status || !backlight_levels.count)
		goto err;

	backlight = backlight_device_register(LENSL_BACKLIGHT_NAME,
			NULL, NULL, &lensl_backlight_ops);
	backlight->props.max_brightness = backlight_levels.count - 1;
	backlight->props.brightness = lensl_bd_get_brightness(backlight);

	goto out;
err:
	if (backlight_levels.count) {
		kfree(backlight_levels.values);
		backlight_levels.count = 0;
	}
out:
	return status;
}

/*************************************************************************
    hotkeys
 *************************************************************************/

static int hkey_poll_hz = 5;
static u8 hkey_ec_prev_offset = 0;
static struct mutex hkey_poll_mutex;
static struct task_struct *hkey_poll_task;

struct key_entry {
	char type;
	u8 scancode;
	int keycode;
};

enum { KE_KEY, KE_END };

static struct key_entry ec_keymap[] = {
	/* Fn F2 */
	{KE_KEY, 0x0B, KEY_COFFEE },
	/* Fn F3 */
	{KE_KEY, 0x0C, KEY_BATTERY },
	/* Fn F4; dispatches an ACPI event */
	{KE_KEY, 0x0D, /* KEY_SLEEP */ KEY_RESERVED },
	/* Fn F5; FIXME: should this be KEY_BLUETOOTH? */
	{KE_KEY, 0x0E, KEY_WLAN },
	/* Fn F7; dispatches an ACPI event */
	{KE_KEY, 0x10, /* KEY_SWITCHVIDEOMODE */ KEY_RESERVED },
	/* Fn F8 - ultranav; FIXME: find some keycode that fits this properly */
	{KE_KEY, 0x11, KEY_PROG1 },
	/* Fn F9 */
	{KE_KEY, 0x12, KEY_EJECTCD },
	/* Fn F12 */
	{KE_KEY, 0x15, KEY_SUSPEND },
	{KE_KEY, 0x69, KEY_VOLUMEUP },
	{KE_KEY, 0x6A, KEY_VOLUMEDOWN },
	{KE_KEY, 0x6B, KEY_MUTE },
	/* Fn Home; dispatches an ACPI event */
	{KE_KEY, 0x6C, KEY_BRIGHTNESSDOWN /*KEY_RESERVED*/ },
	/* Fn End; dispatches an ACPI event */
	{KE_KEY, 0x6D, KEY_BRIGHTNESSUP /*KEY_RESERVED*/ },
	/* Fn spacebar - zoom */	
	{KE_KEY, 0x71, KEY_ZOOM },
	/* Lenovo Care key */
	{KE_KEY, 0x80, KEY_VENDOR },
	{KE_END, 0},
};

static int ec_scancode_to_keycode(u8 scancode)
{
	struct key_entry *key;

	for (key = ec_keymap; key->type != KE_END; key++)
		if (scancode == key->scancode)
			return key->keycode;

	return -EINVAL;
}

static int hkey_inputdev_getkeycode(struct input_dev *dev, int scancode, int *keycode)
{
	int result;

	if (!dev)
		return -EINVAL;

	result = ec_scancode_to_keycode(scancode);
	if (result >= 0) {
		*keycode = result;
		return 0;
	}
	return result;
}

static int hkey_inputdev_setkeycode(struct input_dev *dev, int scancode, int keycode)
{
	struct key_entry *key;

	if (!dev)
		return -EINVAL;

	for (key = ec_keymap; key->type != KE_END; key++)
		if (scancode == key->scancode) {
			clear_bit(key->keycode, dev->keybit);
			key->keycode = keycode;
			set_bit(key->keycode, dev->keybit);
			return 0;
		}

	return -EINVAL;
}

static int hkey_ec_get_offset(void)
{
	/* Hotkey events are stored in EC registers 0x0A .. 0x11
	 * Address of last event is stored in EC registers 0x12 and
	 * 0x14; if address is 0x01, last event is in register 0x0A;
	 * if address is 0x07, last event is in register 0x10;
	 * if address is 0x00, last event is in register 0x11 */

	u8 offset;

	if (ec_read(0x12, &offset))
		return -EINVAL;
	if (!offset)
		offset = 8;
	offset -= 1;
	if (offset > 7)
		return -EINVAL;
	return offset;
}

static int hkey_poll_kthread(void *data)
{
	unsigned long t = 0;
	int offset, level;
	unsigned int keycode;
	u8 scancode;

	mutex_lock(&hkey_poll_mutex);

	offset = hkey_ec_get_offset();
	if (offset < 0) {
		printk(LENSL_ERR "Failed to read hotkey register offset from EC\n");
		hkey_ec_prev_offset = 0;
	} else
		hkey_ec_prev_offset = offset;

	while (!kthread_should_stop() && hkey_poll_hz) {
		if (t == 0)
			t = 1000/hkey_poll_hz;
		t = msleep_interruptible(t);
		if (unlikely(kthread_should_stop()))
		 	break;
		try_to_freeze(); /* FIXME */
		if (t > 0)
			continue;
		offset = hkey_ec_get_offset();
		if (offset < 0) {
			printk(LENSL_ERR "Failed to read hotkey register offset from EC\n");
			continue;
		}
		if (offset == hkey_ec_prev_offset)
			continue;

		if (ec_read(0x0A + offset, &scancode)) {
			printk(LENSL_ERR "Failed to read hotkey code from EC\n");
			continue;
		}
		keycode = ec_scancode_to_keycode(scancode);
		printk(LENSL_DEBUG "Got hotkey keycode %d\n", keycode);

		/* Special handling for brightness keys. We do it here and not
		   via an ACPI notifier to prevent possible conflights with
		   video.c */
		if (keycode == KEY_BRIGHTNESSDOWN) {
			if (control_backlight && backlight) {
				level = lensl_bd_get_brightness(backlight);
				if (0 <= --level)
					lensl_bd_set_brightness_int(level);
			}
			else
				keycode = KEY_RESERVED;
		} else if (keycode == KEY_BRIGHTNESSUP) {
			if (control_backlight && backlight) {
				level = lensl_bd_get_brightness(backlight);
				if (backlight_levels.count > ++level)
					lensl_bd_set_brightness_int(level);
			}
			else
				keycode = KEY_RESERVED;
		}

		if (keycode != KEY_RESERVED) {
			/* TODO : handle KEY_UNKNOWN case */
			input_report_key(hkey_inputdev, keycode, 1);
			input_sync(hkey_inputdev);
			input_report_key(hkey_inputdev, keycode, 0);
			input_sync(hkey_inputdev);
		}
		hkey_ec_prev_offset = offset;
	}

	mutex_unlock(&hkey_poll_mutex);
	return 0;
}

static void hkey_poll_start(void)
{
	mutex_lock(&hkey_poll_mutex);
	hkey_poll_task = kthread_run(hkey_poll_kthread,
		NULL, LENSL_HKEY_POLL_KTHREAD_NAME);
	if (IS_ERR(hkey_poll_task)) {
		hkey_poll_task = NULL;
		printk(LENSL_ERR "could not create kernel thread for hotkey polling\n");
	}
	mutex_unlock(&hkey_poll_mutex);
}

static void hkey_poll_stop(void)
{
	if (hkey_poll_task) {
		if (frozen(hkey_poll_task) || freezing(hkey_poll_task))
			thaw_process(hkey_poll_task);

		kthread_stop(hkey_poll_task);
		hkey_poll_task = NULL;
		mutex_lock(&hkey_poll_mutex);
		/* at this point, the thread did exit */
		mutex_unlock(&hkey_poll_mutex);
	}
}

static void hkey_inputdev_exit(void)
{
	if (hkey_inputdev)
		input_unregister_device(hkey_inputdev);
	hkey_inputdev = NULL;
}

static int hkey_inputdev_init(void)
{
	int result;
	struct key_entry *key;

	hkey_inputdev = input_allocate_device();
	if (!hkey_inputdev) {
		printk(LENSL_ERR "Unable to allocate input device\n");
		return -ENODEV;
	}
	hkey_inputdev->name = "Lenovo ThinkPad SL Series Extra Buttons";
	hkey_inputdev->phys = LENSL_HKEY_FILE "/input0";
	hkey_inputdev->id.bustype = BUS_HOST;
	hkey_inputdev->getkeycode = hkey_inputdev_getkeycode;
	hkey_inputdev->setkeycode = hkey_inputdev_setkeycode;
	set_bit(EV_KEY, hkey_inputdev->evbit);

	for (key = ec_keymap; key->type != KE_END; key++)
		set_bit(key->keycode, hkey_inputdev->keybit);

	result = input_register_device(hkey_inputdev);
	if (result) {
		printk(LENSL_ERR "Unable to register input device\n");
		input_free_device(hkey_inputdev);
		hkey_inputdev = NULL;
		return -ENODEV;
	}
	return 0;
}


/*************************************************************************
    procfs debugging interface
 *************************************************************************/

#define LENSL_PROC_EC "ec_dump"
#define LENSL_PROC_DIRNAME "lenovo_sl"

static struct proc_dir_entry *proc_dir;

int lensl_ec_read_procmem(char *buf, char **start, off_t offset,
		int count, int *eof, void *data)
{
	int err, len = 0;
	u8 i, result;
	/* note: ec_read at i = 255 locks up my SL300 hard. -AR */
	for (i = 0; i < 255; i++) {
		if (!(i % 16)) {
			if (i)
				len += sprintf(buf+len, "\n");
			len += sprintf(buf+len, "%02X:", i);
		}
		err = ec_read(i, &result);
		if (!err)
			len += sprintf(buf+len, " %02X", result);
		else
			len += sprintf(buf+len, " **");
	}
	len += sprintf(buf+len, "\n");
	*eof = 1;
	return len;
}

static void lenovo_sl_procfs_exit(void)
{
	if (proc_dir) {
		remove_proc_entry(LENSL_PROC_EC, proc_dir);
		remove_proc_entry(LENSL_PROC_DIRNAME, acpi_root_dir);
	}
}

static int lenovo_sl_procfs_init(void)
{
	struct proc_dir_entry *proc_ec;

	proc_dir = proc_mkdir(LENSL_PROC_DIRNAME, acpi_root_dir);
	if (!proc_dir) {
		printk(LENSL_ERR "unable to create proc dir acpi/%s/\n", LENSL_PROC_DIRNAME);
		return -ENODEV;
	}
	proc_dir->owner = THIS_MODULE;
	proc_ec = create_proc_read_entry(LENSL_PROC_EC, 0, proc_dir, 
		lensl_ec_read_procmem, NULL /* client data */);
	if (!proc_ec) {
		printk(LENSL_ERR "unable to create proc entry acpi/%s/%s\n",
			LENSL_PROC_DIRNAME, LENSL_PROC_EC);
		return -ENODEV;
	}

	return 0;
}

/*************************************************************************
    init/exit
 *************************************************************************/

static int __init lenovo_sl_laptop_init(void)
{
	int ret;
	acpi_status status;

	hkey_handle = NULL;

	if (acpi_disabled)
		return -ENODEV;

	status = acpi_get_handle(NULL, LENSL_HKEY, &hkey_handle);
	if (ACPI_FAILURE(status)) {
		printk(LENSL_ERR "Failed to get ACPI handle for %s\n", LENSL_HKEY);
		return -EIO;
	}

	lensl_pdev = platform_device_register_simple(LENSL_DRVR_NAME, -1,
							NULL, 0);
	if (IS_ERR(lensl_pdev)) {
		ret = PTR_ERR(lensl_pdev);
		lensl_pdev = NULL;
		printk(LENSL_ERR "unable to register platform device\n");
		return ret;
	}

	ret = hkey_inputdev_init();
	if (ret)
		return -EIO;

	bluetooth_init();
	if (control_backlight)
		backlight_init();

	mutex_init(&hkey_poll_mutex);
	hkey_poll_start();

	if (debug_ec)
		lenovo_sl_procfs_init();

	printk(LENSL_INFO "Loaded Lenovo ThinkPad SL Series Driver\n");
	return 0;
}

static void __exit lenovo_sl_laptop_exit(void)
{
	lenovo_sl_procfs_exit();
	hkey_poll_stop();
	backlight_exit();
	bluetooth_exit();
	hkey_inputdev_exit();
	if (lensl_pdev)
		platform_device_unregister(lensl_pdev);
	printk(LENSL_INFO "Unloaded Lenovo ThinkPad SL Series Driver\n");
}

module_init(lenovo_sl_laptop_init);
module_exit(lenovo_sl_laptop_exit);