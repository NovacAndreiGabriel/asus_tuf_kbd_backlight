#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/led-class-multicolor.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define TUF_KBD_VENDOR		0x0b05
#define TUF_KBD_PRODUCT		0x19b6

#define TUF_RID_ATTRS		0x41
#define TUF_RID_LAMP_REQ	0x42
#define TUF_RID_LAMP_RESP	0x43
#define TUF_RID_RANGE		0x45
#define TUF_RID_CTRL		0x46

#define TUF_ATTRS_LEN		23
#define TUF_LAMP_REQ_LEN	3
#define TUF_LAMP_RESP_LEN	29
#define TUF_RANGE_LEN		10
#define TUF_CTRL_LEN		2

#define TUF_ATTRS_COUNT		1
#define TUF_ATTRS_KIND		15
#define TUF_ATTRS_MIN_UPDATE	19
#define TUF_RESP_LEVELS		23

#define TUF_RANGE_COMPLETE	BIT(0)
#define TUF_MAX_BRIGHTNESS	255
#define TUF_MAX_UPDATE_US	100000U
#define TUF_MIN_RESUME_MS	10U
#define TUF_BUF_LEN		64

#define TUF_LED_NAME		"asus:rgb:kbd_color"

static unsigned int resume_interval_ms = 50;
module_param(resume_interval_ms, uint, 0644);
MODULE_PARM_DESC(resume_interval_ms, "Re-apply interval after resume in ms (default: 50)");

static unsigned int resume_window_ms = 4000;
module_param(resume_window_ms, uint, 0644);
MODULE_PARM_DESC(resume_window_ms, "How long to keep re-applying after resume in ms (default: 4000)");

enum tuf_channel {
	TUF_RED,
	TUF_GREEN,
	TUF_BLUE,
	TUF_INTENSITY,
	TUF_CHANNELS,
};

struct tuf_kbd {
	struct hid_device *hdev;
	struct led_classdev_mc mc;
	struct mc_subled subled[3];
	struct delayed_work resume_work;
	struct mutex lock;
	unsigned long resume_deadline;
	ktime_t last_update;
	u32 min_update_us;
	u16 lamp_count;
	u8 levels[TUF_CHANNELS];
	bool host_mode;
	u8 *buf;
};

static int tuf_kbd_check_report(struct hid_device *hdev, u8 id, u32 len, bool required)
{
	struct hid_report *report = hdev->report_enum[HID_FEATURE_REPORT].report_id_hash[id];

	if (report && hid_report_len(report) >= len)
		return 0;

	if (required)
		hid_err(hdev, "feature report 0x%02x missing or shorter than %u bytes\n", id, len);

	return -ENODEV;
}

static int tuf_kbd_check_reports(struct hid_device *hdev, bool *has_lamp_attrs)
{
	int ret;

	ret = tuf_kbd_check_report(hdev, TUF_RID_ATTRS, TUF_ATTRS_LEN, true);
	if (ret)
		return ret;

	ret = tuf_kbd_check_report(hdev, TUF_RID_RANGE, TUF_RANGE_LEN, true);
	if (ret)
		return ret;

	ret = tuf_kbd_check_report(hdev, TUF_RID_CTRL, TUF_CTRL_LEN, true);
	if (ret)
		return ret;

	*has_lamp_attrs = !tuf_kbd_check_report(hdev, TUF_RID_LAMP_REQ, TUF_LAMP_REQ_LEN, false) &&
			  !tuf_kbd_check_report(hdev, TUF_RID_LAMP_RESP, TUF_LAMP_RESP_LEN, false);

	return 0;
}

static int tuf_kbd_set_report(struct tuf_kbd *kbd, size_t len)
{
	int ret;

	lockdep_assert_held(&kbd->lock);

	ret = hid_hw_raw_request(kbd->hdev, kbd->buf[0], kbd->buf, len,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	return ret < 0 ? ret : 0;
}

static int tuf_kbd_get_report(struct tuf_kbd *kbd, u8 id, size_t len)
{
	int ret;

	lockdep_assert_held(&kbd->lock);

	memset(kbd->buf, 0, len);
	kbd->buf[0] = id;

	ret = hid_hw_raw_request(kbd->hdev, id, kbd->buf, len,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;

	return kbd->buf[0] == id ? 0 : -EPROTO;
}

static int tuf_kbd_set_autonomous(struct tuf_kbd *kbd, bool autonomous)
{
	memset(kbd->buf, 0, TUF_CTRL_LEN);
	kbd->buf[0] = TUF_RID_CTRL;
	kbd->buf[1] = autonomous;

	return tuf_kbd_set_report(kbd, TUF_CTRL_LEN);
}

static u8 tuf_kbd_scale(unsigned int value, u8 levels)
{
	value = min(value, (unsigned int)U8_MAX);

	if (levels < 2 || levels == U8_MAX)
		return value;

	return DIV_ROUND_CLOSEST(value * (levels - 1), U8_MAX);
}

static void tuf_kbd_throttle(struct tuf_kbd *kbd)
{
	s64 elapsed;

	if (!kbd->min_update_us)
		return;

	elapsed = ktime_us_delta(ktime_get(), kbd->last_update);
	if (elapsed >= 0 && elapsed < kbd->min_update_us)
		fsleep(kbd->min_update_us - elapsed);
}

static int tuf_kbd_apply(struct tuf_kbd *kbd, enum led_brightness brightness)
{
	unsigned int rgb[3];
	unsigned int intensity;
	int ret;
	int i;

	lockdep_assert_held(&kbd->lock);

	if (!kbd->host_mode) {
		ret = tuf_kbd_set_autonomous(kbd, false);
		if (ret)
			return ret;
		kbd->host_mode = true;
	}

	if (kbd->levels[TUF_INTENSITY] == 1) {
		led_mc_calc_color_components(&kbd->mc, brightness);
		for (i = 0; i < 3; i++)
			rgb[i] = kbd->subled[i].brightness;
		intensity = 0;
	} else {
		for (i = 0; i < 3; i++)
			rgb[i] = kbd->subled[i].intensity;
		intensity = brightness;
	}

	tuf_kbd_throttle(kbd);

	memset(kbd->buf, 0, TUF_RANGE_LEN);
	kbd->buf[0] = TUF_RID_RANGE;
	kbd->buf[1] = TUF_RANGE_COMPLETE;
	put_unaligned_le16(0, kbd->buf + 2);
	put_unaligned_le16(kbd->lamp_count - 1, kbd->buf + 4);
	kbd->buf[6] = tuf_kbd_scale(rgb[0], kbd->levels[TUF_RED]);
	kbd->buf[7] = tuf_kbd_scale(rgb[1], kbd->levels[TUF_GREEN]);
	kbd->buf[8] = tuf_kbd_scale(rgb[2], kbd->levels[TUF_BLUE]);
	kbd->buf[9] = tuf_kbd_scale(intensity, kbd->levels[TUF_INTENSITY]);

	ret = tuf_kbd_set_report(kbd, TUF_RANGE_LEN);
	kbd->last_update = ktime_get();
	if (ret)
		kbd->host_mode = false;

	return ret;
}

static int tuf_kbd_brightness_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct tuf_kbd *kbd = container_of(lcdev_to_mccdev(cdev), struct tuf_kbd, mc);
	int ret;

	mutex_lock(&kbd->lock);
	ret = tuf_kbd_apply(kbd, brightness);
	mutex_unlock(&kbd->lock);

	return ret;
}

static int tuf_kbd_query(struct tuf_kbd *kbd, bool has_lamp_attrs)
{
	struct hid_device *hdev = kbd->hdev;
	u32 kind;
	int ret;

	lockdep_assert_held(&kbd->lock);

	ret = tuf_kbd_get_report(kbd, TUF_RID_ATTRS, TUF_ATTRS_LEN);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "failed to read LampArrayAttributes\n");

	kbd->lamp_count = get_unaligned_le16(kbd->buf + TUF_ATTRS_COUNT);
	kind = get_unaligned_le32(kbd->buf + TUF_ATTRS_KIND);
	kbd->min_update_us = min(get_unaligned_le32(kbd->buf + TUF_ATTRS_MIN_UPDATE),
				 TUF_MAX_UPDATE_US);

	if (!kbd->lamp_count)
		return dev_err_probe(&hdev->dev, -ENODEV, "device reports no lamps\n");

	memset(kbd->levels, U8_MAX, sizeof(kbd->levels));

	if (has_lamp_attrs) {
		memset(kbd->buf, 0, TUF_LAMP_REQ_LEN);
		kbd->buf[0] = TUF_RID_LAMP_REQ;
		put_unaligned_le16(0, kbd->buf + 1);

		ret = tuf_kbd_set_report(kbd, TUF_LAMP_REQ_LEN);
		if (!ret)
			ret = tuf_kbd_get_report(kbd, TUF_RID_LAMP_RESP, TUF_LAMP_RESP_LEN);

		if (ret)
			hid_warn(hdev, "LampAttributes query failed (%d), assuming 8-bit channels\n", ret);
		else
			memcpy(kbd->levels, kbd->buf + TUF_RESP_LEVELS, TUF_CHANNELS);
	}

	hid_info(hdev, "%u lamp(s), kind %u, min update %u us, levels R/G/B/I %u/%u/%u/%u\n",
		 kbd->lamp_count, kind, kbd->min_update_us,
		 kbd->levels[TUF_RED], kbd->levels[TUF_GREEN],
		 kbd->levels[TUF_BLUE], kbd->levels[TUF_INTENSITY]);

	return 0;
}

static void tuf_kbd_resume_work(struct work_struct *work)
{
	struct tuf_kbd *kbd = container_of(to_delayed_work(work), struct tuf_kbd, resume_work);
	unsigned int interval = max(READ_ONCE(resume_interval_ms), TUF_MIN_RESUME_MS);

	mutex_lock(&kbd->lock);
	kbd->host_mode = false;
	tuf_kbd_apply(kbd, kbd->mc.led_cdev.brightness);
	mutex_unlock(&kbd->lock);

	if (time_before(jiffies, READ_ONCE(kbd->resume_deadline)))
		schedule_delayed_work(&kbd->resume_work, msecs_to_jiffies(interval));
}

static void tuf_kbd_init_led(struct tuf_kbd *kbd)
{
	struct led_classdev *cdev = &kbd->mc.led_cdev;
	int i;

	kbd->subled[0].color_index = LED_COLOR_ID_RED;
	kbd->subled[1].color_index = LED_COLOR_ID_GREEN;
	kbd->subled[2].color_index = LED_COLOR_ID_BLUE;

	for (i = 0; i < 3; i++) {
		kbd->subled[i].channel = i;
		kbd->subled[i].intensity = TUF_MAX_BRIGHTNESS;
	}

	kbd->mc.subled_info = kbd->subled;
	kbd->mc.num_colors = ARRAY_SIZE(kbd->subled);

	cdev->name = TUF_LED_NAME;
	cdev->max_brightness = TUF_MAX_BRIGHTNESS;
	cdev->brightness = TUF_MAX_BRIGHTNESS;
	cdev->brightness_set_blocking = tuf_kbd_brightness_set;
}

static int tuf_kbd_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct tuf_kbd *kbd;
	bool has_lamp_attrs;
	int ret;

	kbd = devm_kzalloc(&hdev->dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;

	kbd->buf = devm_kzalloc(&hdev->dev, TUF_BUF_LEN, GFP_KERNEL);
	if (!kbd->buf)
		return -ENOMEM;

	ret = devm_mutex_init(&hdev->dev, &kbd->lock);
	if (ret)
		return ret;

	kbd->hdev = hdev;
	INIT_DELAYED_WORK(&kbd->resume_work, tuf_kbd_resume_work);
	hid_set_drvdata(hdev, kbd);

	ret = hid_parse(hdev);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "hid_parse failed\n");

	ret = tuf_kbd_check_reports(hdev, &has_lamp_attrs);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "hid_hw_start failed\n");

	mutex_lock(&kbd->lock);
	ret = tuf_kbd_query(kbd, has_lamp_attrs);
	mutex_unlock(&kbd->lock);
	if (ret)
		goto err_stop;

	tuf_kbd_init_led(kbd);

	mutex_lock(&kbd->lock);
	ret = tuf_kbd_apply(kbd, kbd->mc.led_cdev.brightness);
	mutex_unlock(&kbd->lock);
	if (ret)
		hid_warn(hdev, "initial backlight update failed: %d\n", ret);

	ret = led_classdev_multicolor_register(&hdev->dev, &kbd->mc);
	if (ret) {
		dev_err_probe(&hdev->dev, ret, "failed to register LED\n");
		goto err_release;
	}

	return 0;

err_release:
	mutex_lock(&kbd->lock);
	tuf_kbd_set_autonomous(kbd, true);
	mutex_unlock(&kbd->lock);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void tuf_kbd_remove(struct hid_device *hdev)
{
	struct tuf_kbd *kbd = hid_get_drvdata(hdev);

	cancel_delayed_work_sync(&kbd->resume_work);
	led_classdev_multicolor_unregister(&kbd->mc);

	mutex_lock(&kbd->lock);
	tuf_kbd_set_autonomous(kbd, true);
	kbd->host_mode = false;
	mutex_unlock(&kbd->lock);

	hid_hw_stop(hdev);
}

static int tuf_kbd_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct tuf_kbd *kbd = hid_get_drvdata(hdev);

	cancel_delayed_work_sync(&kbd->resume_work);

	return 0;
}

static int tuf_kbd_resume(struct hid_device *hdev)
{
	struct tuf_kbd *kbd = hid_get_drvdata(hdev);

	WRITE_ONCE(kbd->resume_deadline, jiffies + msecs_to_jiffies(READ_ONCE(resume_window_ms)));
	schedule_delayed_work(&kbd->resume_work, 0);

	return 0;
}

static const struct hid_device_id tuf_kbd_devices[] = {
	{ HID_I2C_DEVICE(TUF_KBD_VENDOR, TUF_KBD_PRODUCT) },
	{ }
};
MODULE_DEVICE_TABLE(hid, tuf_kbd_devices);

static struct hid_driver tuf_kbd_driver = {
	.name		= "tuf_kbd",
	.id_table	= tuf_kbd_devices,
	.probe		= tuf_kbd_probe,
	.remove		= tuf_kbd_remove,
	.suspend	= pm_ptr(tuf_kbd_suspend),
	.resume		= pm_ptr(tuf_kbd_resume),
	.reset_resume	= pm_ptr(tuf_kbd_resume),
};
module_hid_driver(tuf_kbd_driver);

MODULE_AUTHOR("Andrei-Gabriel Novac");
MODULE_DESCRIPTION("ASUS TUF Gaming keyboard backlight (HID LampArray) driver");
MODULE_LICENSE("GPL");
