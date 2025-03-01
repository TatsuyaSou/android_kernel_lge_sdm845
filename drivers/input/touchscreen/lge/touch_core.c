/*
 * touch_core.c
 *
 * Copyright (c) 2015 LGE.
 *
 * author : hoyeon.jang@lge.com
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define TS_MODULE "[core]"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/input/lge_touch_notify.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>

/*
 *  Include to touch core Header File
 */
#include <touch_core.h>

u32 touch_debug_mask = BASE_INFO;
/* Debug mask value
 * usage: echo [debug_mask] > /sys/module/touch_core/parameters/debug_mask
 */
module_param_named(debug_mask, touch_debug_mask, int, S_IRUGO|S_IWUSR|S_IWGRP);

#if defined(CONFIG_SECURE_TOUCH)
static irqreturn_t secure_touch_filter_interrupt(struct touch_core_data *ts);
#endif

static void touch_send_uevent(struct touch_core_data *ts, int type);

#if defined(CONFIG_LGE_TOUCH_CORE_QCT)
#if IS_ENABLED(CONFIG_LGE_DISPLAY_RECOVERY_ESD) || IS_ENABLED(CONFIG_LGE_TOUCH_PANEL_GLOBAL_RESET)
extern void lge_mdss_report_panel_dead(void);
#endif
#endif
#if defined(CONFIG_LGE_TOUCH_CORE_MTK)
extern void mtkfb_esd_recovery(void);
#endif

static void touch_report_cancel_event(struct touch_core_data *ts)
{
	u16 old_mask = ts->old_mask;
	int i = 0;

	for (i = 0; i < MAX_FINGER; i++) {
		if (old_mask & (1 << i)) {
			input_mt_slot(ts->input, i);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER,
						   true);
			input_report_key(ts->input, BTN_TOUCH, 1);
			input_report_key(ts->input, BTN_TOOL_FINGER, 1);
			input_report_abs(ts->input, ABS_MT_PRESSURE,
					255);
			TOUCH_I("finger canceled:<%d>(%4d,%4d,%4d)\n",
					i,
					ts->tdata[i].x,
					ts->tdata[i].y,
					ts->tdata[i].pressure);
		}
	}

	input_sync(ts->input);
}

static void touch_report_event(struct touch_core_data *ts)
{
	u16 old_mask = ts->old_mask;
	u16 new_mask = ts->new_mask;
	u16 press_mask = 0;
	u16 release_mask = 0;
	u16 change_mask = 0;
	int i;

	TOUCH_TRACE();

	change_mask = old_mask ^ new_mask;
	press_mask = new_mask & change_mask;
	release_mask = old_mask & change_mask;

	TOUCH_D(ABS, "mask [new: %04x, old: %04x]\n",
			new_mask, old_mask);
	TOUCH_D(ABS, "mask [change: %04x, press: %04x, release: %04x]\n",
			change_mask, press_mask, release_mask);

	/* Palm state - Report Pressure value 255 */
	if (ts->is_cancel) {
		touch_report_cancel_event(ts);
		ts->is_cancel = 0;
	}

	for (i = 0; i < MAX_FINGER; i++) {
		if (new_mask & (1 << i)) {
			input_mt_slot(ts->input, i);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER,
						   true);
			input_report_key(ts->input, BTN_TOUCH, 1);
			input_report_key(ts->input, BTN_TOOL_FINGER, 1);
			input_report_abs(ts->input, ABS_MT_TRACKING_ID, i);
			input_report_abs(ts->input, ABS_MT_POSITION_X,
					ts->tdata[i].x);
			input_report_abs(ts->input, ABS_MT_POSITION_Y,
					ts->tdata[i].y);
			input_report_abs(ts->input, ABS_MT_PRESSURE,
					ts->tdata[i].pressure);
			input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR,
					ts->tdata[i].width_major);
			input_report_abs(ts->input, ABS_MT_WIDTH_MINOR,
					ts->tdata[i].width_minor);
			input_report_abs(ts->input, ABS_MT_ORIENTATION,
					ts->tdata[i].orientation);
		} else if (release_mask & (1 << i)) {
			input_mt_slot(ts->input, i);
			//input_report_abs(ts->input, ABS_MT_TRACKING_ID, -1);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		}
	}

	if (!ts->tcount) {
		input_report_key(ts->input, BTN_TOUCH, 0);
		input_report_key(ts->input, BTN_TOOL_FINGER, 0);
	}

	ts->old_mask = new_mask;
	input_sync(ts->input);
}

void touch_report_all_event(struct touch_core_data *ts)
{
	ts->is_cancel = 1;
	if (ts->old_mask) {
		ts->new_mask = 0;
		touch_report_event(ts);
		ts->tcount = 0;
		memset(ts->tdata, 0, sizeof(struct touch_data) * MAX_FINGER);
	}
	ts->is_cancel = 0;
}

static void touch_core_initialize(struct touch_core_data *ts)
{
	/* lockscreen */
	touch_report_all_event(ts);
}

irqreturn_t touch_irq_handler(int irq, void *dev_id)
{
	struct touch_core_data *ts = (struct touch_core_data *) dev_id;

	TOUCH_TRACE();

	if (atomic_cmpxchg((&ts->state.pm), DEV_PM_SUSPEND, DEV_PM_SUSPEND_IRQ)
			>= DEV_PM_SUSPEND) {
		TOUCH_I("interrupt in suspend[%d]\n", atomic_read(&ts->state.pm));
		pm_wakeup_event(ts->dev, 1000);
		return IRQ_HANDLED;
    }
	return IRQ_WAKE_THREAD;
}

irqreturn_t touch_irq_thread(int irq, void *dev_id)
{
	struct touch_core_data *ts = (struct touch_core_data *) dev_id;
	int ret = 0;

	TOUCH_TRACE();

	pm_qos_update_request(&ts->pm_touch_req, 100);
	pm_qos_update_request(&ts->pm_spi_req, 100);
#if defined(CONFIG_SECURE_TOUCH)
	if (secure_touch_filter_interrupt(ts) == IRQ_HANDLED) {
	        pm_qos_update_request(&ts->pm_spi_req, PM_QOS_DEFAULT_VALUE);
	        pm_qos_update_request(&ts->pm_touch_req, PM_QOS_DEFAULT_VALUE);
		return IRQ_HANDLED;
	}
#endif
	mutex_lock(&ts->lock);

	ts->intr_status = 0;
	ret = ts->driver->irq_handler(ts->dev);

	if (ret >= 0) {
		if (ts->intr_status & TOUCH_IRQ_FINGER)
			touch_report_event(ts);

		if (ts->intr_status & TOUCH_IRQ_KNOCK)
			touch_send_uevent(ts, TOUCH_UEVENT_KNOCK);

		if (ts->intr_status & TOUCH_IRQ_PASSWD)
			touch_send_uevent(ts, TOUCH_UEVENT_PASSWD);

		if (ts->intr_status & TOUCH_IRQ_SWIPE_DOWN)
			touch_send_uevent(ts, TOUCH_UEVENT_SWIPE_DOWN);

		if (ts->intr_status & TOUCH_IRQ_SWIPE_UP)
			touch_send_uevent(ts, TOUCH_UEVENT_SWIPE_UP);

		if (ts->intr_status & TOUCH_IRQ_SWIPE_RIGHT)
			touch_send_uevent(ts, TOUCH_UEVENT_SWIPE_RIGHT);

		if (ts->intr_status & TOUCH_IRQ_SWIPE_LEFT)
			touch_send_uevent(ts, TOUCH_UEVENT_SWIPE_LEFT);

		if (ts->intr_status & TOUCH_IRQ_AI_BUTTON)
			touch_send_uevent(ts, TOUCH_UEVENT_AI_BUTTON);

		if (ts->intr_status & TOUCH_IRQ_AI_PICK)
			touch_send_uevent(ts, TOUCH_UEVENT_AI_PICK);

		if (ts->intr_status & TOUCH_IRQ_SWIPE_LEFT2)
			touch_send_uevent(ts, TOUCH_UEVENT_SWIPE_LEFT2);

		if (ts->intr_status & TOUCH_IRQ_SWIPE_RIGHT2)
			touch_send_uevent(ts, TOUCH_UEVENT_SWIPE_RIGHT2);
	} else {
		if (ret == -ERESTART) {
			TOUCH_I("IRQ - IC reset delay = %d\n",
				ts->caps.hw_reset_delay);
			touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
			ts->driver->power(ts->dev, POWER_OFF);
			ts->driver->power(ts->dev, POWER_ON);
			touch_msleep(ts->caps.hw_reset_delay);
			mod_delayed_work(ts->wq, &ts->init_work, 0);
		} else if (ret == -EGLOBALRESET) {
			queue_delayed_work(ts->wq, &ts->panel_reset_work, 0);
		} else if (ret == -EHWRESET) {
			ts->driver->power(ts->dev, POWER_HW_RESET);
		} else if (ret == -ESWRESET) {
			ts->driver->power(ts->dev, POWER_SW_RESET);
		} else if (ret == -EUPGRADE) {
			ts->force_fwup = 1;
			queue_delayed_work(ts->wq, &ts->upgrade_work, 0);
		}
	}

	mutex_unlock(&ts->lock);
	pm_qos_update_request(&ts->pm_spi_req, PM_QOS_DEFAULT_VALUE);
        pm_qos_update_request(&ts->pm_touch_req, PM_QOS_DEFAULT_VALUE);
	return IRQ_HANDLED;
}

static void touch_init_work_func(struct work_struct *init_work)
{
	struct touch_core_data *ts =
		container_of(to_delayed_work(init_work),
				struct touch_core_data, init_work);

	TOUCH_TRACE();

	TOUCH_I("touch_ic_init Start\n");
	mutex_lock(&ts->lock);
	touch_core_initialize(ts);
	ts->driver->init(ts->dev);
	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
	mutex_unlock(&ts->lock);

	if (atomic_read(&ts->state.core) == CORE_PROBE) {
		queue_delayed_work(ts->wq, &ts->upgrade_work, 0);
		return;
	}

	atomic_set(&ts->state.core, CORE_NORMAL);
	TOUCH_I("touch_ic_init End\n");
}

static void touch_upgrade_work_func(struct work_struct *upgrade_work)
{
	struct touch_core_data *ts =
		container_of(to_delayed_work(upgrade_work),
				struct touch_core_data, upgrade_work);
	int ret;

	TOUCH_TRACE();

	atomic_set(&ts->state.core, CORE_UPGRADE);
	mutex_lock(&ts->lock);
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
	ret = ts->driver->upgrade(ts->dev);

	/* init force_upgrade */
	ts->force_fwup = 0;
	ts->test_fwpath[0] = '\0';

	if (ret < 0) {
		TOUCH_I("There is no need to reset (ret: %d)\n", ret);
		mutex_unlock(&ts->lock);
		mod_delayed_work(ts->wq, &ts->init_work, 0);
		return;
	}

	ts->driver->power(ts->dev, POWER_OFF);
	ts->driver->power(ts->dev, POWER_ON);
	touch_msleep(ts->caps.hw_reset_delay);
	mutex_unlock(&ts->lock);

	mod_delayed_work(ts->wq, &ts->init_work, 0);
}

static void touch_fb_work_func(struct work_struct *fb_work)
{
	struct touch_core_data *ts =
			container_of(to_delayed_work(fb_work),
				struct touch_core_data, fb_work);

	if(atomic_read(&ts->state.fb) == FB_SUSPEND)
		touch_suspend(ts->dev);
	else if(atomic_read(&ts->state.fb) == FB_RESUME)
		touch_resume(ts->dev);
}

static void touch_panel_reset_work_func(struct work_struct *panel_reset_work)
{
	TOUCH_I("Request panel reset !!!\n");

#if defined(CONFIG_LGE_TOUCH_CORE_QCT)
#if IS_ENABLED(CONFIG_LGE_DISPLAY_RECOVERY_ESD) || IS_ENABLED(CONFIG_LGE_TOUCH_PANEL_GLOBAL_RESET)
	lge_mdss_report_panel_dead();
#endif
#endif

#if defined(CONFIG_LGE_TOUCH_CORE_MTK)
	mtkfb_esd_recovery();
#endif

}

static int touch_check_driver_function(struct touch_core_data *ts)
{
	if (ts->driver->probe &&
			ts->driver->remove &&
			ts->driver->suspend &&
			ts->driver->resume &&
			ts->driver->init &&
			ts->driver->irq_handler &&
			ts->driver->power &&
			ts->driver->upgrade)
		return 0;

	return -EPERM;
}

static int touch_init_platform_data(struct touch_core_data *ts)
{
	int ret;

	ret = touch_check_driver_function(ts);

	if (ret) {
		TOUCH_E("failed to check functions\n");
		return ret;
	}

	if (ts->dev->of_node)
		ret = touch_get_dts(ts);
	else
		ret = touch_get_platform_data(ts);

	if (ret)
		TOUCH_E("failed to get platform data\n");

	return ret;
}

static void touch_init_locks(struct touch_core_data *ts)
{
	mutex_init(&ts->lock);
	device_init_wakeup(ts->dev, true);
}

static int touch_init_input(struct touch_core_data *ts)
{
	struct input_dev *input;
	int ret;

	input = input_allocate_device();

	if (!input) {
		TOUCH_E("failed to allocate memory for input\n");
		return -ENOMEM;
	}

	input->name = "touch_dev";

	TOUCH_I("%s %d-%d-%d-%d-%d-%d-%d\n", __func__,
			ts->caps.max_x,
			ts->caps.max_y,
			ts->caps.max_pressure,
			ts->caps.max_width_major,
			ts->caps.max_width_minor,
			ts->caps.max_orientation,
			ts->caps.max_id);
	set_bit(EV_SYN, input->evbit);
	set_bit(EV_ABS, input->evbit);
	set_bit(EV_KEY, input->evbit);
	set_bit(BTN_TOUCH, input->keybit);
	set_bit(BTN_TOOL_FINGER, input->keybit);
	set_bit(KEY_WAKEUP, input->keybit);
	set_bit(KEY_GESTURE_SWIPE_UP, input->keybit);
	set_bit(KEY_GESTURE_SWIPE_DOWN, input->keybit);
	set_bit(KEY_GESTURE_SWIPE_LEFT, input->keybit);
	set_bit(KEY_GESTURE_SWIPE_RIGHT, input->keybit);
	set_bit(KEY_GESTURE_SWIPE_LEFT2, input->keybit);
	set_bit(KEY_GESTURE_SWIPE_RIGHT2, input->keybit);
	set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0,
			ts->caps.max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
			ts->caps.max_y, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0,
			ts->caps.max_pressure, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0,
			ts->caps.max_width_major, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MINOR, 0,
			ts->caps.max_width_minor, 0, 0);
	input_set_abs_params(input, ABS_MT_ORIENTATION, 0,
			ts->caps.max_orientation, 0, 0);

	ret = input_mt_init_slots(input, ts->caps.max_id, 0);

	if (ret < 0) {
		TOUCH_E("failed to initialize input device(ret:%d)\n", ret);
		goto error;
	}

	ret = input_register_device(input);

	if (ret < 0) {
		TOUCH_E("failed to register input device(ret:%d)\n", ret);
		goto error_register;
	}

	input_set_drvdata(input, ts);
	ts->input = input;

	return 0;

error_register:
	input_mt_destroy_slots(input);

error:
	input_free_device(input);

	return ret;
}

extern int tap2wake_status;
extern int lpwg_status;

void touch_suspend(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	TOUCH_TRACE();

	TOUCH_I("%s Start\n", __func__);
#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled))
		secure_touch_stop(ts, true);
#endif
	cancel_delayed_work_sync(&ts->init_work);
	cancel_delayed_work_sync(&ts->upgrade_work);
	atomic_set(&ts->state.uevent, UEVENT_IDLE);
	mutex_lock(&ts->lock);
	touch_report_all_event(ts);
	atomic_set(&ts->state.fb, FB_SUSPEND);
	/* if need skip, return value is not 0 in pre_suspend */
	ret = ts->driver->suspend(dev);
	mutex_unlock(&ts->lock);

	if (ts->driver->lpwg) {
	    int tap2wake_knocked[4] = { 0, 0, 1, 0 };
	    tap2wake_knocked[0] = tap2wake_status;
		mutex_lock(&ts->lock);
		TOUCH_I("tap2wake %s\n", (tap2wake_status) ? "Enabled" : "Disabled");
		ts->driver->lpwg(ts->dev, LPWG_MASTER, tap2wake_knocked);
		lpwg_status = tap2wake_status;
		mutex_unlock(&ts->lock);
	}

	TOUCH_I("%s End\n", __func__);

	if (ret == 1)
		mod_delayed_work(ts->wq, &ts->init_work, 0);
}

void touch_resume(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	TOUCH_TRACE();

	TOUCH_I("%s Start\n", __func__);
#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled))
		secure_touch_stop(ts, true);
#endif
	mutex_lock(&ts->lock);
	atomic_set(&ts->state.fb, FB_RESUME);
	/* if need skip, return value is not 0 in pre_resume */
	ret = ts->driver->resume(dev);
	mutex_unlock(&ts->lock);

	if (ts->driver->lpwg) {
	    int tap2wake_knocked[4] = { 0, 1, 1, 0 };
	    tap2wake_knocked[0] = tap2wake_status;
		mutex_lock(&ts->lock);
		TOUCH_I("tap2wake %s\n", (tap2wake_status) ? "Enabled" : "Disabled");
		ts->driver->lpwg(ts->dev, LPWG_MASTER, tap2wake_knocked);
		lpwg_status = tap2wake_status;
		mutex_unlock(&ts->lock);
	}

	TOUCH_I("%s End\n", __func__);

	if (ret == 0)
		mod_delayed_work(ts->wq, &ts->init_work, 0);
}

#if defined(CONFIG_HAS_EARLYSUSPEND)
static void touch_early_suspend(struct early_suspend *h)
{
	struct touch_core_data *ts =
		container_of(h, struct touch_core_data, early_suspend);
	TOUCH_TRACE();
	touch_suspend(ts->dev);
}

static void touch_early_resume(struct early_suspend *h)
{
	struct touch_core_data *ts =
		container_of(h, struct touch_core_data, early_suspend);
	TOUCH_TRACE();
	touch_resume(ts->dev);
}

static int touch_init_pm(struct touch_core_data *ts)
{
	TOUCH_TRACE();
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 1;
	ts->early_suspend.suspend = touch_early_suspend;
	ts->early_suspend.resume = touch_early_resume;
	register_early_suspend(&ts->early_suspend);
	return 0;
}

#elif defined(CONFIG_DRM) && defined(CONFIG_FB)
static int touch_drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(self, struct touch_core_data, drm_notif);
	struct msm_drm_notifier *ev = (struct msm_drm_notifier *)data;

	if (ev && ev->data && event == MSM_DRM_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		if (*blank == MSM_DRM_BLANK_UNBLANK)
			touch_resume(ts->dev);
		else if(*blank == MSM_DRM_BLANK_POWERDOWN)
			touch_suspend(ts->dev);
	}

	return 0;
}

static int touch_init_pm(struct touch_core_data *ts)
{
	TOUCH_TRACE();

	ts->drm_notif.notifier_call = touch_drm_notifier_callback;
	return msm_drm_register_client(&ts->drm_notif);
}

#elif defined(CONFIG_FB)
static void touch_pm_worker(struct work_struct *work)
{
	struct touch_core_data *ts = container_of(work, typeof(*ts), pm_work);

	if (ts->screen_off)
		touch_suspend(ts->dev);
	else
		touch_resume(ts->dev);
}

static int touch_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(self, struct touch_core_data, fb_notif);
	struct fb_event *ev = (struct fb_event *)data;

	if (ev && ev->data && event == FB_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		flush_work(&ts->pm_work);
		ts->screen_off = *blank != FB_BLANK_UNBLANK;
		schedule_work(&ts->pm_work);
	}

	return 0;
}

static int touch_init_pm(struct touch_core_data *ts)
{
	TOUCH_TRACE();

	INIT_WORK(&ts->pm_work, touch_pm_worker);
	ts->fb_notif.notifier_call = touch_fb_notifier_callback;
	return fb_register_client(&ts->fb_notif);
}
#endif

static struct bus_type touch_uevent_subsys = {
	.name = LGE_TOUCH_NAME,
	.dev_name = LGE_TOUCH_NAME,
};

static struct device device_uevent_touch = {
	.id    = 0,
	.bus   = &touch_uevent_subsys,
};

static int touch_init_uevent(struct touch_core_data *ts)
{
	int ret = 0;

	ret = subsys_system_register(&touch_uevent_subsys, NULL);
	if (ret < 0)
		TOUCH_E("%s, bus is not registered, ret : %d\n",
				__func__, ret);
	ret = device_register(&device_uevent_touch);
	if (ret < 0)
		TOUCH_E("%s, device is not registered, ret : %d\n",
				__func__, ret);

	return ret;
}

char *uevent_str[TOUCH_UEVENT_SIZE][2] = {
	{"TOUCH_GESTURE_WAKEUP=WAKEUP", NULL},
	{"TOUCH_GESTURE_WAKEUP=PASSWORD", NULL},
	{"TOUCH_GESTURE_WAKEUP=SWIPE_DOWN", NULL},
	{"TOUCH_GESTURE_WAKEUP=SWIPE_UP", NULL},
	{"TOUCH_GESTURE_WAKEUP=SWIPE_RIGHT", NULL},
	{"TOUCH_GESTURE_WAKEUP=SWIPE_LEFT", NULL},
	{"TOUCH_GESTURE_WAKEUP=SWIPE_RIGHT2", NULL},
	{"TOUCH_GESTURE_WAKEUP=SWIPE_LEFT2", NULL},
	{"TOUCH_GESTURE_WAKEUP=AI_BUTTON", NULL},
	{"TOUCH_GESTURE_WAKEUP=AI_PICK", NULL}
};

static void touch_send_uevent(struct touch_core_data *ts, int type)
{
	TOUCH_TRACE();
	if (atomic_read(&ts->state.uevent) == UEVENT_IDLE ||
			touch_boot_mode_check(ts->dev) != NORMAL_BOOT) {
		pm_wakeup_event(ts->dev, 3000);
		atomic_set(&ts->state.uevent, UEVENT_BUSY);
		kobject_uevent_env(&device_uevent_touch.kobj,
				KOBJ_CHANGE, uevent_str[type]);
		TOUCH_I("%s\n",  uevent_str[type][0]);
		touch_report_all_event(ts);
	}
	switch (type) {
		case TOUCH_UEVENT_KNOCK:
			input_report_key(ts->input, KEY_WAKEUP, 1);
			TOUCH_I("Simulate power button depress\n");
			input_sync(ts->input);
			input_report_key(ts->input, KEY_WAKEUP, 0);
			TOUCH_I("Simulate power button release\n");
			input_sync(ts->input);
			break;
		case TOUCH_UEVENT_SWIPE_DOWN:
			TOUCH_I("Swipe DOWN reported\n");
			input_report_key(ts->input, KEY_GESTURE_SWIPE_DOWN, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_GESTURE_SWIPE_DOWN, 0);
			input_sync(ts->input);
			break;
		case TOUCH_UEVENT_SWIPE_UP:
			TOUCH_I("Swipe UP reported\n");
			input_report_key(ts->input, KEY_GESTURE_SWIPE_UP, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_GESTURE_SWIPE_UP, 0);
			input_sync(ts->input);
			break;
		case TOUCH_UEVENT_SWIPE_LEFT:
			TOUCH_I("Swipe LEFT reported\n");
			input_report_key(ts->input, KEY_GESTURE_SWIPE_LEFT, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_GESTURE_SWIPE_LEFT, 0);
			input_sync(ts->input);
			break;
		case TOUCH_UEVENT_SWIPE_RIGHT:
			TOUCH_I("Swipe RIGHT reported\n");
			input_report_key(ts->input, KEY_GESTURE_SWIPE_RIGHT, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_GESTURE_SWIPE_RIGHT, 0);
			input_sync(ts->input);
			break;
		case TOUCH_UEVENT_SWIPE_LEFT2:
			TOUCH_I("Swipe LEFT2 reported\n");
			input_report_key(ts->input, KEY_GESTURE_SWIPE_LEFT2, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_GESTURE_SWIPE_LEFT2, 0);
			input_sync(ts->input);
			break;
		case TOUCH_UEVENT_SWIPE_RIGHT2:
			TOUCH_I("Swipe RIGHT2 reported\n");
			input_report_key(ts->input, KEY_GESTURE_SWIPE_RIGHT2, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_GESTURE_SWIPE_RIGHT2, 0);
			input_sync(ts->input);
			break;
		default:
			break;
	}
}

void touch_notify_connect(u32 type)
{
	touch_atomic_notifier_call(NOTIFY_CONNECTION, &type);
}
EXPORT_SYMBOL(touch_notify_connect);

void touch_notify_wireless(u32 type)
{
	touch_atomic_notifier_call(NOTIFY_WIRELEES, &type);
}

EXPORT_SYMBOL(touch_notify_wireless);

void touch_notify_earjack(u32 type)
{
	touch_atomic_notifier_call(NOTIFY_EARJACK, &type);
}

EXPORT_SYMBOL(touch_notify_earjack);

static int touch_notify(struct touch_core_data *ts,
				   unsigned long event, void *data)
{
	int ret = 0;

	if (touch_boot_mode() == TOUCH_CHARGER_MODE)
		return 0;

	if (ts->driver->notify) {
		mutex_lock(&ts->lock);
		switch (event) {
		case NOTIFY_TOUCH_RESET:
			ret = ts->driver->notify(ts->dev, event, data);
			break;

		case NOTIFY_CONNECTION:
			if (atomic_read(&ts->state.connect) != *(int *)data) {
				atomic_set(&ts->state.connect, *(int *)data);
				ret = ts->driver->notify(ts->dev, event, data);
			}
			break;

		case NOTIFY_WIRELEES:
			atomic_set(&ts->state.wireless, *(int *)data);
			ret = ts->driver->notify(ts->dev, event, data);
			break;

		case NOTIFY_FB:
			atomic_set(&ts->state.fb, *(int *)data);
			queue_delayed_work(ts->wq, &ts->fb_work, 0);
			break;

		case NOTIFY_EARJACK:
			atomic_set(&ts->state.earjack, *(int *)data);
			ret = ts->driver->notify(ts->dev, event, data);
			break;

		default:
			ret = ts->driver->notify(ts->dev, event, data);
			break;
		}
		mutex_unlock(&ts->lock);
	}

	if (ret == LCD_EVENT_LCD_MODE && event == LCD_EVENT_LCD_MODE) {
		if (atomic_read(&ts->state.fb) == FB_SUSPEND)
			touch_suspend(ts->dev);
		else if(atomic_read(&ts->state.fb) == FB_RESUME)
			touch_resume(ts->dev);
		ret = 0;
	}
	return ret;
}

static int display_notify(struct touch_core_data *ts,
				   unsigned long event, void *data)
{
	int ret = 0;
	u32 display_notify_data = 0;
	struct lge_panel_notifier *panel_data = data;

	if (touch_boot_mode() == TOUCH_CHARGER_MODE)
		return 0;

	if(atomic_read(&ts->state.core) == CORE_SHUTDOWN){//MSP-14546
		TOUCH_I("%s:skip in shutdonw state\n", __func__);
		return 0;
	}

	if (ts->driver->notify) {
		mutex_lock(&ts->lock);
		switch (event) {
		case LGE_PANEL_EVENT_BLANK:
			TOUCH_D(TRACE, "EVENT_BLANK : %d %d", panel_data->display_id,
					panel_data->state);
			if (panel_data->state == LGE_PANEL_STATE_UNBLANK)
				display_notify_data = 3;
			else if (panel_data->state == LGE_PANEL_STATE_BLANK)
				display_notify_data = 0;
			else
				display_notify_data = panel_data->state;
			ret = ts->driver->notify(ts->dev, LCD_EVENT_LCD_MODE,
					&display_notify_data);
			break;
		case LGE_PANEL_EVENT_RESET:
			TOUCH_D(TRACE, "EVENT_RESET : %d %d", panel_data->display_id,
					panel_data->state);
			if (panel_data->state == LGE_PANEL_RESET_HIGH)
				ret = ts->driver->notify(ts->dev, NOTIFY_TOUCH_RESET, data);
			break;
		case LGE_PANEL_EVENT_POWER:
			TOUCH_D(TRACE, "EVENT_POWER : %d %d", panel_data->display_id,
					panel_data->state);
			break;
		case LGE_PANEL_EVENT_RECOVERY:
			TOUCH_D(TRACE, "EVENT_RECOVERY : %d %d", panel_data->display_id,
					panel_data->state);
			if (panel_data->state == LGE_PANEL_RECOVERY_DEAD) {
				TOUCH_I("%s: Panel Recovery !!\n", __func__);
				touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
			}
			break;
		default:
			TOUCH_D(TRACE, "unknwon type : %ld %d %d\n", event,
					panel_data->display_id,
					panel_data->state);
			break;
		}
		mutex_unlock(&ts->lock);
	}

	return ret;
}

static int touch_blocking_notifier_callback(struct notifier_block *this,
				   unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(this, struct touch_core_data, blocking_notif);
	return touch_notify(ts, event, data);
}

static void touch_atomic_notifer_work_func(struct work_struct *notify_work)
{
	struct touch_core_data *ts =
		container_of(to_delayed_work(notify_work),
				struct touch_core_data, notify_work);
	struct atomic_notify_event *arr = ts->notify_event_arr;
	int i = 0;

	for (i = 0; i < ATOMIC_NOTIFY_EVENT_SIZE; i++) {
		if (arr[i].event != NO_EVENT) {
			touch_notify(ts, arr[i].event, &(arr[i].data));
			arr[i].event = NO_EVENT;
		}
	}
}

static int touch_atomic_notifier_callback(struct notifier_block *this,
				   unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(this, struct touch_core_data, atomic_notif);
	struct atomic_notify_event *arr = ts->notify_event_arr;

	switch (event) {
	case NOTIFY_CONNECTION:
		arr[ATOMIC_NOTIFY_CONNECTION].event = event;
		arr[ATOMIC_NOTIFY_CONNECTION].data = *(int *)data;
		break;
	case NOTIFY_WIRELEES:
		arr[ATOMIC_NOTIFY_WIRELESS].event = event;
		arr[ATOMIC_NOTIFY_WIRELESS].data = *(int *)data;
		break;
	case NOTIFY_EARJACK:
		arr[ATOMIC_NOTIFY_EARJACK].event = event;
		arr[ATOMIC_NOTIFY_EARJACK].data = *(int *)data;
		break;
	default:
		TOUCH_I("%s: unknown event(%lu)\n", __func__, event);
		return 0;
	}

	queue_delayed_work(ts->wq, &ts->notify_work, 0);

	return 0;
}

static int display_notifier_callback(struct notifier_block *this,
				   unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(this, struct touch_core_data, display_notif);
	return display_notify(ts, event, data);
}

static int touch_init_notify(struct touch_core_data *ts)
{
	int ret = 0;

	ts->blocking_notif.notifier_call = touch_blocking_notifier_callback;
	ret = touch_blocking_notifier_register(&ts->blocking_notif);

	if (ret < 0) {
		TOUCH_E("failed to regiseter touch blocking_notify callback\n");
	}

	ts->atomic_notif.notifier_call = touch_atomic_notifier_callback;
	ret = touch_atomic_notifier_register(&ts->atomic_notif);

	if (ret < 0) {
		TOUCH_E("failed to regiseter touch atomic_notify callback\n");
	}

	ts->display_notif.notifier_call = display_notifier_callback;
	ret = lge_panel_notifier_register_client(&ts->display_notif);

	if (ret < 0) {
		TOUCH_E("failed to regiseter lge_panel_notify callback\n");
	}

	return ret;
}

static int touch_init_works(struct touch_core_data *ts)
{
	ts->wq = create_singlethread_workqueue("touch_wq");

	if (!ts->wq) {
		TOUCH_E("failed to create workqueue\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&ts->init_work, touch_init_work_func);
	INIT_DELAYED_WORK(&ts->upgrade_work, touch_upgrade_work_func);
	INIT_DELAYED_WORK(&ts->notify_work, touch_atomic_notifer_work_func);
	INIT_DELAYED_WORK(&ts->fb_work, touch_fb_work_func);
	INIT_DELAYED_WORK(&ts->panel_reset_work, touch_panel_reset_work_func);

	return 0;
}

#if defined(CONFIG_SECURE_TOUCH)
void secure_touch_notify(struct touch_core_data *ts)
{
	TOUCH_I("secure_touch_notify\n");
	sysfs_notify(&ts->kobj, NULL, "secure_touch");
}

void secure_touch_init(struct touch_core_data *ts)
{
	ts->st_initialized = 0;
	init_completion(&ts->st_powerdown);
	init_completion(&ts->st_irq_processed);

	/* Get clocks */
	ts->core_clk = devm_clk_get(ts->pdev->dev.parent, "core_clk");
	if (IS_ERR(ts->core_clk)) {
		TOUCH_E("failed to get core_clk : %ld\n", PTR_ERR(ts->core_clk));
		ts->core_clk = NULL;
	}
	ts->iface_clk = devm_clk_get(ts->pdev->dev.parent, "iface_clk");
	if (IS_ERR(ts->iface_clk)) {
		TOUCH_E("failed to get iface_clk : %ld\n", PTR_ERR(ts->iface_clk));
		ts->iface_clk = NULL;
	}

	ts->st_initialized = 1;
}

static irqreturn_t secure_touch_filter_interrupt(struct touch_core_data *ts)
{
	if (atomic_read(&ts->st_enabled)) {
		if (atomic_cmpxchg(&ts->st_pending_irqs, 0, 1) == 0) {
			reinit_completion(&ts->st_irq_processed);
			secure_touch_notify(ts);
			wait_for_completion_interruptible(
					&ts->st_irq_processed);
		}
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

/*
 *   'blocking' variable will have value 'true' when we want to prevent the driver
 *   from accessing the xPU/SMMU protected HW resources while the session is
 *   active.
 */
void secure_touch_stop(struct touch_core_data *ts, bool blocking)
{
	TOUCH_I("secure_touch_stop : %d", blocking);
	if (atomic_read(&ts->st_enabled)) {
		atomic_set(&ts->st_pending_irqs, -1);
		secure_touch_notify(ts);
		if (blocking)
			wait_for_completion_interruptible(
					&ts->st_powerdown);
	}
}
#endif

static int touch_core_probe_normal(struct platform_device *pdev)
{
	struct touch_core_data *ts;
	int ret;

	TOUCH_TRACE();

	ts = (struct touch_core_data *) pdev->dev.platform_data;

	atomic_set(&ts->state.core, CORE_PROBE);
	ret = touch_init_platform_data(ts);
	if (ret) {
		TOUCH_E("failed to initialize platform_data\n");
		return -EINVAL;
	}

	ret = ts->driver->probe(ts->dev);
	if (ret) {
		TOUCH_E("failed to device probe\n");
		return ret;
	}

	/* set defalut lpwg value because of AAT */
	ts->lpwg.screen = 1;
	ts->lpwg.sensor = PROX_FAR;

	ts->driver->power(ts->dev, POWER_OFF);
	ts->driver->power(ts->dev, POWER_ON);

	touch_init_locks(ts);
	ret = touch_init_works(ts);
	if (ret) {
		TOUCH_E("failed to initialize works\n");
		goto error_init_work;
	}

	ret = touch_init_input(ts);
	if (ret) {
		TOUCH_E("failed to register input device(ret:%d)\n", ret);
		goto error_init_input;
	}

		ts->pm_spi_req.type = PM_QOS_REQ_AFFINE_IRQ;
		ts->pm_spi_req.irq = ts->irq;
		pm_qos_add_request(&ts->pm_spi_req, PM_QOS_CPU_DMA_LATENCY,
			PM_QOS_DEFAULT_VALUE);

		ts->pm_touch_req.type = PM_QOS_REQ_AFFINE_IRQ;
		ts->pm_touch_req.irq = ts->irq;
		pm_qos_add_request(&ts->pm_touch_req, PM_QOS_CPU_DMA_LATENCY,
				PM_QOS_DEFAULT_VALUE);


	ts->irqflags |= IRQF_ONESHOT | IRQF_PERF_CRITICAL;
	ret = touch_request_irq(ts->irq, touch_irq_handler, touch_irq_thread,
				ts->irqflags, LGE_TOUCH_NAME, ts);

	if (ret) {
		TOUCH_E("failed to request_thread_irq(irq:%d, ret:%d)\n",
				ts->irq, ret);
		goto error_request_irq;
	}

	touch_disable_irq(ts->irq);
	touch_init_pm(ts);

	touch_init_notify(ts);
	touch_blocking_notifier_call(LCD_EVENT_TOUCH_DRIVER_REGISTERED, NULL);

	ret = touch_init_uevent(ts);
	ret = touch_init_sysfs(ts);

	TOUCH_I("hw_reset_delay : %d ms\n", ts->caps.hw_reset_delay);
	queue_delayed_work(ts->wq, &ts->init_work,
			msecs_to_jiffies(ts->caps.hw_reset_delay));

	return 0;

error_request_irq:
	free_irq(ts->irq, ts);
	pm_qos_remove_request(&ts->pm_touch_req);
	pm_qos_remove_request(&ts->pm_spi_req);
error_init_input:
	if (ts->input) {
		input_mt_destroy_slots(ts->input);
		input_free_device(ts->input);
	}
error_init_work:
	if (ts->wq)
		destroy_workqueue(ts->wq);

	return ret;
}

static int touch_core_probe_charger(struct platform_device *pdev)
{
	struct touch_core_data *ts;
	int ret = 0;

	TOUCH_TRACE();

	ts = (struct touch_core_data *) pdev->dev.platform_data;

	ret = touch_init_platform_data(ts);
	if (ret) {
		TOUCH_E("failed to initialize platform_data\n");
		return -EINVAL;
	}

	ret = ts->driver->probe(ts->dev);
	if (ret) {
		TOUCH_E("failed to device probe\n");
		return ret;
	}

	touch_init_locks(ts);
	ret = touch_init_works(ts);
	if (ret) {
		TOUCH_E("failed to initialize works\n");
		return ret;
	}

	touch_init_pm(ts);
	touch_init_notify(ts);
	touch_blocking_notifier_call(LCD_EVENT_TOUCH_DRIVER_REGISTERED, NULL);

	return ret;
}

static int touch_core_probe(struct platform_device *pdev)
{
	if (touch_boot_mode() == TOUCH_CHARGER_MODE) {
		TOUCH_I("CHARGER MODE\n");
		return touch_core_probe_charger(pdev);
	}

	return touch_core_probe_normal(pdev);
}

static int touch_core_remove(struct platform_device *pdev)
{
	struct touch_core_data *ts;

	TOUCH_TRACE();

	ts = (struct touch_core_data *) pdev->dev.platform_data;

	TOUCH_I("%s\n", __func__);

#if defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&ts->early_suspend);
#elif defined(CONFIG_DRM) && defined(CONFIG_FB)
        msm_drm_unregister_client(&ts->drm_notif);
#elif defined(CONFIG_FB)
	fb_unregister_client(&ts->fb_notif);
#endif

	kobject_del(&ts->kobj);

	touch_atomic_notifier_unregister(&ts->atomic_notif);

	destroy_workqueue(ts->wq);
	device_init_wakeup(ts->dev, false);

	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
	free_irq(ts->irq, ts);

	input_unregister_device(ts->input);

	ts->driver->remove(ts->dev);
	ts->driver->power(ts->dev, POWER_OFF);

	devm_kfree(ts->dev, ts);

	return 0;
}

void touch_core_shutdown(struct platform_device *pdev)
{
	struct touch_core_data *ts;

	TOUCH_TRACE();

	ts = (struct touch_core_data *) pdev->dev.platform_data;

	TOUCH_I("%s\n", __func__);

	atomic_set(&ts->state.core, CORE_SHUTDOWN);//MSP-14546

#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		secure_touch_stop(ts, false);
		complete(&ts->st_irq_processed);
	}
#endif

	if (ts->dev == NULL)
		return;

	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
	free_irq(ts->irq, ts);

	if (ts->driver->shutdown)
		ts->driver->shutdown(ts->dev);
}

static struct platform_driver touch_core_driver = {
	.driver = {
		.name = LGE_TOUCH_DRIVER_NAME,
		.owner = THIS_MODULE,
/*
		.of_match_table = touch_match_ids,
*/
	},
	.probe = touch_core_probe,
	.remove = touch_core_remove,
	.shutdown = touch_core_shutdown,
};

static void touch_core_async_init(void *data, async_cookie_t cookie)
{
	int ret = platform_driver_register(&touch_core_driver);

	TOUCH_TRACE();

	if (ret)
		TOUCH_E("async_init failed\n");
}

static int __init touch_core_init(void)
{
	TOUCH_TRACE();
	async_schedule(touch_core_async_init, NULL);
	return 0;
}

static void __exit touch_core_exit(void)
{
	TOUCH_TRACE();
	platform_driver_unregister(&touch_core_driver);
}

module_init(touch_core_init);
module_exit(touch_core_exit);

MODULE_AUTHOR("hoyeon.jang@lge.com");
MODULE_DESCRIPTION("LGE touch driver v3");
MODULE_LICENSE("GPL");
