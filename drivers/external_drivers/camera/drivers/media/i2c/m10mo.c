/*
 * Copyright (c) 2014 Intel Corporation. All Rights Reserved.
 *
 * Partially based on m-5mols kernel driver,
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 *
 * Partially based on jc_v4l2 kernel driver from http://opensource.samsung.com
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/intel-mid.h>
#include <asm/irq.h>
#include <linux/atomisp_platform.h>
#include <linux/atomisp.h>
#include <media/m10mo_atomisp.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-chip-ident.h>
#include <media/v4l2-device.h>
#include <linux/m10mo.h>
#include <linux/HWVersion.h>
#include <linux/m10mo_workaround.h>
#include <asm/intel_scu_pmic.h>
#include <linux/switch.h>
#include <linux/proc_fs.h>

#define off_c51
#define PREVIEW_TEST_PATTERN 0
#define GPIO4CTLO_REG 0x82 //This REGISTER is for SKY81296 enable pin.
//#define NOIRQ
/* cross component debug message flag */
int dbglvl = 0;
static u8 cancel_zoom_protect;
static u32 last_optical_zoom_step = 1;
static u32 previous_preview_cmd;
int isp_update_status=1;
u8 m10mo_capture_pre_flag;
static bool isCaptureMode = false;
static bool openIntelISP = true;
u32 asus_camera_flag;
u8 m10mo_boot_state;
u8 m10mo_trace_log_lock = DOWNLOAD_PROTECT;
u32 fw_version_val1=0;
u32 fw_version_val2=0;
int DIT_version=0;
int ASUS_flag=0;
int ois_status = 0;
int num_LLS = 0;
struct v4l2_subdev *sdd = NULL;
module_param(dbglvl, int, 0644);
MODULE_PARM_DESC(dbglvl, "debug message on/off (default:off)");
int m10mo_send_still_capture_cmds(struct v4l2_subdev *sd);
static void m10mo_lens_state_machine(struct v4l2_subdev *sd, m10mo_state_machine_event_t event, u32* data_ptr);
static int m10mo_set_af_execution(struct v4l2_subdev *sd, s32 val);
static int m10mo_set_optical_zoom_position(struct v4l2_subdev *sd, u32 data);
static u32 get_m10mo_wait_timeout_val(struct v4l2_subdev *sd, u8 requested_cmd);

void m10mo_performance_log(const char *trace_log){
	struct timeval  tv;
	long int time_in_mill = 0;
	do_gettimeofday(&tv);
	time_in_mill = (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000 ;
	printk("_PFMS_%s:%ld\n",trace_log,time_in_mill);
}

#if 0
void notify_m10mo_atomisp_timeout(){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);
    m10mo_ispd4(dev);
}
EXPORT_SYMBOL(notify_m10mo_atomisp_timeout);
#endif
// = M10MO_START_AF;//M10MO_NO_CMD_REQUEST;
#define __m10mo_reset_irq_wait_status()                                           \
	 dev->cmd = dev->requested_cmd;                                               \
     wake_up(&dev->irq_waitq);                                      \
	 flush_workqueue(dev->lens_wq);                                               \
	 dev->cmd = dev->requested_cmd = M10MO_NO_CMD_REQUEST;                        \
	 dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE
////////////////////////////////////////////////////////////////////


static void optical_zoom_work_routine(struct work_struct *ws){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);
    u32 data;

	data = dev->m10mo_request_cmd_lens_data;
    printk(KERN_INFO "m10mo, @optical_zoom_work_routine before mutex lock, @m10mo_lens_work_lock, data is %d \n", data);
	mutex_lock(&dev->m10mo_lens_work_lock);
    printk(KERN_INFO "m10mo, @optical_zoom_work_routine get mutex lock, @m10mo_lens_work_lock\n");

    (void) m10mo_request_cmd_effect(sdd, M10MO_START_OPTICAL_ZOOM, &data);

	mutex_unlock(&dev->m10mo_lens_work_lock);
     printk(KERN_INFO "m10mo, @optical_zoom_work_routine mutex unlock, @m10mo_lens_work_lock\n");
}
static DECLARE_DELAYED_WORK(optical_zoom_work, optical_zoom_work_routine);

static void auto_focus_work_routine(struct work_struct *ws){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);

    printk(KERN_INFO "m10mo, @auto_focus_work_routine before mutex lock, @m10mo_lens_work_lock\n");
	mutex_lock(&dev->m10mo_lens_work_lock);
    printk(KERN_INFO "m10mo, @auto_focus_work_routine get mutex lock, @m10mo_lens_work_lock\n");

    (void) m10mo_request_cmd_effect(sdd, M10MO_START_AF, NULL);

	mutex_unlock(&dev->m10mo_lens_work_lock);
    printk(KERN_INFO "m10mo, @auto_focus_work_routine mutex unlock, @m10mo_lens_work_lock\n");
}
static DECLARE_DELAYED_WORK(auto_focus_work, auto_focus_work_routine);


//////////////////////////////////////////////////////////////////////////////////////////////////////
//#if 0
static void digital_zoom_work_routine(struct work_struct *ws){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);
	u32 data, ret;
	data = dev->m10mo_request_cmd_lens_data;
	printk(KERN_INFO "m10mo, digital_zoom_work_routine start\n");
	if(dev->lens_state_machine != LENS_IDLE) {
	    printk(KERN_INFO "m10mo, digital_zoom_work_routine, WAIT until lens is free.\n");
	    ret = wait_event_timeout(dev->d_zoom_waitq, dev->lens_state_machine == LENS_IDLE, msecs_to_jiffies(30000));
		if (ret == 0) {
		    printk(KERN_ERR "digital_zoom_work_routine, WAIT until lens is free, timed out\n");
			return;
		}
	}

    (void) m10mo_request_cmd_effect(sdd, M10MO_START_DIGITAL_ZOOM, &data);

	printk(KERN_INFO "m10mo, @digital_zoom_work_routine return! \n");
}
static DECLARE_DELAYED_WORK(digital_zoom_work, digital_zoom_work_routine);
//#endif
static struct switch_dev m10mo_switch_dev;
static ssize_t m10mo_switch_name(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%02x%02x-%05x,V%03x,%x\n", fw_version_val1, fw_version_val2,DIT_version, ene_fw_version,ASUS_flag);
    //return sprintf(buf, "%02x%02x-%05x ,%x\n", fw_version_val1, fw_version_val2,DIT_version ,ASUS_flag);
}

static void cancel_auto_focus_work_routine(struct work_struct *ws){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);

    printk(KERN_INFO "m10mo, @cancel_auto_focus_work_routine before mutex lock, @m10mo_lens_work_lock\n");
    mutex_lock(&dev->m10mo_lens_work_lock);
    printk(KERN_INFO "m10mo, @cancel_auto_focus_work_routine get mutex lock, @m10mo_lens_work_lock\n");

    (void) m10mo_request_cmd_effect(sdd, M10MO_ASUS_FOCUS_CANCEL, NULL);

	mutex_unlock(&dev->m10mo_lens_work_lock);
	printk(KERN_INFO "m10mo, @cancel_auto_focus_work_routine mutex unlock, @m10mo_lens_work_lock\n");
}
static DECLARE_DELAYED_WORK(cancel_auto_focus_work, cancel_auto_focus_work_routine);

static void cancel_optical_zoom_work_routine(struct work_struct *ws){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);

    printk(KERN_INFO "m10mo, @cancel_optical_zoom_work_routine before mutex lock, @m10mo_lens_work_lock\n");
    mutex_lock(&dev->m10mo_lens_work_lock);
    printk(KERN_INFO "m10mo, @cancel_optical_zoom_work_routine get mutex lock, @m10mo_lens_work_lock\n");

    (void) m10mo_request_cmd_effect(sdd, M10MO_ASUS_ZOOM_CANCEL, NULL);

	mutex_unlock(&dev->m10mo_lens_work_lock);
	cancel_zoom_protect = 0;
	printk(KERN_INFO "m10mo, @cancel_optical_zoom_work_routine mutex unlock, @m10mo_lens_work_lock\n");
}
static DECLARE_DELAYED_WORK(cancel_optical_zoom_work, cancel_optical_zoom_work_routine);

//=== OIS, S1 mode for Preview and video. This is the default mode. ===//
static void m10mo_set_ois_s1_work_routine(struct work_struct *ws){
//    (void) m10mo_writeb(sdd, CATEGORY_TEST, OIS_LIB_API_START, 0x5);
    (void) m10mo_writew(sdd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0107);
    (void) m10mo_writeb(sdd, CATEGORY_TEST, OIS_DATA31_24, 0x0);
	(void) m10mo_writeb(sdd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);

}
static DECLARE_DELAYED_WORK(m10mo_set_ois_s1_work, m10mo_set_ois_s1_work_routine);

//=== OIS, S2 mode for Capture and other usages. ===//
static void m10mo_set_ois_s2_work_routine(struct work_struct *ws){
//    (void) m10mo_writeb(sdd, CATEGORY_TEST, OIS_LIB_API_START, 0x5);
    (void) m10mo_writew(sdd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0107);
    (void) m10mo_writeb(sdd, CATEGORY_TEST, OIS_DATA31_24, 0x11);
    (void) m10mo_writeb(sdd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);
}
static DECLARE_DELAYED_WORK(m10mo_set_ois_s2_work, m10mo_set_ois_s2_work_routine);

//static DECLARE_DELAYED_WORK(optical_zoom_work, lens_work_routine);
//static DECLARE_DELAYED_WORK(auto_focus_work, lens_work_routine);
static void m10mo_lens_state_machine(struct v4l2_subdev *sd, m10mo_state_machine_event_t event, u32* data_ptr)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	static s32 af_data;
	static u32 optical_zoom_data;
	u32 read_val = 0;
	u32 ret;
///////////////////////////////////
//mutex_lock(&dev->m10mo_lens_state_machine_lock);
//////////////////////////////////
    printk("@%s %d LENES state machine is %d , event = %d \n", __func__, __LINE__, dev->lens_state_machine, event);
	if(data_ptr != NULL){
	    printk("@%s %d, data = %d \n", __func__, __LINE__, *data_ptr);
	}else {
	    printk("@%s %d, No data in. \n", __func__, __LINE__);
	}


	if(data_ptr != NULL){
	    if(event == AUTO_FOCUS_EVENT){
            af_data = *data_ptr;
		}else if(event == OPTICAL_ZOOM_EVENT){
            optical_zoom_data = *data_ptr;
		}
	}

	switch (dev->lens_state_machine) {
	case LENS_IDLE:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = FOCUSING;
		    (void) m10mo_set_af_execution(sd, af_data);

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &read_val);
            if(read_val == optical_zoom_data){
	             pr_info("m10mo, get optical-zoom step is %d, set optical-zoom data is %d. No need to set\n", read_val, optical_zoom_data);
				 dev->lens_state_machine = LENS_IDLE;
	             goto m10mo_lens_state_machine_out;
            }
			dev->lens_state_machine = ZOOMING;
		    (void) m10mo_set_optical_zoom_position(sd, optical_zoom_data);
		}
	    break;
    case FOCUSING:
	    if(event == AUTO_FOCUS_EVENT) {
			dev->lens_state_machine = FOCUS_CANCELING_WOZ_WF;
			dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();
			printk("@%s %d, 006 Before queue_delayed_work(dev->cancel_auto_focus_wq). \n", __func__, __LINE__);
			queue_delayed_work(dev->cancel_auto_focus_wq, &cancel_auto_focus_work, 0);

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WZ_WOF;
			dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();
			printk("@%s %d, 005 Before queue_delayed_work(dev->cancel_auto_focus_wq). \n", __func__, __LINE__);
			queue_delayed_work(dev->cancel_auto_focus_wq, &cancel_auto_focus_work, 0);

		} else if (event == M10MO_IRQ_EVENT) {
		    dev->lens_state_machine = LENS_IDLE;
		}
	    break;
	case FOCUS_CANCELING_WZ_WOF:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WZ_WF;

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WZ_WF;

		}else if (event == M10MO_IRQ_EVENT) {
		    dev->lens_state_machine = LENS_IDLE;
		    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &read_val);
            if(read_val == optical_zoom_data){
                  pr_info("m10mo, get optical-zoom step is %d, set optical-zoom data is %d. No need to set\n", read_val, optical_zoom_data);
                  goto m10mo_lens_state_machine_out;
            }
		    dev->lens_state_machine = ZOOMING;
            (void) m10mo_set_optical_zoom_position(sd, optical_zoom_data);
		}
        break;
	case FOCUS_CANCELING_WOZ_WF:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WOZ_WF;

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WZ_WOF;

		}else if (event == M10MO_IRQ_EVENT) {
	         dev->lens_state_machine = FOCUSING;
		    (void) m10mo_set_af_execution(sd, af_data);
		}
	    break;
	case FOCUS_CANCELING_WZ_WF:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WZ_WF;

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = FOCUS_CANCELING_WZ_WOF;

		}else if (event == M10MO_IRQ_EVENT) {
		    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &read_val);
            if(read_val == optical_zoom_data){
                 pr_info("m10mo, get optical-zoom step is %d, set optical-zoom data is %d. No need to set\n", read_val, optical_zoom_data);
				 dev->lens_state_machine = FOCUSING;
				 (void) m10mo_set_af_execution(sd, af_data);
                 goto m10mo_lens_state_machine_out;
            }
		    (void) m10mo_set_optical_zoom_position(sd, optical_zoom_data);
		    dev->lens_state_machine = ZOOMING_WF;
		}
	    break;
    case ZOOMING:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = ZOOMING_WF;

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = ZOOM_CANCELING_WZ_WOF;
			dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();
			printk("@%s %d, 000 Before queue_delayed_work(dev->cancel_optical_zoom_wq). \n", __func__, __LINE__);
			cancel_zoom_protect = 1;
			queue_delayed_work(dev->cancel_optical_zoom_wq, &cancel_optical_zoom_work, 0);

		}else if (event == M10MO_IRQ_EVENT) {
		   dev->lens_state_machine = LENS_IDLE;
		}
	    break;
	case ZOOMING_WF:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = ZOOMING_WF;

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = ZOOM_CANCELING_WZ_WOF;
			dev->cancel_irq_flag = 1;
            __m10mo_reset_irq_wait_status();
			printk("@%s %d, 001 Before queue_delayed_work(dev->cancel_optical_zoom_wq). \n", __func__, __LINE__);
			cancel_zoom_protect = 1;
			queue_delayed_work(dev->cancel_optical_zoom_wq, &cancel_optical_zoom_work, 0);

		}else if (event == M10MO_IRQ_EVENT) {
		   dev->lens_state_machine = FOCUSING;
		   (void) m10mo_set_af_execution(sd, af_data);
		}
	    break;
	case ZOOM_CANCELING_WZ_WOF:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = ZOOM_CANCELING_WZ_WF;

		} else if (event == OPTICAL_ZOOM) {
		    dev->lens_state_machine = ZOOM_CANCELING_WZ_WOF;

		}else if (event == M10MO_IRQ_EVENT) {
		    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &read_val);
            if(read_val == last_optical_zoom_step){
                dev->lens_state_machine = LENS_IDLE;
                pr_info("m10mo, get zoom step is %d, set zoom data is %d. No need to set\n", read_val, last_optical_zoom_step);
				goto m10mo_lens_state_machine_out;
            }
			dev->lens_state_machine = ZOOMING;
		    (void) m10mo_set_optical_zoom_position(sd, last_optical_zoom_step);
		}
	    break;
	case ZOOM_CANCELING_WZ_WF:
	    if(event == AUTO_FOCUS_EVENT) {
		    dev->lens_state_machine = ZOOM_CANCELING_WZ_WF;

		} else if (event == OPTICAL_ZOOM_EVENT) {
		    dev->lens_state_machine = ZOOM_CANCELING_WZ_WOF;

		}else if (event == M10MO_IRQ_EVENT) {
		    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &read_val);
            if(read_val == last_optical_zoom_step){
			     dev->lens_state_machine = FOCUSING;
                 pr_info("m10mo, get optical-zoom step is %d, set optical-zoom data is %d. No need to set\n", read_val, last_optical_zoom_step);
                 (void) m10mo_set_af_execution(sd, af_data);
				 goto m10mo_lens_state_machine_out;
            }
		    (void) m10mo_set_optical_zoom_position(sd, last_optical_zoom_step);
		    dev->lens_state_machine = ZOOMING_WF;
		}
	    break;
    default:
	    dev->lens_state_machine = LENS_IDLE;
		break;

    }
m10mo_lens_state_machine_out:
    if(dev->lens_state_machine == LENS_IDLE) {
        wake_up(&dev->d_zoom_waitq);
	}
//    mutex_unlock(&dev->m10mo_lens_state_machine_lock);
	return;
}

/*
 * m10mo_read -  I2C read function
 * @reg: combination of size, category and command for the I2C packet
 * @size: desired size of I2C packet
 * @val: read value
 *
 * Returns 0 on success, or else negative errno.
*/

int getOpenIntelISP(void){
    return openIntelISP;
}

EXPORT_SYMBOL(getOpenIntelISP);

void setOpenIntelISP(bool b){

    openIntelISP = b;
}

EXPORT_SYMBOL(setOpenIntelISP);

void m10mo_startCapture(void){
    struct m10mo_device *dev = to_m10mo_sensor(sdd);
    u32 val;
	if(dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_BURST){
	    printk("m10mo, @%s. During Burst mode, not called by atomisp driver. \n", __func__);
		return;
	}
	printk("m10mo, @%s, Wait start! called by atomisp driver. \n", __func__);

    val = get_m10mo_wait_timeout_val(sdd, dev->requested_cmd);
    (void) m10mo_wait_mode_change(sdd, dev->requested_cmd, val);
	dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE;
	printk("m10mo, @%s, Wait End!  \n", __func__);
    mutex_unlock(&dev->m10mo_request_cmd_lock);
    printk("@%s %d, UNLOCK m10mo_request_cmd_lock.\n", __func__, __LINE__);
    m10mo_performance_log("CAPTURE_B_KSIGCAP-");
}

EXPORT_SYMBOL(m10mo_startCapture);

void notify_m10mo_atomisp_dead(void) {
    struct m10mo_device *dev = to_m10mo_sensor(sdd);
	u8 previous_state;
	if(m10mo_trace_log_lock != DOWNLOAD_AVAILABLE) {
	      return;
	}
	if(dev->power == 1){
	      previous_state = m10mo_trace_log_lock;
	      dev->lock_i2c_write_flag = 0;
		  m10mo_trace_log_lock = DOWNLOADING;
          m10mo_ispd4(dev);
		  m10mo_trace_log_lock = previous_state;
	}
}
EXPORT_SYMBOL(notify_m10mo_atomisp_dead);

static int m10mo_read(struct v4l2_subdev *sd, u8 len, u8 category, u8 reg, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	unsigned char data[5];
	struct i2c_msg msg[2];
	unsigned char recv_data[len + 1];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	if (len != 1 && len != 2 && len != 4)	{
		dev_err(&client->dev, "Wrong data size\n");
		return -EINVAL;
	}

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = sizeof(data);
	msg[0].buf = data;

	data[0] = 5;
	data[1] = M10MO_BYTE_READ;
	data[2] = category;
	data[3] = reg;
        data[4] = len;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len + 1;
	msg[1].buf = recv_data;

	/* isp firmware becomes stable during this time*/
	usleep_range(200, 200);

	ret = i2c_transfer(client->adapter, msg, 2);

	if (ret == 2) {
		if (len == 0x01)
			*val = recv_data[1];
		else if (len == 0x02)
			*val = recv_data[1] << 8 | recv_data[2];
		else
			*val = recv_data[1] << 24 | recv_data[2] << 16
				| recv_data[3] << 8 | recv_data[4];
	}

	printk("%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n", __func__, len, category, reg, *val);
	dev_dbg(&client->dev,
		"%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n",
		__func__, len, category, reg, *val);

	return (ret == 2) ? 0 : -EIO;
}

/**
 * m10mo_write - I2C command write function
 * @reg: combination of size, category and command for the I2C packet
 * @val: value to write
 *
 * Returns 0 on success, or else negative errno.
 */
static int m10mo_write(struct v4l2_subdev *sd, u8 len, u8 category, u8 reg, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	u8 data[len + 4];
	struct i2c_msg msg;
	int ret;
	const int num_msg = 1;
	if (!client->adapter)
		return -ENODEV;

	if (len != 1 && len != 2 && len != 4) {
		dev_err(&client->dev, "Wrong data size\n");
		return -EINVAL;
	}

	dev_dbg(&client->dev,
		"%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n",
		__func__, len, category, reg, val);

	if(dev->lock_i2c_write_flag) {
	     printk("%s After we do ZSL capture, ILLIGAL to send i2c cmds until preview streaming\n", __func__);
		 return -EINVAL;
	}
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = sizeof(data);
	msg.buf = data;

	data[0] = msg.len;
	data[1] = M10MO_BYTE_WRITE;
	data[2] = category;
	data[3] = reg;
	switch (len) {
	case 1:
		data[4] = val;
		break;
	case 2:
		data[4] = ((val >> 8) & 0xFF);
		data[5] = (val & 0xFF);
		break;
	case 4:
		data[4] = ((val >> 24) & 0xFF);
		data[5] = ((val >> 16) & 0xFF);
		data[6] = ((val >> 8) & 0xFF);
		data[7] = (val & 0xFF);
		break;
	default:
		/* No possible to happen - len is already validated */
		break;
	}

	/* isp firmware becomes stable during this time*/
	usleep_range(200, 200);

	ret = i2c_transfer(client->adapter, &msg, 1);

	printk("@%s, Write reg. len = %d, Category=0x%02X Reg=0x%02X Value=0x%X ret=%s\n", __func__,
		len, category, reg, val, (ret == num_msg) ? "OK" : "Error");

	return ret == num_msg ? 0 : -EIO;
}

int m10mo_writeb(struct v4l2_subdev *sd, u8 category, u8 reg, u32 val)
{
	return m10mo_write(sd, 1, category, reg, val);
}

int m10mo_writew(struct v4l2_subdev *sd, u8 category, u8 reg, u32 val)
{
	return m10mo_write(sd, 2, category, reg, val);
}

int m10mo_writel(struct v4l2_subdev *sd, u8 category, u8 reg, u32 val)
{
	return m10mo_write(sd, 4, category, reg, val);
}

int m10mo_readb(struct v4l2_subdev *sd, u8 category, u8 reg, u32 *val)
{
	return m10mo_read(sd, 1, category, reg, val);
}

int m10mo_readw(struct v4l2_subdev *sd, u8 category, u8 reg, u32 *val)
{
	return m10mo_read(sd, 2, category, reg, val);
}

int m10mo_readl(struct v4l2_subdev *sd, u8 category, u8 reg, u32 *val)
{
	return m10mo_read(sd, 4, category, reg, val);
}

int m10mo_memory_write(struct v4l2_subdev *sd, u8 cmd, u16 len, u32 addr, u8 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_device *m10mo_dev =  to_m10mo_sensor(sd);
	struct i2c_msg msg;
	u8 *data = m10mo_dev->message_buffer;
	int i, ret;

	if(len==1)
	    dev_err(&client->dev, "Write mem. cmd=0x%02X len=%d addr=0x%X val=0x%X\n", cmd, len, addr, *val);
	else{
	   	dev_dbg(&client->dev, "Write mem. cmd=0x%02X len=%d addr=0x%X\n", cmd, len, addr);
	    for(i=0;i<len;i++){
	   	dev_dbg(&client->dev, "i=%d val=0x%X  ",i, val[i]);
	    }
	}

	if (!client->adapter)
		return -ENODEV;

	if ((len + 8) > sizeof(m10mo_dev->message_buffer))
		return -ENOMEM;
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len + 8;
	msg.buf = data;

	/* high byte goes out first */
	data[0] = 0x00;
	data[1] = cmd;
	data[2] = (u8)((addr >> 24) & 0xFF);
	data[3] = (u8)((addr >> 16) & 0xFF);
	data[4] = (u8)((addr >> 8) & 0xFF);
	data[5] = (u8)(addr & 0xFF);
	data[6] = len >> 8;
	data[7] = len;
	/* Payload starts at offset 8 */
	memcpy(data + 8, val, len);

	usleep_range(200, 200);

	for (i = M10MO_I2C_RETRY; i; i--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1) {
			return 0;
		}
		msleep(20);
	}

	return ret;
}

int m10mo_memory_read(struct v4l2_subdev *sd, u16 len, u32 addr, u8 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_device *m10mo_dev =  to_m10mo_sensor(sd);
	struct i2c_msg msg;
	unsigned char data[8];
	u8 *recv_data = m10mo_dev->message_buffer;
	int i, err = 0;

	if (!client->adapter)
		return -ENODEV;

	if (len <= 0)
		return -EINVAL;

	if ((len + 3) > sizeof(m10mo_dev->message_buffer))
		return -ENOMEM;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = sizeof(data);
	msg.buf = data;

	/* high byte goes out first */
	data[0] = 0x00;
	data[1] = 0x03;
	data[2] = (addr >> 24) & 0xFF;
	data[3] = (addr >> 16) & 0xFF;
	data[4] = (addr >> 8) & 0xFF;
	data[5] = addr & 0xFF;
	data[6] = (len >> 8) & 0xFF;
	data[7] = len & 0xFF;

	for (i = M10MO_I2C_RETRY; i; i--) {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (err == 1)
			break;
		msleep(20);
	}

	if (err == 0)
		return -EIO;

	if (err != 1)
		return err;

	msg.flags = I2C_M_RD;
	msg.len = len + 3;
	msg.buf = recv_data;
	for (i = M10MO_I2C_RETRY; i; i--) {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (err == 1)
			break;
		msleep(20);
	}

	if (err == 0)
		return -EIO;

	if (err != 1)
		return err;

	if (len != (recv_data[1] << 8 | recv_data[2])) {
		dev_err(&client->dev,
			"expected length %d, but return length %d\n",
			len, recv_data[1] << 8 | recv_data[2]);
		return -EIO;
	}

	memcpy(val, recv_data + 3, len);

	dev_dbg(&client->dev, "Read mem. len=%d addr=0x%X\n", len, addr);
	return 0;
}

static ssize_t asus_camera_show(struct file *dev, char *buffer, size_t count, loff_t *ppos)
{
    char *buff = NULL;
    ssize_t ret = 0;
    int len = 0;
    int status = (int) asus_camera_flag;

    len += sprintf(buff+len, "%d\n", status);
    ret = simple_read_from_buffer(buffer, count, ppos, buff, len);
    kfree(buff);
    return ret;
}

static ssize_t asus_camera_store(struct file *dev, const char *buf, size_t count, loff_t *loff)
{
    int camera_status = -1;

    sscanf(buf, "%d ", &camera_status);
	printk("@%s %d, camera_status is %d\n", __func__, __LINE__, camera_status);
    if(camera_status > 0){
        asus_camera_flag = 1;
    }else{
        asus_camera_flag = 0;
    }
    return count;
}

static const struct file_operations asus_camera_proc_fops = {
     .read = asus_camera_show,
     .write = asus_camera_store,
};
/**
 * m10mo_setup_flash_controller - initialize flash controller
 *
 * Flash controller requires additional setup before
 * the use.
 */
int m10mo_setup_flash_controller(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 data = 0x7F;
	int res;

	res = m10mo_memory_write(sd, M10MO_MEMORY_WRITE_8BIT,
				 1, 0x13000005, &data);
	if (res < 0)
		dev_err(&client->dev, "Setup flash controller failed\n");
	return res;
}

/**
 * m10mo_wait_interrupt - Clear interrupt pending bits and unmask interrupts
 *
 * Before writing desired interrupt value the INT_FACTOR register should
 * be read to clear pending interrupts.
 */

static int m10mo_enable_interrupt(struct v4l2_subdev *sd, u8 requested_cmd){
    int ret = 0;
	u32 enable_interrupt_bit = 0;
	switch (requested_cmd) {

	case M10MO_MOVE_LENS_TO_PR:
	     enable_interrupt_bit = 0x06;
		 break;

    case M10MO_ASUS_ZOOM_CANCEL:
	case M10MO_ASUS_FOCUS_CANCEL:
	case M10MO_MANUAL_ZOOM:
    case M10MO_MANUAL_FOCUS:
    case M10MO_START_AF:
	case M10MO_START_OPTICAL_ZOOM:
	     enable_interrupt_bit = 0x02;
	     break;

	case M10MO_CAMERA_START:
	case M10MO_SINGLE_CAPTURE_MODE:
         enable_interrupt_bit = 0x08;
	     break;

    	case M10MO_MONITOR_MODE_ZSL_REQUEST_CMD:
	case M10MO_HEAD_HALF_HOME_SEARCHING_MODE:
         enable_interrupt_bit = 0x0F;
	     break;

	case M10MO_PARAMETER_MODE_REQUEST_CMD:
	     enable_interrupt_bit = 0x01;
         break;

	case M10MO_START_DIGITAL_ZOOM:
	     enable_interrupt_bit = 0x04;
         break;

	case M10MO_WRITE_SHD_TABLE:
	     enable_interrupt_bit = 0x40;
		 break;
    default:
	     printk("@%s, unexpected request cmd effect: %d \n",__func__, requested_cmd);
	     return -ENODEV;
	}
    ret = m10mo_writeb(sd, CATEGORY_SYSTEM, SYSTEM_INT_ENABLE, enable_interrupt_bit);
    return ret;
}

static u32 get_m10mo_wait_timeout_val(struct v4l2_subdev *sd, u8 requested_cmd){
    u32 timeout_val = 0;
    u32 read_val = 0xFF;

	switch (requested_cmd) {

	case M10MO_MOVE_LENS_TO_PR:
	     timeout_val = M10MO_BACK_TO_PR_TIMEOUT;
		 break;

    case M10MO_ASUS_ZOOM_CANCEL:
	case M10MO_ASUS_FOCUS_CANCEL:
	case M10MO_MANUAL_ZOOM:
    case M10MO_MANUAL_FOCUS:
    case M10MO_START_AF:
	case M10MO_START_OPTICAL_ZOOM:
	     timeout_val = M10MO_INIT_TIMEOUT;
	     break;

	case M10MO_CAMERA_START:
	     timeout_val = M10MO_INIT_TIMEOUT;
	     break;

	case M10MO_SINGLE_CAPTURE_MODE:
         (void) m10mo_readb(sd, CATEGORY_ASUS, ASUS_SHUTTER_SPEED, &read_val);

		 if(read_val == SHUTTER_SPEED_32) { // Means exposure time is 32s, set the timeout is 40s.
		      timeout_val = 40000;
		 } else {
              timeout_val = M10MO_INIT_TIMEOUT;
		 }
	     break;

    case M10MO_MONITOR_MODE_ZSL_REQUEST_CMD:
	case M10MO_HEAD_HALF_HOME_SEARCHING_MODE:
         timeout_val = M10MO_INIT_TIMEOUT;
	     break;

	case M10MO_PARAMETER_MODE_REQUEST_CMD:
	     timeout_val = M10MO_PARAMETER_MODE_TIMEOUT;
         break;

	case M10MO_START_DIGITAL_ZOOM:
	     timeout_val = M10MO_INIT_TIMEOUT;
         break;

	case M10MO_WRITE_SHD_TABLE:
	     timeout_val = M10MO_INIT_TIMEOUT;
		 break;
    default:
	     printk("@%s, unexpected request cmd effect: %d \n",__func__, requested_cmd);
	     timeout_val = M10MO_INIT_TIMEOUT;
		 break;
	}
    printk("@%s, request cmd effect is %d, related timeout is %d ms\n",__func__, requested_cmd, timeout_val);
    return timeout_val;
}

int m10mo_request_cmd_effect(struct v4l2_subdev *sd, u8 requested_cmd, void* data)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
//	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;
	u32 val = 0;
	u32 optical_zoom_check = 0;

	wake_lock_timeout(&dev->m10mo_wake_lock, msecs_to_jiffies(60000));
	printk("@%s %d start, requested_cmd = %d. Try to get the mutex lock. TRYing...TRYing...\n", __func__, __LINE__, requested_cmd);
	mutex_lock(&dev->m10mo_request_cmd_lock);
	dev->m10mo_request_cmd_lock_flag = 1;
	if(!dev->stream) {
	    if(requested_cmd == M10MO_START_AF ||
		     requested_cmd == M10MO_START_OPTICAL_ZOOM) {
			    printk("@%s %d start, requested_cmd is %d. app already stop preview, break here!!! \n", __func__, __LINE__, requested_cmd);
			    goto out_m10mo_request_cmd_effect;
			 }
	}
    dev->requested_cmd = requested_cmd;
    printk("@%s %d start, dev->requested_cmd = %d. Successful to get the mutex lock.\n", __func__, __LINE__, dev->requested_cmd);

//    m10mo_enable_interrupt(sd, requested_cmd);

	switch (dev->requested_cmd) {
	case M10MO_CAMERA_START:
	    m10mo_enable_interrupt(sd, M10MO_CAMERA_START);
		ret = m10mo_writeb(sd, CATEGORY_FLASHROM, FLASH_CAM_START, 0x01);
		break;

	case M10MO_PARAMETER_MODE_REQUEST_CMD:
	    m10mo_enable_interrupt(sd, M10MO_PARAMETER_MODE_REQUEST_CMD);
        ret = m10mo_writeb(sd, CATEGORY_SYSTEM, SYSTEM_SYSMODE, 0x01);
		break;

	case M10MO_MONITOR_MODE_ZSL_REQUEST_CMD:
	    m10mo_enable_interrupt(sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD);
		ret = m10mo_writeb(sd, CATEGORY_SYSTEM, SYSTEM_SYSMODE, 0x02);
		break;

	case M10MO_SINGLE_CAPTURE_MODE:
//		ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL,0x05, 0x01);
        m10mo_send_still_capture_cmds(sd);
		ret = 0;
//		gpio_set_value(60, 0);
//		printk(KERN_INFO "m10mo driver, gpio 60 set low\n");
		break;

	case M10MO_HEAD_HALF_HOME_SEARCHING_MODE:
	    m10mo_enable_interrupt(sd, M10MO_HEAD_HALF_HOME_SEARCHING_MODE);
		ret = m10mo_writeb(sd, CATEGORY_PARAM, 0x50, 0x03);
		isp_update_status = M10MO_HEAD_HALF_HOME_SEARCHING_MODE;
        break;

	case M10MO_MOVE_LENS_TO_PR:
	    m10mo_enable_interrupt(sd, M10MO_MOVE_LENS_TO_PR);
		ret = m10mo_writeb(sd, CATEGORY_PARAM, 0x50, 0x10);
		isp_update_status = M10MO_MOVE_LENS_TO_PR;
        break;

	case M10MO_START_OPTICAL_ZOOM:
	    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
	    if(val == *(u32*)data){
	        pr_info("m10mo, get optical-zoom step is %d, set optical-zoom data is %d. No need to set\n", val, *(u32*)data);
			mutex_unlock(&dev->m10mo_request_cmd_lock);
			printk("@%s, UNLOCK m10mo_request_cmd_lock\n", __func__);
	        return 0;
	    }
		m10mo_enable_interrupt(sd, M10MO_START_OPTICAL_ZOOM);
		ret = m10mo_writeb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, *(u32*)data);
        printk("@%s %d, zoom_check, write zoom position succeed, step is %d \n", __func__, __LINE__, *(u32*)data);
        break;

	case M10MO_MANUAL_FOCUS:
	    m10mo_enable_interrupt(sd, M10MO_MANUAL_FOCUS);
		ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_FOCUS_STEP, *(u32*)data);
        break;

	case M10MO_MANUAL_ZOOM:
	    m10mo_enable_interrupt(sd, M10MO_MANUAL_ZOOM);
		ret = m10mo_writew(sd, CATEGORY_TEST, MANUAL_ZOOM_CTRL_H, *(u32*)data);
        break;

	case M10MO_ASUS_FOCUS_CANCEL:
	    m10mo_enable_interrupt(sd, M10MO_ASUS_FOCUS_CANCEL);
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_FOCUS_CANCEL, 0x01);
		break;

	case M10MO_ASUS_ZOOM_CANCEL:
	    m10mo_enable_interrupt(sd, M10MO_ASUS_ZOOM_CANCEL);
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_ZOOM_CANCEL, 0x01);
		break;

	case M10MO_START_AF:
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_FOCUS_RESULT, 0x00); //=== Reset the Af result. ===//
	    m10mo_enable_interrupt(sd, M10MO_START_AF);
		ret = m10mo_writeb(sd, CATEGORY_LENS, 0x2, 0x13);
        break;

	case M10MO_START_DIGITAL_ZOOM:
	     m10mo_enable_interrupt(sd, M10MO_START_DIGITAL_ZOOM);
		 ret = m10mo_writeb(sd, CATEGORY_MONITOR, DIGIT_ZOOM, *(u32*)data);
	     break;

    case M10MO_WRITE_SHD_TABLE:
	     m10mo_enable_interrupt(sd, M10MO_WRITE_SHD_TABLE);
	     ret = m10mo_writeb(sd, CATEGORY_LOGLEDFLASH, 0xaf, 0x01);
		 break;
	default:
        printk("@%s %d, UNLOCK m10mo_request_cmd_lock. Invalid Mode is %d, break!\n", __func__, __LINE__, requested_cmd);
//        if (dev->m10mo_request_cmd_lock_flag == 1){
		    mutex_unlock(&dev->m10mo_request_cmd_lock);
//			dev->m10mo_request_cmd_lock_flag = 0;
//        }
		return -ENODEV;
	}

    printk("m10mo, 45678\n");
    if(dev->requested_cmd == M10MO_START_AF || dev->requested_cmd == M10MO_MANUAL_FOCUS){
	      dev->wait_irq_flag = M10MO_FOCUSING;
		  printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);
    }else if(dev->requested_cmd == M10MO_START_OPTICAL_ZOOM || dev->requested_cmd == M10MO_MANUAL_ZOOM){
	      dev->wait_irq_flag = M10MO_ZOOMING;
		  printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);
	}else if (dev->requested_cmd == M10MO_ASUS_ZOOM_CANCEL){
	      dev->wait_irq_flag = M10MO_OPTICAL_ZOOM_CANCELING;
		  printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);
	}else if (dev->requested_cmd == M10MO_ASUS_FOCUS_CANCEL){
	      dev->wait_irq_flag = M10MO_AF_CANCELING;
		  printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);
	}else if (dev->requested_cmd == M10MO_START_DIGITAL_ZOOM) {
	      dev->wait_irq_flag = M10MO_DIGITAL_ZOOMING;
		  printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);
	}else {
	      dev->wait_irq_flag = M10MO_NOT_LENS_RELATED_BUSYING;
		  printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);
	}
	if (dev->requested_cmd == M10MO_START_OPTICAL_ZOOM
	    || dev->requested_cmd == M10MO_START_AF
	    || dev->requested_cmd == M10MO_ASUS_ZOOM_CANCEL
	    || dev->requested_cmd == M10MO_ASUS_FOCUS_CANCEL) {
              ret = m10mo_readb(sd, CATEGORY_LENS, 0x0b, &val);
			  printk("@%s Lens status is %d, requested_cmd is %d \n", __func__, val, dev->requested_cmd);
			  if(val == 0) {
			      printk("@%s  IRQ before wait!!! requested_cmd is %d \n", __func__, dev->requested_cmd);
				  if(dev->cancel_irq_flag == 1) {
	                     dev->cancel_irq_flag = 0;
				  }
                  m10mo_lens_state_machine(sd, M10MO_IRQ_EVENT, NULL);
				  goto out_m10mo_request_cmd_effect;
			  }

	}
	val = get_m10mo_wait_timeout_val(sd, dev->requested_cmd);
    ret = m10mo_wait_mode_change(sd, dev->requested_cmd, val);
//	}
	printk("m10mo, 87654\n");
//#if 0
    if(dev->cancel_irq_flag == 1) {printk("m10mo, 76543\n");
	    dev->cancel_irq_flag = 0;
	}else {printk("m10mo, 65432\n");
        if (dev->requested_cmd == M10MO_START_AF
             || dev->requested_cmd == M10MO_START_OPTICAL_ZOOM
             || dev->requested_cmd == M10MO_ASUS_ZOOM_CANCEL
             || dev->requested_cmd == M10MO_ASUS_FOCUS_CANCEL) {printk("m10mo, 54321\n");
		        m10mo_lens_state_machine(sd, M10MO_IRQ_EVENT, NULL);
        }
	}printk("m10mo, 43210\n");
//#endif
    if(m10mo_boot_state == 0) {
		if(dev->requested_cmd == M10MO_MONITOR_MODE_ZSL_REQUEST_CMD) {
		    dev->requested_cmd = M10MO_BOTTOM_HALF_HOME_SEARCHING_MODE;
		    printk("@%s            dev->requested_cmd = %d, start to do BOTTOM_HALF home-searching. Successful to keep the mutex lock.\n", __func__, dev->requested_cmd);
            (void) m10mo_writeb(sd, 0x0D, 0x39, 0x01);
			m10mo_boot_state = 1;
			isp_update_status = M10MO_BOTTOM_HALF_HOME_SEARCHING_MODE;
//			dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE;
			dev->m10mo_mode = M10MO_MONITOR_MODE_ZSL;
			dev->cmd = M10MO_NO_CMD_REQUEST;
			return 0;
	    }
	}
    if(dev->requested_cmd == M10MO_START_OPTICAL_ZOOM) {
	    ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &optical_zoom_check);
		if(optical_zoom_check != *(u32*)data) {
		     printk("@%s, zoom_check. optical_zoom_check is %d, previously optical_zoom set is %d, NOT equal, Zoom FAIL!!!!!\n", __func__, optical_zoom_check, *(u32*)data);
		} else {
		     printk("@%s, zoom_check. optical_zoom_check is %d, previously optical_zoom set is %d, equal!\n", __func__, optical_zoom_check, *(u32*)data);
		}
	}
out_m10mo_request_cmd_effect:
	if(requested_cmd == M10MO_PARAMETER_MODE_REQUEST_CMD){
	    dev->m10mo_mode = M10MO_PARAMETER_MODE;

	}else if( requested_cmd == M10MO_MONITOR_MODE_ZSL_REQUEST_CMD ){
	    dev->m10mo_mode = M10MO_MONITOR_MODE_ZSL;
    }

	dev->cmd = dev->requested_cmd = M10MO_NO_CMD_REQUEST;

	if(requested_cmd == M10MO_HEAD_HALF_HOME_SEARCHING_MODE || requested_cmd == M10MO_MOVE_LENS_TO_PR)
		isp_update_status = M10MO_NO_CMD_REQUEST;

	dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE;

//	if (dev->m10mo_request_cmd_lock_flag == 1){
	    mutex_unlock(&dev->m10mo_request_cmd_lock);
		printk("@%s %d, UNLOCK m10mo_request_cmd_lock.\n", __func__, __LINE__);
//		dev->m10mo_request_cmd_lock_flag = 0;
//    }
    wake_unlock(&dev->m10mo_wake_lock);
	return ret;
}

static int is_m10mo_in_monitor_mode(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	if (dev->cmd == M10MO_MONITOR_MODE_PANORAMA ||
	    dev->cmd == M10MO_MONITOR_MODE_ZSL_REQUEST_CMD ||
	    dev->cmd == M10MO_MONITOR_MODE ||
	    dev->cmd == M10MO_MONITOR_MODE_HIGH_SPEED)
		return 1;

	return 0;
}

int m10mo_wait_mode_change(struct v4l2_subdev *sd, u8 mode, u32 timeout)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u8 previous_state;
//    printk("m10mo, 56789  dev->cmd = 0x%x mode = 0x%x\n",dev->cmd, mode);
//	ret = wait_event_timeout(dev->irq_waitq, dev->cmd == mode, msecs_to_jiffies(timeout));
//	printk("m10mo, 98765  dev->cmd = 0x%x mode = 0x%x, ret is %d\n",dev->cmd, mode, ret);
    printk("m10mo, 56789  dev->cmd = 0x%x mode = 0x%x\n",dev->cmd, dev->requested_cmd);
	ret = wait_event_timeout(dev->irq_waitq, dev->cmd == dev->requested_cmd, msecs_to_jiffies(timeout));
	printk("m10mo, 98765  dev->cmd = 0x%x mode = 0x%x, ret is %d\n",dev->cmd, dev->requested_cmd, ret);
	if (ret > 0) {
		return 0;
	} else if (ret == 0) {
		dev_err(&client->dev, "m10mo_wait_mode_change timed out. dev->cmd = %x dev->requested_cmd = %x, timeout is %d\n",dev->cmd, dev->requested_cmd, timeout);
		if(dev->requested_cmd == M10MO_MOVE_LENS_TO_PR) {
		    printk("@m10mo_wait_mode_change. It is PR position timeout. Ignore it. dev->requested_cmd is %d \n", dev->requested_cmd);
			return 0;
		}
		if(m10mo_trace_log_lock != DOWNLOAD_AVAILABLE) {
              return -ETIMEDOUT;
		}
		mutex_unlock(&dev->input_lock);
        dev->lock_i2c_write_flag = 0;
		previous_state = m10mo_trace_log_lock;
		m10mo_trace_log_lock = DOWNLOADING;
		m10mo_ispd4(dev);
		m10mo_trace_log_lock = previous_state;
		return -ETIMEDOUT;
	}

	return ret;
}

int __m10mo_param_mode_set(struct v4l2_subdev *sd)
{
	int ret;
	u32 read_val;
	u8 requested_cmd;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
    printk("@%s %d start\n", __func__, __LINE__);
	(void) m10mo_readb(sd, CATEGORY_SYSTEM, SYSTEM_SYSMODE, &read_val);
	if(read_val == M10MO_PARAMETER_MODE_REQUEST_CMD){
	    printk(KERN_INFO "@%s, Already in parameter mode, no need to set again. \n", __func__);
		return 0;
	}
	requested_cmd = dev->requested_cmd;
	read_val = 0;
	(void) m10mo_readb(sd, CATEGORY_LENS, 0x0b, &read_val);

	if(requested_cmd == M10MO_START_AF) {
		    dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();
		    (void) m10mo_request_cmd_effect(sd, M10MO_ASUS_FOCUS_CANCEL, NULL);
	} else if(requested_cmd == M10MO_START_OPTICAL_ZOOM) {
		    dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();
	        (void) m10mo_request_cmd_effect(sd, M10MO_ASUS_ZOOM_CANCEL, NULL);
	}

	ret = m10mo_request_cmd_effect(sd, M10MO_PARAMETER_MODE_REQUEST_CMD, NULL);
	if (ret)
		return ret;

	return ret;
}

int m10mo_detect(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_version *ver = &dev->ver;
	int ret;

        ret = m10mo_read(sd, 1, CATEGORY_SYSTEM, SYSTEM_CUSTOMER_CODE,
			&ver->customer);
	if (!ret)
		ret = m10mo_read(sd, 1, CATEGORY_SYSTEM, SYSTEM_PROJECT_CODE,
				&ver->project);
	dev_info(&client->dev, "Customer/Project[0x%x/0x%x]\n", dev->ver.customer,
				dev->ver.project);
	return 0;
}

static int __m10mo_fw_start(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
    printk("@%s %d start\n", __func__, __LINE__);
	/*
	 * Correct the pll value before fw start
	 */
    ret = m10mo_update_pll_setting(sd);
    if (ret < 0)
        return ret;

	ret = m10mo_setup_flash_controller(sd);
	if (ret < 0)
		return ret;

	ret = m10mo_request_cmd_effect(sd, M10MO_CAMERA_START, NULL);
	if (ret)
		return ret;

	ret = m10mo_detect(sd);
	if (ret)
		return ret;

	dev_info(&client->dev, "ISP Booted Successfully\n");

//    ret = m10mo_request_cmd_effect(sd, M10MO_HEAD_HALF_HOME_SEARCHING_MODE, NULL);
    if (ret)
        return ret;

	return 0;
}

static int m10mo_fw_start(struct v4l2_subdev *sd, u32 val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret;
    printk("@%s %d start\n", __func__, __LINE__);

	mutex_lock(&dev->input_lock);

	ret = __m10mo_fw_start(sd);

	mutex_unlock(&dev->input_lock);

	return ret;
}

static int m10mo_set_af_mode(struct v4l2_subdev *sd, unsigned int val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret = 0;
	int polling_times = 10;
	u32 read_back = 0;

    printk("@%s value %d start\n", __func__, val);

    if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set AF during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}

	//======= Hard code here for polling status by Derek says so.
	(void) m10mo_readb(sd, CATEGORY_ASUS,ASUS_FOCUS_MODE,&read_back);
	if(read_back != val){
		(void) m10mo_writeb(sd, CATEGORY_ASUS,ASUS_FOCUS_MODE,val);
		for(polling_times = 10; polling_times > 0; --polling_times){
			(void) m10mo_readb(sd, CATEGORY_ASUS,ASUS_FOCUS_MODE,&read_back);
			if(read_back == val){
				printk("@%s %d success !\n", __func__, __LINE__);
				return 0;
			}
		}
	}else{
	         printk("@%s, already in AF normal mode, no need to set! \n", __func__);
			 return 0;
	}
	printk("@%s %d fail !\n", __func__, __LINE__);
	return -EINVAL;
#if 0
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int id = M10MO_GET_FOCUS_MODE(dev->fw_type);

	u32 cur_af_mode;

	switch (val) {
	case EXT_ISP_FOCUS_MODE_NORMAL:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_normal;
		break;
	case EXT_ISP_FOCUS_MODE_MACRO:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_macro;
		break;
	case EXT_ISP_FOCUS_MODE_TOUCH_AF:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_touch;
		break;
	case EXT_ISP_FOCUS_MODE_PREVIEW_CAF:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_preview_caf;
		break;
	case EXT_ISP_FOCUS_MODE_MOVIE_CAF:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_movie_caf;
		break;
	case EXT_ISP_FOCUS_MODE_FACE_CAF:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_face_caf;
		break;
	case EXT_ISP_FOCUS_MODE_TOUCH_MACRO:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_touch_macro;
		break;
	case EXT_ISP_FOCUS_MODE_TOUCH_CAF:
		dev->monitor_params.af_mode = m10m0_af_parameters[id].af_touch_caf;
		break;
	default:
		return -EINVAL;
	}

	if (!is_m10mo_in_monitor_mode(sd))
		return ret;

	ret = m10mo_read(sd, 1, CATEGORY_LENS,
			m10m0_af_parameters[id].af_mode,
			&cur_af_mode);

	/*
	 * If af_mode has changed as expected already
	 * no need to set any more
	 */
	if (dev->monitor_params.af_mode == cur_af_mode)
		return ret;

	dev_info(&client->dev, "%s: In monitor mode, set AF mode to %d",
		 __func__, dev->monitor_params.af_mode);

	/* We are in monitor mode already, */
	/* af_mode can be applied immediately */
	ret = m10mo_writeb(sd, CATEGORY_LENS,
			m10m0_af_parameters[id].af_mode,
			dev->monitor_params.af_mode);
#endif
	return ret;
}

static int m10mo_set_af_execution(struct v4l2_subdev *sd, s32 val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int id = M10MO_GET_FOCUS_MODE(dev->fw_type);
	int ret = 0;

	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set AF during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
	switch (val) {
	case EXT_ISP_FOCUS_STOP:
		dev->monitor_params.exe_mode = m10m0_af_parameters[id].af_stop;
		break;
	case EXT_ISP_FOCUS_SEARCH:
		dev->monitor_params.exe_mode = m10m0_af_parameters[id].af_search;
		break;
	case EXT_ISP_PAN_FOCUSING:
		dev->monitor_params.exe_mode = m10m0_af_parameters[id].af_pan_focusing;
		break;
	default:
		return -EINVAL;
	}

	printk(KERN_INFO "%s: In monitor mode, set AF exe_mode to %d",
			 __func__, dev->monitor_params.exe_mode);
#if 0
		/* We are in monitor mode already, */
		/* exe_mode can be applied immediately */
		ret = m10mo_writeb(sd, CATEGORY_LENS,
				   m10m0_af_parameters[id].af_execution,
				   dev->monitor_params.exe_mode);
#else
	if(val == EXT_ISP_FOCUS_SEARCH){
//	    dev->les_wq_requested_cmd = M10MO_START_AF;
//	    dev->m10mo_request_cmd_lens_data = 0;
//		cancel_delayed_work(&lens_work);
//	    flush_workqueue(dev->auto_focus_wq);
        printk("@%s %d, 007 Before queue_delayed_work(dev->lens_wq, &auto_focus_work). \n", __func__, __LINE__);
        queue_delayed_work(dev->lens_wq, &auto_focus_work, 0);
//		ret = m10mo_request_cmd_effect(sd, M10MO_START_AF, NULL);
		if (ret < 0)
			return ret;
#endif
	}
	return ret;
}

static int m10mo_set_af_position_x(struct v4l2_subdev *sd, unsigned int x)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int id = M10MO_GET_FOCUS_MODE(dev->fw_type);
	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
    printk("@%s: set DIT AF x = %d\n", __func__, x);

	dev->monitor_params.af_touch_posx = x;

    /* Set X Position to DIT*/
    ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_TOUCH_ROI_LEFT_UPPER_X, dev->monitor_params.af_touch_posx);
	if (ret){
		dev_err(&client->dev, "set DIT AF position x failed %d\n", ret);
        return ret;
    }

	if (is_m10mo_in_monitor_mode(sd)) {
		dev_info(&client->dev,
			 "%s: In monitor mode, set AF touch X pos to 0x%x",
			 __func__, dev->monitor_params.af_touch_posx);

		/* Set X Position */
		ret = m10mo_writew(sd, CATEGORY_LENS,
				   m10m0_af_parameters[id].af_touch_posx,
				   dev->monitor_params.af_touch_posx);
	}

	if (ret)
		dev_err(&client->dev, "AutoFocus position x failed %d\n", ret);

	return ret;
}

static int m10mo_set_af_position_y(struct v4l2_subdev *sd, unsigned int y)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int id = M10MO_GET_FOCUS_MODE(dev->fw_type);
	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
    printk("@%s: set DIT AF y = %d\n", __func__, y);

	dev->monitor_params.af_touch_posy = y;

    /* Set Y Position to DIT*/
    ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_TOUCH_ROI_LEFT_UPPER_Y, dev->monitor_params.af_touch_posy);
	if (ret){
		dev_err(&client->dev, "set DIT AF position y failed %d\n", ret);
        return ret;
    }

	if (is_m10mo_in_monitor_mode(sd)) {
		dev_info(&client->dev,
			 "%s: In monitor mode, set AF touch Y pos to 0x%x",
			 __func__, dev->monitor_params.af_touch_posy);

		/* Set Y Position */
		ret = m10mo_writew(sd, CATEGORY_LENS,
				   m10m0_af_parameters[id].af_touch_posy,
				   dev->monitor_params.af_touch_posy);
	}

	if (ret)
		dev_err(&client->dev, "AutoFocus position y failed %d\n", ret);

	return ret;
}

static int m10mo_set_af_width(struct v4l2_subdev *sd, unsigned int w)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    int ret = 0;
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
    printk("@%s: set DIT AF ROI width = %d\n", __func__, w);
    dev->monitor_params.af_touch_width = w;

    /* Set AF ROI width to DIT */
    ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_TOUCH_WIDTH, dev->monitor_params.af_touch_width);

    if (ret)
        dev_err(&client->dev, "set DIT AF ROI width failed %d\n", ret);

    return ret;
}

static int m10mo_set_af_height(struct v4l2_subdev *sd, unsigned int h)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    int ret = 0;
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
    printk("@%s: set DIT AF ROI height = %d\n", __func__, h);
    dev->monitor_params.af_touch_height = h;

    /* Set AF ROI height to DIT */
    ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_TOUCH_HEIGHT, dev->monitor_params.af_touch_height);

    if (ret)
        dev_err(&client->dev, "set DIT AF ROI height failed %d\n", ret);

    return ret;
}

static int m10mo_set_ae_position_x(struct v4l2_subdev *sd, unsigned int x)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    int ret = 0;
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
    printk("@%s: set DIT AE x = %d\n", __func__, x);
    dev->monitor_params.ae_touch_posx = x;

    /* Set AE X Position to DIT */
    ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_TOUCH_POSITION_X, dev->monitor_params.ae_touch_posx);

    if (ret)
        dev_err(&client->dev, "set DIT AE position x failed %d\n", ret);

    return ret;
}

static int m10mo_set_ae_position_y(struct v4l2_subdev *sd, unsigned int y)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    int ret = 0;
    struct m10mo_device *dev = to_m10mo_sensor(sd);

	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}

    dev->monitor_params.ae_touch_posy = y;

    /* Set AE Y Position to DIT */
    ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_TOUCH_POSITION_Y, dev->monitor_params.ae_touch_posy);

    if (ret)
        dev_err(&client->dev, "set DIT AE position y failed %d\n", ret);

    return ret;
}

/* Because of different m10m0 firmwares and parameter values, transform
   the values to the macro definition */
static u32 m10mo_af_parameter_transform(struct v4l2_subdev *sd, u32 val)
{
	u32 ret = 0xffffffff;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int id = M10MO_GET_FOCUS_MODE(dev->fw_type);

	if (val == m10m0_af_parameters[id].caf_status_focusing) {
		ret = CAF_STATUS_FOCUSING;
	} else if (val == m10m0_af_parameters[id].caf_status_success) {
		ret = CAF_STATUS_SUCCESS;
	} else if (val == m10m0_af_parameters[id].caf_status_fail) {
		ret = CAF_STATUS_FAIL;
	} else if (val == m10m0_af_parameters[id].caf_status_restart_check) {
		ret = CAF_STATUS_RESTART_CHECK;
	} else if (val == m10m0_af_parameters[id].af_status_invalid) {
		ret = AF_STATUS_INVALID;
	} else if (val == m10m0_af_parameters[id].af_status_focusing) {
		ret = AF_STATUS_FOCUSING;
	} else if (val == m10m0_af_parameters[id].af_status_success) {
		ret = AF_STATUS_SUCCESS;
	} else if (val == m10m0_af_parameters[id].af_status_fail) {
		ret = AF_STATUS_FAIL;
	} else if (val == m10m0_af_parameters[id].af_normal) {
		ret = AF_NORMAL;
	} else if (val == m10m0_af_parameters[id].af_macro) {
		ret = AF_MACRO;
	} else if (val == m10m0_af_parameters[id].af_touch) {
		ret = AF_TOUCH;
	} else if (val == m10m0_af_parameters[id].af_preview_caf) {
		ret = AF_PREVIEW_CAF;
	} else if (val == m10m0_af_parameters[id].af_movie_caf) {
		ret = AF_MOVIE_CAF;
	} else if (val == m10m0_af_parameters[id].af_face_caf) {
		ret = AF_FACE_CAF;
	} else if (val == m10m0_af_parameters[id].af_touch_macro) {
		ret = AF_TOUCH_MACRO;
	} else if (val == m10m0_af_parameters[id].af_touch_caf) {
		ret = AF_TOUCH_CAF;
	}

	return ret;
}

static int m10mo_get_caf_status(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 af_result;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int id = M10MO_GET_FOCUS_MODE(dev->fw_type);

	ret = m10mo_read(sd, 1, CATEGORY_LENS,
			m10m0_af_parameters[id].af_result, &af_result);
	if (ret)
		return ret;

	af_result = m10mo_af_parameter_transform(sd, af_result);

	switch (af_result) {
	case CAF_STATUS_FOCUSING:
		*status = EXT_ISP_CAF_STATUS_FOCUSING;
		break;
	case CAF_STATUS_SUCCESS:
		*status = EXT_ISP_CAF_STATUS_SUCCESS;
		break;
	case CAF_STATUS_FAIL:
		*status = EXT_ISP_CAF_STATUS_FAIL;
		break;
	case CAF_STATUS_RESTART_CHECK:
		*status = EXT_ISP_CAF_RESTART_CHECK;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}


static int m10mo_set_hal_i2c_write(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 reg_val;
	u8 cat, reg_addr;
	int ret;

	reg_val  = *status & 0xFF;
	reg_addr = (*status >> 8) & 0xFF;
	cat      = (*status >> 16) & 0xFF;

	ret = m10mo_writeb(sd, cat, reg_addr, reg_val);
	return ret;
}

static int m10mo_get_hal_i2c_read(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 reg_val;
	u8 cat, reg_addr;
	int ret;

	reg_addr = (*status >> 8) & 0xFF;
	cat      = (*status >> 16) & 0xFF;

	ret = m10mo_readb(sd, cat, reg_addr, &reg_val);

	if(!ret){
	     *status = reg_val;
	} else {
	     *status = EXT_ISP_CID_M10MO_I2C_ONE_BYTE_READ_INVALID;
	}

	return ret;
}

static int m10mo_get_hal_i2c_wread(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 reg_val;
	u8 cat, reg_addr;
	int ret;

	reg_addr = (*status >> 8) & 0xFF;
	cat      = (*status >> 16) & 0xFF;

	ret = m10mo_readw(sd, cat, reg_addr, &reg_val);
	printk("EXT_ISP_CID_M10MO_I2C_WORD_READ  %x\n",reg_val);
	if(!ret){
	     *status = reg_val;
	} else {
	     *status = EXT_ISP_CID_M10MO_I2C_ONE_BYTE_READ_INVALID;
	}

	return ret;
}

static int m10mo_get_hal_i2c_lread(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 reg_val;
	u8 cat, reg_addr;
	int ret;

	reg_addr = (*status >> 8) & 0xFF;
	cat      = (*status >> 16) & 0xFF;

	ret = m10mo_readl(sd, cat, reg_addr, &reg_val);

	if(!ret){
	     *status = reg_val;
	} else {
	     *status = EXT_ISP_CID_M10MO_I2C_ONE_BYTE_READ_INVALID;
	}

	return ret;
}

static int m10mo_get_asus_flash_needed(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 val;
	int ret;
	ret = m10mo_readb(sd, CATEGORY_ASUS, ASUS_FLASH_NEEDED, &val);
	*status = val;

	return ret;
}

static int m10mo_get_irq_status(struct v4l2_subdev *sd, unsigned int *status)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);

    if(dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE
	     && dev->lens_state_machine == LENS_IDLE){
	      *status = EXT_ISP_M10MO_IRQ_IDLE;
    } else {
	       *status = EXT_ISP_M10MO_IRQ_BUSY;
	}
	return 0;
}

static int m10mo_get_optical_zoom_number(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 val;
	int ret;
	ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
	*status = val;

	return ret;
}

static int m10mo_set_battery_capacity(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_BATTERY_CAPACITY, val);
	return ret;
}

static int m10mo_get_lens_initailzation_done(struct v4l2_subdev *sd, unsigned int *status)
{
    if(m10mo_boot_state == 2) {
	    *status = EXT_ISP_LENS_INITIALIZATION_DONE;
	} else {
	    *status = EXT_ISP_LENS_INITIALIZATION_NOT_DONE;
	}

	return 0;
}

static int m10mo_get_LV(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;

	ret = m10mo_readw(sd, CATEGORY_ASUS, ASUS_LV, &val);
	*status = val;
	return ret;
}

static int m10mo_set_R_GAIN(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_GAIN2_RGAIN, val);
	return ret;
}

static int m10mo_get_R_GAIN(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;

	ret = m10mo_readw(sd, CATEGORY_ASUS, ASUS_GAIN1_RGAIN, &val);
	*status = val;
	return ret;
}

static int m10mo_set_B_GAIN(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_GAIN2_BGAIN, val);
	return ret;
}

static int m10mo_get_B_GAIN(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;

	ret = m10mo_readw(sd, CATEGORY_ASUS, ASUS_GAIN1_BGAIN, &val);
	*status = val;
	return ret;
}

static int m10mo_set_3A_lock(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_3A_LOCK, val);
	return ret;
}

static int m10mo_set_g_sensor_x(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writew(sd, CATEGORY_ASUS, 0x3C, val);
	return ret;
}

static int m10mo_set_g_sensor_y(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writew(sd, CATEGORY_ASUS, 0x3E, val);
	return ret;
}

static int m10mo_set_g_sensor_z(struct v4l2_subdev *sd, unsigned int *status)
{
	u32 val;
	int ret;
    val = *status;

	ret = m10mo_writew(sd, CATEGORY_ASUS, 0x40, val);
	return ret;
}

static int m10mo_get_af_result(struct v4l2_subdev *sd, unsigned int *status)
{
//  struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 read_val = 0;
	int ret;

	ret = m10mo_readb(sd, CATEGORY_ASUS, ASUS_FOCUS_RESULT, &read_val);

	if (read_val == AF_SUCCESS) {
	    *status = EXT_ISP_AF_RESULT_SUCCESS;

	} else if (read_val == AF_FAIL) {
	    *status = EXT_ISP_AF_RESULT_FAIL;

	} else if (read_val == AF_INVALID) {
	    *status = EXT_ISP_AF_RESULT_INVALID;

	} else {
	     *status = EXT_ISP_AF_RESULT_INVALID;
		 printk(KERN_ERR "@%s, Error, value (%d) not defined! \n", __func__, read_val);
		 return -EINVAL;
	}
	return ret;
}

static int m10mo_get_af_status(struct v4l2_subdev *sd, unsigned int *status)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 read_val = 0;

    (void) m10mo_readb(sd, CATEGORY_ASUS, ASUS_FOCUS_RESULT, &read_val);
	if(read_val == AF_PRE_DONE) {
	      *status = EXT_ISP_AF_STATUS_PRE_DONE;
		  return 0;
	}

	if(dev->wait_irq_flag == M10MO_FOCUSING){
	      *status = EXT_ISP_AF_STATUS_FOCUSING;
	}else if (dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE){
	       *status = EXT_ISP_AF_STATUS_SUCCESS;
	}else{
	       *status = EXT_ISP_LENS_STATUS_INVALID;
	}
	return 0;
}

static int m10mo_get_optical_zoom_status(struct v4l2_subdev *sd, unsigned int *status)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);

	if(dev->wait_irq_flag == M10MO_ZOOMING){
	      *status = EXT_ISP_OPTICAL_ZOOM_STATUS_ZOOMING;
	}else if (dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE){
	       *status = EXT_ISP_OPTICAL_ZOOM_STATUS_AVAILABLE;
	}else{
	       *status = EXT_ISP_LENS_STATUS_INVALID;
	}
	return 0;
}

static int m10mo_get_digital_zoom_status(struct v4l2_subdev *sd, unsigned int *status)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);

	if(dev->wait_irq_flag == M10MO_DIGITAL_ZOOMING){
	      *status = EXT_ISP_DIGITAL_ZOOM_STATUS_ZOOMING;
	}else if (dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE){
	       *status = EXT_ISP_DIGITAL_ZOOM_STATUS_AVAILABLE;
	}else{
	       *status = EXT_ISP_LENS_STATUS_INVALID;
	}
	return 0;

}

static int m10mo_get_cancel_af_cmd_status(struct v4l2_subdev *sd, unsigned int *status)
{
//    struct m10mo_device *dev = to_m10mo_sensor(sd);
//    if(dev->wait_irq_flag == M10MO_AF_CANCELING){
//	      *status = EXT_ISP_AF_CANCELING;
//	}else if (dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE){
	       *status = EXT_ISP_AF_CANCEL_CMD_AVAILABLE;
//	}else{
//	       *status = EXT_ISP_LENS_STATUS_INVALID;
//	}
	return 0;
}

static int m10mo_get_cancel_optical_zoom_cmd_status(struct v4l2_subdev *sd, unsigned int *status)
{
//    struct m10mo_device *dev = to_m10mo_sensor(sd);
//    if(dev->wait_irq_flag == M10MO_OPTICAL_ZOOM_CANCELING){
//	      *status = EXT_ISP_OPTICAL_ZOOM_CANCELING;
//	}else if (dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE){
	       *status = EXT_ISP_OPTICAL_ZOOM_CANCEL_CMD_AVAILABEL;
//	}else{
//	       *status = EXT_ISP_LENS_STATUS_INVALID;
//	}
	return 0;
}
/* Adding this as requested by HAL. Can be removed if HAL is saving af mode */
static int m10mo_get_af_mode(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 af_mode;

	ret =  m10mo_readb(sd, CATEGORY_ASUS,ASUS_FOCUS_MODE,&af_mode);
	if (ret)
		return ret;
	*status = af_mode;
	return ret;
#if 0
	af_mode = m10mo_af_parameter_transform(sd, af_mode);

	switch (af_mode) {
	case AF_NORMAL:
		*status = EXT_ISP_FOCUS_MODE_NORMAL;
		break;
	case AF_MACRO:
		*status = EXT_ISP_FOCUS_MODE_MACRO;
		break;
	case AF_TOUCH:
		*status = EXT_ISP_FOCUS_MODE_TOUCH_AF;
		break;
	case AF_PREVIEW_CAF:
		*status = EXT_ISP_FOCUS_MODE_PREVIEW_CAF;
		break;
	case AF_MOVIE_CAF:
		*status = EXT_ISP_FOCUS_MODE_MOVIE_CAF;
		break;
	case AF_FACE_CAF:
		*status = EXT_ISP_FOCUS_MODE_FACE_CAF;
		break;
	case AF_TOUCH_MACRO:
		*status = EXT_ISP_FOCUS_MODE_TOUCH_MACRO;
		break;
	case AF_TOUCH_CAF:
		*status = EXT_ISP_FOCUS_MODE_TOUCH_CAF;
		break;
	default:
		return -EINVAL;
	}

	return ret;
#endif
}

static int m10mo_get_exposure_time_numerator(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 numerator;
	ret = m10mo_readl(sd, CATEGORY_EXIF,EXIF_INFO_EXPTIME_NU, &numerator);
	*status = numerator;
	return ret;
}

static int m10mo_get_exposure_time_denominator(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 denominator;
	ret = m10mo_readl(sd, CATEGORY_EXIF,EXIF_INFO_EXPTIME_DE, &denominator);
	*status = denominator;
	return ret;
}

static int m10mo_get_preview_exposure_time_numerator(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 numerator;
	ret = m10mo_readl(sd, CATEGORY_ASUS,ASUS_PREVIEW_INFO_EXP_NUMERATOR, &numerator);
	*status = numerator;
	return ret;
}

static int m10mo_get_preview_exposure_time_denominator(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 denominator;
	ret = m10mo_readl(sd, CATEGORY_ASUS,ASUS_PREVIEW_INFO_EXP_DENOMINATOR, &denominator);
	*status = denominator;
	return ret;
}

static int m10mo_get_preview_iso(struct v4l2_subdev *sd, unsigned int *status)
{
	int ret;
	u32 denominator;
	ret = m10mo_readw(sd, CATEGORY_ASUS,ASUS_PREVIEW_ISO, &denominator);
	*status = denominator;
	return ret;
}

static int m10mo_set_digital_zoom_position(struct v4l2_subdev *sd, u32 data)
{
    int ret = 0;
	u32 val = 0;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	pr_info("%s, data is %d \n", __func__, data);

    if(data < 1 || data > 31 ){
	   pr_info("m10mo, wrong digital zoom data is %d \n", data);
	   return -EINVAL;;
	}

	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set digital-zoom during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}

	ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
	if(val != 0x20){
	    pr_info("%s, illegal to set digital-zoom because steps of optical-zoom is %d, break!\n", __func__, val);
		return -EINVAL;
	}
	ret = m10mo_readb(sd, CATEGORY_MONITOR, DIGIT_ZOOM, &val);

	if(val == data){
	    pr_info("m10mo, get digital-zoom step is %d, set digital-zoom data is %d. No need to set\n", val, data);
	    return 0;
	}

	//ret = m10mo_request_cmd_effect(sd, M10MO_START_DIGITAL_ZOOM, &data);
	dev->m10mo_request_cmd_lens_data = data;
	queue_delayed_work(dev->auto_update_wq, &digital_zoom_work, 0);
    if (ret)
        return ret;

    return 0;
}

static int m10mo_set_optical_zoom_position(struct v4l2_subdev *sd, u32 data)
{
    int ret = 0;
	u32 val = 0;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	pr_info("%s, data is %d \n", __func__, data);
#if 0
    if(data < 1 || data > 33 ){
	   pr_info("m10mo, wrong optical zoom data is %d \n", data);
	   return -EINVAL;;
	}

	if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	    pr_info("%s, illegal to set AF during M10MO_PARAMETER_MODE, break!\n", __func__);
		return -EINVAL;
	}
#endif

	ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
#if 0
	if(val == data){
	    pr_info("m10mo, get zoom step is %d, set zoom data is %d. No need to set\n", val, data);
	    return 0;
	}
#endif
//	dev->les_wq_requested_cmd = M10MO_START_OPTICAL_ZOOM;
	dev->m10mo_request_cmd_lens_data = data;
//	cancel_delayed_work(&lens_work);
//	flush_workqueue(dev->optical_zoom_wq);
	pr_info("%s, Before adding into workqueue. \n", __func__);
	printk("@%s %d, 009 Before queue_delayed_work(dev->lens_wq, &optical_zoom_work). \n", __func__, __LINE__);
    queue_delayed_work(dev->lens_wq, &optical_zoom_work, 0);
//	ret = m10mo_request_cmd_effect(sd, M10MO_START_OPTICAL_ZOOM, &data);
    if (ret)
        return ret;


    return 0;
}
extern int set_torch_on_via_SOC(bool enable);
static int m10mo_set_flash_mode(struct v4l2_subdev *sd, unsigned int val)
{
	int ret = 0;
#if 1
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	/* by default force the torch off, value depends on incoming flash mode */
	dev->monitor_params.torch = LED_TORCH_OFF;

	switch (val) {
	case EXT_ISP_FLASH_MODE_OFF:
		dev->monitor_params.flash_mode = FLASH_MODE_OFF;
		if(dev->m10mo_mode == M10MO_PARAMETER_MODE) {
		    printk("@%s %d, parameter mode.\n", __func__, __LINE__);
		    set_torch_on_via_SOC(0);
		}
		break;
	case EXT_ISP_FLASH_MODE_ON:
		dev->monitor_params.flash_mode = FLASH_MODE_ON;
		break;
	case EXT_ISP_FLASH_MODE_AUTO:
		dev->monitor_params.flash_mode = FLASH_MODE_AUTO;
		break;
	case EXT_ISP_LED_TORCH_OFF:
		dev->monitor_params.flash_mode = LED_TORCH_OFF;
		dev->monitor_params.torch = LED_TORCH_OFF;
		if(dev->m10mo_mode == M10MO_PARAMETER_MODE) {
		    printk("@%s %d, parameter mode.\n", __func__, __LINE__);
		    set_torch_on_via_SOC(0);
		}
		break;
	case EXT_ISP_LED_TORCH_ON:
		dev->monitor_params.flash_mode = LED_TORCH_ON;
		dev->monitor_params.torch = LED_TORCH_ON;
		if(dev->m10mo_mode == M10MO_PARAMETER_MODE) {
		    printk("@%s %d, parameter mode.\n", __func__, __LINE__);
		    set_torch_on_via_SOC(1);
		}
		break;
	default:
		return -EINVAL;
	}
#if 0 //It seems no need anymore.
	/* Only apply setting if we are in monitor mode */
	if (!is_m10mo_in_monitor_mode(sd))
		return ret;
#endif
	dev_info(&client->dev, "%s: In monitor mode, set flash mode to %d",
		 __func__, dev->monitor_params.flash_mode);

	/* TODO get current flash mode, and apply new setting only when needed? */

	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_FLASH,dev->monitor_params.flash_mode);

#endif
	return ret;
}

static int m10mo_set_lens_position(struct v4l2_subdev *sd)
{
    int ret = 0;
	pr_info("%s\n", __func__);

    ret = m10mo_request_cmd_effect(sd, M10MO_MOVE_LENS_TO_PR, NULL);
    return ret;
}

static int power_up(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret;

	pr_info("%s\n", __func__);

	ret = gpio_request(52, "flash_enable");
	gpio_direction_output(52, 1);
	printk("Higher the GPIO52 for camera on\n");

	if (dev->pdata->common.power_ctrl) {
		ret = dev->pdata->common.power_ctrl(sd, 1);
		if (ret)
			goto fail_power_ctrl;
	}

	if (dev->pdata->common.flisclk_ctrl) {
		ret = dev->pdata->common.flisclk_ctrl(sd, 1);
		if (ret)
			goto fail_clk_off;
	}

	/**ISP RESET**/
	ret = dev->pdata->common.gpio_ctrl(sd, 1);
	if (ret)
		goto fail_power_off;
	return 0;

fail_power_off:
	dev->pdata->common.gpio_ctrl(sd, 0);
fail_clk_off:
	if (dev->pdata->common.flisclk_ctrl)
		ret = dev->pdata->common.flisclk_ctrl(sd, 0);
fail_power_ctrl:
	if (dev->pdata->common.power_ctrl)
		ret = dev->pdata->common.power_ctrl(sd, 0);
	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	pr_info("%s\n", __func__);

	ret = gpio_request(52, "flash_enable");
	gpio_direction_output(52, 0);
	printk("Lower the GPIO52 for camera off\n");

	ret = dev->pdata->common.gpio_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "gpio failed\n");

	/* Even if the first one fails we still want to turn clock off */
	if (dev->pdata->common.flisclk_ctrl) {
		ret = dev->pdata->common.flisclk_ctrl(sd, 0);
		if (ret)
			dev_err(&client->dev, "stop clock failed\n");
	}

	if (dev->pdata->common.power_ctrl) {
		ret = dev->pdata->common.power_ctrl(sd, 0);
		if (ret)
			dev_err(&client->dev, "power off fail\n");
	}
	return ret;
}

static int __m10mo_bootrom_mode_start(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_adapter *adapter = client->adapter;
	u32 dummy;
	int ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;
	/* Dummy read to verify I2C functionality */
	ret = m10mo_readl(sd, CATEGORY_FLASHROM,  REG_FLASH_ADD, &dummy);
	printk(KERN_INFO "__m10mo_bootrom_mode_start, dummy is %d \n", dummy);
	if (ret < 0)
		dev_err(&client->dev, "Dummy I2C access fails\n");

	return ret;
}

int __m10mo_s_power(struct v4l2_subdev *sd, int on, bool fw_update_mode)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 read_val;
	int ret;

    struct i2c_client *client = v4l2_get_subdevdata(sd);

	pr_info("%s: on: %d, fw_update_mode is %d\n", __func__, on, fw_update_mode);
    dev->m10mo_mode = M10MO_PARAMETER_MODE;
	if (dev->power == on)
		return 0;

	dev->stream = 0;
    m10mo_boot_state = 0;
	num_LLS = 0;
	m10mo_capture_pre_flag = 0;
	last_optical_zoom_step = 1;
	cancel_zoom_protect = 0;
	dev->lens_state_machine = LENS_IDLE;
	dev->lock_i2c_write_flag = 0;
	dev->cancel_irq_flag = 0;
	isCaptureMode = false;
    cancel_delayed_work(&optical_zoom_work);
	cancel_delayed_work(&auto_focus_work);
	__m10mo_reset_irq_wait_status();//(dev, &lens_work);
//	if (dev->m10mo_request_cmd_lock_flag == 1){
	mutex_unlock(&dev->m10mo_request_cmd_lock);
	printk("@%s %d, UNLOCK m10mo_request_cmd_lock.\n", __func__, __LINE__);
//		dev->m10mo_request_cmd_lock_flag = 0;
//    }

	if (on) {
	    if(dev->disable_irq_flag){
	        enable_irq(client->irq);
			dev->disable_irq_flag = 0;
		}
//		dev->cmd = M10MO_POWERING_ON;
		ret = power_up(sd);
		if (ret)
			return ret;
		dev->power = 1;

		ret = __m10mo_bootrom_mode_start(sd);
		if (ret)
			goto startup_failure;

		if (!fw_update_mode) {

			ret = __m10mo_fw_start(sd);
			if (ret)
				goto startup_failure;

		ret = m10mo_request_cmd_effect(sd, M10MO_HEAD_HALF_HOME_SEARCHING_MODE, NULL);

		//=== For debugging, to see the ISP FW version ===//
		(void) m10mo_readb(sd, 0x00, 0x02, &fw_version_val1);
		(void) m10mo_readb(sd, 0x00, 0x03, &fw_version_val2);
        printk(KERN_INFO "m10mo, FW version is %X%X-%x  \n", fw_version_val1, fw_version_val2,DIT_version);

        //===  Category:0x0D, Byte: 0x26, line blanking (Upper byte) (0x00～0x0F)  ===//
        //===  Category:0x0D, Byte: 0x27, line blanking (Lower byte) (0x00～0xFF)  ===//
		(void) m10mo_writeb(sd, CATEGORY_TEST, 0x26, 0x01);
		(void) m10mo_writeb(sd, CATEGORY_TEST, 0x27, 0xFF);
#ifdef CONFIG_ASUS_FACTORY_MODE
            //=== Only for factory, tell m10mo in fac image now! ===//
            (void) m10mo_writeb(sd, CATEGORY_ASUS, 0x02, 0x01);
#endif
		}
	} else {
	    if (!fw_update_mode) {
		(void) m10mo_readb(sd, CATEGORY_SYSTEM, SYSTEM_SYSMODE, &read_val);
	        if(read_val == M10MO_PARAMETER_MODE_REQUEST_CMD) {  //legally power down, set lens to PR,
		        ret = m10mo_set_lens_position(sd);              //otherwise directly power down.
			    if (ret)
				    goto startup_failure;
            	}
	    }
		asus_camera_flag = 0;
		wake_unlock(&dev->m10mo_wake_lock);
		disable_irq(client->irq);
		dev->disable_irq_flag = 1;
		ret = power_down(sd);
		set_torch_on_via_SOC(0);
		dev->power = 0;
	}

	return ret;

startup_failure:
	power_down(sd);
	dev->power = 0;
	return ret;
}

int m10mo_set_panorama_monitor(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u32 val;

	dev_info(&client->dev,
		"%s mode: %d Width: %d, height: %d, cmd: 0x%x vdis: %d\n",
		__func__, dev->cmd, dev->curr_res_table[dev->fmt_idx].width,
		dev->curr_res_table[dev->fmt_idx].height,
		dev->curr_res_table[dev->fmt_idx].command,
		(int)dev->curr_res_table[dev->fmt_idx].vdis);

	/* Check if m10mo already streaming @ required resolution */
	ret = m10mo_readb(sd, CATEGORY_PARAM,  PARAM_MON_SIZE, &val);
	if (ret)
		goto out;

	/* If mode is monitor mode and size same, do not configure again*/
	if (dev->cmd == M10MO_MONITOR_MODE_PANORAMA &&
		val == dev->curr_res_table[dev->fmt_idx].command) {
		dev_info(&client->dev,
			"%s Already streaming with required size\n", __func__);
		return 0;
	}

	if (dev->cmd != M10MO_CAMERA_START &&
		dev->cmd != M10MO_PARAMETER_MODE_REQUEST_CMD) {
		/* Already in panorama mode. So swith to parameter mode */
		ret = __m10mo_param_mode_set(sd);
		if (ret)
			goto out;
	}

	/* Change the Monitor Size */
	ret = m10mo_write(sd, 1, CATEGORY_PARAM, PARAM_MON_SIZE,
			dev->curr_res_table[dev->fmt_idx].command);
	if (ret)
		goto out;

	/* Set Panorama mode */
	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, CAPTURE_MODE,
			CAP_MODE_PANORAMA);
	if (ret)
		goto out;

	/* Setting output to NV12/NV21. */
	ret = m10mo_writeb(sd, CATEGORY_PARAM, OUTPUT_FMT_SELECT,
			OUTPUT_FMT_SELECT_NV12NV21);
	if (ret)
		goto out;

	/* Select either NV12 or NV21 based on the format set from user space */
	val = dev->format.code == V4L2_MBUS_FMT_CUSTOM_NV21 ?
		CHOOSE_NV12NV21_FMT_NV21 : CHOOSE_NV12NV21_FMT_NV12;
	ret = m10mo_writeb(sd, CATEGORY_PARAM, CHOOSE_NV12NV21_FMT, val);
	if (ret)
		goto out;

	/* Enable metadata (the command sequence PDF-example) */
	ret = m10mo_writeb(sd, CATEGORY_PARAM, MON_METADATA_SUPPORT_CTRL,
			MON_METADATA_SUPPORT_CTRL_EN);
	if (ret)
		goto out;

	/* Enable interrupt signal */
	ret = m10mo_writeb(sd, CATEGORY_SYSTEM, SYSTEM_INT_ENABLE, 0x01);
	if (ret)
		goto out;

	/* Go to Panorama Monitor mode */
	ret = m10mo_request_cmd_effect(sd, M10MO_MONITOR_MODE_PANORAMA, NULL);
	if (ret)
		goto out;

	ret = m10mo_wait_mode_change(sd, M10MO_MONITOR_MODE_PANORAMA,
				M10MO_INIT_TIMEOUT);
	if (ret < 0)
		goto out;

	return 0;
out:
	dev_err(&client->dev, "m10mo_set_panorama_monitor failed %d\n", ret);
	return ret;
}

int m10mo_set_zsl_monitor(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
//	int mode = M10MO_GET_RESOLUTION_MODE(dev->fw_type);
    const struct m10mo_resolution *capture_res =
			resolutions[M10MO_MODE_CAPTURE_INDEX];
	int ret;
	u32 read_val = 1;
	u32 polling_times = 10;
    /*u32 fmt;*/

	previous_preview_cmd = dev->curr_res_table[dev->fmt_idx].command;
	dev_info(&client->dev,
		"%s dev->cmd: %d width: %d, height: %d, cmd: 0x%x vdis: %d\n",
		__func__, dev->cmd, dev->curr_res_table[dev->fmt_idx].width,
		dev->curr_res_table[dev->fmt_idx].height,
		dev->curr_res_table[dev->fmt_idx].command,
		(int)dev->curr_res_table[dev->fmt_idx].vdis);

	dev_info(&client->dev, "%s capture width: %d, height: %d, cmd: 0x%x\n",
		__func__, capture_res[dev->capture_res_idx].width,
		capture_res[dev->capture_res_idx].height,
		capture_res[dev->capture_res_idx].command);
/*
	if (dev->cmd != M10MO_CAMERA_START &&
		dev->cmd != M10MO_PARAMETER_MODE_REQUEST_CMD &&
        dev->cmd != M10MO_HEAD_HALF_HOME_SEARCHING_MODE) {
		//
		// At this stage means we are already at ZSL. So switch to
		// param mode first and reset all the parameters.
		//

		ret = __m10mo_param_mode_set(sd);
		if (ret)
			goto out;
	}
*/
    //=== For  monitor size 1920x1440, add line-blanking to 0x122, other sizes to 0x1FF ===//
    //=== to avoid buffer-overflow from Intel ISP.                                      ===//
    if(dev->curr_res_table[dev->fmt_idx].command == 0x52) {
         (void) m10mo_writeb(sd, CATEGORY_TEST, 0x26, 0x01);
		 (void) m10mo_writeb(sd, CATEGORY_TEST, 0x27, 0x22);
    } else if(dev->curr_res_table[dev->fmt_idx].command == 0x18
	           || dev->curr_res_table[dev->fmt_idx].command == 0x5E) {
         (void) m10mo_writeb(sd, CATEGORY_TEST, 0x26, 0x02);
		 (void) m10mo_writeb(sd, CATEGORY_TEST, 0x27, 0x58);
    } else {
         (void) m10mo_writeb(sd, CATEGORY_TEST, 0x26, 0x1);
	     (void) m10mo_writeb(sd, CATEGORY_TEST, 0x27, 0xFF);
    }
    /* Change the Monitor Size */
	if(dev->shot_mode == CAP_HILIGHT && dev->run_mode == CI_MODE_PREVIEW) {
	     if(dev->curr_res_table[dev->fmt_idx].command == 0x28) {
		        (void) m10mo_writeb(sd, CATEGORY_PARAM, PARAM_MON_SIZE, 0x53);
		 }else if(dev->curr_res_table[dev->fmt_idx].command == 0x52) {
		        (void) m10mo_writeb(sd, CATEGORY_PARAM, PARAM_MON_SIZE, 0x52);
		 }
	} else { //=== Binning mode only provided in preview, didn't provided with recording. ===//
	        ret = m10mo_writeb(sd, CATEGORY_PARAM, PARAM_MON_SIZE,
			      dev->curr_res_table[dev->fmt_idx].command);
	        if (ret)
		          goto out;
    }

	/* Set ZSL mode */
	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, CAPTURE_MODE,
				CAP_MODE_INFINITY_ZSL);
	if (ret)
		goto out;



	/* Change the capture size */
	/*ret = m10mo_writeb(sd, CATEGORY_CAPTURE_PARAM, CAPP_MAIN_IMAGE_SIZE,*/
			/*capture_res[dev->capture_res_idx].command);*/
	/*if (ret)*/
		/*goto out;*/

	/* Select monitor/movie mode */
	//=== Here always use Monitor mode since Intel ISP are not available to use virtual channel. ===///

	ret = m10mo_write(sd, 1, CATEGORY_PARAM, MOVIE_MODE, 0x00);
	if (ret)
		goto out;

	/* vdis on/off */
	/*m10mo_writeb(sd, CATEGORY_MONITOR, PARAM_VDIS,*/
			 /*dev->curr_res_table[dev->fmt_idx].vdis ? 0x01 : 0x00);*/

	/* Select either NV12 or NV21 based on the format set from user space */
	/*fmt = dev->format.code == V4L2_MBUS_FMT_CUSTOM_NV21 ?*/
		/*CHOOSE_NV12NV21_FMT_NV21 : CHOOSE_NV12NV21_FMT_NV12;*/
	/*ret = m10mo_writeb(sd, CATEGORY_PARAM, CHOOSE_NV12NV21_FMT, fmt);*/
	/*if (ret)*/
		/*goto out;*/


	/* Go to ZSL Monitor mode */
	ret = m10mo_request_cmd_effect(sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
	if (ret)
		goto out;

	ret = m10mo_writeb(sd, CATEGORY_MONITOR, IQCMD_CAP_GRP_NO, 0x02);

	for(polling_times = 10; polling_times > 0; --polling_times) {
	    (void) m10mo_readb(sd, CATEGORY_MONITOR, IQCMD_CAP_GRP_UPDATE,&read_val);
		if(read_val == 0x00){
		   break;
		}
    }
	//For DIT request, enable shot mode for
	(void) m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, dev->shot_mode + PRE_AUTO);
	return 0;
out:
	dev_err(&client->dev, "m10mo_set_zsl_monitor failed %d\n", ret);
	return ret;
}

static u32 __get_dual_capture_value(u8 capture_mode)
{
	switch(capture_mode) {
	case M10MO_CAPTURE_MODE_ZSL_BURST:
		return DUAL_CAPTURE_BURST_CAPTURE_START;
	case APP_LLS_CAP_MODE_ZSL:
		return DUAL_CAPTURE_LLS_CAPTURE_START;
	case M10MO_CAPTURE_MODE_ZSL_NORMAL:
		return DUAL_CAPTURE_ZSL_CAPTURE_START;
	case APP_HDR_CAP_MODE_ZSL:
	case M10MO_CAPTURE_MODE_ZSL_RAW:
	default:
		return DUAL_CAPTURE_SINGLE_CAPTURE_START;
	}
}

static int m10mo_set_zsl_capture(struct v4l2_subdev *sd, int sel_frame)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret, i;
	u32 val, dual_status;
	int finish = 0;

	/* TODO: Fix this. Currently we do not use this */
	(void) sel_frame;

	printk("@%s %d Setting ZSL capture \n", __func__, __LINE__);
	val = __get_dual_capture_value(dev->capture_mode);

	/* Check dual capture status before the capture request */
	for (i = POLL_NUM; i; i--) {
		ret = m10mo_readb(sd, CATEGORY_CAPTURE_CTRL, START_DUAL_STATUS,
				&dual_status);
		if (ret)
			continue;

		if ((dual_status == 0) || (dual_status == DUAL_STATUS_AF_WORKING)) {
			finish = 1;
			break;
		}

		msleep(10);
	}

	/*
	* If last capture not finished yet, return error code
	*/
	if (!finish) {
		dev_err(&client->dev, "%s Device busy. Status check failed %d\n",
			__func__, dual_status);
		return -EBUSY;
	}

	/* Start capture, JPEG encode & transfer start */
	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, START_DUAL_CAPTURE, val);
	dev_dbg(&client->dev, "%s zsl capture trigger result: %d\n",
			__func__, ret);
	return ret;
}

int m10mo_set_zsl_raw_capture(struct v4l2_subdev *sd)
{
	int ret;

	printk("@%s %d Setting Raw capture \n", __func__, __LINE__);
	/* Set capture mode - Infinity capture 3 */
	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, CAPTURE_MODE,
						CAP_MODE_INFINITY_ZSL);
	if (ret)
		return ret;

	/* Enable interrupt signal */
	ret = m10mo_writeb(sd, CATEGORY_SYSTEM, SYSTEM_INT_ENABLE, 0x01);
	if (ret)
		return ret;

	/* Go to ZSL Monitor mode */
	ret = m10mo_request_cmd_effect(sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
	if (ret)
		return ret;

	ret = m10mo_wait_mode_change(sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD,
						M10MO_INIT_TIMEOUT);
	if (ret < 0)
		return ret;

	/* switch to RAW capture mode */
	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, REG_CAP_NV12_MODE,
						RAW_CAPTURE);
	if (ret)
		return ret;

	/* RAW mode is set. Now do a normal capture */
	return m10mo_set_zsl_capture(sd, 1);
}

int m10mo_set_burst_mode(struct v4l2_subdev *sd, unsigned int val)
{
#if 0
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret = 0;

	switch(val) {
	case EXT_ISP_BURST_CAPTURE_CTRL_START:
		/* First check if already in ZSL monitor mode. If not start */
		if (dev->cmd != M10MO_MONITOR_MODE_ZSL_REQUEST_CMD) {
			ret = m10mo_set_zsl_monitor(sd);
			if (ret)
				return ret;
		}
		/* set cap mode to burst so that ZSL cap can differentiate */
		dev->capture_mode = M10MO_CAPTURE_MODE_ZSL_BURST;
		return m10mo_set_zsl_capture(sd, 1);
	case EXT_ISP_BURST_CAPTURE_CTRL_STOP:
		if (dev->capture_mode != M10MO_CAPTURE_MODE_ZSL_BURST)
			return 0;
		/* Stop the burst capture */
		ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL,
			START_DUAL_CAPTURE, DUAL_CAPTURE_BURST_CAPTURE_STOP);
		if (ret)
			return ret;
		/* captutre mode back to normal */
		dev->capture_mode = M10MO_CAPTURE_MODE_ZSL_NORMAL;
		break;
	default:
		return -EINVAL;
	}
#endif
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	if(!val){
	    dev->capture_mode &= ~M10MO_CAPTURE_MODE_ZSL_BURST;
	}else{
        dev->capture_mode |= M10MO_CAPTURE_MODE_ZSL_BURST;
	}
	return 0;
}

static int m10mo_set_lls_mode(struct v4l2_subdev *sd, unsigned int val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	if(!val){
	    dev->capture_mode &= ~APP_LLS_CAP_MODE_ZSL;
	}else{
        dev->capture_mode |= APP_LLS_CAP_MODE_ZSL;
	}
	return 0;

}

static int m10mo_set_hdr_mode(struct v4l2_subdev *sd, unsigned int val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
/*
	switch(val) {
	case STOP_HDR_MODE:
		//=== switch to normal capture. HDR MODE off ===//
		printk(KERN_INFO "@m10mo_set_hdr_mode, STOP_HDR_MODE\n");
		dev->capture_mode = M10MO_CAPTURE_MODE_ZSL_NORMAL;
		break;
	case START_HDR_MODE:
		//=== switch to HDR mode ===//
		printk(KERN_INFO "@m10mo_set_hdr_mode, START_HDR_MODE\n");
		dev->capture_mode = APP_HDR_CAP_MODE_ZSL;
		break;
	case RESUME_PREVIEW_IN_HDR_MODE:
		//=== switch to HDR mode ===//
		printk(KERN_INFO "@m10mo_set_hdr_mode, RESUME_PREVIEW_IN_HDR_MODE\n");
		dev->capture_mode = APP_HDR_CAP_MODE_ZSL;
		break;
	default:
		return -EINVAL;
	}
*/
    if(!val){
	    dev->capture_mode &= ~APP_HDR_CAP_MODE_ZSL;
	}else{
        dev->capture_mode |= APP_HDR_CAP_MODE_ZSL;
	}
	return 0;
}
/*
#define EXT_ISP_VIDEO_MODE_NORMAL 40
#define EXT_ISP_VIDEO_MODE_PRO 41
#define EXT_ISP_VIDEO_MODE_MMS 42
#define EXT_ISP_VIDEO_MODE_MINIATURE 43
#define EXT_ISP_VIDEO_MODE_SLOWMOTION 44
#define EXT_ISP_VIDEO_MODE_HISPEED 45
#define EXT_ISP_VIDEO_MODE_TIMELAPSE 46
#define EXT_ISP_VIDEO_MODE_LOWLIGHT 47
#define EXT_ISP_VIDEO_MODE_EFFECT 48
*/
static u32 movie_scene_mode_cmds (u32 val){
    switch (val) {
        case EXT_ISP_VIDEO_MODE_NORMAL:
		return VID_NORMAL;

        case EXT_ISP_VIDEO_MODE_PRO:
		return VID_NORMAL;

        case EXT_ISP_VIDEO_MODE_MMS:
        return VID_MMS;

        case EXT_ISP_VIDEO_MODE_MINIATURE:
	    return VID_MINIATURE;

        case EXT_ISP_VIDEO_MODE_SLOWMOTION:
		return VID_SLOWMOTION;

        case EXT_ISP_VIDEO_MODE_HISPEED:
		return VID_HISPEED;

        case EXT_ISP_VIDEO_MODE_TIMELAPSE:
		return VID_TIMELAPSE;

        case EXT_ISP_VIDEO_MODE_LOWLIGHT:
		return VID_HILIGHT;

        case EXT_ISP_VIDEO_MODE_EFFECT:
		return VID_NORMAL;
    }
	return 0;
}
static int m10mo_set_shot_mode(struct v4l2_subdev *sd, unsigned int val)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 movie_cmd;
	printk("@%s %d val is %d \n", __func__, __LINE__, val);

	if(val >= 40) {
        movie_cmd = movie_scene_mode_cmds(val);
	    return m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, movie_cmd);
	}

	switch (val) {
    case EXT_ISP_SHOT_MODE_PRO:
	    dev->shot_mode = CAP_AUTO;
		break;
	case EXT_ISP_SHOT_MODE_NORMAL:
	    dev->shot_mode = CAP_AUTO;
	    break;
	case EXT_ISP_SHOT_MODE_EFFECT:
	    dev->shot_mode = CAP_AUTO;
	    break;
	case EXT_ISP_VIDEO_MODE_TIMELAPSE:
	    dev->shot_mode = CAP_AUTO;
		break;
	case EXT_ISP_SHOT_MODE_SUPERRESOLUTION:
	    dev->shot_mode = CAP_AUTO;
	    break;
	case EXT_ISP_SHOT_MODE_BEAUTY_FACE:
	    dev->shot_mode = CAP_BEAUTY;
	    break;
	case EXT_ISP_SHOT_MODE_BEST_FACE:
		dev->shot_mode = CAP_ALLSMILES;
	    break;
	case EXT_ISP_SHOT_MODE_RICH_TONE_HDR:
	    dev->shot_mode = CAP_HDR;
	    break;
	case EXT_ISP_SHOT_MODE_LOWLIGHT:
	    dev->shot_mode = CAP_HILIGHT;
	    break;
	case EXT_ISP_SHOT_MODE_NIGHT:
	    dev->shot_mode = CAP_NIGHT;
		break;
	case EXT_ISP_SHOT_MODE_DEFOCUS:
	    dev->shot_mode = CAP_DEFOCUS;
	    break;
	case EXT_ISP_SHOT_MODE_SELFIE:
	    dev->shot_mode = CAP_SELFIE;
		break;
	case EXT_ISP_SHOT_MODE_ANIMATED_PHOTO:
	    dev->shot_mode = CAP_GIF;
		break;
	case EXT_ISP_SHOT_MODE_PANORAMA:
	    dev->shot_mode = CAP_PANORAMA;
		break;
	case EXT_ISP_SHOT_MODE_MINIATURE:
	    dev->shot_mode = CAP_MINIATURE;
		break;
	case EXT_ISP_SHOT_MODE_ERASER:
	    dev->shot_mode = CAP_SMARTREMOVE;
		break;
	case EXT_ISP_SHOT_MODE_TIMEREWIND:
	    dev->shot_mode = CAP_TIMEREWIND;
		break;
	case EXT_ISP_SHOT_MODE_PANOSELFIE:
	    dev->shot_mode = CAP_SPHERE;
		break;
#if 0
	case EXT_ISP_SHOT_MODE_AUTO:
		dev->shot_mode = SHOT_MODE_AUTO;
		break;
	case EXT_ISP_SHOT_MODE_BEAUTY_FACE:
		dev->shot_mode = SHOT_MODE_BEAUTY_FACE;
		break;
	case EXT_ISP_SHOT_MODE_BEST_PHOTO:
		dev->shot_mode = SHOT_MODE_BEST_PHOTO;
		break;
	case EXT_ISP_SHOT_MODE_DRAMA:
		dev->shot_mode = SHOT_MODE_DRAMA;
		break;
	case EXT_ISP_SHOT_MODE_BEST_FACE:
		dev->shot_mode = SHOT_MODE_BEST_FACE;
		break;
	case EXT_ISP_SHOT_MODE_ERASER:
		dev->shot_mode = SHOT_MODE_ERASER;
		break;
	case EXT_ISP_SHOT_MODE_PANORAMA:
		dev->shot_mode = SHOT_MODE_PANORAMA;
		break;
	case EXT_ISP_SHOT_MODE_RICH_TONE_HDR:
		dev->shot_mode = SHOT_MODE_RICH_TONE_HDR;
		break;
	case EXT_ISP_SHOT_MODE_NIGHT:
		dev->shot_mode = SHOT_MODE_NIGHT;
		break;
	case EXT_ISP_SHOT_MODE_SOUND_SHOT:
		dev->shot_mode = SHOT_MODE_SOUND_SHOT;
		break;
	case EXT_ISP_SHOT_MODE_ANIMATED_PHOTO:
		dev->shot_mode = SHOT_MODE_ANIMATED_PHOTO;
		break;
	case EXT_ISP_SHOT_MODE_SPORTS:
		dev->shot_mode = SHOT_MODE_SPORTS;
		break;
#endif
	default:
		return -EINVAL;
	}
	                                 //=== See ASUS_AE_SCENE_MODE_ in m10mo.h ===//
	(void) m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, dev->shot_mode + PRE_AUTO);

	return 0;
}

static int m10mo_s_power(struct v4l2_subdev *sd, int on)
{
	int ret;
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	if(dev->m10mo_gpio_set_after_power_on_before_streaming_flag) {
	    dev->m10mo_gpio_set_after_power_on_before_streaming_flag = 0;
		m10mo_USB_status(0);
	}
	dev->hal_to_driver_power_on = on;
	mutex_lock(&dev->input_lock);
	ret = __m10mo_s_power(sd, on, false);
	mutex_unlock(&dev->input_lock);

	return ret;
}

int m10mo_single_capture_process(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret = 0;
	u32 fmt;
    printk("@%s %d start\n", __func__, __LINE__);

	/* Select frame */
	/*ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL,*/
			  /*CAPC_SEL_FRAME_MAIN, 0x01);*/
	/*if (ret)*/
		/*return ret;*/

	/* Image format */
	if (dev->format.code == V4L2_MBUS_FMT_JPEG_1X8){
		fmt = CAPTURE_FORMAT_JPEG8;
        printk("%s %d, capture format is JPEG8\n", __func__, __LINE__);
    }
	else{
		fmt = CAPTURE_FORMAT_YUV422;
        printk("%s %d, capture format is YUV422\n", __func__, __LINE__);
    }

	/*ret = m10mo_writeb(sd, CATEGORY_CAPTURE_PARAM, CAPP_YUVOUT_MAIN, fmt);*/
	/*if (ret)*/
		/*return ret;*/

	/* Image size */
	/*ret = m10mo_writeb(sd, CATEGORY_CAPTURE_PARAM, CAPP_MAIN_IMAGE_SIZE,*/
			   /*dev->curr_res_table[dev->fmt_idx].command);*/
	/*if (ret)*/
		/*return ret;*/

	/* Start image transfer */
	/*ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL,*/
			  /*CAPC_TRANSFER_START, 0x01);*/

	return ret;
}
#ifndef NOIRQ
static irqreturn_t m10mo_irq_thread(int irq, void *dev_id)
{
	struct v4l2_subdev *sd = (struct v4l2_subdev *)dev_id;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 int_factor;
    printk("%s %d get IRQ, requested_cmd = %d\n", __func__, __LINE__, dev->requested_cmd);

	/* Clear interrupt by reading interrupt factor register */
	(void) m10mo_read(sd, 1, CATEGORY_SYSTEM, SYSTEM_INT_FACTOR,
			  &int_factor);

	dev_info(&client->dev, "INT_FACTOR: 0x%x\n", int_factor);

	switch (dev->requested_cmd) {
	case M10MO_CAMERA_START:
		if (int_factor & REG_INT_STATUS_MODE) {
			dev->cmd = M10MO_CAMERA_START;
		}
		break;
	case M10MO_PARAMETER_MODE_REQUEST_CMD:
		if (int_factor & REG_INT_STATUS_MODE) {
			dev->cmd = M10MO_PARAMETER_MODE_REQUEST_CMD;
		}
		break;
	case M10MO_MONITOR_MODE:
		if (int_factor & REG_INT_STATUS_MODE) {
			dev->cmd = M10MO_MONITOR_MODE;
		}
		break;
	case M10MO_MONITOR_MODE_ZSL_REQUEST_CMD:
		if (int_factor & REG_INT_STATUS_MODE) {
			dev->cmd = M10MO_MONITOR_MODE_ZSL_REQUEST_CMD;
		}
		break;
	case M10MO_SINGLE_CAPTURE_MODE:
		if (int_factor & REG_INT_STATUS_CAPTURE) {
			dev->cmd = M10MO_SINGLE_CAPTURE_MODE;
            openIntelISP = true;
			dev->fw_ops->single_capture_process(sd);
//			gpio_set_value(60, 1);
//			printk(KERN_INFO "m10mo driver, gpio 60 set high\n");
		}
		break;
	case M10MO_BURST_CAPTURE_MODE:
		if (int_factor & REG_INT_STATUS_MODE){
			dev->cmd = M10MO_BURST_CAPTURE_MODE;
        }
		break;
	case M10MO_MONITOR_MODE_HIGH_SPEED:
		if (int_factor & REG_INT_STATUS_MODE) {
			dev->cmd = M10MO_MONITOR_MODE_HIGH_SPEED;
		}
		break;
    case M10MO_HEAD_HALF_HOME_SEARCHING_MODE:
        printk("%s %d, HEAD_HALF, home search finished\n", __func__, __LINE__);
        if (int_factor & 0X06) {
            dev->cmd = M10MO_HEAD_HALF_HOME_SEARCHING_MODE;
        }
        break;
    case M10MO_MOVE_LENS_TO_PR:
        printk("%s %d, set lens to PR finished\n", __func__, __LINE__);
        if (int_factor & 0X06) {
            dev->cmd = M10MO_MOVE_LENS_TO_PR;
        }
        break;

    case M10MO_START_OPTICAL_ZOOM:
        printk("%s %d, 0 optical ZOOM mode start finished\n", __func__, __LINE__);
        if (int_factor & 0x2) {
            printk("%s %d, 1 optical ZOOM mode start finished\n", __func__, __LINE__);
            dev->cmd = M10MO_START_OPTICAL_ZOOM;
        }
        break;
    	case M10MO_START_AF:
        printk("%s %d, AF start finished\n", __func__, __LINE__);
        if (int_factor & 0x2) {
            dev->cmd = M10MO_START_AF;
        }
		break;
	case M10MO_ASUS_ZOOM_CANCEL:
        printk("%s %d, ASUS_ZOOM_CANCEL finished\n", __func__, __LINE__);
        if (int_factor & 0x2) {
            dev->cmd = M10MO_ASUS_ZOOM_CANCEL;
        }
		break;
	case M10MO_ASUS_FOCUS_CANCEL:
        printk("%s %d, ASUS_FOCUS_CANCEL finished\n", __func__, __LINE__);
        if (int_factor & 0x2) {
            dev->cmd = M10MO_ASUS_FOCUS_CANCEL;
        }
        break;
    case M10MO_MANUAL_FOCUS:
        printk("%s %d, MANUAL_FOCUS finished\n", __func__, __LINE__);
        if (int_factor & 0x2) {
            dev->cmd = M10MO_MANUAL_FOCUS;
        }
        break;
    case M10MO_MANUAL_ZOOM:
        printk("%s %d, MANUAL_ZOOM finished\n", __func__, __LINE__);
        if (int_factor & 0x2) {
            dev->cmd = M10MO_MANUAL_ZOOM;
        }
        break;
	case M10MO_START_DIGITAL_ZOOM:
        printk("%s %d, 0 digital ZOOM mode start finished\n", __func__, __LINE__);
        if (int_factor & 0x4) {
            printk("%s %d,  1 digital ZOOM mode start finished\n", __func__, __LINE__);
            dev->cmd = M10MO_START_DIGITAL_ZOOM;
        }
        break;
	case M10MO_WRITE_SHD_TABLE:
        if (int_factor & 0x40) {
            printk("%s %d,  M10MO_WRITE_SHD_TABLE write finished\n", __func__, __LINE__);
            dev->cmd = M10MO_WRITE_SHD_TABLE;
        }
		break;
	case M10MO_BOTTOM_HALF_HOME_SEARCHING_MODE:
	    printk("%s %d, BOTTOM_HALF, home search finished\n", __func__, __LINE__);
        if (int_factor & 0X06) {
		    if(m10mo_boot_state == 1) {
	            m10mo_boot_state = 2;
			    if(isp_update_status != 0) {
			        isp_update_status = M10MO_NO_CMD_REQUEST;
					dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE;
	                dev->cmd = dev->requested_cmd = M10MO_NO_CMD_REQUEST;
					mutex_unlock(&dev->m10mo_request_cmd_lock);
		            printk("@%s %d, UNLOCK m10mo_request_cmd_lock.\n", __func__, __LINE__);
                    wake_unlock(&dev->m10mo_wake_lock);
				}
		    }
		}
		break;
	default:
		dev_err(&client->dev, "m10mo_irq_thread :default case\n");
		return IRQ_HANDLED;
	}
pr_info("m10mo, 1111111111\n");
#if 0
    if (0&&dev->requested_cmd == M10MO_START_AF
         || dev->requested_cmd == M10MO_START_DIGITAL_ZOOM
         || dev->requested_cmd == M10MO_START_OPTICAL_ZOOM
         || dev->requested_cmd == M10MO_MANUAL_FOCUS
         || dev->requested_cmd == M10MO_MANUAL_ZOOM
         || dev->requested_cmd == M10MO_ASUS_ZOOM_CANCEL
         || dev->requested_cmd == M10MO_ASUS_FOCUS_CANCEL) {
#endif

    wake_up(&dev->irq_waitq);
////////////////////////////////////////////////////////////////////////
#if 0
    if (dev->requested_cmd == M10MO_START_AF
         || dev->requested_cmd == M10MO_START_OPTICAL_ZOOM
         || dev->requested_cmd == M10MO_ASUS_ZOOM_CANCEL
         || dev->requested_cmd == M10MO_ASUS_FOCUS_CANCEL) {
		    m10mo_lens_state_machine(sd, M10MO_IRQ_EVENT, NULL);
    }
#endif
///////////////////////////////////////////////////////////////////////////

pr_info("m10mo, 2222222222, m10mo_irq_thread return !\n");
//	printk(KERN_INFO "%s dev->wait_irq_flag is %d \n", __func__, dev->wait_irq_flag);

	return IRQ_HANDLED;
}

static int m10mo_setup_irq(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int pin, ret;

    printk("%s %d start\n", __func__, __LINE__);
	if (!dev->pdata->common.gpio_intr_ctrl) {
		dev_err(&client->dev,
			"Missing gpio information in interrupt setup!\n");
		return -ENODEV;
	}

	pin = dev->pdata->common.gpio_intr_ctrl(sd);
	if (pin < 0) {
		ret = pin;
		goto out;
	}

	ret = gpio_to_irq(pin);

	if (ret < 0) {
		dev_err(&client->dev, "Configure gpio to irq failed!\n");
		goto out;
	}
	client->irq = ret;

	ret = request_threaded_irq(client->irq, NULL, m10mo_irq_thread,
			  IRQF_TRIGGER_RISING | IRQF_ONESHOT, M10MO_NAME, sd);
//	ret = request_threaded_irq(client->irq, NULL, m10mo_irq_thread,
//			  IRQF_TRIGGER_RISING, M10MO_NAME, sd);
	if (ret < 0) {
		dev_err(&client->dev, "Cannot register IRQ: %d\n", ret);
		goto out;
	}
	return 0;

out:
	return ret;
}
#endif
int get_resolution_index(const struct m10mo_resolution *res,
			int entries, u32 w, u32 h)
{
	int i;

	for (i = 0; i < entries; i++) {
		if (w != res[i].width)
			continue;
		if (h != res[i].height)
			continue;
		/* Found it */
		return i;
	}
	return -1;
}

int __m10mo_try_mbus_fmt(struct v4l2_subdev *sd,
			struct v4l2_mbus_framefmt *fmt, bool update_fmt)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct atomisp_input_stream_info *stream_info =
			(struct atomisp_input_stream_info *)fmt->reserved;
	const struct m10mo_resolution *res;
	u32 read_val = 0;
	int entries, idx;
	int mode = M10MO_GET_RESOLUTION_MODE(dev->fw_type);

    printk("@%s %d, format code = 0x%x\n", __func__, __LINE__, fmt->code);
//=== FIXME: Why HAL set so so many different format to driver?
//=== M10MO - temporarily hard code here. Start. ======//
	if (fmt->code != V4L2_MBUS_FMT_JPEG_1X8 &&
	    fmt->code != V4L2_MBUS_FMT_UYVY8_1X16 &&
		fmt->code != V4L2_MBUS_FMT_SGBRG8_1X8 &&
	    fmt->code != V4L2_MBUS_FMT_CUSTOM_NV12 &&
	    fmt->code != V4L2_MBUS_FMT_CUSTOM_NV21 &&
	    fmt->code != V4L2_MBUS_FMT_CUSTOM_M10MO_RAW) {
		printk("%s unsupported code: 0x%x. Set to NV12\n", __func__, fmt->code);
		fmt->code = V4L2_MBUS_FMT_CUSTOM_NV12;
	}
    if(fmt->code == V4L2_MBUS_FMT_SGBRG8_1X8){
	    (void) m10mo_readb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, &read_val);
		if(read_val != RAW_CAP){
			// if (dev->m10mo_request_cmd_lock_flag == 1) {
			// 	mutex_unlock(&dev->m10mo_request_cmd_lock);
			// 	printk("@%s %d, UNLOCK m10mo_request_cmd_lock.\n", __func__, __LINE__);
			// 	dev->m10mo_request_cmd_lock_flag = 0;
		    // }
			// dev->lock_i2c_write_flag = 0;
	        (void)__m10mo_param_mode_set(sd);
			printk(KERN_INFO "@%s, Raw Capture.\n", __func__);
            (void) m10mo_writeb(sd, CATEGORY_PARAM, 0x0a, 0x2a);                           // Set the DataType 0x2a in MIPI Header, it means RAW8.
            (void) m10mo_writeb(sd, CATEGORY_PARAM, 0x18, 0x12);                           // Set the line-unit to 4736.(0x1280)
            (void) m10mo_writeb(sd, CATEGORY_PARAM, 0x19, 0x80);                           // Set the line-unit to 4736.(0x1280)
			(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, RAW_CAP);
			// if (dev->m10mo_request_cmd_lock_flag == 1) {
			// 	mutex_unlock(&dev->m10mo_request_cmd_lock);
			// 	printk("@%s %d, UNLOCK m10mo_request_cmd_lock.\n", __func__, __LINE__);
			// 	dev->m10mo_request_cmd_lock_flag = 0;
		    // }
			// wake_up(&dev->irq_waitq);
			(void) m10mo_request_cmd_effect(sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL); // Enter into m10mo's Monitor mode.
			dev->capture_mode = M10MO_CAPTURE_MODE_ZSL_RAW;
			// dev->lock_i2c_write_flag = 1;
			mdelay(500);
		}
	}else{
        fmt->code = V4L2_MBUS_FMT_UYVY8_1X16;
    }
//=== M10MO - temporarily hard code here. End. ======//
	/* In ZSL case, capture table needs to be handled separately */
    printk("@%s %d, mode = %d, stream = %d, run_mode = 0x%x, fmt->code is 0x%x\n", __func__, __LINE__, mode, stream_info->stream, dev->run_mode, fmt->code);

    if (stream_info->stream == ATOMISP_INPUT_STREAM_CAPTURE &&
			(dev->run_mode == CI_MODE_PREVIEW ||
			 dev->run_mode == CI_MODE_VIDEO ||
			 dev->run_mode == CI_MODE_CONTINUOUS)) {
		res = resolutions[M10MO_MODE_CAPTURE_INDEX];
		entries =
		     resolutions_sizes[M10MO_MODE_CAPTURE_INDEX];
	} else {
		res = dev->curr_res_table;
		entries = dev->entries_curr_table;
	}

	/* check if the given resolutions are spported */
	idx = get_resolution_index(res, entries, fmt->width, fmt->height);
	if (idx < 0) {
		dev_err(&client->dev, "%s unsupported resolution: %dx%d\n",
			__func__, fmt->width, fmt->height);
		return -EINVAL;
	}

	/* If the caller wants to get updated fmt values based on the search */
	if (update_fmt) {
		if (fmt->code == V4L2_MBUS_FMT_JPEG_1X8) {
			fmt->width = dev->mipi_params.jpeg_width;
			fmt->height = dev->mipi_params.jpeg_height;
		} else if (fmt->code == V4L2_MBUS_FMT_CUSTOM_M10MO_RAW) {
			fmt->width = dev->mipi_params.raw_width;
			fmt->height = dev->mipi_params.raw_height;
		} else {
			fmt->width = res[idx].width;
			fmt->height = res[idx].height;
		}
	}
	return idx;
}

static int m10mo_try_mbus_fmt(struct v4l2_subdev *sd,
				struct v4l2_mbus_framefmt *fmt)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int idx;

	mutex_lock(&dev->input_lock);
	idx = dev->fw_ops->try_mbus_fmt(sd, fmt, true);
	mutex_unlock(&dev->input_lock);
	return idx >= 0 ? 0 : -EINVAL;
}

static int m10mo_get_mbus_fmt(struct v4l2_subdev *sd,
				struct v4l2_mbus_framefmt *fmt)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	mutex_lock(&dev->input_lock);

	fmt->width = dev->curr_res_table[dev->fmt_idx].width;
	fmt->height = dev->curr_res_table[dev->fmt_idx].height;
	fmt->code = dev->format.code;

	mutex_unlock(&dev->input_lock);

	return 0;
}

int __m10mo_update_stream_info(struct v4l2_subdev *sd,
			       struct v4l2_mbus_framefmt *fmt)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct atomisp_input_stream_info *stream_info =
			(struct atomisp_input_stream_info*)fmt->reserved;
	int mode = M10MO_GET_RESOLUTION_MODE(dev->fw_type);

	/* TODO: Define a FW Type as well. Resolution could be reused */
	switch (mode) {
	case M10MO_RESOLUTION_MODE_0:
		/* TODO: handle FW type cases here */
		break;
	case M10MO_RESOLUTION_MODE_1:
		/* Raw Capture is a special case. Capture data comes like HDR */
		if (fmt->code == V4L2_MBUS_FMT_JPEG_1X8 ||
			fmt->code == V4L2_MBUS_FMT_CUSTOM_M10MO_RAW) {
			/* fill stream info */
			stream_info->ch_id = M10MO_ZSL_JPEG_VIRTUAL_CHANNEL;
			stream_info->isys_configs = 1;
			stream_info->isys_info[0].input_format =
				(u8)ATOMISP_INPUT_FORMAT_USER_DEF3;
			stream_info->isys_info[0].width = 0;
			stream_info->isys_info[0].height = 0;
		} else if (fmt->code == V4L2_MBUS_FMT_CUSTOM_NV12 ||
			   fmt->code == V4L2_MBUS_FMT_CUSTOM_NV21) {
			stream_info->ch_id = M10MO_ZSL_NV12_VIRTUAL_CHANNEL;
			stream_info->isys_configs = 2;
			/* first stream */
			stream_info->isys_info[0].input_format =
				(u8)ATOMISP_INPUT_FORMAT_USER_DEF1;
			stream_info->isys_info[0].width = (u16)fmt->width;
			stream_info->isys_info[0].height = (u16)fmt->height;
			/* Second stream */
			stream_info->isys_info[1].input_format =
				(u8)ATOMISP_INPUT_FORMAT_USER_DEF2;
			stream_info->isys_info[1].width = (u16)fmt->width;
			stream_info->isys_info[1].height = (u16)fmt->height / 2;
		}
		break;
	}
	return 0;
}

int __m10mo_set_mbus_fmt(struct v4l2_subdev *sd,
			      struct v4l2_mbus_framefmt *fmt)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct atomisp_input_stream_info *stream_info =
			(struct atomisp_input_stream_info *)fmt->reserved;
	int mode = M10MO_GET_RESOLUTION_MODE(dev->fw_type);
	int index;
    printk(KERN_INFO "@%s, start! fmt->width is %d, fmt->height is %d\n", __func__, fmt->width, fmt->height);
	mutex_lock(&dev->input_lock);

	index = dev->fw_ops->try_mbus_fmt(sd, fmt, false);
	if (index < 0) {
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}

	dev->format.code = fmt->code;
	dev->fmt_idx = index;
	if (stream_info->stream == ATOMISP_INPUT_STREAM_CAPTURE) {
		/* Save the index for selecting the capture resolution */
		dev->capture_res_idx = dev->fmt_idx;
	}

	/*
	 * In ZSL Capture cases, for capture an image the run mode is not
	 * changed. So we need to maintain a separate capture table index
	 * to select the snapshot sizes.
	 */
	if (stream_info->stream == ATOMISP_INPUT_STREAM_CAPTURE &&
	    (dev->run_mode == CI_MODE_PREVIEW ||
	     dev->run_mode == CI_MODE_VIDEO ||
	     dev->run_mode == CI_MODE_CONTINUOUS))
		dev->capture_res_idx = dev->fmt_idx;

	dev_info(&client->dev,
		"%s index prev/cap: %d/%d width: %d, height: %d, code; 0x%x\n",
		 __func__, dev->fmt_idx, dev->capture_res_idx, fmt->width,
		 fmt->height, dev->format.code);

	/* Make the fixed width and height for JPEG and RAW formats */
	if (dev->format.code == V4L2_MBUS_FMT_JPEG_1X8) {
		/* The m10mo can only run JPEG in 30fps or lower */
		dev->fps = M10MO_NORMAL_FPS;
		fmt->width = dev->mipi_params.jpeg_width;
		fmt->height = dev->mipi_params.jpeg_height;
	} else if (dev->format.code == V4L2_MBUS_FMT_CUSTOM_M10MO_RAW) {
		fmt->width = dev->mipi_params.raw_width;
		fmt->height = dev->mipi_params.raw_height;
	}

	/* Update the stream info. Atomisp uses this for configuring mipi */
	__m10mo_update_stream_info(sd, fmt);

	/*
	 * Handle raw capture mode separately. Update the capture mode to RAW
	 * capture now. So that the next streamon call will start RAW capture.
	 */
	if (mode == M10MO_RESOLUTION_MODE_1 &&
	    dev->format.code == V4L2_MBUS_FMT_CUSTOM_M10MO_RAW) {
		dev_dbg(&client->dev, "%s RAW capture mode\n", __func__);
		dev->capture_mode = M10MO_CAPTURE_MODE_ZSL_RAW;
		dev->capture_res_idx = dev->fmt_idx;
		dev->fmt_idx = 0;
	}

    if (PREVIEW_TEST_PATTERN) {
        if (fmt->width == 1440 && fmt->height == 1080) {
            m10mo_writeb(sd, 0x01, 0x01, 0x37); // 1440 x 1080
        } else if (fmt->width == 640 && fmt->height == 480) {
            m10mo_writeb(sd, 0x01, 0x01, 0x17);
        }
    }

	mutex_unlock(&dev->input_lock);
	return 0;
}

static int m10mo_set_mbus_fmt(struct v4l2_subdev *sd,
			      struct v4l2_mbus_framefmt *fmt)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	return dev->fw_ops->set_mbus_fmt(sd, fmt);
}

static int m10mo_set_run_mode(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret = 0;
	u32 read_val;

    printk("@%s %d start, capture_mode = %d, run_mode = 0x%x\n", __func__, __LINE__, dev->capture_mode, dev->run_mode);
#if 0
	/*
	 * Handle RAW capture mode separately irrespective of the run mode
	 * being configured. Start the RAW capture right away.
	 */
	if (dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_RAW) {
		/*
		 * As RAW capture is done from a command line tool, we are not
		 * restarting the preview after the RAW capture. So it is ok
		 * to reset the RAW capture mode here because the next RAW
		 * capture has to start from the Set format onwards.
		 */
		dev->capture_mode = M10MO_CAPTURE_MODE_ZSL_NORMAL;
		return m10mo_set_zsl_raw_capture(sd);
	}
#endif
	switch (dev->run_mode) {
	case CI_MODE_STILL_CAPTURE:
	    dev->lock_i2c_write_flag = 0;
	    (void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, CAPC_TRANSFER_START, 0x1);
		dev->lock_i2c_write_flag = 1;
#ifndef off_c51
        ret = m10mo_set_still_capture(sd);
#endif
		break;
    case CI_MODE_PREVIEW:
	case CI_MODE_VIDEO:
            if(!isCaptureMode || dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_BURST){
                printk("@%s %d, preview set monitor mode\n", __func__, __LINE__);
                ret = m10mo_set_zsl_monitor(sd);
            }
            else{
                printk("@%s %d, after capturing, start the preview frame \n", __func__, __LINE__);
				dev->lock_i2c_write_flag = 0;
#if 0
				//=== Hard code to set OIS S1 mode for preview. ===//
//				(void) m10mo_writeb(sd, CATEGORY_TEST, OIS_LIB_API_START, 0x5);
				(void) m10mo_writew(sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0107);
                (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_DATA31_24, 0x0);
	            (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);
#endif
				(void) m10mo_readb(sd, CATEGORY_CAPTURE_CTRL, AFT_CAP_SELECT, &read_val);
				if(read_val == 0x01 || dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_RAW){
					//=== Send command to stop Capture, and start streaming preview frames. ===//
                   	printk("@%s %d Raw capture STOP!\n", __func__, __LINE__);
					(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, START_DUAL_CAPTURE, STOP_RAW_CAP);
                    if(previous_preview_cmd != dev->curr_res_table[dev->fmt_idx].command) {
					    __m10mo_param_mode_set(sd);
					    m10mo_set_zsl_monitor(sd);
					}else {
					    (void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, START_DUAL_CAPTURE, 0x03);
					}
				}
                ret = 0;
            }
		(void) m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, dev->shot_mode + PRE_AUTO);
        isCaptureMode = false;
		break;
	default:
		/* TODO: Revisit this logic on switching to panorama */
		if (dev->curr_res_table[dev->fmt_idx].command == 0x43)
			ret = m10mo_set_panorama_monitor(sd);
		else{
            if(!isCaptureMode){
                printk("@%s %d, preview set monitor mode\n", __func__, __LINE__);
                ret = m10mo_set_zsl_monitor(sd);
            }
            else{
                printk("@%s %d, after capture do not set monitor mode\n", __func__, __LINE__);
                ret = 0;
            }
        }
        isCaptureMode = false;
	}
	return ret;
}

static void m10mo_check_app_cap_mode(struct v4l2_subdev *sd)
{
    struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 read_val;
    printk("@%s %d start, capture_mode = %d, run_mode = 0x%x\n", __func__, __LINE__, dev->capture_mode, dev->run_mode);

	(void) m10mo_readb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, &read_val);
	if(read_val == dev->capture_mode){
	     printk("@%s, already in capture mode: %d, no need to set again. \n", __func__, read_val);
		 return;
	}
	switch(dev->capture_mode){
	case APP_HDR_CAP_MODE_ZSL:
	    printk(KERN_INFO "m10mo, APP HDR capture mode.\n");
	    (void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, APP_HDR_CAP_MODE_ZSL);
	    break;
	case APP_LLS_CAP_MODE_ZSL:
		printk(KERN_INFO "m10mo, APP set %d frame for LLS capture mode.\n",num_LLS);
		(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, 0xC, num_LLS);
	    printk(KERN_INFO "m10mo, APP LLS capture mode.\n");
		(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, APP_LLS_CAP_MODE_ZSL);
		break;
	case M10MO_CAPTURE_MODE_ZSL_NORMAL:
	    printk(KERN_INFO "m10mo, APP Auto capture mode.\n");
		(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, M10MO_CAPTURE_MODE_ZSL_NORMAL);
		break;
	case M10MO_CAPTURE_MODE_ZSL_RAW:
		printk(KERN_INFO "m10mo, APP RAW capture mode.\n");
//	    (void)__m10mo_param_mode_set(sd);
	    (void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, RAW_CAP);
//		(void) m10mo_set_run_mode(sd);
		break;
	default:
	    printk(KERN_INFO "m10mo, erro APP capture mode %d \n", dev->capture_mode);
		break;
	}
}

int m10mo_identify_fw_type(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct m10mo_fw_id *fw_ids = NULL;
	char buffer[M10MO_MAX_FW_ID_STRING];
	int ret;

	int m10mo_fw_address_cnt = m10mo_get_fw_address_count();
	int i;

	ret = dev->pdata->identify_fw();
	if (ret != -1) {
		dev->fw_type = ret;
		return 0;
	}

	for (i = 0; i < m10mo_fw_address_cnt; i++) {
		fw_ids = dev->pdata->fw_ids;
		if (!fw_ids)
			return 0;

		ret = m10mo_get_isp_fw_version_string(dev, buffer,
				sizeof(buffer), i);
		if (ret)
			return ret;

		while (fw_ids->id_string) {
				dev_err(&client->dev, "Find FW id %s detected\n", buffer);
			if (!strncmp(fw_ids->id_string, buffer,
				     strlen(fw_ids->id_string)))
			{
				dev_info(&client->dev, "FW id %s detected\n", buffer);
				dev->fw_type = fw_ids->fw_type;
				dev->fw_addr_id = i;
				return 0;
			}
			fw_ids++;
		}
	}

	dev_err(&client->dev, "FW id string table given but no match found");
	return 0;
}

static void m10mo_mipi_initialization(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int id = M10MO_GET_MIPI_PACKET_SIZE_IDX(dev->fw_type);
	u32 mipi_packet_size = dev->pdata->mipi_packet_size[id];

	dev->mipi_params.jpeg_width = mipi_packet_size;
	dev->mipi_params.jpeg_height = M10MO_MAX_YUV422_SIZE / mipi_packet_size;

	dev->mipi_params.raw_width = mipi_packet_size;
	dev->mipi_params.raw_height = M10MO_MAX_RAW_SIZE / mipi_packet_size;

	dev_dbg(&client->dev,
		"%s JPEG: W=%d H=%d, RAW: W=%d H=%d\n", __func__,
		dev->mipi_params.jpeg_width, dev->mipi_params.jpeg_height,
		dev->mipi_params.raw_width, dev->mipi_params.raw_height);
}
//=========== TEST START ============//
int m10mo_send_still_capture_cmds(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u8 res_cmd;
	u32 read_val;
    openIntelISP = false;
    isCaptureMode = true;
	res_cmd = dev->curr_res_table[dev->fmt_idx].command;
	printk("@%s %d start\n", __func__, __LINE__);

	dev_info(&client->dev, "%s mode: %d width: %d, height: %d, cmd: 0x%x\n",
		__func__, dev->cmd, dev->curr_res_table[dev->fmt_idx].width,
		dev->curr_res_table[dev->fmt_idx].height,
		dev->curr_res_table[dev->fmt_idx].command);

    //=== Set capture resolution command, but no need to set this command during binning capture. ===//
	if(res_cmd != BINNING_CAP_CMD){
	    ret = m10mo_writeb(sd, CATEGORY_CAPTURE_PARAM, PARAM_CAP_SIZE, res_cmd);
    }
	//=== Check "application capture mode" and send command to m10mo if needed. ===//
    m10mo_check_app_cap_mode(sd);

	//=== Choose the action of preview frame after capturing ===//
	// 0x00: start YUV Preview output directly
	// 0x01: start YUV Preview output if CateC/5 = 0x2
//	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, AFT_CAP_SELECT, 0x1);

    //=== Send shot_mode to m10mo for DIT AE. ===//
	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, dev->shot_mode);

	(void) m10mo_readb(sd, CATEGORY_SYSTEM, SYSTEM_SYSMODE, &read_val);
	if(read_val == M10MO_PARAMETER_MODE_REQUEST_CMD){
          printk(KERN_INFO "@%s, In parameter mode, ILLIGAL!!! ILLIGAL!!! ILLIGAL to CAPTURE!!! \n", __func__);
	}else if(read_val == 0x02){
          printk(KERN_INFO "@%s, In monitor mode, It is legal to CAPTURE. \n", __func__);
	}
	(void) m10mo_readb(sd, CATEGORY_MONITOR, IQCMD_CAP_GRP_UPDATE, &read_val);

	m10mo_enable_interrupt(sd, M10MO_SINGLE_CAPTURE_MODE);
    //=== If size is smaller than 1920x1080, need to add delay between INT and capture frame. ===//
    if(0 && res_cmd <= 0x19) {
        ret = m10mo_writeb(sd, 0x04, 0x42, 0x01);
    }
    m10mo_performance_log("CAPTURE_B_KSIGCAP+");
    //=== Send command to capture  ===//
    ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, START_DUAL_CAPTURE, 0x01);

	return ret;
}
//=========== TEST END ==============//
int m10mo_set_still_capture(struct v4l2_subdev *sd)
{
int ret;
#if 0
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u8 res_cmd;
	u32 read_val;
    openIntelISP = false;
    isCaptureMode = true;
	res_cmd = dev->curr_res_table[dev->fmt_idx].command;
	printk("@%s %d start\n", __func__, __LINE__);

	dev_info(&client->dev, "%s mode: %d width: %d, height: %d, cmd: 0x%x\n",
		__func__, dev->cmd, dev->curr_res_table[dev->fmt_idx].width,
		dev->curr_res_table[dev->fmt_idx].height,
		dev->curr_res_table[dev->fmt_idx].command);

    //=== Set capture resolution command, but no need to set this command during binning capture. ===//
	if(res_cmd != BINNING_CAP_CMD){
	    ret = m10mo_writeb(sd, CATEGORY_CAPTURE_PARAM, PARAM_CAP_SIZE, res_cmd);
    }
	//=== Check "application capture mode" and send command to m10mo if needed. ===//
    m10mo_check_app_cap_mode(sd);

	//=== Choose the action of preview frame after capturing ===//
	// 0x00: start YUV Preview output directly
	// 0x01: start YUV Preview output if CateC/5 = 0x2
//	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, AFT_CAP_SELECT, 0x1);

    //=== Send shot_mode to m10mo for DIT AE. ===//
	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, dev->shot_mode);

	(void) m10mo_readb(sd, CATEGORY_SYSTEM, SYSTEM_SYSMODE, &read_val);
	if(read_val == M10MO_PARAMETER_MODE_REQUEST_CMD){
          printk(KERN_INFO "@%s, In parameter mode, ILLIGAL!!! ILLIGAL!!! ILLIGAL to CAPTURE!!! \n", __func__);
	}else if(read_val == 0x02){
          printk(KERN_INFO "@%s, In monitor mode, It is legal to CAPTURE. \n", __func__);
	}
#endif
	//=== Send command to capture  ===//
    ret = m10mo_request_cmd_effect(sd, M10MO_SINGLE_CAPTURE_MODE, NULL);

	return ret;
}

int m10mo_set_burst_capture_streamon(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u8 res_burst_cap_main_size_cmd;
	u8 res_burst_cap_monitor_size_cmd;

	res_burst_cap_main_size_cmd = dev->curr_res_table[dev->fmt_idx].command;
	res_burst_cap_monitor_size_cmd = dev->curr_res_table[dev->fmt_idx].burst_capture_monitor_size_command;
	printk("@%s %d start\n", __func__, __LINE__);

	dev_info(&client->dev, "%s mode: %d width: %d, height: %d, cmd: 0x%x\n",
		__func__, dev->cmd, dev->curr_res_table[dev->fmt_idx].width,
		dev->curr_res_table[dev->fmt_idx].height,
		dev->curr_res_table[dev->fmt_idx].command);

    //=== Need to set monitor size during burst capture. ===//
	ret = m10mo_writeb(sd, CATEGORY_PARAM, PARAM_MON_SIZE, res_burst_cap_monitor_size_cmd);
	if(ret) goto fail_out;

    //=== Set capture resolution command, but no need to set this command during binning capture. ===//
	if(res_burst_cap_main_size_cmd != BINNING_CAP_CMD){
	    ret = m10mo_writeb(sd, CATEGORY_CAPTURE_PARAM, PARAM_CAP_SIZE, res_burst_cap_main_size_cmd);
		if(ret) goto fail_out;
    }

	/* Set ZSL mode */
	ret = m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, CAPTURE_MODE, CAPTURE_MODE);
	if (ret) goto fail_out;

	//=== Send shot_mode to m10mo for DIT AE. ===//
	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, dev->shot_mode);

	/* Go to ZSL Monitor mode */
	ret = m10mo_request_cmd_effect(sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
	if (ret) goto fail_out;

	return ret;

fail_out:
    return ret;
}

static int m10mo_burst_run(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret = 0;

    printk("@%s %d start, capture_mode = %d, run_mode = 0x%x\n", __func__, __LINE__, dev->capture_mode, dev->run_mode);

	switch (dev->run_mode) {
	case CI_MODE_STILL_CAPTURE:
        m10mo_performance_log("CAPTURE_B_KBURSTCAP+");
        ret = m10mo_set_burst_capture_streamon(sd);
        m10mo_performance_log("CAPTURE_B_KBURSTCAP-");
		break;
    case CI_MODE_PREVIEW:
        printk("@%s %d, preview set monitor mode\n", __func__, __LINE__);
        ret = m10mo_set_zsl_monitor(sd);
		break;
	default:
		/* TODO: Revisit this logic on switching to panorama */
		if (dev->curr_res_table[dev->fmt_idx].command == 0x43)
			ret = m10mo_set_panorama_monitor(sd);
		else{
            if(!isCaptureMode){
                printk("@%s %d, preview set monitor mode\n", __func__, __LINE__);
                ret = m10mo_set_zsl_monitor(sd);
            }
            else{
                printk("@%s %d, after capture do not set monitor mode\n", __func__, __LINE__);
                ret = 0;
            }
        }
	}
	return ret;
}

int m10mo_normal_streamoff(struct v4l2_subdev *sd)
{
//    struct m10mo_device *dev = to_m10mo_sensor(sd);
    pr_info("%s, m10mo_capture_pre_flag is %d", __func__, m10mo_capture_pre_flag);

    switch(m10mo_capture_pre_flag){
    case M10MO_NOT_CAPTURE_0:
	    return __m10mo_param_mode_set(sd);

	case M10MO_CAP_BEFORE_1ST_STREAMOFF_1:
	    pr_info("m10mo, 1st fake stream off during capture\n");
		m10mo_capture_pre_flag = M10MO_CAP_BETWEEN_1ST_AND_2ND_STREAMOFF_2;
//		previous_preview_cmd = dev->curr_res_table[dev->fmt_idx].command;
		return 0;

    case M10MO_CAP_BETWEEN_1ST_AND_2ND_STREAMOFF_2:
    	m10mo_performance_log("CAPTURE_B_KCAPSTROFF");
	    pr_info("m10mo, 2nd fake stream off during capture\n");
		m10mo_capture_pre_flag = M10MO_NOT_CAPTURE_0;
		return 0;

	default:
        return -EIO;
	}

}

static int m10mo_burst_streamoff(struct v4l2_subdev *sd)
{
	pr_info("%s, m10mo_capture_pre_flag is %d", __func__, m10mo_capture_pre_flag);

    switch(m10mo_capture_pre_flag){
    case M10MO_NOT_CAPTURE_0:
	    break;

	case M10MO_CAP_BEFORE_1ST_STREAMOFF_1:
		printk("@%s 1st real stream off during burst capture. \n", __func__);
		m10mo_capture_pre_flag = M10MO_CAP_BETWEEN_1ST_AND_2ND_STREAMOFF_2;
		break;

    case M10MO_CAP_BETWEEN_1ST_AND_2ND_STREAMOFF_2:
    	m10mo_performance_log("CAPTURE_B_KBURSTSTROFF");
		printk("@%s 2nd real stream off during burst capture. \n", __func__);
		m10mo_capture_pre_flag = M10MO_NOT_CAPTURE_0;
		break;

	default:
        return -EIO;
	}
	return __m10mo_param_mode_set(sd);

}

int m10mo_test_pattern(struct v4l2_subdev *sd, u8 val)
{

	struct m10mo_device *dev = to_m10mo_sensor(sd);
	pr_info("%s, run mode: %x\n", __func__, dev->run_mode);

	m10mo_writeb(sd, CATEGORY_TEST, 0xEE, val);

	return 0;
}

static const struct m10mo_fw_ops fw_ops = {
	.set_run_mode           = m10mo_set_run_mode,
	.set_burst_mode         = m10mo_set_burst_mode,
	.stream_off             = m10mo_normal_streamoff,
	.single_capture_process = m10mo_single_capture_process,
	.try_mbus_fmt           =  __m10mo_try_mbus_fmt,
	.set_mbus_fmt           =  __m10mo_set_mbus_fmt,
	.test_pattern           = m10mo_test_pattern,
};

void m10mo_handlers_init(struct v4l2_subdev *sd)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	pr_info("%s, dev->fw_type: %d\n", __func__, dev->fw_type);

	switch (dev->fw_type) {
	case M10MO_FW_TYPE_1:
		dev->fw_ops = &fw_type1_5_ops;
		break;
	case M10MO_FW_TYPE_2:
		dev->fw_ops = &fw_type2_ops;
		break;
	case M10MO_FW_TYPE_5:
		dev->fw_ops = &fw_type1_5_ops;
		break;
	default:
		dev->fw_ops = &fw_ops;
	}
}

static int m10mo_s_config(struct v4l2_subdev *sd, int irq)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u16 result = M10MO_INVALID_CHECKSUM;
	int id = 0;

	pr_info("%s\n", __func__);

	mutex_lock(&dev->input_lock);

	init_waitqueue_head(&dev->irq_waitq);
    init_waitqueue_head(&dev->d_zoom_waitq);
	dev->fw_type = dev->pdata->def_fw_type;
	dev->ref_clock = 19200000;//dev->pdata->ref_clock_rate[id];

	if (dev->pdata->common.platform_init) {
		ret = dev->pdata->common.platform_init(client);
		if (ret) {
			mutex_unlock(&dev->input_lock);
			return ret;
		}
	}

#ifndef NOIRQ
	/* set up irq */
	ret = m10mo_setup_irq(sd);
	if (ret) {
		dev_err(&client->dev, "IRQ err.\n");
		mutex_unlock(&dev->input_lock);
		return ret;
	}
#else
	wake_up(&dev->irq_waitq);
#endif
	ret = __m10mo_s_power(sd, 1, true);
	if (ret) {
		dev_err(&client->dev, "power-up err.\n");
		goto free_irq;
	}

	if (dev->pdata->common.csi_cfg) {
		ret = dev->pdata->common.csi_cfg(sd, 1);
		if (ret)
			goto fail;
	}

	ret = m10mo_fw_checksum(dev, &result);
	if (ret) {
		dev_err(&client->dev, "Checksum calculation fails.\n");
		goto fail;
	}
	if (result != M10MO_VALID_CHECKSUM){
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is not 0.\n");
		dev_err(&client->dev, "Firmware checksum is %x.\n",result);
	}
		/* TBD: Trig FW update here */

	/*
	 * We don't care about the return value here. Even in case of
	 * wrong or non-existent fw this phase must pass.
	 * FW can be updated later.
	 */
	m10mo_identify_fw_type(sd);

	/*
	 * Only after identify_fw_type the correct dev->fw_type
	 * can be got, so here update the ref_clock
	 */
	id = M10MO_GET_CLOCK_RATE_MODE(dev->fw_type);
	dev->ref_clock = 19200000;//dev->pdata->ref_clock_rate[id];

	m10mo_mipi_initialization(sd);

	/* Set proper function pointers based on FW_TYPE */
	m10mo_handlers_init(sd);

	ret = __m10mo_s_power(sd, 0, true);
	if (ret) {
		dev_err(&client->dev, "power-down err.\n");
		goto free_irq;
	}

	mutex_unlock(&dev->input_lock);
	return 0;

fail:
	__m10mo_s_power(sd, 0, true);
free_irq:
#ifndef NOIRQ
	free_irq(client->irq, sd);
#endif
	mutex_unlock(&dev->input_lock);
	dev_err(&client->dev, "External ISP power-gating failed\n");
	return ret;

}

static int m10mo_recovery(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	/* still power off sensor in case power is cut off abnormally*/
	ret = __m10mo_s_power(sd, 0, false);
	if (ret) {
		dev_err(&client->dev, "power-down err.\n");
		return ret;
	}
	usleep_range(100, 200);

	ret = __m10mo_s_power(sd, 1, false);
	if (ret) {
		dev_err(&client->dev, "power-up err.\n");
		return ret;
	}

	return ret;
}

static int m10mo_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret = 0;

	pr_info("%s, enable: %d\n", __func__, enable);
    if(dev->stream == enable)
        return 0;

    dev->stream = enable;
	mutex_lock(&dev->input_lock);
	if (enable) {

		if (!PREVIEW_TEST_PATTERN){
			  if(dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_BURST){
			        ret = m10mo_burst_run(sd);
			  }else{
			        ret = dev->fw_ops->set_run_mode(sd);
			  }
		}else{
			m10mo_test_pattern(sd, 1); // enable test mode.
        }
		if (ret) {
			ret = m10mo_recovery(sd);
			if (ret) {
				mutex_unlock(&dev->input_lock);
				return ret;
			}

			if (!PREVIEW_TEST_PATTERN)
				ret = dev->fw_ops->set_run_mode(sd);
			else
				m10mo_test_pattern(sd, 1); // enable test mode.
 		}
	} else {

		if (!PREVIEW_TEST_PATTERN){
			  if(dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_BURST){
			        ret = m10mo_burst_streamoff(sd);
			  }else{
			        ret = dev->fw_ops->stream_off(sd);
			  }
		}else{
			m10mo_test_pattern(sd, 0); // enable test mode.
 	    }
	}
	mutex_unlock(&dev->input_lock);

	/*
	 * Dump M10MO log when stream-off with checking folder
	 */
	if (!enable)
		m10mo_dump_log(sd);

	return ret;
}

static int
m10mo_enum_framesizes(struct v4l2_subdev *sd, struct v4l2_frmsizeenum *fsize)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
    printk("%s, start \n", __func__);
	if (fsize->index >= dev->entries_curr_table)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = dev->curr_res_table[fsize->index].width;
	fsize->discrete.height = dev->curr_res_table[fsize->index].height;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static int
m10mo_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
			struct v4l2_subdev_frame_size_enum *fse)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int index = fse->index;
	printk("%s, start, m10mo_capture_pre_flag is %d \n", __func__, m10mo_capture_pre_flag);

	mutex_lock(&dev->input_lock);
	if(m10mo_capture_pre_flag != M10MO_NOT_CAPTURE_0){
	    dev->curr_res_table = resolutions[M10MO_MODE_CAPTURE_INDEX];
	}

	if (index >= dev->entries_curr_table) {
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}

	fse->min_width = dev->curr_res_table[index].width;
	fse->min_height = dev->curr_res_table[index].height;
	fse->max_width = dev->curr_res_table[index].width;
	fse->max_height = dev->curr_res_table[index].height;
	mutex_unlock(&dev->input_lock);
	return 0;
}

static int m10mo_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_fh *fh,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	if (code->index)
		return -EINVAL;

	code->code = dev->format.code;
	return 0;
}

static struct v4l2_mbus_framefmt *
__m10mo_get_pad_format(struct m10mo_device *sensor,
			 struct v4l2_subdev_fh *fh, unsigned int pad,
			 enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(fh, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->format;
	default:
		return NULL;
	}
}

static int
m10mo_get_pad_format(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
		       struct v4l2_subdev_format *fmt)
{
	struct m10mo_device *snr = to_m10mo_sensor(sd);
	struct v4l2_mbus_framefmt *format =
			__m10mo_get_pad_format(snr, fh, fmt->pad, fmt->which);

	if (format == NULL)
		return -EINVAL;

	mutex_lock(&snr->input_lock);
	fmt->format = *format;
	mutex_unlock(&snr->input_lock);

	return 0;
}

static int
m10mo_set_pad_format(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
		       struct v4l2_subdev_format *fmt)
{
	struct m10mo_device *snr = to_m10mo_sensor(sd);
	struct v4l2_mbus_framefmt *format =
			__m10mo_get_pad_format(snr, fh, fmt->pad, fmt->which);

	if (format == NULL)
		return -EINVAL;

	mutex_lock(&snr->input_lock);
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		snr->format = fmt->format;
	mutex_unlock(&snr->input_lock);

	return 0;
}

static const struct media_entity_operations m10mo_entity_ops = {
	.link_setup = NULL,
};

static int m10mo_set_flicker_freq(struct v4l2_subdev *sd, s32 val)
{
#if 1
	unsigned int flicker_freq;

	switch (val) {
	case V4L2_CID_POWER_LINE_FREQUENCY_DISABLED:
		flicker_freq = M10MO_FLICKER_OFF;
		break;
	case V4L2_CID_POWER_LINE_FREQUENCY_50HZ:
		flicker_freq = M10MO_FLICKER_50HZ;
		break;
	case V4L2_CID_POWER_LINE_FREQUENCY_60HZ:
		flicker_freq = M10MO_FLICKER_60HZ;
		break;
	case V4L2_CID_POWER_LINE_FREQUENCY_AUTO:
		flicker_freq = M10MO_FLICKER_AUTO;
		break;
	default:
		return -EINVAL;
	}
#endif
	printk("Flicker_freq (%d/%d)",val,flicker_freq);
	return m10mo_writeb(sd, CATEGORY_ASUS, ASUS_FLICKER, (u32)flicker_freq);
}

static int m10mo_get_metering(struct v4l2_subdev *sd, s32 *val)
{
	int ret;
	u32 metering;

	ret = m10mo_readb(sd, CATEGORY_ASUS, ASUS_METERING_MODE, &metering);
	if (ret)
		return ret;
	*val = metering;
	return 0;
}

static int m10mo_set_metering(struct v4l2_subdev *sd, s32 val)
{
#if 0
	unsigned int metering;

	switch (val) {
	case V4L2_EXPOSURE_METERING_CENTER_WEIGHTED:
		metering = M10MO_METERING_CENTER;
		break;
	case V4L2_EXPOSURE_METERING_SPOT:
		metering = M10MO_METERING_SPOT;
		break;
	case V4L2_EXPOSURE_METERING_AVERAGE:
		metering = M10MO_METERING_AVERAGE;
		break;
	default:
		return -EINVAL;
	}

#endif
	return m10mo_writeb(sd, CATEGORY_ASUS, ASUS_METERING_MODE, (u32) val);
}

static int m10mo_set_white_balance(struct v4l2_subdev *sd, s32 val)
{
//=== The value of white-balance please refer to the below file:  ===//
//=== linux/kernel/include/uapi/linux/v4l2-controls.h             ===//
    int ret;
    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_WB_MANUAL, val);
    return ret;
}

static int m10mo_set_ev_bias(struct v4l2_subdev *sd, s32 val)
{
	/* 0x06 refers to 0.0EV value in m10mo HW */
	int ret;
	int ev_bias = 0x06 + val;
	printk("V4L2_CID_EXPOSURE (%d)",ev_bias);
	ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_EV, ev_bias);
	return ret;
}

static int m10mo_get_ev_bias(struct v4l2_subdev *sd, s32 *val)
{
	int ret;
	u32 ev_bias;

	ret = m10mo_readb(sd, CATEGORY_AE, AE_EV_BIAS, &ev_bias);
	if (ret)
		return ret;

	*val = (ev_bias-6) * M10MO_EV_STEP;
	return 0;
}

static u32 m10mo_iso_value_translate_to_command(s32 iso_value){
   int i;
   for (i = 0; i < ARRAY_SIZE(iso_table); i++){
		if (iso_value == iso_table[i][0]){
		    return iso_table[i][1];
		}
	}
	return REG_AE_ISOMODE_NOT_FOUND;
}
static int m10mo_set_iso_sensitivity(struct v4l2_subdev *sd, s32 val)
{
	u32 iso_cmd;

	iso_cmd = m10mo_iso_value_translate_to_command(val);
	if(iso_cmd != REG_AE_ISOMODE_NOT_FOUND){
	     return m10mo_writeb(sd, CATEGORY_ASUS, ASUS_ISO, iso_cmd);
	}

    printk(KERN_INFO "@%s: ISO value is %d, not supported! \n", __func__, val);
	return -EINVAL;
}

static int m10mo_set_iso_mode(struct v4l2_subdev *sd, s32 val)
{
//=== This function seems not used. ===//
	return 0;
}

static int m10mo_get_iso_mode(struct v4l2_subdev *sd, s32 *val)
{
	// struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret;

	ret = m10mo_readw(sd, CATEGORY_EXIF, EXIF_INFO_ISO, val);
	printk("get ISO mode result = %d",*val);
	return 0;
}

static const unsigned short ce_lut[][2] = {
	{ V4L2_COLORFX_NONE,     COLOR_EFFECT_NONE},
	{ V4L2_COLORFX_NEGATIVE, COLOR_EFFECT_NEGATIVE},
	{ V4L2_COLORFX_WARM,     COLOR_EFFECT_WARM},
	{ V4L2_COLORFX_COLD,     COLOR_EFFECT_COLD},
	{ V4L2_COLORFX_WASHED,   COLOR_EFFECT_WASHED},
};

static const unsigned short cbcr_lut[][3] = {
	{ V4L2_COLORFX_SEPIA,   COLOR_CFIXB_SEPIA,   COLOR_CFIXR_SEPIA},
	{ V4L2_COLORFX_BW,      COLOR_CFIXB_BW,      COLOR_CFIXR_BW},
	{ V4L2_COLORFX_RED,     COLOR_CFIXB_RED,     COLOR_CFIXR_RED},
	{ V4L2_COLORFX_GREEN,   COLOR_CFIXB_GREEN,   COLOR_CFIXR_GREEN},
	{ V4L2_COLORFX_BLUE,    COLOR_CFIXB_BLUE,    COLOR_CFIXR_BLUE},
	{ V4L2_COLORFX_PINK ,   COLOR_CFIXB_PINK,    COLOR_CFIXR_PINK},
	{ V4L2_COLORFX_YELLOW,  COLOR_CFIXB_YELLOW,  COLOR_CFIXR_YELLOW},
	{ V4L2_COLORFX_PURPLE,  COLOR_CFIXB_PURPLE,  COLOR_CFIXR_PURPLE},
	{ V4L2_COLORFX_ANTIQUE, COLOR_CFIXB_ANTIQUE, COLOR_CFIXR_ANTIQUE},
};

static int m10mo_set_cb_cr(struct v4l2_subdev *sd, u8 cfixb, u8 cfixr)
{
	int ret;

	ret = m10mo_writeb(sd, CATEGORY_MONITOR,
			   MONITOR_CFIXB, cfixb);
	if (ret)
		return ret;

	ret = m10mo_writeb(sd, CATEGORY_MONITOR,
			   MONITOR_CFIXR, cfixr);
	if (ret)
		return ret;

	ret = m10mo_writeb(sd, CATEGORY_MONITOR,
			   MONITOR_COLOR_EFFECT, COLOR_EFFECT_ON);
	return ret;
}

static int m10mo_set_color_effect(struct v4l2_subdev *sd, s32 val)
{
	int ret;

	switch (val) {
	case V4L2_COLORFX_NONE:
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_EFFECT, NONE);
		break;
	case V4L2_COLORFX_SEPIA:
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_EFFECT, SEPIA);
		break;
	case V4L2_COLORFX_BW:
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_EFFECT, MONO);
		break;
	case V4L2_COLORFX_NEGATIVE:
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_EFFECT, NEGATIVE);
		break;
	default:
		ret = -EINVAL;
		break;
	};

	return ret;
}

static int m10mo_set_color_effect_cbcr(struct v4l2_subdev *sd, s32 val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int ret;
	u8 cr, cb;

	cr = val & 0xff;
	cb = (val >> 8) & 0xff;

	if (dev->colorfx->cur.val == V4L2_COLORFX_SET_CBCR) {
		ret = m10mo_set_cb_cr(sd, cb, cr);
		if (ret)
			return ret;
	}

	dev->colorfx_cr = cr;
	dev->colorfx_cb = cb;

	return 0;
}

static int m10mo_get_focal(struct v4l2_subdev *sd, s32 *val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 optical_step;
	printk(KERN_INFO "@%s \n", __func__);
	while(dev->wait_irq_flag == M10MO_ZOOMING){
	      msleep(1);
	      printk(KERN_INFO "@%s: Warning!!! WAIT! Still moving zoom les!!!!! Cannot get f number. \n", __func__);
	}

	(void) m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &optical_step);
	*val = (s32) m10mo_focus_data_table[optical_step].focal_len;
	return 0;
}

static int m10mo_get_fnumber(struct v4l2_subdev *sd, s32 *val)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	u32 optical_step;
	printk(KERN_INFO "@%s \n", __func__);
	while(dev->wait_irq_flag == M10MO_ZOOMING){
	      msleep(1);
	      printk(KERN_INFO "@%s: Warning!!! WAIT! Still moving zoom les!!!!! Cannot get f number. \n", __func__);
	}
	(void) m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &optical_step);
	*val = (s32) m10mo_focus_data_table[optical_step].f_number;
	return 0;
}

static int m10mo_get_fnumber_range(struct v4l2_subdev *sd, s32 *val)
{
	*val = (M10MO_F_NUMBER_DEFAULT_NUM << 24) |
		(M10MO_F_NUMBER_DEM << 16) |
		(M10MO_F_NUMBER_DEFAULT_NUM << 8) | M10MO_F_NUMBER_DEM;
	return 0;
}

static int m10mo_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct m10mo_device *dev = container_of(
		ctrl->handler, struct m10mo_device, ctrl_handler);
	int ret = 0;
	printk("@%s ctrl->id = %d",__func__,ctrl->id);
	if (!dev->power)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_POWER_LINE_FREQUENCY:
	/*	We need to set Flicker frequence mode
		Please verify this. */
		ret = m10mo_set_flicker_freq(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE_METERING:
	/*	We use EXT_ISP_CID_GET_METERING_MODE/EXT_ISP_CID_SET_METERING_MODE
	*/
		ret = m10mo_set_metering(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_START_ZSL_CAPTURE:
		if (ctrl->val)
			ret = m10mo_set_zsl_capture(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
		printk("DDDD V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE (%d)",ctrl->val);
		ret = m10mo_set_white_balance(&dev->sd, ctrl->val);
		ret = 0;
		break;
	case V4L2_CID_EXPOSURE:
		ret = m10mo_set_ev_bias(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_ISO_SENSITIVITY:
	/*	Should be OK now.
		We use 0 for V4L2_CID_ISO_SENSITIVITY with iso-AUTO.
	*/  printk("DDDD V4L2_CID_ISO_SENSITIVITY (%d)",ctrl->val);
		ret = m10mo_set_iso_sensitivity(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_ISO_SENSITIVITY_AUTO:
	/*	Have no idea about this control.
		We use 0 in V4L2_CID_ISO_SENSITIVITY with iso-AUTO.
	*/  printk("DDDD V4L2_CID_ISO_SENSITIVITY_AUTO (%d)",ctrl->val);
		ret = m10mo_set_iso_mode(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_COLORFX:
	    printk("DDDD V4L2_CID_COLORFX (%d)",ctrl->val);
		ret = m10mo_set_color_effect(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_COLORFX_CBCR:
		ret = m10mo_set_color_effect_cbcr(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = dev->fw_ops->test_pattern(&dev->sd, ctrl->val);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static const u32 m10mo_mipi_freq[] = {
	M10MO_MIPI_FREQ_0,
	M10MO_MIPI_FREQ_1,
};

static int m10mo_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct m10mo_device *dev = container_of(
		ctrl->handler, struct m10mo_device, ctrl_handler);
	int ret = 0;
	int id = M10MO_GET_MIPI_FREQ_MODE(dev->fw_type);

	switch (ctrl->id) {
	case V4L2_CID_LINK_FREQ:
		ctrl->val = m10mo_mipi_freq[id];
		break;
	case V4L2_CID_EXPOSURE:
		ret = m10mo_get_ev_bias(&dev->sd, &ctrl->val);
		break;
	case V4L2_CID_FOCAL_ABSOLUTE:
		ret = m10mo_get_focal(&dev->sd, &ctrl->val);
		break;
	case V4L2_CID_FNUMBER_ABSOLUTE:
		ret = m10mo_get_fnumber(&dev->sd, &ctrl->val);
		break;
	case V4L2_CID_FNUMBER_RANGE:
		ret = m10mo_get_fnumber_range(&dev->sd, &ctrl->val);
		break;
	case V4L2_CID_ISO_SENSITIVITY:
	/*	We get iso from here (For capture)
		Need change this method from FW.
	*/
		ret = m10mo_get_iso_mode(&dev->sd, &ctrl->val);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static struct v4l2_ctrl_ops m10mo_ctrl_ops = {
	.g_volatile_ctrl = m10mo_g_volatile_ctrl,
	.s_ctrl = m10mo_s_ctrl,
};

/* TODO: To move this to s_ctrl framework */
static int m10mo_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	int index;
	int mode = M10MO_GET_RESOLUTION_MODE(dev->fw_type);

    printk("@%s %d start, mode = %d, run_mode = 0x%x\n", __func__, __LINE__, mode, param->parm.capture.capturemode);
	mutex_lock(&dev->input_lock);
	dev->run_mode = param->parm.capture.capturemode;
	switch (dev->run_mode) {
	case CI_MODE_VIDEO:
	    index = M10MO_MODE_VIDEO_INDEX;
		break;
	case CI_MODE_STILL_CAPTURE:
		index = M10MO_MODE_CAPTURE_INDEX;
		break;
	default:
		index = M10MO_MODE_PREVIEW_INDEX;
		break;
	}
	dev->entries_curr_table = resolutions_sizes[index];
	dev->curr_res_table = resolutions[index];

	mutex_unlock(&dev->input_lock);
	return 0;
}

static long m10mo_ioctl(struct v4l2_subdev *sd, unsigned int cmd,
			void *arg)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct atomisp_ext_isp_ctrl *m10mo_ctrl
		= (struct atomisp_ext_isp_ctrl *)arg;
	int ret = 0;
	int read = 0;

	dev_info(&client->dev, "m10mo ioctl id is %d, data 0x%x\n",
			m10mo_ctrl->id, m10mo_ctrl->data);
	printk("dev->m10mo_mode == M10MO_PARAMETER_MODE (%d) / (%d)\n",dev->m10mo_mode == M10MO_PARAMETER_MODE,dev->m10mo_mode);
	printk("m10mo ioctl id is %d, data 0x%x, dev->wait_irq_flag is %d\n",m10mo_ctrl->id, m10mo_ctrl->data, dev->wait_irq_flag);
#if 0
	if(dev->wait_irq_flag){
	    printk(KERN_INFO "m10mo_ioctl, dev->wait_irq_flag is %d \n", dev->wait_irq_flag);
	    if(m10mo_ctrl->id == EXT_ISP_CID_FOCUS_EXECUTION
		   || m10mo_ctrl->id == EXT_ISP_CID_TOUCH_POSX
		   || m10mo_ctrl->id == EXT_ISP_CID_TOUCH_POSY
		   || m10mo_ctrl->id == EXT_ISP_CID_TOUCH_WIDTH
		   || m10mo_ctrl->id == EXT_ISP_CID_TOUCH_HEIGHT
		   || m10mo_ctrl->id == EXT_ISP_CID_ZOOM){
		       return -EBUSY;
		}
	}
#endif

	mutex_lock(&dev->input_lock);
	switch(m10mo_ctrl->id)
	{
	case EXT_ISP_CID_ISO:
		dev_info(&client->dev, "m10mo ioctl ISO\n");
		break;
	case EXT_ISP_CID_CAPTURE_HDR:
		ret = m10mo_set_hdr_mode(sd, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_CAPTURE_LLS:

		break;
	case EXT_ISP_CID_FOCUS_MODE:
	/*	We need to set AF mode (like Smart AF/CAF/infinity/MANUAL)
		Maybe it need modify Gategory 04/0x3B*/

		ret = m10mo_set_af_mode(sd, m10mo_ctrl->data);
//Workaround first
		printk("FORCE EXT_ISP_CID_FOCUS_MODE (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_FOCUS_EXECUTION:
        printk("FORCE EXT_ISP_CID_FOCUS_EXECUTION (%d)",m10mo_ctrl->data);

        if(m10mo_ctrl->data != EXT_ISP_FOCUS_SEARCH){
	       ret = -EINVAL;
		   goto m10mo_ioctl_out;
	    }
        if(m10mo_capture_pre_flag){
	       ret = -EINVAL;
		   goto m10mo_ioctl_out;
	    }
	    if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	        pr_info("%s, illegal to set AF during M10MO_PARAMETER_MODE, break!\n", __func__);
		    ret = -EINVAL;
			goto m10mo_ioctl_out;
        }
		m10mo_lens_state_machine(sd, AUTO_FOCUS_EVENT, &(m10mo_ctrl->data));
		ret = 0;
#if 0
        if (dev->wait_irq_flag == M10MO_FOCUSING) {
		    dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();//(dev, &lens_work);
			queue_delayed_work(dev->cancel_auto_focus_wq, &cancel_auto_focus_work, 0);
			queue_delayed_work(dev->lens_wq, &dummy_work, 0);
		}
		ret = m10mo_set_af_execution(sd, m10mo_ctrl->data);
#endif
		break;
	case EXT_ISP_CID_TOUCH_POSX:
	/*	We need to set Focus X position
		Maybe it need modify Gategory 04/0x17-0x18*/

		ret = m10mo_set_af_position_x(sd, m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_CID_TOUCH_POSX (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_TOUCH_POSY:
	/*	We need to set Focus Y position
		Maybe it need modify Gategory 04/0x19-0x1A*/

		ret = m10mo_set_af_position_y(sd, m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_CID_TOUCH_POSY (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_TOUCH_WIDTH:
	/*	We need to set focus width
		Maybe it need modify Gategory 04/0x34-0x35*/

		ret = m10mo_set_af_width(sd, m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_CID_TOUCH_WIDTH (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_TOUCH_HEIGHT:
	/*	We need to set focus height
		Maybe it need modify Gategory 04/0x36-0x37*/

		ret = m10mo_set_af_height(sd, m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_CID_TOUCH_HEIGHT (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_TOUCH_AE_POSX:
	/*	We need to set AE position X */

		ret = m10mo_set_ae_position_x(sd, m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_CID_TOUCH_AE_POSX (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_TOUCH_AE_POSY:
	/*	We need to set AE position Y */

		ret = m10mo_set_ae_position_y(sd, m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_CID_TOUCH_AE_POSY (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_CAF_STATUS:
		ret = m10mo_get_caf_status(sd, &m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_AF_STATUS:
	/*	We need to get now AF status
		Maybe it need to check */

		ret = m10mo_get_af_status(sd, &m10mo_ctrl->data);
		printk("DDDD FORCE EXT_ISP_CID_AF_STATUS");
		ret = 0;

		break;
	case EXT_ISP_CID_OPTICAL_ZOOM_STATUS:
        printk("DDDD FORCE EXT_ISP_CID_OPTICAL_ZOOM_STATUS");
		ret = m10mo_get_optical_zoom_status(sd, &m10mo_ctrl->data);
	    break;
	case EXT_ISP_CID_DIGITAL_ZOOM_STATUS:
        printk("DDDD FORCE EXT_ISP_CID_DIGITAL_ZOOM_STATUS");
		ret = m10mo_get_digital_zoom_status(sd, &m10mo_ctrl->data);
	    break;
	case EXT_ISP_CID_CANCEL_OPTICAL_ZOOM:
        printk("DDDD FORCE EXT_ISP_CID_CANCEL_OPTICAL_ZOOM");
	    ret = 0;
//		dev->cancel_irq_flag = 1;
//		__m10mo_reset_irq_wait_status();
//		dev->cancel_irq_flag = 0;
//	    ret = m10mo_request_cmd_effect(sd, M10MO_ASUS_ZOOM_CANCEL, NULL);
		break;
    case EXT_ISP_CID_CANCEL_AF:
	    printk("DDDD FORCE EXT_ISP_CID_CANCEL_AF");
		ret = 0;
//		dev->cancel_irq_flag = 1;
//        __m10mo_reset_irq_wait_status();
//		dev->cancel_irq_flag = 0;
 //       ret = m10mo_request_cmd_effect(sd, M10MO_ASUS_FOCUS_CANCEL, NULL);
        break;
	case EXT_ISP_CID_CANCEL_OPTICAL_ZOOM_CMD_STATUS:
	    printk("DDDD FORCE EXT_ISP_CID_CANCEL_OPTICAL_ZOOM_CMD_STATUS");
        ret = m10mo_get_cancel_optical_zoom_cmd_status(sd, &m10mo_ctrl->data);
        break;
	case EXT_ISP_CID_CANCEL_AF_CMD_STATUS:
	    printk("DDDD FORCE EXT_ISP_CID_CANCEL_FOCUS_CMD_STATUS");
		ret = m10mo_get_cancel_af_cmd_status(sd, &m10mo_ctrl->data);
        break;
	case EXT_ISP_CID_GET_AF_MODE:
	/*	We need to set AF mode (like Smart AF/CAF/infinity/MANUAL)
		Maybe it need modify Gategory 04/0x3B*/

		ret = m10mo_get_af_mode(sd, &m10mo_ctrl->data);
//Workaround first
		printk("DDDD FORCE EXT_ISP_FOCUS_MODE_NORMAL (%d)",m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_CAPTURE_BURST:
	/*	We need to set now Burst Capture mode (for 5 frame to super resolution)
		Maybe it need modify Gategory 0C/0x0A ?*/
		printk("DDDD force EXT_ISP_CID_CAPTURE_BURST (%d)", m10mo_ctrl->data);
		if( m10mo_ctrl->data > 0 ){
			num_LLS = m10mo_ctrl->data;
		}else{
			num_LLS = 0;
		}
//		ret = m10mo_set_burst_mode(sd, m10mo_ctrl->data);
        ret = m10mo_set_lls_mode(sd, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_FLASH_MODE:
	/*	We need to set Flash mode (AUTO/OFF/ON/TORCH)
		Maybe it need modify Gategory 04/0x15 or Gategory 0D/0xF0*/
/*
#define EXT_ISP_FLASH_MODE_OFF		0
#define EXT_ISP_FLASH_MODE_ON		1
#define EXT_ISP_FLASH_MODE_AUTO		2
#define EXT_ISP_LED_TORCH_OFF		3 //We don't send this parameter, using EXT_ISP_FLASH_MODE_OFF instead.
#define EXT_ISP_LED_TORCH_ON		4
*/
		ret = m10mo_set_flash_mode(sd, m10mo_ctrl->data);
		printk("DDDD FORCE EXT_ISP_CID_FLASH_MODE (%d)",m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_ZOOM:  //=== Optical-zoom. ===//
	    printk(KERN_INFO "DDDD FORCE EXT_ISP_CID_ZOOM (%d) \n", m10mo_ctrl->data);

		if(dev->m10mo_mode == M10MO_PARAMETER_MODE) {
		    ret = -EINVAL;
		    goto m10mo_ioctl_out;
		}
        if(m10mo_ctrl->data > 32 || m10mo_ctrl->data < 1){
	           pr_info("m10mo, wrong optical zoom data is %d \n", m10mo_ctrl->data);
	       ret = -EINVAL;
		   goto m10mo_ioctl_out;
	    }

		while(dev->requested_cmd == M10MO_START_DIGITAL_ZOOM) {
		    pr_info("%s, Digital-zooming, Wait!!!!!\n", __func__);
			msleep(5);
		}
		ret = m10mo_readb(sd, CATEGORY_MONITOR, DIGIT_ZOOM, &read);
        if(read != 0x1){
            pr_info("%s, get digital-zoom step is %d, illegal to set Optical-zoom, break!\n", __func__, read);
//	        ret = -EINVAL;
//		    goto m10mo_ioctl_out;
            read = 0x1;
            (void) m10mo_request_cmd_effect(sd, M10MO_START_DIGITAL_ZOOM, &read);
	    }

		if(m10mo_capture_pre_flag){
	       ret = -EINVAL;
		   goto m10mo_ioctl_out;
	    }

	    if(dev->m10mo_mode == M10MO_PARAMETER_MODE){
	        pr_info("%s, illegal to set Optical-zoom during M10MO_PARAMETER_MODE, break!\n", __func__);
		    ret = -EINVAL;
			goto m10mo_ioctl_out;
	    }

		last_optical_zoom_step = m10mo_ctrl->data;
		if(cancel_zoom_protect == 1 ) {
		   pr_info("m10mo, zoom canceling, do not zoom, zoom data is %d \n", m10mo_ctrl->data);
	       ret = -EINVAL;
		   goto m10mo_ioctl_out;
		}
		m10mo_lens_state_machine(sd, OPTICAL_ZOOM_EVENT, &last_optical_zoom_step);
		ret = 0;
#if 0
		ret = m10mo_readb(sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &read);
	    if(read == m10mo_ctrl->data){
		    pr_info("%s, read_step is %d, step is %d, no need to set again! \n", __func__, read, m10mo_ctrl->data);
		}
#endif
#if 0
		cancel_delayed_work(&optical_zoom_work);
		cancel_delayed_work(&auto_focus_work);

		if (dev->wait_irq_flag == M10MO_FOCUSING) {
			dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();//(dev, &lens_work);
            queue_delayed_work(dev->cancel_auto_focus_wq, &cancel_auto_focus_work, 0);
			queue_delayed_work(dev->lens_wq, &dummy_work, 0);
		}else if (dev->wait_irq_flag == M10MO_ZOOMING) {
		    dev->cancel_irq_flag = 1;
		    __m10mo_reset_irq_wait_status();//(dev, &lens_work);
		    queue_delayed_work(dev->cancel_optical_zoom_wq, &cancel_optical_zoom_work, 0);
		    queue_delayed_work(dev->lens_wq, &dummy_work, 0);
		}
		m10mo_ctrl->data = clamp_t(unsigned int, m10mo_ctrl->data,
						ZOOM_POS_MIN, ZOOM_POS_MAX);
		ret = m10mo_set_optical_zoom_position(sd, m10mo_ctrl->data);
#endif
		break;
	case EXT_ISP_CID_SHOT_MODE:
/*
#define EXT_ISP_SHOT_MODE_AUTO 0
#define EXT_ISP_SHOT_MODE_BEAUTY_FACE 1
#define EXT_ISP_SHOT_MODE_BEST_PHOTO 2
#define EXT_ISP_SHOT_MODE_DRAMA 3
#define EXT_ISP_SHOT_MODE_BEST_FACE 4
#define EXT_ISP_SHOT_MODE_ERASER 5
#define EXT_ISP_SHOT_MODE_PANORAMA 6
#define EXT_ISP_SHOT_MODE_RICH_TONE_HDR 7
#define EXT_ISP_SHOT_MODE_NIGHT 8
#define EXT_ISP_SHOT_MODE_SOUND_SHOT 9
#define EXT_ISP_SHOT_MODE_ANIMATED_PHOTO 10
#define EXT_ISP_SHOT_MODE_SPORTS 11
#define EXT_ISP_SHOT_MODE_PRO 12
#define EXT_ISP_SHOT_MODE_LOWLIGHT 13
#define EXT_ISP_SHOT_MODE_TIMEREWIND 14
#define EXT_ISP_SHOT_MODE_SELFIE 15
#define EXT_ISP_SHOT_MODE_MINIATURE 16
#define EXT_ISP_SHOT_MODE_DEFOCUS 17
#define EXT_ISP_SHOT_MODE_PANOSELFIE 18
#define EXT_ISP_SHOT_MODE_SUPERRESOLUTION 19
#define EXT_ISP_SHOT_MODE_NORMAL 20
#define EXT_ISP_SHOT_MODE_EFFECT 21
#define EXT_ISP_SHOT_MODE_CONTINUOUS 22
#define EXT_ISP_VIDEO_MODE_NORMAL 40
#define EXT_ISP_VIDEO_MODE_PRO 41
#define EXT_ISP_VIDEO_MODE_MMS 42
#define EXT_ISP_VIDEO_MODE_MINIATURE 43
#define EXT_ISP_VIDEO_MODE_SLOWMOTION 44
#define EXT_ISP_VIDEO_MODE_HISPEED 45
#define EXT_ISP_VIDEO_MODE_TIMELAPSE 46
#define EXT_ISP_VIDEO_MODE_LOWLIGHT 47
#define EXT_ISP_VIDEO_MODE_EFFECT 48
*/
		/*	We need to set Asus Camera mode  */
		printk("DDDD FORCE EXT_ISP_CID_SHOT_MODE (%d)",m10mo_ctrl->data);
		ret = m10mo_set_shot_mode(sd, m10mo_ctrl->data);
		break;
	case EXT_ISP_M10MO_SET_CAPTURE_MODE:
	        //=== Hard code to set ois s2 mode. ===//
#ifndef CONFIG_ASUS_FACTORY_MODE
#if 0
      (void) m10mo_writew(sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0110);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_DATA31_24, 0x00);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);
		msleep(10);
      (void) m10mo_writew(sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0107);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_DATA31_24, 0x11);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);
		msleep(10);
      (void) m10mo_writew(sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0102);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_DATA31_24, 0x02);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);
		msleep(10);
      (void) m10mo_writew(sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, 0x0110);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_DATA31_24, 0x03);
      (void) m10mo_writeb(sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, 0x11);
		msleep(10);
#endif
#endif
	    m10mo_capture_pre_flag = M10MO_CAP_BEFORE_1ST_STREAMOFF_1;
		ret = 0;
		break;
	case EXT_ISP_CID_GET_METERING_MODE:
	/*	We need to get now AE metering mode (Average(0x00)/Center(0x01)/Spot(0x02))
		Maybe it need modify Gategory 04/0x16  */
		m10mo_get_metering(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_GET_METERING_MODE (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_SET_METERING_MODE:
	/*	We need to get now AE metering mode (Average(0x00)/Center(0x01)/Spot(0x02))
		Maybe it need modify Gategory 04/0x16*/
		m10mo_set_metering(sd, m10mo_ctrl->data);
		printk("EXT_ISP_CID_SET_METERING_MODE (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_EIS_MODE:
	/*	We need to set EIS to AE (0/1)
		It need to check wit DIT
		*/
		printk("DDDD force EXT_ISP_CID_EIS_MODE (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_VIDEO_OPTIMIZE:
	/*	We need to set video AE mode
		Just bypass the HAL value.
		*/
		printk("EXT_ISP_CID_VIDEO_OPTIMIZE (0x%x)", m10mo_ctrl->data);
	    ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE,m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SHUTTER_SPEED:
		printk("DDDD force EXT_ISP_CID_SHUTTER_SPEED (%d)", m10mo_ctrl->data);
		ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_SHUTTER_SPEED, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_COLOR_TEMPERATURE:
	/*	We need to set color temperature to AWB
		It need to check wit DIT.
		If value = 0 , it means auto.
		*/
		printk("DDDD force EXT_ISP_CID_COLOR_TEMPERATURE (%d)", m10mo_ctrl->data);
		ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_COLOR_TEMPERATURE, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_MANUAL_FOCUS:
	/*	We need to set lens postion for focus
		It need to check wit DIT.
		If value = 0 , it means auto.
		*/
		printk("EXT_ISP_CID_MANUAL_FOCUS (%d)\n", m10mo_ctrl->data);
		if(m10mo_ctrl->data != 0){
			ret = m10mo_readb(sd, CATEGORY_ASUS, ASUS_FOCUS_MODE, &read);
			if(read != 3){
				printk("EXT_ISP_CID_MANUAL_FOCUS now FOCUS_MODE = %d, force to MANUAL_MODE.\n", read);
				ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_FOCUS_MODE, 3);
			}
			ret = m10mo_request_cmd_effect(sd, M10MO_MANUAL_FOCUS, &m10mo_ctrl->data);
		}
		break;
	case EXT_ISP_CID_SATURATION:
	/*	We need to set saturation
		It need to check wit DIT.
		The value should be  value/100.
		*/
		printk("EXT_ISP_CID_SATURATION (%d)", m10mo_ctrl->data);
		ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_SATURATION, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_CONTRAST:
	/*	We need to set saturation
		It need to check wit DIT.
		The value should be  value/100.
		*/
		printk("EXT_ISP_CID_CONTRAST (%d)", m10mo_ctrl->data);
		ret = m10mo_writeb(sd, CATEGORY_ASUS, ASUS_CONTRAST, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_EXPOSURETIME_NUMERATOR:
		ret = m10mo_get_exposure_time_numerator(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_EXPOSURETIME_NUMERATOR (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_EXPOSURETIME_DENOMINATOR:
		ret = m10mo_get_exposure_time_denominator(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_EXPOSURETIME_DENOMINATOR (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_PREVIEW_EXPOSURETIME_NUMERATOR:
		ret = m10mo_get_preview_exposure_time_numerator(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_PREVIEW_EXPOSURETIME_NUMERATOR (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_PREVIEW_EXPOSURETIME_DENOMINATOR:
		ret = m10mo_get_preview_exposure_time_denominator(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_PREVIEW_EXPOSURETIME_DENOMINATOR (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_PREVIEW_ISO:
		ret = m10mo_get_preview_iso(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_PREVIEW_ISO (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_AT_SCENE:
		ret = m10mo_writew(sd, CATEGORY_ASUS, ASUS_AT_SCENE, m10mo_ctrl->data);
		printk("EXT_ISP_CID_AT_SCENE (%d)", m10mo_ctrl->data);
		ret = 0;
		break;
	case EXT_ISP_CID_GET_AF_RESULT:
	    ret = m10mo_get_af_result(sd, &m10mo_ctrl->data);
        printk("EXT_ISP_CID_GET_AF_RESULT (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_M10MO_I2C_ONE_BYTE_WRITE:
        printk("EXT_ISP_CID_M10MO_I2C_ONE_BYTE_WRITE (%d)", m10mo_ctrl->data);
		if(dev->wait_irq_flag != M10MO_IRQ_COMMAND_AVAILABDLE
		   || dev->lens_state_machine != LENS_IDLE) {
		    printk("EXT_ISP_CID_M10MO_I2C_ONE_BYTE_WRITE, dev->wait_irq_flag is %d, dev->lens_state_machine is %d, illegal for HAL to do i2c-write, skip this cmd.\n",
                   dev->wait_irq_flag, dev->lens_state_machine);
			if(dev->wait_irq_flag == M10MO_IRQ_COMMAND_AVAILABDLE && dev->lens_state_machine == LENS_IDLE) {
			      while (1){
				      printk(KERN_INFO "Warning!!! EXT_ISP_CID_M10MO_I2C_ONE_BYTE_WRITE, it is ILLIGAL to set i2c cmds during irq_staus is busy!!!\n");
				      msleep(1);
				  }
			}
		    ret = -EINVAL;
		    goto m10mo_ioctl_out;
		}
        ret = m10mo_set_hal_i2c_write(sd, &m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_CONTINUOUS_CAPTURE:
	    printk("EXT_ISP_CID_CONTINUOUS_CAPTURE (%d)", m10mo_ctrl->data);
		if(m10mo_ctrl->data > 0){
			if( m10mo_ctrl->data > 10 ){
				//This should be real continuous mode, for DIT request, should set to 1.
				(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, 0xC, 1);
			}else{
				//This should be NR mode, for DIT request, should set to 3.
				(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, 0xC, 3);
			}
		}else{
			(void) m10mo_writeb(sd, CATEGORY_CAPTURE_CTRL, 0xC, 0);
		}
		ret = m10mo_set_burst_mode(sd, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_M10MO_I2C_ONE_BYTE_READ:
	    printk("Before EXT_ISP_CID_M10MO_I2C_ONE_BYTE_READ (%d)", m10mo_ctrl->data);
	    ret = m10mo_get_hal_i2c_read(sd, &m10mo_ctrl->data);
		printk("After EXT_ISP_CID_M10MO_I2C_ONE_BYTE_READ (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_M10MO_I2C_WORD_READ:
	    printk("Before EXT_ISP_CID_M10MO_I2C_WORD_READ (%d)", m10mo_ctrl->data);
	    ret = m10mo_get_hal_i2c_wread(sd, &m10mo_ctrl->data);
		printk("After EXT_ISP_CID_M10MO_I2C_WORD_READ (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_M10MO_I2C_DWORD_READ:
	    printk("Before EXT_ISP_CID_M10MO_I2C_DWORD_READ (%d)", m10mo_ctrl->data);
	    ret = m10mo_get_hal_i2c_lread(sd, &m10mo_ctrl->data);
		printk("After EXT_ISP_CID_M10MO_I2C_DWORD_READ (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_GET_OPTICAL_ZOOM_NUMBER:
	    ret = m10mo_get_optical_zoom_number(sd, &m10mo_ctrl->data);
	    printk("EXT_ISP_CID_GET_OPTICAL_ZOOM_NUMBER (%d)", m10mo_ctrl->data);
	    break;
	case EXT_ISP_CID_SET_BATTERY_CAPACITY:
	    printk("EXT_ISP_CID_SET_BATTERY_CAPACITY (%d)", m10mo_ctrl->data);
	    ret = m10mo_set_battery_capacity(sd, &m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_3A_LOCK:
	    printk("EXT_ISP_CID_SET_3A_LOCK (%d)", m10mo_ctrl->data);
		ret = m10mo_set_3A_lock(sd, &m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_GET_FLASH_NEEDED:
	    ret = m10mo_get_asus_flash_needed(sd, &m10mo_ctrl->data);
		printk("EXT_ISP_CID_GET_FLASH_NEEDED (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_DIGITAL_ZOOM:
	    printk("EXT_ISP_CID_SET_DIGITAL_ZOOM (%d)", m10mo_ctrl->data);
		ret = m10mo_set_digital_zoom_position(sd, m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_ASUS_CAMERA:
	    printk("EXT_ISP_CID_SET_ASUS_CAMERA (%d)", m10mo_ctrl->data);
	    break;
	case EXT_ISP_CID_GET_M10MO_IRQ_STATUS:
		ret = m10mo_get_irq_status(sd, &m10mo_ctrl->data);
	    printk("EXT_ISP_CID_GET_M10MO_IRQ_STATUS (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_M10MO_START_CAPTURE:
	    printk("EXT_ISP_CID_SET_M10MO_START_CAPTURE (0x%x)", m10mo_ctrl->data);
		if(dev->capture_mode == M10MO_CAPTURE_MODE_ZSL_BURST){
	        printk("m10mo, @%s. During Burst mode, do not send capture cmds here. \n", __func__);
		    ret = 0;
		    goto m10mo_ioctl_out;
	    }
		{
		int entries, idx;
		u32 width, height;
		const struct m10mo_resolution *res;
		dev->entries_curr_table = resolutions_sizes[M10MO_MODE_CAPTURE_INDEX];
	    dev->curr_res_table = resolutions[M10MO_MODE_CAPTURE_INDEX];
		res = resolutions[M10MO_MODE_CAPTURE_INDEX];
        entries = resolutions_sizes[M10MO_MODE_CAPTURE_INDEX];
        /* check if the given resolutions are spported */
		width = (m10mo_ctrl->data & 0xFFFF0000) >> 16;
	    height = m10mo_ctrl->data & 0xFFFF;
        /* check if the given resolutions are spported */
	    idx = get_resolution_index(res, entries, width, height);
        if (idx < 0) {
		    dev_err(&client->dev, "%s unsupported resolution: %dx%d\n",
			__func__, width, height);
		    ret = -EINVAL;
			goto m10mo_ioctl_out;
	    }
		dev->capture_res_idx = dev->fmt_idx = idx;

		printk("@%s %d start, requested_cmd = %d. Try to get the mutex lock. TRYing...TRYing...\n", __func__, __LINE__, M10MO_SINGLE_CAPTURE_MODE);
	    mutex_lock(&dev->m10mo_request_cmd_lock);
		dev->requested_cmd = M10MO_SINGLE_CAPTURE_MODE;
		dev->cmd = M10MO_NO_CMD_REQUEST;
		dev->wait_irq_flag = M10MO_NOT_LENS_RELATED_BUSYING;
		printk("@%s %d start, dev->requested_cmd = %d. Successful to get the mutex lock.\n", __func__, __LINE__, dev->requested_cmd);
        m10mo_send_still_capture_cmds(sdd);
	    dev->lock_i2c_write_flag = 1;
		}

		break;
    case EXT_ISP_CID_SET_M10MO_START_PREVIEW:
	    printk("EXT_ISP_CID_SET_M10MO_START_PREVIEW (%d)", m10mo_ctrl->data);
        ret = 0;
		break;
    case EXT_ISP_CID_GET_LENS_INITIALIZATION_DONE:
		ret = m10mo_get_lens_initailzation_done(sd, &m10mo_ctrl->data);
	    printk("EXT_ISP_CID_GET_LENS_INITIALIZATION_DONE (%d)", m10mo_ctrl->data);
        break;
	case EXT_ISP_CID_GET_DIT_LV:
		ret = m10mo_get_LV(sd, &m10mo_ctrl->data);
	    printk("EXT_ISP_CID_GET_DIT_LV (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_GET_DIT_R_GAIN:
		ret = m10mo_get_R_GAIN(sd, &m10mo_ctrl->data);
	    printk("EXT_ISP_CID_GET_DIT_R_GAIN (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_GET_DIT_B_GAIN:
		ret = m10mo_get_B_GAIN(sd, &m10mo_ctrl->data);
	    printk("EXT_ISP_CID_GET_DIT_B_GAIN (%d)", m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_DIT_R_GAIN:
	    printk("EXT_ISP_CID_SET_DIT_R_GAIN (%d)", m10mo_ctrl->data);
		ret = m10mo_set_R_GAIN(sd, &m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_DIT_B_GAIN:
	    printk("EXT_ISP_CID_SET_DIT_B_GAIN (%d)", m10mo_ctrl->data);
		ret = m10mo_set_B_GAIN(sd, &m10mo_ctrl->data);
		break;
	case EXT_ISP_CID_SET_G_SENSOR_X:
	    printk("EXT_ISP_CID_SET_G_SENSOR_X (%d)", m10mo_ctrl->data);
		ret = m10mo_set_g_sensor_x(sd, &m10mo_ctrl->data);
        break;
	case EXT_ISP_CID_SET_G_SENSOR_Y:
	    printk("EXT_ISP_CID_SET_G_SENSOR_Y (%d)", m10mo_ctrl->data);
		ret = m10mo_set_g_sensor_y(sd, &m10mo_ctrl->data);
        break;
	case EXT_ISP_CID_SET_G_SENSOR_Z:
	    printk("EXT_ISP_CID_SET_G_SENSOR_Y (%d)", m10mo_ctrl->data);
		ret = m10mo_set_g_sensor_z(sd, &m10mo_ctrl->data);
        break;
	default:
		ret = -EINVAL;
		dev_err(&client->dev, "m10mo ioctl: Unsupported ID\n");
		break;
	};

m10mo_ioctl_out:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static const struct v4l2_ctrl_config ctrls[] = {
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_LINK_FREQ,
		.name = "Link Frequency",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 1,
		.max = 1500000 * 1000,
		.step = 1,
		.def = 1,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_POWER_LINE_FREQUENCY,
		.name = "Light frequency filter",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.def = 3,
		.max = 3,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_EXPOSURE_METERING,
		.name = "Metering",
		.type = V4L2_CTRL_TYPE_MENU,
		.min = 0,
		.max = 2,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_START_ZSL_CAPTURE,
		.name = "Start zsl capture",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.def = 1,
		.max = 4,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		.name = "White Balance, Auto & Preset",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = V4L2_WHITE_BALANCE_MANUAL,
		.def = V4L2_WHITE_BALANCE_AUTO,
		.max = V4L2_WHITE_BALANCE_SHADE,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_EXPOSURE,
		.name = "Exposure Bias",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = M10MO_MIN_EV,
		.def = 0,
		.max = M10MO_MAX_EV,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_ISO_SENSITIVITY,
		.name = "Iso",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.def = 0,
		.max = 3200,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_ISO_SENSITIVITY_AUTO,
		.name = "Iso mode",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = V4L2_ISO_SENSITIVITY_MANUAL,
		.def = V4L2_ISO_SENSITIVITY_AUTO,
		.max = V4L2_ISO_SENSITIVITY_AUTO,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_COLORFX,
		.name = "Image Color Effect",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = V4L2_COLORFX_NONE,
		.def = V4L2_COLORFX_NONE,
		.max = V4L2_COLORFX_PURPLE,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_COLORFX_CBCR,
		.name = "Image Color Effect CbCr",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.def = 0,
		.max = 0xffff,
		.step = 1,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_TEST_PATTERN,
		.name = "Test Pattern (Color Bar)",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.def = 0,
		.max = 1,
		.step = 1
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_FOCAL_ABSOLUTE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "focal length",
		.min = M10MO_FOCAL_LENGTH_DEFAULT,
		.max = M10MO_FOCAL_LENGTH_DEFAULT,
		.step = 1,
		.def = M10MO_FOCAL_LENGTH_DEFAULT,
		.flags = 0,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_FNUMBER_ABSOLUTE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "f-number",
		.min = M10MO_F_NUMBER_DEFAULT,
		.max = M10MO_F_NUMBER_DEFAULT,
		.step = 1,
		.def = M10MO_F_NUMBER_DEFAULT,
		.flags = 0,
	},
	{
		.ops = &m10mo_ctrl_ops,
		.id = V4L2_CID_FNUMBER_RANGE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "f-number range",
		.min = M10MO_F_NUMBER_RANGE,
		.max =  M10MO_F_NUMBER_RANGE,
		.step = 1,
		.def = M10MO_F_NUMBER_RANGE,
		.flags = 0,
	},
};

static int __m10mo_init_ctrl_handler(struct m10mo_device *dev)
{
	struct v4l2_ctrl_handler *hdl;
	int ret, i;

	hdl = &dev->ctrl_handler;

	ret = v4l2_ctrl_handler_init(&dev->ctrl_handler, ARRAY_SIZE(ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ctrls); i++)
		v4l2_ctrl_new_custom(&dev->ctrl_handler, &ctrls[i], NULL);

	if (dev->ctrl_handler.error) {
		ret = dev->ctrl_handler.error;
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		return ret;
	}

	dev->ctrl_handler.lock = &dev->input_lock;
	dev->sd.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);

	dev->link_freq = v4l2_ctrl_find(&dev->ctrl_handler, V4L2_CID_LINK_FREQ);
	if (NULL == dev->link_freq)
		return -ENODEV;
	v4l2_ctrl_s_ctrl(dev->link_freq, V4L2_CID_LINK_FREQ);

	dev->zsl_capture = v4l2_ctrl_find(&dev->ctrl_handler,
					V4L2_CID_START_ZSL_CAPTURE);
	if (NULL == dev->zsl_capture)
		return -ENODEV;
	v4l2_ctrl_s_ctrl(dev->zsl_capture, V4L2_CID_START_ZSL_CAPTURE);

	dev->colorfx = v4l2_ctrl_find(&dev->ctrl_handler,
				      V4L2_CID_COLORFX);
	return 0;
}

static int m10mo_s_routing(struct v4l2_subdev *sd, u32 input, u32 output, u32 config)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct atomisp_camera_caps *caps =
		dev->pdata->common.get_camera_caps();

	/* Select operating sensor. */
	if (caps->sensor_num > 1) {
		return m10mo_write(sd, 1, CATEGORY_SYSTEM,
			SYSTEM_MASTER_SENSOR, !output);
	}

	return 0;
}

static int m10mo_s_frame_interval(struct v4l2_subdev *sd,
		struct v4l2_subdev_frame_interval *interval)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int fps = 0;

	mutex_lock(&dev->input_lock);
	if (interval->interval.numerator != 0)
		fps = interval->interval.denominator / interval->interval.numerator;
	if (!fps) {
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}
	dev->fps = fps;
	dev_dbg(&client->dev, "%s: fps is %d\n", __func__, dev->fps);
	mutex_unlock(&dev->input_lock);

	return 0;
}

int m10mo_g_frame_interval(struct v4l2_subdev *sd,
		struct v4l2_subdev_frame_interval *interval)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	mutex_lock(&dev->input_lock);
	interval->interval.denominator = 30;
	interval->interval.numerator = 1;
	mutex_unlock(&dev->input_lock);
	return 0;
}

static const struct v4l2_subdev_video_ops m10mo_video_ops = {
	.try_mbus_fmt = m10mo_try_mbus_fmt,
	.s_mbus_fmt = m10mo_set_mbus_fmt,
	.g_mbus_fmt = m10mo_get_mbus_fmt,
	.s_stream = m10mo_s_stream,
	.s_parm = m10mo_s_parm,
	.enum_framesizes = m10mo_enum_framesizes,
	.s_routing = m10mo_s_routing,
	.s_frame_interval = m10mo_s_frame_interval,
	.g_frame_interval = m10mo_g_frame_interval,
};

static const struct v4l2_subdev_core_ops m10mo_core_ops = {
	.g_ctrl = v4l2_subdev_g_ctrl,
	.s_ctrl = v4l2_subdev_s_ctrl,
	.s_power = m10mo_s_power,
	.init	= m10mo_fw_start,
	.ioctl  = m10mo_ioctl,
};

static const struct v4l2_subdev_pad_ops m10mo_pad_ops = {
	.enum_mbus_code = m10mo_enum_mbus_code,
	.enum_frame_size = m10mo_enum_frame_size,
	.get_fmt = m10mo_get_pad_format,
	.set_fmt = m10mo_set_pad_format,

};

static int m10mo_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	struct m10mo_device *dev = to_m10mo_sensor(sd);
	pr_info("%s, run mode: 0x%x\n", __func__, dev->run_mode);

	mutex_lock(&dev->input_lock);

#ifdef off_c51
    *frames = 0;
#else
    if (dev->run_mode == CI_MODE_STILL_CAPTURE)
		*frames = 0;
    else
        *frames = 1;
#endif
	mutex_unlock(&dev->input_lock);

	return 0;
}

static const struct v4l2_subdev_sensor_ops m10mo_sensor_ops = {
	.g_skip_frames	= m10mo_g_skip_frames,
};

static const struct v4l2_subdev_ops m10mo_ops = {
	.core	= &m10mo_core_ops,
	.pad	= &m10mo_pad_ops,
	.video	= &m10mo_video_ops,
	.sensor = &m10mo_sensor_ops,
};

static void auto_Update_fw(struct work_struct *ws)
{
	struct i2c_client *client = v4l2_get_subdevdata(sdd);
        struct m10mo_device *dev = to_m10mo_sensor(sdd);
	int ret = 0, flag=0, timeout=6000;
	u16 result=0;

	m10mo_USB_status(1);
	wake_lock_timeout(&dev->m10mo_update_wake_lock, msecs_to_jiffies(180000));
	mutex_lock(&dev->input_lock);

	while ((dev->power == 1) && --timeout){
		printk("m10mo Auto FW upgrade timeout = %x !!!!!!!!!!!!!!!!!!!!!\n", timeout);
		msleep(10);
	}
	if (!timeout) {
		printk("m10mo Auto FW upgrade fail!!!!!!!!!!!!!!!!!!!!!\n");
		goto leave;
	}

	isp_update_status = 0;

	__m10mo_s_power(sdd, 1, true);
	ret = m10mo_fw_checksum(dev, &result);
	__m10mo_s_power(sdd, 0, true);
	dev_info(&client->dev, "m10mo FW checksum: %d\n", result);
	if(result != 0){
		isp_update_status = 0;
		msleep(200);
		__m10mo_s_power(sdd, 1, true);
		ret = m10mo_program_device(dev, 1, 0, 0);
		__m10mo_s_power(sdd, 0, true);
		if (ret)
			goto leave;
		goto readFWversion;
	}

	msleep(200);
        io3730_auto_fw_update(dev);


	printk("m10mo Auto FW upgrade isp_update_status=%x\n",isp_update_status);
	//read fw version
	msleep(200);
	m10mo_s_power_fac(1);
	ASUS_flag = flag = readFwForceUpdateFlag(dev);
	DIT_version = readDitVersion(dev);
	m10mo_s_power_fac(0);

	if(!isNeedUpdateFwWhenBoot())
		goto readFWversion;
	else if(BIT(0) & flag)
		goto readFWversion;
	else{

	msleep(200);
	__m10mo_s_power(sdd, 1, true);
	ret = m10mo_program_device(dev, 1, (fw_version_val1 << 8 |  fw_version_val2), DIT_version);
	__m10mo_s_power(sdd, 0, true);
	if (ret)
		goto leave;

	msleep(200);
	/* Power cycle chip and re-identify the version */
	__m10mo_s_power(sdd, 1, true);
	ret = m10mo_identify_fw_type(&dev->sd);
	__m10mo_s_power(sdd, 0, true);
	if (ret)
		goto leave;

	msleep(200);
	__m10mo_s_power(sdd, 1, true);
	ret = m10mo_fw_checksum(dev, &result);
	__m10mo_s_power(sdd, 0, true);
	dev_info(&client->dev, "m10mo FW checksum: %d\n", result);

	}

readFWversion:
	msleep(200);
	__m10mo_s_power(sdd, 1, false);
	m10mo_set_zsl_monitor(sdd);
	msleep(3000);
	__m10mo_s_power(sdd, 0, false);

	//read fw version
	msleep(200);
	m10mo_s_power_fac(1);
	ASUS_flag = flag = readFwForceUpdateFlag(dev);
	DIT_version = readDitVersion(dev);
	m10mo_s_power_fac(0);

leave:
	isp_update_status = 1;
	printk("m10mo Auto FW upgrade isp_update_status=%x\n",isp_update_status);
	dev_info(&client->dev, "m10mo auto_Update_fw: %d\n", ret);
	mutex_unlock(&dev->input_lock);
    	wake_unlock(&dev->m10mo_update_wake_lock);
	m10mo_USB_status(0);
}
static DECLARE_DELAYED_WORK(auto_Update_fw_work, auto_Update_fw);

static int m10mo_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct m10mo_device *dev = to_m10mo_sensor(sd);

	sysfs_remove_group(&client->dev.kobj,
			   m10mo_attribute_group);

	if (dev->pdata->common.platform_deinit)
		dev->pdata->common.platform_deinit();

	media_entity_cleanup(&dev->sd.entity);
	v4l2_device_unregister_subdev(sd);
#ifndef NOIRQ
	free_irq(client->irq, sd);
#endif
	kfree(dev);

	return 0;
}

static int m10mo_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct m10mo_device *dev;
	struct camera_mipi_info *mipi_info = NULL;
	struct proc_dir_entry* proc_entry_asus_camera;
	void* dummy = NULL;
	int ret;

	if (!client->dev.platform_data) {
		dev_err(&client->dev, "platform data missing\n");
		return -ENODEV;
	}

	/* allocate sensor device & init sub device */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(&client->dev, "%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	/*
	 * This I2C device is created by atomisp driver. Atomisp driver has
	 * a nasty assumption that all sensors uses similar platform data.
	 * For this driver we need more information. Platform data what was
	 * received with I2C device data structure points to the common
	 * structure. Pick the real platform data for this driver.
	 */
	dev->pdata = container_of(client->dev.platform_data,
				  struct m10mo_platform_data,
				  common);

	dev->cmd = M10MO_NO_CMD_REQUEST;
	dev->m10mo_mode = M10MO_PARAMETER_MODE;
	dev->requested_cmd = M10MO_NO_CMD_REQUEST;
	dev->iso_sensitivity =  REG_AE_ISOMODE_ISO100;
	dev->iso_mode = V4L2_ISO_SENSITIVITY_AUTO;
	dev->monitor_params.af_mode = AF_NORMAL;
	dev->monitor_params.exe_mode = AF_STOP;
	dev->asus_exposure_control_mode = 0;
	dev->fadj_command = 0;
	dev->fadj_offset = 0;
	dev->disable_irq_flag = 0;
	dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE;
	dev->m10mo_request_cmd_lock_flag = 0;
    dev->lock_i2c_write_flag = 0;
	dev->cancel_irq_flag = 0;
	dev->front_camera_power = 0;
	dev->m10mo_gpio_set_after_power_on_before_streaming_flag = 0;
	dev->hal_to_driver_power_on = 0;

	mutex_init(&dev->input_lock);
	mutex_init(&dev->m10mo_request_cmd_lock);
	mutex_init(&dev->m10mo_lens_work_lock);
	wake_lock_init(&dev->m10mo_wake_lock, WAKE_LOCK_SUSPEND, "m10mo_wakelock");
	wake_lock_init(&dev->m10mo_update_wake_lock, WAKE_LOCK_SUSPEND, "m10mo_update_wake_lock");
//	mutex_init(&dev->m10mo_lens_state_machine_lock);

	dev->lens_wq = create_singlethread_workqueue("m10mo_lens_wq");
	dev->cancel_auto_focus_wq = create_singlethread_workqueue("m10mo_cancel_auto_focus_wq");
	dev->cancel_optical_zoom_wq = create_singlethread_workqueue("m10mo_cancel_optical_zoom_wq");
	dev->auto_update_wq = create_singlethread_workqueue("m10mo_auto_update_wq");
	dev->gpio_set_m10mo_power_on_wq = create_singlethread_workqueue("gpio_set_m10mo_power_on_wq");
    dev->gpio_set_check_if_m10mo_stream_on_wq = create_singlethread_workqueue("gpio_set_check_if_m10mo_stream_on_wq");
//    dev->optical_zoom_wq = create_singlethread_workqueue("m10mo_optical_zoom_wq");
//    dev->auto_focus_wq = create_singlethread_workqueue("m10mo_auto_focus_wq");

	v4l2_i2c_subdev_init(&(dev->sd), client, &m10mo_ops);

	ret = m10mo_s_config(&dev->sd, client->irq);
	if (ret)
		goto out_free;

	/*
	 * We must have a way to reset the chip. If that is missing we
	 * simply can't continue.
	 */
	if (!dev->pdata->common.gpio_ctrl) {
		dev_err(&client->dev, "gpio control function missing\n");
		ret = -ENODEV;
		goto out_free_irq;
	}

	mipi_info = v4l2_get_subdev_hostdata(&dev->sd);

	ret = __m10mo_init_ctrl_handler(dev);
	if (ret)
		goto out_free_irq;

	if (mipi_info)
		dev->num_lanes = mipi_info->num_lanes;

	dev->curr_res_table = resolutions[0];
	dev->entries_curr_table = resolutions_sizes[0];

	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.ops = &m10mo_entity_ops;
	dev->sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	dev->format.code = V4L2_MBUS_FMT_UYVY8_1X16;
	dev->shot_mode = CAP_AUTO;
    dev->lens_state_machine = LENS_IDLE;

	ret = media_entity_init(&dev->sd.entity, 1, &dev->pad, 0);
	if (ret)
		goto out_free_irq;

	ret = sysfs_create_group(&client->dev.kobj,
				m10mo_attribute_group);
	if (ret) {
		dev_err(&client->dev, "%s Failed to create sysfs\n", __func__);
		goto out_sysfs_fail;
	}
    ret = gpio_request(60, "m10mo_cap_debug");
	gpio_direction_output(60, 1);

	/* Request SPI interface to enable FW update over the SPI */
	if (dev->pdata->spi_setup)
		dev->pdata->spi_setup(&dev->pdata->spi_pdata, dev);
	sdd = &dev->sd;

	queue_delayed_work(dev->auto_update_wq, &auto_Update_fw_work, 0);

	/* register switch device for m10mo information versions report */
	m10mo_switch_dev.name = "camera";
	m10mo_switch_dev.print_name = m10mo_switch_name;
	if (switch_dev_register(&m10mo_switch_dev) < 0)
		dev_err(&client->dev, "%s: fail to register m10mo switch\n", __func__);

	proc_entry_asus_camera = proc_create_data("driver/asus_camera_open", 0660, NULL, &asus_camera_proc_fops, dummy);
    proc_set_user(proc_entry_asus_camera, 1000, 1000);

	return 0;

out_sysfs_fail:
	media_entity_cleanup(&dev->sd.entity);

out_free_irq:
#ifndef NOIRQ
	free_irq(client->irq, &dev->sd);
#endif
out_free:
	v4l2_device_unregister_subdev(&dev->sd);
	destroy_workqueue(dev->lens_wq);
	destroy_workqueue(dev->cancel_auto_focus_wq);
	destroy_workqueue(dev->cancel_optical_zoom_wq);
	destroy_workqueue(dev->auto_update_wq);
	destroy_workqueue(dev->gpio_set_m10mo_power_on_wq);
	destroy_workqueue(dev->gpio_set_check_if_m10mo_stream_on_wq);
	wake_lock_destroy(&dev->m10mo_wake_lock);
	wake_lock_destroy(&dev->m10mo_update_wake_lock);
//  destroy_workqueue(dev->optical_zoom_wq);
//	destroy_workqueue(dev->auto_focus_wq);
	kfree(dev);
	return ret;
}

static const struct i2c_device_id m10mo_id[] = {
	{ M10MO_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, m10mo_id);

static struct i2c_driver m10mo_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = M10MO_NAME,
	},
	.probe = m10mo_probe,
	.remove = m10mo_remove,
	.id_table = m10mo_id,
};

static __init int init_m10mo(void)
{
	int ret;
    switch (Read_PROJ_ID()) {
		case PROJ_ID_ZX550ML:
			ret = intel_scu_ipc_iowrite8(GPIO4CTLO_REG, 0x31);
			pr_info("Project ID is ZX550ML, enable flash PIN... (%d)\n",ret);
		case PROJ_ID_ZE551ML:
		case PROJ_ID_ZE550ML:
			pr_info("Project ID is ZX550ML, M10MO init... ()\n");
			return i2c_add_driver(&m10mo_driver);
		break;

		case PROJ_ID_ZE500ML:
			pr_info("Project ID is NOT ZX550ML, M10MO exit...\n");
			return 0;
		break;
		default:
			pr_info("Project ID is NOT ZX550ML, M10MO exit...\n");
			return 0;
		break;
    }//end switch
}

static __exit void exit_m10mo(void)
{
	int ret;
	ret = intel_scu_ipc_iowrite8(GPIO4CTLO_REG, 0x30);
	i2c_del_driver(&m10mo_driver);
}

int m10mo_s_power_fac(int on)
{
	struct m10mo_device *dev = to_m10mo_sensor(sdd);
	int ret;
    struct i2c_client *client = v4l2_get_subdevdata(sdd);

	printk(KERN_INFO "%s: on: %d \n", __func__, on);
    dev->m10mo_mode = M10MO_PARAMETER_MODE;
	if (dev->power == on)
		return 0;

	dev->wait_irq_flag = M10MO_IRQ_COMMAND_AVAILABDLE;
	if (on) {
	    if(dev->disable_irq_flag){
	        enable_irq(client->irq);
			dev->disable_irq_flag = 0;
		}
//		dev->cmd = M10MO_POWERING_ON;
		ret = power_up(sdd);
		if (ret)
			return ret;
		dev->power = 1;

		ret = __m10mo_bootrom_mode_start(sdd);
		if (ret)
			goto startup_failure;

			ret = __m10mo_fw_start(sdd);
			if (ret)
				goto startup_failure;

			//ret = m10mo_request_cmd_effect(sdd, M10MO_HEAD_HALF_HOME_SEARCHING_MODE, NULL);
			//=== For debugging, to see the ISP FW version ===//
			(void) m10mo_readb(sdd, 0x00, 0x02, &fw_version_val1);
			(void) m10mo_readb(sdd, 0x00, 0x03, &fw_version_val2);
			printk(KERN_INFO "m10mo, FW version is %X%X-%x  \n", fw_version_val1, fw_version_val2, DIT_version);

#ifdef CONFIG_ASUS_FACTORY_MODE
            //=== Only for factory, tell m10mo in fac image now! ===//
            (void) m10mo_writeb(sdd, CATEGORY_ASUS, 0x02, 0x01);
#endif

	} else {
        disable_irq(client->irq);
		__m10mo_reset_irq_wait_status();
		dev->disable_irq_flag = 1;
		ret = power_down(sdd);
		dev->power = 0;
	}

	return ret;

startup_failure:
	power_down(sdd);
	dev->power = 0;
	return ret;
}

int m10mo_read_laser(u8 len, u8 category, u8 reg, u32 *val)
{
#if 1
	struct i2c_client *client = v4l2_get_subdevdata(sdd);
	struct m10mo_device *dev = to_m10mo_sensor(sdd);
	unsigned char data[5];
	struct i2c_msg msg[2];
	unsigned char recv_data[len + 1];
	int ret;

	if(m10mo_trace_log_lock == DOWNLOADING) {
        printk("@%s %d, m10mo_trace_log_lock is %d, so break here!\n", __func__, __LINE__, m10mo_trace_log_lock);
		return -ENODEV;
	}

	if(dev->m10mo_mode == M10MO_PARAMETER_MODE) {
	    printk("@%s %d, Laser value does not take effect during parameter mode, so break here!\n", __func__, __LINE__);
		return -ENODEV;
    }

	if(dev->lens_state_machine != LENS_IDLE) {
        printk("@%s %d, Laser value does not take effect if lens status is not free, so break here!\n", __func__, __LINE__);
		return -ENODEV;
	}

	if(dev->lock_i2c_write_flag) {
	    printk("@%s %d, Laser value does not take effect if m10mo is capturing, so break here!\n", __func__, __LINE__);
		return -ENODEV;
	}

	if (!client->adapter)
		return -ENODEV;

	if (len != 1 && len != 2 && len != 4)	{
		dev_err(&client->dev, "Wrong data size\n");
		return -EINVAL;
	}

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = sizeof(data);
	msg[0].buf = data;

	data[0] = 5;
	data[1] = M10MO_BYTE_READ;
	data[2] = category;
	data[3] = reg;
    data[4] = len;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len + 1;
	msg[1].buf = recv_data;

	/* isp firmware becomes stable during this time*/
	usleep_range(200, 200);

	ret = i2c_transfer(client->adapter, msg, 2);

	if (ret == 2) {
		if (len == 0x01)
			*val = recv_data[1];
		else if (len == 0x02)
			*val = recv_data[1] << 8 | recv_data[2];
		else
			*val = recv_data[1] << 24 | recv_data[2] << 16
				| recv_data[3] << 8 | recv_data[4];
	}

	printk("%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n", __func__, len, category, reg, *val);
	dev_dbg(&client->dev,
		"%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n",
		__func__, len, category, reg, *val);

	return (ret == 2) ? 0 : -EIO;
#endif
return 0;
}

int m10mo_read_fac(u8 len, u8 category, u8 reg, u32 *val)
{
#if 1
	struct i2c_client *client = v4l2_get_subdevdata(sdd);
	unsigned char data[5];
	struct i2c_msg msg[2];
	unsigned char recv_data[len + 1];
	int ret;

	if(m10mo_trace_log_lock == DOWNLOADING) {
        printk("@%s %d, m10mo_trace_log_lock is %d, so break here!\n", __func__, __LINE__, m10mo_trace_log_lock);
		return -ENODEV;
	}
	if (!client->adapter)
		return -ENODEV;

	if (len != 1 && len != 2 && len != 4)	{
		dev_err(&client->dev, "Wrong data size\n");
		return -EINVAL;
	}

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = sizeof(data);
	msg[0].buf = data;

	data[0] = 5;
	data[1] = M10MO_BYTE_READ;
	data[2] = category;
	data[3] = reg;
    data[4] = len;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len + 1;
	msg[1].buf = recv_data;

	/* isp firmware becomes stable during this time*/
	usleep_range(200, 200);

	ret = i2c_transfer(client->adapter, msg, 2);

	if (ret == 2) {
		if (len == 0x01)
			*val = recv_data[1];
		else if (len == 0x02)
			*val = recv_data[1] << 8 | recv_data[2];
		else
			*val = recv_data[1] << 24 | recv_data[2] << 16
				| recv_data[3] << 8 | recv_data[4];
	}

	printk("%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n", __func__, len, category, reg, *val);
	dev_dbg(&client->dev,
		"%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n",
		__func__, len, category, reg, *val);

	return (ret == 2) ? 0 : -EIO;
#endif
return 0;
}


int m10mo_write_fac(u8 len, u8 category, u8 reg, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sdd);
	u8 data[len + 4];
	struct i2c_msg msg;
	int ret;
	const int num_msg = 1;
	if (!client->adapter)
		return -ENODEV;

	if (len != 1 && len != 2 && len != 4) {
		dev_err(&client->dev, "Wrong data size\n");
		return -EINVAL;
	}

	dev_dbg(&client->dev,
		"%s len :%d cat, reg, val: 0x%02x, 0x%02x, 0x%02x\n",
		__func__, len, category, reg, val);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = sizeof(data);
	msg.buf = data;

	data[0] = msg.len;
	data[1] = M10MO_BYTE_WRITE;
	data[2] = category;
	data[3] = reg;
	switch (len) {
	case 1:
		data[4] = val;
		break;
	case 2:
		data[4] = ((val >> 8) & 0xFF);
		data[5] = (val & 0xFF);
		break;
	case 4:
		data[4] = ((val >> 24) & 0xFF);
		data[5] = ((val >> 16) & 0xFF);
		data[6] = ((val >> 8) & 0xFF);
		data[7] = (val & 0xFF);
		break;
	default:
		/* No possible to happen - len is already validated */
		break;
	}

	/* isp firmware becomes stable during this time*/
	usleep_range(200, 200);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if(len ==1)
	printk("@%s, Write reg. len = %d, Category=0x%02X Reg=0x%02X Value=0x%X ret=%s\n", __func__,
		len, category, reg, data[4], (ret == 1) ? "OK" : "Error");
	else
	printk("@%s, Write reg. len = %d, Category=0x%02X Reg=0x%02X Value=0x%X ret=%s\n", __func__,
		len, category, reg, val, (ret == 1) ? "OK" : "Error");
	return ret == num_msg ? 0 : -EIO;
}

int m10mo_status_fac(void)
{
	struct m10mo_device *dev;
        if (sdd == NULL) return 0;

	dev = to_m10mo_sensor(sdd);
        return dev->power;
}

#if 0
static void inline m10mo_set_ois_mode(struct v4l2_subdev *sd, m10mo_ois_mode_t ois_mode) {
    struct m10mo_device *dev = to_m10mo_sensor(sd);

	mutex_lock(&dev->m10mo_ois_work_lock);

	mutex_unlock(&dev->m10mo_ois_work_lock);
}
#endif

static void gpio_set_check_if_m10mo_stream_on_work_routine(struct work_struct *ws) {

    struct m10mo_device *dev;
	dev = to_m10mo_sensor(sdd);
	printk(KERN_INFO "@%s %d, Start. \n", __func__, __LINE__);
	if(mutex_is_locked(&dev->input_lock))
	    return;

    if(!dev->hal_to_driver_power_on) {
	    m10mo_s_power_fac(0);
		m10mo_USB_status(0);
    }
	printk(KERN_INFO "@%s %d, End. \n", __func__, __LINE__);
}

static DECLARE_DELAYED_WORK(gpio_set_check_if_m10mo_stream_on_work, gpio_set_check_if_m10mo_stream_on_work_routine);

static void gpio_set_m10mo_power_on_work_routine(struct work_struct *ws) {

    struct m10mo_device *dev;
	dev = to_m10mo_sensor(sdd);
	printk(KERN_INFO "@%s %d, Start. \n", __func__, __LINE__);

    if(dev->power
	    || dev->front_camera_power)
	    return;

	if(mutex_is_locked(&dev->input_lock))
	    return;

	mutex_lock(&dev->input_lock);
	dev->m10mo_gpio_set_after_power_on_before_streaming_flag = 1;
	m10mo_USB_status(1);
	__m10mo_s_power(sdd, 1, false);
	queue_delayed_work(dev->gpio_set_check_if_m10mo_stream_on_wq, &gpio_set_check_if_m10mo_stream_on_work, msecs_to_jiffies(5000));
	mutex_unlock(&dev->input_lock);
	printk(KERN_INFO "@%s %d, End. \n", __func__, __LINE__);
}

static DECLARE_DELAYED_WORK(gpio_set_m10mo_power_on_work, gpio_set_m10mo_power_on_work_routine);

void m10mo_gpio_set_power_on(void) {

    struct m10mo_device *dev;
	printk(KERN_INFO "@%s %d, Start. \n", __func__, __LINE__);
	if(sdd == NULL)
	    return;

	dev = to_m10mo_sensor(sdd);
    if(dev == NULL)
	    return;

    if(dev->power
	    || dev->front_camera_power)
	    return;

    queue_delayed_work(dev->gpio_set_m10mo_power_on_wq, &gpio_set_m10mo_power_on_work, 0);
    printk(KERN_INFO "@%s %d, End. \n", __func__, __LINE__);
}

void notify_m10mo_front_camera_power_status(u8 power) {
    struct m10mo_device *dev;
	printk(KERN_INFO "@%s %d, Start. power is %d \n", __func__, __LINE__, power);
	if(sdd == NULL)
	    return;

	dev = to_m10mo_sensor(sdd);
    if(dev == NULL)
	    return;

	dev->front_camera_power = power;
}

int m10mo_s_power_for_ov5670(int on) {
    struct m10mo_device *dev;
	int ret;
	dev = to_m10mo_sensor(sdd);
	printk(KERN_INFO "@%s %d, Start. on is %d\n", __func__, __LINE__, on);
	if(dev->m10mo_gpio_set_after_power_on_before_streaming_flag) {
	    dev->m10mo_gpio_set_after_power_on_before_streaming_flag = 0;
		m10mo_USB_status(0);
	}

	mutex_lock(&dev->input_lock);
	ret = m10mo_s_power_fac(on);
	mutex_unlock(&dev->input_lock);
	printk(KERN_INFO "@%s %d, End. on is %d\n", __func__, __LINE__, on);
	return ret;
}

EXPORT_SYMBOL(notify_m10mo_front_camera_power_status);
EXPORT_SYMBOL(m10mo_gpio_set_power_on);
EXPORT_SYMBOL(m10mo_write_fac);
EXPORT_SYMBOL(m10mo_read_laser);
EXPORT_SYMBOL(m10mo_read_fac);
EXPORT_SYMBOL(m10mo_s_power_fac);
EXPORT_SYMBOL(m10mo_s_power_for_ov5670);
EXPORT_SYMBOL(m10mo_status_fac);
module_init(init_m10mo);
module_exit(exit_m10mo);

MODULE_DESCRIPTION("M10MO ISP driver");
MODULE_AUTHOR("Kriti Pachhandara <kriti.pachhandara@intel.com>");
MODULE_LICENSE("GPL");

