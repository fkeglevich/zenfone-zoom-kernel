/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * Copyright (c) 2010 Silicon Hive www.siliconhive.com.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */
#include <linux/firmware.h>
#ifndef CONFIG_GMIN_INTEL_MID
#include <linux/intel_mid_pm.h>
#else
#include <linux/pci.h>
#endif
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/pm_runtime.h>
#include <linux/timer.h>
#ifndef CONFIG_GMIN_INTEL_MID
#include <linux/kct.h>
#endif
#include <asm/intel-mid.h>

#include <media/v4l2-event.h>
#include <media/videobuf-vmalloc.h>

#include "atomisp_cmd.h"
#include "atomisp_common.h"
#include "atomisp_fops.h"
#include "atomisp_internal.h"
#include "atomisp_ioctl.h"
#include "atomisp-regs.h"
#include "atomisp_tables.h"
#include "atomisp_acc.h"
#include "atomisp_compat.h"
#include "atomisp_subdev.h"
#include "atomisp_dfs_tables.h"

#include "hrt/hive_isp_css_mm_hrt.h"

#include "sh_css_hrt.h"
#include "sh_css_defs.h"
#include "system_global.h"
#include "sh_css_internal.h"
#include "sh_css_sp.h"
#include "gp_device.h"
#include "device_access.h"
#include "irq.h"

#include "ia_css_types.h"
#include "ia_css_stream.h"
#include "error_support.h"
#include "hrt/bits.h"


/* We should never need to run the flash for more than 2 frames.
 * At 15fps this means 133ms. We set the timeout a bit longer.
 * Each flash driver is supposed to set its own timeout, but
 * just in case someone else changed the timeout, we set it
 * here to make sure we don't damage the flash hardware. */
#define FLASH_TIMEOUT 800 /* ms */

union host {
	struct {
		void *kernel_ptr;
		void __user *user_ptr;
		int size;
	} scalar;
	struct {
		void *hmm_ptr;
	} ptr;
};

/*
 * atomisp_kernel_malloc: chooses whether kmalloc() or vmalloc() is preferable.
 *
 * It is also a wrap functions to pass into css framework.
 */
void *atomisp_kernel_malloc(size_t bytes)
{
	/* vmalloc() is preferable if allocating more than 1 page */
	if (bytes > PAGE_SIZE)
		return vmalloc(bytes);

	return kmalloc(bytes, GFP_KERNEL);
}

/*
 * atomisp_kernel_zalloc: chooses whether set 0 to the allocated memory.
 *
 * It is also a wrap functions to pass into css framework.
 */
void *atomisp_kernel_zalloc(size_t bytes, bool zero_mem)
{
	void *ptr = atomisp_kernel_malloc(bytes);

	if (ptr && zero_mem)
		memset(ptr, 0, bytes);

	return ptr;
}

/*
 * Free buffer allocated with atomisp_kernel_malloc()/atomisp_kernel_zalloc
 * helper
 */
void atomisp_kernel_free(void *ptr)
{
	/* Verify if buffer was allocated by vmalloc() or kmalloc() */
	if (is_vmalloc_addr(ptr))
		vfree(ptr);
	else
		kfree(ptr);
}

/*
 * get sensor:dis71430/ov2720 related info from v4l2_subdev->priv data field.
 * subdev->priv is set in mrst.c
 */
struct camera_mipi_info *atomisp_to_sensor_mipi_info(struct v4l2_subdev *sd)
{
	return (struct camera_mipi_info *)v4l2_get_subdev_hostdata(sd);
}

/*
 * get struct atomisp_video_pipe from v4l2 video_device
 */
struct atomisp_video_pipe *atomisp_to_video_pipe(struct video_device *dev)
{
	return (struct atomisp_video_pipe *)
	    container_of(dev, struct atomisp_video_pipe, vdev);
}

static unsigned short atomisp_get_sensor_fps(struct atomisp_sub_device *asd)
{
	struct v4l2_subdev_frame_interval frame_interval;
	struct atomisp_device *isp = asd->isp;
	unsigned short fps;

	if (v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
		video, g_frame_interval, &frame_interval)) {
		fps = 0;
	} else {
		if (frame_interval.interval.numerator)
			fps = frame_interval.interval.denominator /
			    frame_interval.interval.numerator;
		else
			fps = 0;
	}
	return fps;
}

#ifdef CONFIG_GMIN_INTEL_MID
	int atomisp_punit_hpll_freq;
#endif
/*
 * DFS progress is shown as follows:
 * 1. Target frequency is calculated according to FPS/Resolution/ISP running
 *    mode.
 * 2. Ratio is calculated using formula: 2 * HPLL / target frequency - 1
 *    with proper rounding.
 * 3. Set ratio to ISPFREQ40, 1 to FREQVALID and ISPFREQGUAR40
 *    to 200MHz in ISPSSPM1.
 * 4. Wait for FREQVALID to be cleared by P-Unit.
 * 5. Wait for field ISPFREQSTAT40 in ISPSSPM1 turn to ratio set in 3.
 */
static int write_target_freq_to_hw(struct atomisp_device *isp,
				   unsigned int new_freq)
{
	unsigned int ratio, timeout, guar_ratio;
	u32 isp_sspm1 = 0;
	int i;
	if (!isp->hpll_freq) {
		dev_err(isp->dev, "failed to get hpll_freq. no change to freq\n");
		return -EINVAL;
	}

	isp_sspm1 = intel_mid_msgbus_read32(PUNIT_PORT, ISPSSPM1);
	if (isp_sspm1 & ISP_FREQ_VALID_MASK) {
		dev_dbg(isp->dev, "clearing ISPSSPM1 valid bit.\n");
		intel_mid_msgbus_write32(PUNIT_PORT, ISPSSPM1,
				    isp_sspm1 & ~(1 << ISP_FREQ_VALID_OFFSET));
	}

	ratio = (2 * isp->hpll_freq + new_freq / 2) / new_freq - 1;
	guar_ratio = (2 * isp->hpll_freq + 200 / 2) / 200 - 1;

	isp_sspm1 = intel_mid_msgbus_read32(PUNIT_PORT, ISPSSPM1);
	isp_sspm1 &= ~(0x1F << ISP_REQ_FREQ_OFFSET);

	for (i = 0; i < ISP_DFS_TRY_TIMES; i++) {
		intel_mid_msgbus_write32(PUNIT_PORT, ISPSSPM1,
				   isp_sspm1
				   | ratio << ISP_REQ_FREQ_OFFSET
				   | 1 << ISP_FREQ_VALID_OFFSET
				   | guar_ratio << ISP_REQ_GUAR_FREQ_OFFSET);

		isp_sspm1 = intel_mid_msgbus_read32(PUNIT_PORT, ISPSSPM1);

		timeout = 20;
		while ((isp_sspm1 & ISP_FREQ_VALID_MASK) && timeout) {
			isp_sspm1 = intel_mid_msgbus_read32(PUNIT_PORT, ISPSSPM1);
			dev_dbg(isp->dev, "waiting for ISPSSPM1 valid bit to be 0.\n");
			udelay(100);
			timeout--;
		}

		if (timeout != 0)
			break;
	}

	if (timeout == 0) {
		dev_err(isp->dev, "DFS failed due to HW error.\n");
		return -EINVAL;
	}

	isp_sspm1 = intel_mid_msgbus_read32(PUNIT_PORT, ISPSSPM1);
	timeout = 10;
	while (((isp_sspm1 >> ISP_FREQ_STAT_OFFSET) != ratio) && timeout) {
		isp_sspm1 = intel_mid_msgbus_read32(PUNIT_PORT, ISPSSPM1);
		dev_dbg(isp->dev, "waiting for ISPSSPM1 status bit to be 0x%x.\n",
			new_freq);
		udelay(100);
		timeout--;
	}
	if (timeout == 0) {
		dev_err(isp->dev, "DFS target freq is rejected by HW.\n");
		return -EINVAL;
	}

	return 0;
}
int atomisp_freq_scaling(struct atomisp_device *isp,
			 enum atomisp_dfs_mode mode,
			 bool force)
{
	/* FIXME! Only use subdev[0] status yet */
	struct atomisp_sub_device *asd = &isp->asd[0];
	unsigned int new_freq;
	struct atomisp_freq_scaling_rule curr_rules;
	int i, ret;
	unsigned short fps = 0;

	if (isp->sw_contex.power_state != ATOM_ISP_POWER_UP) {
		dev_err(isp->dev, "DFS cannot proceed due to no power.\n");
		return -EINVAL;
	}

	if (isp->dfs->lowest_freq == 0 || isp->dfs->max_freq_at_vmin == 0 ||
	    isp->dfs->highest_freq == 0 || isp->dfs->dfs_table_size == 0 ||
	    !isp->dfs->dfs_table) {
		dev_err(isp->dev, "DFS configuration is invalid.\n");
		return -EINVAL;
	}

	if (mode == ATOMISP_DFS_MODE_LOW) {
		new_freq = isp->dfs->lowest_freq;
		goto done;
	}

	if (mode == ATOMISP_DFS_MODE_MAX) {
		new_freq = isp->dfs->highest_freq;
		goto done;
	}

	fps = atomisp_get_sensor_fps(asd);
	if (fps == 0)
		return -EINVAL;

	curr_rules.width = asd->fmt[asd->capture_pad].fmt.width;
	curr_rules.height = asd->fmt[asd->capture_pad].fmt.height;
	curr_rules.fps = fps;
	curr_rules.run_mode = asd->run_mode->val;
	/*
	 * For continuous mode, we need to make the capture setting applied
	 * since preview mode, because there is no chance to do this when
	 * starting image capture.
	 */
	if (asd->continuous_mode->val) {
		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)
			curr_rules.run_mode = ATOMISP_RUN_MODE_SDV;
		else
			curr_rules.run_mode =
				ATOMISP_RUN_MODE_CONTINUOUS_CAPTURE;
	}

	/* search for the target frequency by looping freq rules*/
	for (i = 0; i < isp->dfs->dfs_table_size; i++) {
		if (curr_rules.width != isp->dfs->dfs_table[i].width &&
		    isp->dfs->dfs_table[i].width != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.height != isp->dfs->dfs_table[i].height &&
		    isp->dfs->dfs_table[i].height != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.fps != isp->dfs->dfs_table[i].fps &&
		    isp->dfs->dfs_table[i].fps != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.run_mode != isp->dfs->dfs_table[i].run_mode &&
		    isp->dfs->dfs_table[i].run_mode != ISP_FREQ_RULE_ANY)
			continue;
		break;
	}

	if (i == isp->dfs->dfs_table_size)
		new_freq = isp->dfs->max_freq_at_vmin;
	else
		new_freq = isp->dfs->dfs_table[i].isp_freq;

done:
	dev_dbg(isp->dev, "DFS target frequency=%d.\n", new_freq);

	if ((new_freq == isp->sw_contex.running_freq) && !force)
		return 0;

	dev_dbg(isp->dev, "Programming DFS frequency to %d\n", new_freq);

	ret = write_target_freq_to_hw(isp, new_freq);
	if (!ret)
		isp->sw_contex.running_freq = new_freq;
	return ret;
}

/*
 * reset and restore ISP
 */
int atomisp_reset(struct atomisp_device *isp)
{
	/* Reset ISP by power-cycling it */
	int ret = 0;

	dev_dbg(isp->dev, "%s\n", __func__);
	atomisp_css_suspend(isp);
	ret = atomisp_runtime_suspend(isp->dev);
	if (ret < 0)
		dev_err(isp->dev, "atomisp_runtime_suspend failed, %d\n", ret);
	ret = atomisp_mrfld_power_down(isp);
	if (ret < 0) {
		dev_err(isp->dev, "can not disable ISP power\n");
	} else {
		ret = atomisp_mrfld_power_up(isp);
		if (ret < 0)
			dev_err(isp->dev, "can not enable ISP power\n");
		ret = atomisp_runtime_resume(isp->dev);
		if (ret < 0)
			dev_err(isp->dev, "atomisp_runtime_resume failed, %d\n", ret);
	}
	ret = atomisp_css_resume(isp);
	if (ret)
		isp->isp_fatal_error = true;

	return ret;
}

/*
 * interrupt enable/disable functions
 */
static void enable_isp_irq(enum hrt_isp_css_irq irq, bool enable)
{
	if (enable) {
		irq_enable_channel(IRQ0_ID, irq);
		/*sh_css_hrt_irq_enable(irq, true, false);*/
		switch (irq) { /*We only have sp interrupt right now*/
		case hrt_isp_css_irq_sp:
			/*sh_css_hrt_irq_enable_sp(true);*/
			cnd_sp_irq_enable(SP0_ID, true);
			break;
		default:
			break;
		}

	} else {
		/*sh_css_hrt_irq_disable(irq);*/
		irq_disable_channel(IRQ0_ID, irq);
		switch (irq) {
		case hrt_isp_css_irq_sp:
			/*sh_css_hrt_irq_enable_sp(false);*/
			cnd_sp_irq_enable(SP0_ID, false);
			break;
		default:
			break;
		}
	}
}

/*
 * interrupt clean function
 */
static void clear_isp_irq(enum hrt_isp_css_irq irq)
{
	irq_clear_all(IRQ0_ID);
}

void atomisp_msi_irq_init(struct atomisp_device *isp, struct pci_dev *dev)
{
	u32 msg32;
	u16 msg16;

	pci_read_config_dword(dev, PCI_MSI_CAPID, &msg32);
	msg32 |= 1 << MSI_ENABLE_BIT;
	pci_write_config_dword(dev, PCI_MSI_CAPID, msg32);

	msg32 = (1 << INTR_IER) | (1 << INTR_IIR);
	pci_write_config_dword(dev, PCI_INTERRUPT_CTRL, msg32);

	pci_read_config_word(dev, PCI_COMMAND, &msg16);
	msg16 |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
		  PCI_COMMAND_INTX_DISABLE);
	pci_write_config_word(dev, PCI_COMMAND, msg16);
}

void atomisp_msi_irq_uninit(struct atomisp_device *isp, struct pci_dev *dev)
{
	u32 msg32;
	u16 msg16;

	pci_read_config_dword(dev, PCI_MSI_CAPID, &msg32);
	msg32 &=  ~(1 << MSI_ENABLE_BIT);
	pci_write_config_dword(dev, PCI_MSI_CAPID, msg32);

	msg32 = 0x0;
	pci_write_config_dword(dev, PCI_INTERRUPT_CTRL, msg32);

	pci_read_config_word(dev, PCI_COMMAND, &msg16);
	msg16 &= ~(PCI_COMMAND_MASTER);
	pci_write_config_word(dev, PCI_COMMAND, msg16);
}

static void atomisp_sof_event(struct atomisp_sub_device *asd)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_FRAME_SYNC;
	event.u.frame_sync.frame_sequence = atomic_read(&asd->sof_count);

	v4l2_event_queue(asd->subdev.devnode, &event);
}

void atomisp_eof_event(struct atomisp_sub_device *asd, uint8_t exp_id)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_FRAME_END;
	event.u.frame_sync.frame_sequence = exp_id;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_3a_stats_ready_event(struct atomisp_sub_device *asd, uint8_t exp_id)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_3A_STATS_READY;
	event.u.frame_sync.frame_sequence = exp_id;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_metadata_ready_event(struct atomisp_sub_device *asd,
						enum atomisp_metadata_type md_type)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_METADATA_READY;
	event.u.data[0] = md_type;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void print_csi_rx_errors(enum ia_css_csi2_port port,
				struct atomisp_device *isp)
{
	u32 infos = 0;

	atomisp_css_rx_get_irq_info(port, &infos);

	dev_err(isp->dev, "CSI Receiver port %d errors:\n", port);
	if (infos & CSS_RX_IRQ_INFO_BUFFER_OVERRUN)
		dev_err(isp->dev, "  buffer overrun");
	if (infos & CSS_RX_IRQ_INFO_ERR_SOT)
		dev_err(isp->dev, "  start-of-transmission error");
	if (infos & CSS_RX_IRQ_INFO_ERR_SOT_SYNC)
		dev_err(isp->dev, "  start-of-transmission sync error");
	if (infos & CSS_RX_IRQ_INFO_ERR_CONTROL)
		dev_err(isp->dev, "  control error");
	if (infos & CSS_RX_IRQ_INFO_ERR_ECC_DOUBLE)
		dev_err(isp->dev, "  2 or more ECC errors");
	if (infos & CSS_RX_IRQ_INFO_ERR_CRC)
		dev_err(isp->dev, "  CRC mismatch");
	if (infos & CSS_RX_IRQ_INFO_ERR_UNKNOWN_ID)
		dev_err(isp->dev, "  unknown error");
	if (infos & CSS_RX_IRQ_INFO_ERR_FRAME_SYNC)
		dev_err(isp->dev, "  frame sync error");
	if (infos & CSS_RX_IRQ_INFO_ERR_FRAME_DATA)
		dev_err(isp->dev, "  frame data error");
	if (infos & CSS_RX_IRQ_INFO_ERR_DATA_TIMEOUT)
		dev_err(isp->dev, "  data timeout");
	if (infos & CSS_RX_IRQ_INFO_ERR_UNKNOWN_ESC)
		dev_err(isp->dev, "  unknown escape command entry");
	if (infos & CSS_RX_IRQ_INFO_ERR_LINE_SYNC)
		dev_err(isp->dev, "  line sync error");
}

/* Clear irq reg */
static void clear_irq_reg(struct atomisp_device *isp)
{
	u32 msg_ret;
	pci_read_config_dword(isp->pdev, PCI_INTERRUPT_CTRL, &msg_ret);
	msg_ret |= 1 << INTR_IIR;
	pci_write_config_dword(isp->pdev, PCI_INTERRUPT_CTRL, msg_ret);
}

static struct atomisp_sub_device *
__get_asd_from_port(struct atomisp_device *isp, mipi_port_ID_t port)
{
	int i;

	/* Check which isp subdev to send eof */
	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd = &isp->asd[i];
		struct camera_mipi_info *mipi_info =
				atomisp_to_sensor_mipi_info(
					isp->inputs[asd->input_curr].camera);
		if (isp->asd[i].streaming == ATOMISP_DEVICE_STREAMING_ENABLED &&
		    __get_mipi_port(isp, mipi_info->port) == port) {
			return &isp->asd[i];
		}
	}

	return NULL;
}

/* interrupt handling function*/
irqreturn_t atomisp_isr(int irq, void *dev)
{
	struct atomisp_device *isp = (struct atomisp_device *)dev;
	struct atomisp_sub_device *asd;
	struct atomisp_css_event eof_event;
	unsigned int irq_infos = 0;
	unsigned long flags;
	unsigned int i;
	int err;

	spin_lock_irqsave(&isp->lock, flags);
	if (isp->sw_contex.power_state != ATOM_ISP_POWER_UP ||
	    isp->css_initialized == false) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_HANDLED;
	}
	err = atomisp_css_irq_translate(isp, &irq_infos);
	if (err) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_NONE;
	}

	dev_dbg(isp->dev, "irq:0x%x\n", irq_infos);

	clear_irq_reg(isp);

	if (!atomisp_streaming_count(isp) && !atomisp_is_acc_enabled(isp))
		goto out_nowake;

	for (i = 0; i < isp->num_of_streams; i++) {
		asd = &isp->asd[i];

		if (asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED)
			continue;
		/*
		 * Current SOF only support one stream, so the SOF only valid
		 * either solely one stream is running
		 */
		if (irq_infos & CSS_IRQ_INFO_CSS_RECEIVER_SOF) {
			atomic_inc(&asd->sof_count);
			atomisp_sof_event(asd);

			/* If sequence_temp and sequence are the same
			 * there where no frames lost so we can increase
			 * sequence_temp.
			 * If not then processing of frame is still in progress
			 * and driver needs to keep old sequence_temp value.
			 * NOTE: There is assumption here that ISP will not
			 * start processing next frame from sensor before old
			 * one is completely done. */
			if (atomic_read(&asd->sequence) == atomic_read(
						&asd->sequence_temp))
				atomic_set(&asd->sequence_temp,
						atomic_read(&asd->sof_count));
		}
		if (irq_infos & CSS_IRQ_INFO_EVENTS_READY)
			atomic_set(&asd->sequence,
					atomic_read(&asd->sequence_temp));
	}

	if (irq_infos & CSS_IRQ_INFO_CSS_RECEIVER_SOF)
		irq_infos &= ~CSS_IRQ_INFO_CSS_RECEIVER_SOF;

	if ((irq_infos & CSS_IRQ_INFO_INPUT_SYSTEM_ERROR) ||
		(irq_infos & CSS_IRQ_INFO_IF_ERROR)) {
		/* handle mipi receiver error */
		u32 rx_infos;
		enum ia_css_csi2_port port;

		for (port = IA_CSS_CSI2_PORT0; port <= IA_CSS_CSI2_PORT2;
		     port++) {
			print_csi_rx_errors(port, isp);
			atomisp_css_rx_get_irq_info(port, &rx_infos);
			atomisp_css_rx_clear_irq_info(port, rx_infos);
		}
	}

	if (irq_infos & IA_CSS_IRQ_INFO_ISYS_EVENTS_READY) {
		while (ia_css_dequeue_isys_event(&(eof_event.event)) ==
		       IA_CSS_SUCCESS) {
			/* EOF Event does not have the css_pipe returned */
			asd = __get_asd_from_port(isp, eof_event.event.port);
			if (!asd) {
				dev_err(isp->dev, "%s:no subdev.event:%d",  __func__,
					eof_event.event.type);
				continue;
			}

			atomisp_eof_event(asd, eof_event.event.exp_id);
			printk(KERN_INFO "ASUSBSP --- @%s EOF exp_id %d\n", __func__,
				eof_event.event.exp_id);
		}

		irq_infos &= ~IA_CSS_IRQ_INFO_ISYS_EVENTS_READY;
		if (irq_infos == 0)
			goto out_nowake;
	}

	spin_unlock_irqrestore(&isp->lock, flags);

	return IRQ_WAKE_THREAD;

out_nowake:
	spin_unlock_irqrestore(&isp->lock, flags);

	return IRQ_HANDLED;
}

void atomisp_clear_css_buffer_counters(struct atomisp_sub_device *asd)
{
	int i;
	memset(asd->s3a_bufs_in_css, 0, sizeof(asd->s3a_bufs_in_css));
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++)
		memset(asd->metadata_bufs_in_css[i], 0,
			sizeof(asd->metadata_bufs_in_css[i]));
	asd->dis_bufs_in_css = 0;
	asd->video_out_capture.buffers_in_css = 0;
	asd->video_out_vf.buffers_in_css = 0;
	asd->video_out_preview.buffers_in_css = 0;
	asd->video_out_video_capture.buffers_in_css = 0;
}

bool atomisp_buffers_queued(struct atomisp_sub_device *asd)
{
	return asd->video_out_capture.buffers_in_css ||
		asd->video_out_vf.buffers_in_css ||
		asd->video_out_preview.buffers_in_css ||
		asd->video_out_video_capture.buffers_in_css ?
		    true : false;
}

/* 0x100000 is the start of dmem inside SP */
#define SP_DMEM_BASE	0x100000

void dump_sp_dmem(struct atomisp_device *isp, unsigned int addr,
		  unsigned int size)
{
	unsigned int data = 0;
	unsigned int size32 = DIV_ROUND_UP(size, sizeof(u32));

	dev_dbg(isp->dev, "atomisp_io_base:%p\n", atomisp_io_base);
	dev_dbg(isp->dev, "%s, addr:0x%x, size: %d, size32: %d\n", __func__,
			addr, size, size32);
	if (size32 * 4 + addr > 0x4000) {
		dev_err(isp->dev, "illegal size (%d) or addr (0x%x)\n",
				size32, addr);
		return;
	}
	addr += SP_DMEM_BASE;
	do {
		data = _hrt_master_port_uload_32(addr);

		dev_dbg(isp->dev, "%s, \t [0x%x]:0x%x\n", __func__, addr, data);
		addr += sizeof(unsigned int);
		size32 -= 1;
	} while (size32 > 0);
}

static struct videobuf_buffer *atomisp_css_frame_to_vbuf(
	struct atomisp_video_pipe *pipe, struct atomisp_css_frame *frame)
{
	struct videobuf_vmalloc_memory *vm_mem;
	struct atomisp_css_frame *handle;
	int i;

	for (i = 0; pipe->capq.bufs[i]; i++) {
		vm_mem = pipe->capq.bufs[i]->priv;
		handle = vm_mem->vaddr;
		if (handle && handle->data == frame->data)
			return pipe->capq.bufs[i];
	}

	return NULL;
}

static void get_buf_timestamp(struct timeval *tv)
{
	struct timespec ts;
	ktime_get_ts(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}

static void atomisp_flush_video_pipe(struct atomisp_sub_device *asd,
				     struct atomisp_video_pipe *pipe)
{
	unsigned long irqflags;
	int i;

	if (!pipe->users)
		return;

	for (i = 0; pipe->capq.bufs[i]; i++) {
		spin_lock_irqsave(&pipe->irq_lock, irqflags);
		if (pipe->capq.bufs[i]->state == VIDEOBUF_ACTIVE ||
		    pipe->capq.bufs[i]->state == VIDEOBUF_QUEUED) {
			get_buf_timestamp(&pipe->capq.bufs[i]->ts);
			pipe->capq.bufs[i]->field_count =
				atomic_read(&asd->sequence) << 1;
			dev_dbg(asd->isp->dev, "release buffers on device %s\n",
				pipe->vdev.name);
			if (pipe->capq.bufs[i]->state == VIDEOBUF_QUEUED)
				list_del_init(&pipe->capq.bufs[i]->queue);
			pipe->capq.bufs[i]->state = VIDEOBUF_ERROR;
			wake_up(&pipe->capq.bufs[i]->done);
		}
		spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
	}
}

/* Returns queued buffers back to video-core */
void atomisp_flush_bufs_and_wakeup(struct atomisp_sub_device *asd)
{
	atomisp_flush_video_pipe(asd, &asd->video_out_capture);
	atomisp_flush_video_pipe(asd, &asd->video_out_vf);
	atomisp_flush_video_pipe(asd, &asd->video_out_preview);
	atomisp_flush_video_pipe(asd, &asd->video_out_video_capture);
}

/* clean out the parameters that did not apply */
void atomisp_flush_params_queue(struct atomisp_video_pipe *pipe)
{
	struct atomisp_css_params_with_list *param;

	while (!list_empty(&pipe->per_frame_params)) {
		param = list_entry(pipe->per_frame_params.next,
				struct atomisp_css_params_with_list, list);
		list_del(&param->list);
		atomisp_free_css_parameters(&param->params);
		atomisp_kernel_free(param);
	}
}

/* find atomisp_video_pipe with css pipe id, buffer type and atomisp run_mode */
static struct atomisp_video_pipe *__atomisp_get_pipe(
		struct atomisp_sub_device *asd,
		enum atomisp_input_stream_id stream_id,
		enum atomisp_css_pipe_id css_pipe_id,
		enum atomisp_css_buffer_type buf_type)
{
	struct atomisp_device *isp = asd->isp;

	if (css_pipe_id == CSS_PIPE_ID_COPY &&
	    isp->inputs[asd->input_curr].camera_caps->
		sensor[asd->sensor_curr].stream_num > 1) {
		switch (stream_id) {
		case ATOMISP_INPUT_STREAM_PREVIEW:
			return &asd->video_out_preview;
		case ATOMISP_INPUT_STREAM_POSTVIEW:
			return &asd->video_out_vf;
		case ATOMISP_INPUT_STREAM_VIDEO:
			return &asd->video_out_video_capture;
		case ATOMISP_INPUT_STREAM_CAPTURE:
		default:
			return &asd->video_out_capture;
		}
	}

	/* video is same in online as in continuouscapture mode */
	if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_LOWLAT) {
		/*
		 * Disable vf_pp and run CSS in still capture mode. In this
		 * mode, CSS does not cause extra latency with buffering, but
		 * scaling is not available.
		 */
		return &asd->video_out_capture;
	} else if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
		/*
		 * Disable vf_pp and run CSS in video mode. This allows using
		 * ISP scaling but it has one frame delay due to CSS internal
		 * buffering.
		 */
		return &asd->video_out_video_capture;
	} else if (css_pipe_id == CSS_PIPE_ID_YUVPP) {
		/*
		 * to SOC camera, yuvpp pipe is run for capture/video/SDV/ZSL.
		 */
		if (asd->continuous_mode->val) {
			if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
				/* SDV case */
				switch (buf_type) {
				case CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME:
					return &asd->video_out_video_capture;
				case CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME:
					return &asd->video_out_preview;
				case CSS_BUFFER_TYPE_OUTPUT_FRAME:
					return &asd->video_out_capture;
				default:
					return &asd->video_out_vf;
				}
			} else if (asd->run_mode->val == ATOMISP_RUN_MODE_PREVIEW) {
				/* ZSL case */
				switch (buf_type) {
				case CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME:
					return &asd->video_out_preview;
				case CSS_BUFFER_TYPE_OUTPUT_FRAME:
					return &asd->video_out_capture;
				default:
					return &asd->video_out_vf;
				}
			}
		} else if (buf_type == CSS_BUFFER_TYPE_OUTPUT_FRAME) {
			switch (asd->run_mode->val) {
			case ATOMISP_RUN_MODE_VIDEO:
				return &asd->video_out_video_capture;
			case ATOMISP_RUN_MODE_PREVIEW:
				return &asd->video_out_preview;
			default:
				return &asd->video_out_capture;
			}
		} else if (buf_type == CSS_BUFFER_TYPE_VF_OUTPUT_FRAME) {
			if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)
				return &asd->video_out_preview;
			else
				return &asd->video_out_vf;
		}
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		/* For online video or SDV video pipe. */
		if (css_pipe_id == CSS_PIPE_ID_VIDEO ||
		    css_pipe_id == CSS_PIPE_ID_COPY ||
		    css_pipe_id == CSS_PIPE_ID_YUVPP) {
			if (buf_type == CSS_BUFFER_TYPE_OUTPUT_FRAME)
				return &asd->video_out_video_capture;
			return &asd->video_out_preview;
		}
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_PREVIEW) {
		/* For online preview or ZSL preview pipe. */
		if (css_pipe_id == CSS_PIPE_ID_PREVIEW ||
		    css_pipe_id == CSS_PIPE_ID_COPY ||
		    css_pipe_id == CSS_PIPE_ID_YUVPP)
			return &asd->video_out_preview;
	}
	/* For capture pipe. */
	if (buf_type == CSS_BUFFER_TYPE_VF_OUTPUT_FRAME)
		return &asd->video_out_vf;
	return &asd->video_out_capture;
}

enum atomisp_metadata_type
atomisp_get_metadata_type(struct atomisp_sub_device *asd,
			  enum ia_css_pipe_id pipe_id)
{
	if (!asd->continuous_mode->val)
		return ATOMISP_MAIN_METADATA;

	if (pipe_id == IA_CSS_PIPE_ID_CAPTURE) /* online capture pipe */
		return ATOMISP_SEC_METADATA;
	else
		return ATOMISP_MAIN_METADATA;
}

void atomisp_buf_done(struct atomisp_sub_device *asd, int error,
		      enum atomisp_css_buffer_type buf_type,
		      enum atomisp_css_pipe_id css_pipe_id,
		      bool q_buffers, enum atomisp_input_stream_id stream_id)
{
	struct videobuf_buffer *vb = NULL;
	struct atomisp_video_pipe *pipe = NULL;
	struct atomisp_css_buffer buffer;
	bool requeue = false;
	int err;
	unsigned long irqflags;
	struct atomisp_css_frame *frame = NULL;
	struct atomisp_s3a_buf *s3a_buf = NULL, *_s3a_buf_tmp;
	struct atomisp_dis_buf *dis_buf = NULL, *_dis_buf_tmp;
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf_tmp;
	enum atomisp_metadata_type md_type;
	struct atomisp_device *isp = asd->isp;
	struct v4l2_control ctrl;

	if (
	    buf_type != CSS_BUFFER_TYPE_METADATA &&
	    buf_type != CSS_BUFFER_TYPE_3A_STATISTICS &&
	    buf_type != CSS_BUFFER_TYPE_DIS_STATISTICS &&
	    buf_type != CSS_BUFFER_TYPE_OUTPUT_FRAME &&
	    buf_type != CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME &&
	    buf_type != CSS_BUFFER_TYPE_RAW_OUTPUT_FRAME &&
	    buf_type != CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME &&
	    buf_type != CSS_BUFFER_TYPE_VF_OUTPUT_FRAME) {
		dev_err(isp->dev, "%s, unsupported buffer type: %d\n",
			__func__, buf_type);
		return;
	}

	memset(&buffer, 0, sizeof(struct atomisp_css_buffer));
	buffer.css_buffer.type = buf_type;
	err = atomisp_css_dequeue_buffer(asd, stream_id, css_pipe_id,
			buf_type, &buffer);
	if (err) {
		dev_err(isp->dev,
			"atomisp_css_dequeue_buffer failed: 0x%x\n", err);
		return;
	}

	/* need to know the atomisp pipe for frame buffers */
	pipe = __atomisp_get_pipe(asd, stream_id, css_pipe_id, buf_type);
	if (pipe == NULL) {
		dev_err(isp->dev, "error getting atomisp pipe\n");
		return;
	}

	switch (buf_type) {
	case CSS_BUFFER_TYPE_3A_STATISTICS:
		list_for_each_entry_safe(s3a_buf, _s3a_buf_tmp,
						&asd->s3a_stats_in_css, list) {
			if (s3a_buf->s3a_data ==
				buffer.css_buffer.data.stats_3a) {
				list_del_init(&s3a_buf->list);
				list_add_tail(&s3a_buf->list,
						&asd->s3a_stats_ready);
				break;
			}
		}

		asd->s3a_bufs_in_css[css_pipe_id]--;
		atomisp_3a_stats_ready_event(asd, buffer.css_buffer.exp_id);
		dev_dbg(isp->dev, "%s: s3a stat with exp_id %d is ready\n",
			__func__, s3a_buf->s3a_data->exp_id);
		break;
	case CSS_BUFFER_TYPE_METADATA:
		if (error)
			break;

		md_type = atomisp_get_metadata_type(asd, css_pipe_id);
		list_for_each_entry_safe(md_buf, _md_buf_tmp,
					&asd->metadata_in_css[md_type], list) {
			if (md_buf->metadata ==
				buffer.css_buffer.data.metadata) {
				list_del_init(&md_buf->list);
				list_add_tail(&md_buf->list,
					&asd->metadata_ready[md_type]);
				break;
			}
		}
		asd->metadata_bufs_in_css[stream_id][css_pipe_id]--;
		atomisp_metadata_ready_event(asd, md_type);
		dev_dbg(isp->dev, "%s: metadata with exp_id %d is ready\n",
			__func__, md_buf->metadata->exp_id);
		break;
	case CSS_BUFFER_TYPE_DIS_STATISTICS:
		list_for_each_entry_safe(dis_buf, _dis_buf_tmp,
						&asd->dis_stats_in_css, list) {
			if (dis_buf->dis_data ==
				buffer.css_buffer.data.stats_dvs) {
				spin_lock_irqsave(&asd->dis_stats_lock,
						irqflags);
				list_del_init(&dis_buf->list);
				list_add(&dis_buf->list, &asd->dis_stats);
				asd->params.dis_proj_data_valid = true;
				spin_unlock_irqrestore(&asd->dis_stats_lock,
						irqflags);
				break;
			}
		}
		asd->dis_bufs_in_css--;
		dev_dbg(isp->dev, "%s: dis stat with exp_id %d is ready\n",
			__func__, dis_buf->dis_data->exp_id);
		break;
	case CSS_BUFFER_TYPE_VF_OUTPUT_FRAME:
	case CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME:
		if (isp->sw_contex.invalid_vf_frame) {
			error = true;
			isp->sw_contex.invalid_vf_frame = 0;
			dev_dbg(isp->dev, "%s css has marked this vf frame as invalid\n",
				 __func__);
		}

		pipe->buffers_in_css--;
		frame = buffer.css_buffer.data.frame;
		if (!frame) {
			WARN_ON(1);
			break;
		}
		if (!frame->valid)
			error = true;

		/* FIXME:
		 * YUVPP doesn't set postview exp_id correctlly in SDV mode.
		 * This is a WORKAROUND to set exp_id. see HSDES-1503911606.
		 */
		if (IS_BYT && buf_type == CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME &&
			asd->continuous_mode->val && ATOMISP_USE_YUVPP(asd))
			frame->exp_id = (asd->postview_exp_id++) %
						(ATOMISP_MAX_EXP_ID + 1);

		dev_dbg(isp->dev, "%s: vf frame with exp_id %d is ready\n",
			__func__, frame->exp_id);
		if (asd->params.flash_state == ATOMISP_FLASH_ONGOING) {
			if (frame->flash_state
			    == CSS_FRAME_FLASH_STATE_PARTIAL)
				dev_dbg(isp->dev, "%s thumb partially flashed\n",
					__func__);
			else if (frame->flash_state
				 == CSS_FRAME_FLASH_STATE_FULL)
				dev_dbg(isp->dev, "%s thumb completely flashed\n",
					__func__);
			else
				dev_dbg(isp->dev, "%s thumb no flash in this frame\n",
					__func__);
		}
		vb = atomisp_css_frame_to_vbuf(pipe, frame);
		WARN_ON(!vb);
		if (vb)
			pipe->frame_config_id[vb->i] = frame->isp_config_id;
		if (css_pipe_id == IA_CSS_PIPE_ID_CAPTURE &&
		    asd->pending_capture_request > 0) {
			err = atomisp_css_offline_capture_configure(asd,
					asd->params.offline_parm.num_captures,
					asd->params.offline_parm.skip_frames,
					asd->params.offline_parm.offset);
			asd->pending_capture_request--;
			dev_dbg(isp->dev, "Trigger capture again for new buffer. err=%d\n",
				err);
		}
		break;
	case CSS_BUFFER_TYPE_OUTPUT_FRAME:
	case CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME:
		if (isp->sw_contex.invalid_frame) {
			error = true;
			isp->sw_contex.invalid_frame = 0;
			dev_dbg(isp->dev, "%s css has marked this frame as invalid\n",
				__func__);
		}
		pipe->buffers_in_css--;
		frame = buffer.css_buffer.data.frame;
		if (!frame) {
			WARN_ON(1);
			break;
		}

		if (!frame->valid)
			error = true;

		/* FIXME:
		 * YUVPP doesn't set preview exp_id correctlly in ZSL mode.
		 * This is a WORKAROUND to set exp_id. see HSDES-1503911606.
		 */
		if (IS_BYT && buf_type == CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME &&
			asd->continuous_mode->val && ATOMISP_USE_YUVPP(asd))
			frame->exp_id = (asd->preview_exp_id++) %
						(ATOMISP_MAX_EXP_ID + 1);

		dev_dbg(isp->dev, "%s: main frame with exp_id %d is ready\n",
			__func__, frame->exp_id);
		vb = atomisp_css_frame_to_vbuf(pipe, frame);
		if (!vb) {
			WARN_ON(1);
			break;
		}

		pipe->frame_config_id[vb->i] = frame->isp_config_id;
		ctrl.id = V4L2_CID_FLASH_MODE;
		if (asd->params.flash_state == ATOMISP_FLASH_ONGOING) {
			if (frame->flash_state
			    == CSS_FRAME_FLASH_STATE_PARTIAL) {
				asd->frame_status[vb->i] =
					ATOMISP_FRAME_STATUS_FLASH_PARTIAL;
				dev_dbg(isp->dev, "%s partially flashed\n",
					 __func__);
			} else if (frame->flash_state
				   == CSS_FRAME_FLASH_STATE_FULL) {
				asd->frame_status[vb->i] =
					ATOMISP_FRAME_STATUS_FLASH_EXPOSED;
				asd->params.num_flash_frames--;
				dev_dbg(isp->dev, "%s completely flashed\n",
					 __func__);
			} else {
				asd->frame_status[vb->i] =
					ATOMISP_FRAME_STATUS_OK;
				dev_dbg(isp->dev,
					 "%s no flash in this frame\n",
					 __func__);
			}

			/* Check if flashing sequence is done */
			if (asd->frame_status[vb->i] ==
				ATOMISP_FRAME_STATUS_FLASH_EXPOSED)
				asd->params.flash_state =
					ATOMISP_FLASH_DONE;
		} else if (v4l2_subdev_call(isp->flash, core, g_ctrl, &ctrl) ==
			0 && ctrl.value == ATOMISP_FLASH_MODE_TORCH) {
			ctrl.id = V4L2_CID_FLASH_TORCH_INTENSITY;
			if (v4l2_subdev_call(isp->flash, core, g_ctrl, &ctrl) ==
				0 && ctrl.value > 0) {
				asd->frame_status[vb->i] =
					ATOMISP_FRAME_STATUS_FLASH_EXPOSED;
			} else {
				asd->frame_status[vb->i] =
					ATOMISP_FRAME_STATUS_OK;
			}
		} else {
			asd->frame_status[vb->i] = ATOMISP_FRAME_STATUS_OK;
		}

		asd->params.last_frame_status = asd->frame_status[vb->i];

		if (asd->continuous_mode->val) {
			if (css_pipe_id == CSS_PIPE_ID_PREVIEW ||
			    css_pipe_id == CSS_PIPE_ID_VIDEO) {
				asd->latest_preview_exp_id = frame->exp_id;
			} else if (css_pipe_id ==
					CSS_PIPE_ID_CAPTURE) {
				if (asd->run_mode->val ==
					ATOMISP_RUN_MODE_VIDEO)
					dev_dbg(isp->dev, "SDV capture raw buffer id: %u\n",
					    frame->exp_id);
				else
					dev_dbg(isp->dev, "ZSL capture raw buffer id: %u\n",
					    frame->exp_id);
			}
		}
		/*
		 * Only after enabled the raw buffer lock
		 * and in continuous mode.
		 * in preview/video pipe, each buffer will
		 * be locked automatically, so record it here.
		 */
		if (((css_pipe_id == CSS_PIPE_ID_PREVIEW) ||
			(css_pipe_id == CSS_PIPE_ID_VIDEO)) &&
			asd->enable_raw_buffer_lock->val &&
			asd->continuous_mode->val) {
			atomisp_set_raw_buffer_bitmap(asd, frame->exp_id);
			WARN_ON(frame->exp_id > ATOMISP_MAX_EXP_ID);
		}

		break;
	default:
		break;
	}
	if (vb) {
		get_buf_timestamp(&vb->ts);
		vb->field_count = atomic_read(&asd->sequence) << 1;
		/*mark videobuffer done for dequeue*/
		spin_lock_irqsave(&pipe->irq_lock, irqflags);
		vb->state = !error ? VIDEOBUF_DONE : VIDEOBUF_ERROR;
		spin_unlock_irqrestore(&pipe->irq_lock, irqflags);

		/*
		 * Frame capture done, wake up any process block on
		 * current active buffer
		 * possibly hold by videobuf_dqbuf()
		 */
		wake_up(&vb->done);
	}

	/*
	 * Requeue should only be done for 3a and dis buffers.
	 * Queue/dequeue order will change if driver recycles image buffers.
	 */
	if (requeue) {
		err = atomisp_css_queue_buffer(asd,
					stream_id, css_pipe_id,
					buf_type, &buffer);
		if (err)
			dev_err(isp->dev, "%s, q to css fails: %d\n",
					__func__, err);
		return;
	}
	if (!error && q_buffers)
		atomisp_qbuffers_to_css(asd);
}

void atomisp_delayed_init_work(struct work_struct *work)
{
	struct atomisp_sub_device *asd = container_of(work,
			struct atomisp_sub_device,
			delayed_init_work);
	/*
	 * to SOC camera, use yuvpp pipe and no support continuous mode.
	 */
	if (!ATOMISP_USE_YUVPP(asd)) {
		struct v4l2_event event = {0};

		atomisp_css_allocate_continuous_frames(false, asd);
		atomisp_css_update_continuous_frames(asd);

		event.type = V4L2_EVENT_ATOMISP_RAW_BUFFERS_ALLOC_DONE;
		v4l2_event_queue(asd->subdev.devnode, &event);
	}

	/* signal streamon after delayed init is done */
	asd->delayed_init = ATOMISP_DELAYED_INIT_DONE;
	complete(&asd->init_done);
}

static void __atomisp_css_recover(struct atomisp_device *isp, bool isp_timeout)
{
	enum atomisp_css_pipe_id css_pipe_id;
	bool stream_restart[MAX_STREAM_NUM] = {0};
	bool depth_mode = false;
	int i, ret, depth_cnt = 0;

	if (!isp->sw_contex.file_input)
		atomisp_css_irq_enable(isp,
				CSS_IRQ_INFO_CSS_RECEIVER_SOF, false);

	BUG_ON(isp->num_of_streams > MAX_STREAM_NUM);

	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd = &isp->asd[i];
		struct ia_css_pipeline *acc_pipeline;
		struct ia_css_pipe *acc_pipe = NULL;

		if (asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED &&
		    !asd->stream_prepared)
			continue;

		/*
		* AtomISP::waitStageUpdate is blocked when WDT happens.
		* By calling acc_done() for all loaded fw_handles,
		* HAL will be unblocked.
		*/
		acc_pipe = asd->stream_env[i].pipes[CSS_PIPE_ID_ACC];
		if (acc_pipe != NULL) {
			acc_pipeline = ia_css_pipe_get_pipeline(acc_pipe);
			if (acc_pipeline) {
				struct ia_css_pipeline_stage *stage;
				for (stage = acc_pipeline->stages; stage;
					stage = stage->next) {
					const struct ia_css_fw_info *fw;
					fw = stage->firmware;
					atomisp_acc_done(asd, fw->handle);
				}
			}
		}

		depth_cnt++;

		if (asd->delayed_init == ATOMISP_DELAYED_INIT_QUEUED)
			cancel_work_sync(&asd->delayed_init_work);

		complete(&asd->init_done);
		asd->delayed_init = ATOMISP_DELAYED_INIT_NOT_QUEUED;

		stream_restart[asd->index] = true;

		asd->streaming = ATOMISP_DEVICE_STREAMING_STOPPING;

		css_pipe_id = atomisp_get_css_pipe_id(asd);
		atomisp_css_stop(asd, css_pipe_id, true);

		atomisp_acc_unload_extensions(asd);

		/* stream off sensor */
		ret = v4l2_subdev_call(
				isp->inputs[asd->input_curr].
				camera, video, s_stream, 0);
		if (ret)
			dev_warn(isp->dev,
					"can't stop streaming on sensor!\n");

		atomisp_clear_css_buffer_counters(asd);
		asd->streaming = ATOMISP_DEVICE_STREAMING_DISABLED;

		asd->preview_exp_id = 1;
		asd->postview_exp_id = 1;
	}

	/* clear irq */
	enable_isp_irq(hrt_isp_css_irq_sp, false);
	clear_isp_irq(hrt_isp_css_irq_sp);

	/* Set the SRSE to 3 before resetting */
	pci_write_config_dword(isp->pdev, PCI_I_CONTROL, isp->saved_regs.i_control |
			MRFLD_PCI_I_CONTROL_SRSE_RESET_MASK);

	/* reset ISP and restore its state */
	isp->isp_timeout = true;
	atomisp_reset(isp);
	isp->isp_timeout = false;

	/* The following frame after an ISP timeout
	 * may be corrupted, so mark it so. */
	isp->sw_contex.invalid_frame = 1;
	isp->sw_contex.invalid_vf_frame = 1;

	if (!isp_timeout) {
		for (i = 0; i < isp->num_of_streams; i++) {
			if (isp->asd[i].depth_mode->val)
				return;
		}
	}

	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd = &isp->asd[i];

		if (!stream_restart[i])
			continue;

		if (isp->inputs[asd->input_curr].type != FILE_INPUT)
			atomisp_css_input_set_mode(asd,
					CSS_INPUT_MODE_SENSOR);

		css_pipe_id = atomisp_get_css_pipe_id(asd);
		atomisp_css_start(asd, css_pipe_id, true);

		asd->streaming = ATOMISP_DEVICE_STREAMING_ENABLED;

		atomisp_csi2_configure(asd);
	}

	if (!isp->sw_contex.file_input) {
		atomisp_css_irq_enable(isp, CSS_IRQ_INFO_CSS_RECEIVER_SOF,
				atomisp_css_valid_sof(isp));

		if (atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_AUTO, true) < 0)
			dev_dbg(isp->dev, "dfs failed!\n");
	} else {
		if (atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_MAX, true) < 0)
			dev_dbg(isp->dev, "dfs failed!\n");
	}

	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd;

		asd = &isp->asd[i];

		if (!stream_restart[i])
			continue;

		if (asd->continuous_mode->val &&
		    asd->delayed_init == ATOMISP_DELAYED_INIT_NOT_QUEUED) {
#ifndef CONFIG_GMIN_INTEL_MID
			INIT_COMPLETION(asd->init_done);
#else
			reinit_completion(&asd->init_done);
#endif
			asd->delayed_init = ATOMISP_DELAYED_INIT_QUEUED;
			queue_work(asd->delayed_init_workq,
					&asd->delayed_init_work);
		}
		/*
		 * dequeueing buffers is not needed. CSS will recycle
		 * buffers that it has.
		 */
		atomisp_flush_bufs_and_wakeup(asd);

		if ((asd->depth_mode->val) &&
			(depth_cnt == ATOMISP_DEPTH_SENSOR_STREAMON_COUNT)) {
			depth_mode = true;
			continue;
		}

		ret = v4l2_subdev_call(
				isp->inputs[asd->input_curr].camera, video,
				s_stream, 1);
		if (ret)
			dev_warn(isp->dev,
					"can't start streaming on sensor!\n");

	}

	if (depth_mode) {
		if (atomisp_stream_on_master_slave_sensor(isp, true))
			dev_warn(isp->dev,
				 "master slave sensor stream on failed!\n");
	}
}

void atomisp_wdt_work(struct work_struct *work)
{
	struct atomisp_device *isp = container_of(work, struct atomisp_device,
						  wdt_work);
	int i;

	rt_mutex_lock(&isp->mutex);
	if (!atomisp_streaming_count(isp)) {
		atomic_set(&isp->wdt_work_queued, 0);
		rt_mutex_unlock(&isp->mutex);
		return;
	}

	dev_err(isp->dev, "timeout %d of %d\n",
		atomic_read(&isp->wdt_count) + 1,
		ATOMISP_ISP_MAX_TIMEOUT_COUNT);

	if (atomic_inc_return(&isp->wdt_count) <
			ATOMISP_ISP_MAX_TIMEOUT_COUNT) {
		unsigned int old_dbglevel = dbg_level;
		atomisp_css_debug_dump_sp_sw_debug_info();
		atomisp_css_debug_dump_debug_info(__func__);
		dbg_level = old_dbglevel;
		for (i = 0; i < isp->num_of_streams; i++) {
			struct atomisp_sub_device *asd = &isp->asd[i];

			if (asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED)
				continue;
			dev_err(isp->dev, "%s, vdev %s buffers in css: %d\n",
				__func__,
				asd->video_out_capture.vdev.name,
				asd->video_out_capture.
				buffers_in_css);
			dev_err(isp->dev,
				"%s, vdev %s buffers in css: %d\n",
				__func__,
				asd->video_out_vf.vdev.name,
				asd->video_out_vf.
				buffers_in_css);
			dev_err(isp->dev,
				"%s, vdev %s buffers in css: %d\n",
				__func__,
				asd->video_out_preview.vdev.name,
				asd->video_out_preview.
				buffers_in_css);
			dev_err(isp->dev,
				"%s, vdev %s buffers in css: %d\n",
				__func__,
				asd->video_out_video_capture.vdev.name,
				asd->video_out_video_capture.
				buffers_in_css);
			dev_err(isp->dev,
				"%s, s3a buffers in css preview pipe:%d\n",
				__func__,
				asd->s3a_bufs_in_css[CSS_PIPE_ID_PREVIEW]);
			dev_err(isp->dev,
				"%s, s3a buffers in css capture pipe:%d\n",
				__func__,
				asd->s3a_bufs_in_css[CSS_PIPE_ID_CAPTURE]);
			dev_err(isp->dev,
				"%s, s3a buffers in css video pipe:%d\n",
				__func__,
				asd->s3a_bufs_in_css[CSS_PIPE_ID_VIDEO]);
			dev_err(isp->dev,
				"%s, dis buffers in css: %d\n",
				__func__, asd->dis_bufs_in_css);
			dev_err(isp->dev,
				"%s, metadata buffers in css preview pipe:%d\n",
				__func__,
				asd->metadata_bufs_in_css
				[ATOMISP_INPUT_STREAM_GENERAL]
				[CSS_PIPE_ID_PREVIEW]);
			dev_err(isp->dev,
				"%s, metadata buffers in css capture pipe:%d\n",
				__func__,
				asd->metadata_bufs_in_css
				[ATOMISP_INPUT_STREAM_GENERAL]
				[CSS_PIPE_ID_CAPTURE]);
			dev_err(isp->dev,
				"%s, metadata buffers in css video pipe:%d\n",
				__func__,
				asd->metadata_bufs_in_css
				[ATOMISP_INPUT_STREAM_GENERAL]
				[CSS_PIPE_ID_VIDEO]);
			if (asd->enable_raw_buffer_lock->val) {
				unsigned int j;

				dev_err(isp->dev, "%s, raw_buffer_locked_count %d\n",
					__func__, asd->raw_buffer_locked_count);
				for (j = 0; j <= ATOMISP_MAX_EXP_ID/32; j++)
					dev_err(isp->dev, "%s, raw_buffer_bitmap[%d]: 0x%x\n",
						__func__, j,
						asd->raw_buffer_bitmap[j]);
			}
		}

		/*sh_css_dump_sp_state();*/
		/*sh_css_dump_isp_state();*/
	} else {
		for (i = 0; i < isp->num_of_streams; i++) {
			struct atomisp_sub_device *asd = &isp->asd[i];
			if (asd->streaming ==
			    ATOMISP_DEVICE_STREAMING_ENABLED) {
				atomisp_clear_css_buffer_counters(asd);
				atomisp_flush_bufs_and_wakeup(asd);
				complete(&asd->init_done);
			}
		}

		atomic_set(&isp->wdt_count, 0);
		isp->isp_fatal_error = true;
		atomic_set(&isp->wdt_work_queued, 0);

		rt_mutex_unlock(&isp->mutex);
		return;
	}

#ifndef CONFIG_GMIN_INTEL_MID
	/* Push CrashEvent log for recovery cases tracking */
	if (dbg_level > 5)
		kct_log(CT_EV_CRASH, "ATOMISP2", "TIMEOUT", 0, "", "", "", "", "", "", "/logs/aplog");
#endif

	__atomisp_css_recover(isp, true);
	atomisp_set_stop_timeout(ATOMISP_CSS_STOP_TIMEOUT_US);
	dev_err(isp->dev, "timeout recovery handling done\n");
	atomic_set(&isp->wdt_work_queued, 0);

	rt_mutex_unlock(&isp->mutex);
}

void atomisp_css_flush(struct atomisp_device *isp)
{
	int i;

	if (!atomisp_streaming_count(isp))
		return;

	/* Disable wdt */
	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd = &isp->asd[i];
		atomisp_wdt_stop(asd, true);
	}

	/* Start recover */
	__atomisp_css_recover(isp, false);
	/* Restore wdt */
	for (i = 0; i < isp->num_of_streams; i++) {
		struct atomisp_sub_device *asd = &isp->asd[i];

		if (asd->streaming !=
				ATOMISP_DEVICE_STREAMING_ENABLED)
			continue;

		atomisp_wdt_refresh(asd,
				isp->sw_contex.file_input ?
				ATOMISP_ISP_FILE_TIMEOUT_DURATION :
				ATOMISP_ISP_TIMEOUT_DURATION);
	}
	dev_dbg(isp->dev, "atomisp css flush done\n");
}

void atomisp_wdt(unsigned long isp_addr)
{
	struct atomisp_device *isp = (struct atomisp_device *)isp_addr;

	if (atomic_read(&isp->wdt_work_queued)) {
		dev_dbg(isp->dev, "ISP watchdog was put into workqueue\n");
		return;
	}
	atomic_set(&isp->wdt_work_queued, 1);
	queue_work(isp->wdt_work_queue, &isp->wdt_work);
}

void atomisp_wdt_refresh(struct atomisp_sub_device *asd, unsigned int delay)
{
	unsigned long next;

	if (delay != ATOMISP_WDT_KEEP_CURRENT_DELAY)
		asd->wdt_duration = delay;

	next = jiffies + asd->wdt_duration;

	/* Override next if it has been pushed beyon the "next" time */
	if (atomisp_is_wdt_running(asd) && time_after(asd->wdt_expires, next))
		next = asd->wdt_expires;

	asd->wdt_expires = next;

	if (atomisp_is_wdt_running(asd))
		dev_dbg(asd->isp->dev, "WDT will hit after %d ms\n",
			((int)(next - jiffies) * 1000 / HZ));
	else
		dev_dbg(asd->isp->dev, "WDT starts with %d ms period\n",
			((int)(next - jiffies) * 1000 / HZ));

	mod_timer(&asd->wdt, next);
	atomic_set(&asd->isp->wdt_count, 0);
}

void atomisp_wdt_stop(struct atomisp_sub_device *asd, bool sync)
{
	dev_dbg(asd->isp->dev, "WDT stop\n");
	if (sync) {
		del_timer_sync(&asd->wdt);
		cancel_work_sync(&asd->isp->wdt_work);
	} else {
		del_timer(&asd->wdt);
	}
}

void atomisp_wdt_start(struct atomisp_sub_device *asd)
{
	atomisp_wdt_refresh(asd, ATOMISP_ISP_TIMEOUT_DURATION);
}

void atomisp_setup_flash(struct atomisp_sub_device *asd)
{
	struct atomisp_device *isp = asd->isp;
	struct v4l2_control ctrl;

	if (asd->params.flash_state != ATOMISP_FLASH_REQUESTED &&
	    asd->params.flash_state != ATOMISP_FLASH_DONE)
		return;

	if (asd->params.num_flash_frames) {
		/* make sure the timeout is set before setting flash mode */
		ctrl.id = V4L2_CID_FLASH_TIMEOUT;
		ctrl.value = FLASH_TIMEOUT;

		if (v4l2_subdev_call(isp->flash, core, s_ctrl, &ctrl)) {
			dev_err(isp->dev, "flash timeout configure failed\n");
			return;
		}

		atomisp_css_request_flash(asd);
		asd->params.flash_state = ATOMISP_FLASH_ONGOING;
	} else {
		asd->params.flash_state = ATOMISP_FLASH_IDLE;
	}
}

irqreturn_t atomisp_isr_thread(int irq, void *isp_ptr)
{
	struct atomisp_device *isp = isp_ptr;
	unsigned long flags;
	bool frame_done_found[MAX_STREAM_NUM] = {0};
	bool css_pipe_done[MAX_STREAM_NUM] = {0};
	unsigned int i;
	struct atomisp_sub_device *asd = &isp->asd[0];

	dev_dbg(isp->dev, ">%s\n", __func__);

	spin_lock_irqsave(&isp->lock, flags);

	if (!atomisp_streaming_count(isp) && !atomisp_is_acc_enabled(isp)) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&isp->lock, flags);

	/*
	 * The standard CSS2.0 API tells the following calling sequence of
	 * dequeue ready buffers:
	 * while (ia_css_dequeue_event(...)) {
	 *	switch (event.type) {
	 *	...
	 *	ia_css_pipe_dequeue_buffer()
	 *	}
	 * }
	 * That is, dequeue event and buffer are one after another.
	 *
	 * But the following implementation is to first deuque all the event
	 * to a FIFO, then process the event in the FIFO.
	 * This will not have issue in single stream mode, but it do have some
	 * issue in multiple stream case. The issue is that
	 * ia_css_pipe_dequeue_buffer() will not return the corrent buffer in
	 * a specific pipe.
	 *
	 * This is due to ia_css_pipe_dequeue_buffer() does not take the
	 * ia_css_pipe parameter.
	 *
	 * So:
	 * For CSS2.0: we change the way to not dequeue all the event at one
	 * time, instead, dequue one and process one, then another
	 */
	rt_mutex_lock(&isp->mutex);
	if (atomisp_css_isr_thread(isp, frame_done_found, css_pipe_done))
		goto out;

	for (i = 0; i < isp->num_of_streams; i++) {
		asd = &isp->asd[i];
		if (asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED)
			continue;
		if (frame_done_found[asd->index] &&
		    asd->params.css_update_params_needed) {
			atomisp_css_update_isp_params(asd);
			asd->params.css_update_params_needed = false;
			frame_done_found[asd->index] = false;
		}
		atomisp_setup_flash(asd);

	}
out:
	rt_mutex_unlock(&isp->mutex);
	for (i = 0; i < isp->num_of_streams; i++) {
		asd = &isp->asd[i];
		if (asd->streaming == ATOMISP_DEVICE_STREAMING_ENABLED
		    && css_pipe_done[asd->index]
		    && isp->sw_contex.file_input)
			v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
					 video, s_stream, 1);
		/* FIXME! FIX ACC implementation */
		if (asd->acc.pipeline && css_pipe_done[asd->index])
			atomisp_css_acc_done(asd);
	}
	dev_dbg(isp->dev, "<%s\n", __func__);

	return IRQ_HANDLED;
}

/*
 * utils for buffer allocation/free
 */

int atomisp_get_frame_pgnr(struct atomisp_device *isp,
			   const struct atomisp_css_frame *frame, u32 *p_pgnr)
{
	if (!frame) {
		dev_err(isp->dev, "%s: NULL frame pointer ERROR.\n", __func__);
		return -EINVAL;
	}

	*p_pgnr = DIV_ROUND_UP(frame->data_bytes, PAGE_SIZE);
	return 0;
}

/*
 * Get internal fmt according to V4L2 fmt
 */
static enum atomisp_css_frame_format
v4l2_fmt_to_sh_fmt(u32 fmt)
{
	switch (fmt) {
	case V4L2_PIX_FMT_YUV420:
		return CSS_FRAME_FORMAT_YUV420;
	case V4L2_PIX_FMT_YVU420:
		return CSS_FRAME_FORMAT_YV12;
	case V4L2_PIX_FMT_YUV422P:
		return CSS_FRAME_FORMAT_YUV422;
	case V4L2_PIX_FMT_YUV444:
		return CSS_FRAME_FORMAT_YUV444;
	case V4L2_PIX_FMT_NV12:
		return CSS_FRAME_FORMAT_NV12;
	case V4L2_PIX_FMT_NV21:
		return CSS_FRAME_FORMAT_NV21;
	case V4L2_PIX_FMT_NV16:
		return CSS_FRAME_FORMAT_NV16;
	case V4L2_PIX_FMT_NV61:
		return CSS_FRAME_FORMAT_NV61;
	case V4L2_PIX_FMT_UYVY:
		return CSS_FRAME_FORMAT_UYVY;
	case V4L2_PIX_FMT_YUYV:
		return CSS_FRAME_FORMAT_YUYV;
	case V4L2_PIX_FMT_RGB24:
		return CSS_FRAME_FORMAT_PLANAR_RGB888;
	case V4L2_PIX_FMT_RGB32:
		return CSS_FRAME_FORMAT_RGBA888;
	case V4L2_PIX_FMT_RGB565:
		return CSS_FRAME_FORMAT_RGB565;
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_CUSTOM_M10MO_RAW:
		return CSS_FRAME_FORMAT_BINARY_8;
	case V4L2_PIX_FMT_SBGGR16:
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		return CSS_FRAME_FORMAT_RAW;
	default:
		return -EINVAL;
	}
}
/*
 * raw format match between SH format and V4L2 format
 */
static int raw_output_format_match_input(u32 input, u32 output)
{
	if ((input == CSS_FORMAT_RAW_12) &&
	    ((output == V4L2_PIX_FMT_SRGGB12) ||
	     (output == V4L2_PIX_FMT_SGRBG12) ||
	     (output == V4L2_PIX_FMT_SBGGR12) ||
	     (output == V4L2_PIX_FMT_SGBRG12)))
		return 0;

	if ((input == CSS_FORMAT_RAW_10) &&
	    ((output == V4L2_PIX_FMT_SRGGB10) ||
	     (output == V4L2_PIX_FMT_SGRBG10) ||
	     (output == V4L2_PIX_FMT_SBGGR10) ||
	     (output == V4L2_PIX_FMT_SGBRG10)))
		return 0;

	if ((input == CSS_FORMAT_RAW_8) &&
	    ((output == V4L2_PIX_FMT_SRGGB8) ||
	     (output == V4L2_PIX_FMT_SGRBG8) ||
	     (output == V4L2_PIX_FMT_SBGGR8) ||
	     (output == V4L2_PIX_FMT_SGBRG8)))
		return 0;

	if ((input == CSS_FORMAT_RAW_16) && (output == V4L2_PIX_FMT_SBGGR16))
		return 0;

	return -EINVAL;
}

static u32 get_pixel_depth(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_YVU420:
		return 12;
	case V4L2_PIX_FMT_YUV422P:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_SBGGR16:
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		return 16;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_YUV444:
		return 24;
	case V4L2_PIX_FMT_RGB32:
		return 32;
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_CUSTOM_M10MO_RAW:
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		return 8;
	default:
		return 8 * 2;	/* raw type now */
	}
}

bool atomisp_is_mbuscode_raw(uint32_t code)
{
	return code >= 0x3000 && code < 0x4000;
}

/*
 * ISP features control function
 */

/*
 * Set ISP capture mode based on current settings
 */
static void atomisp_update_capture_mode(struct atomisp_sub_device *asd)
{
	if (asd->params.gdc_cac_en)
		atomisp_css_capture_set_mode(asd, CSS_CAPTURE_MODE_ADVANCED);
	else if (asd->params.low_light)
		atomisp_css_capture_set_mode(asd, CSS_CAPTURE_MODE_LOW_LIGHT);
	else if (asd->video_out_capture.sh_fmt == CSS_FRAME_FORMAT_RAW)
		atomisp_css_capture_set_mode(asd, CSS_CAPTURE_MODE_RAW);
	else
		atomisp_css_capture_set_mode(asd, CSS_CAPTURE_MODE_PRIMARY);
}

/*
 * Function to enable/disable lens geometry distortion correction (GDC) and
 * chromatic aberration correction (CAC)
 */
int atomisp_gdc_cac(struct atomisp_sub_device *asd, int flag,
		    __s32 *value)
{
	if (flag == 0) {
		*value = asd->params.gdc_cac_en;
		return 0;
	}

	asd->params.gdc_cac_en = !!*value;
	if (asd->params.gdc_cac_en) {
		atomisp_css_set_morph_table(asd,
				asd->params.css_param.morph_table);
	} else {
		atomisp_css_set_morph_table(asd, NULL);
	}
	asd->params.css_update_params_needed = true;
	atomisp_update_capture_mode(asd);
	return 0;
}

/*
 * Function to enable/disable low light mode including ANR
 */
int atomisp_low_light(struct atomisp_sub_device *asd, int flag,
		      __s32 *value)
{
	if (flag == 0) {
		*value = asd->params.low_light;
		return 0;
	}

	asd->params.low_light = (*value != 0);
	atomisp_update_capture_mode(asd);
	return 0;
}

/*
 * Function to enable/disable extra noise reduction (XNR) in low light
 * condition
 */
int atomisp_xnr(struct atomisp_sub_device *asd, int flag,
		int *xnr_enable)
{
	if (flag == 0) {
		*xnr_enable = asd->params.xnr_en;
		return 0;
	}

	atomisp_css_capture_enable_xnr(asd, !!*xnr_enable);

	return 0;
}

/*
 * Function to configure bayer noise reduction
 */
int atomisp_nr(struct atomisp_sub_device *asd, int flag,
	       struct atomisp_nr_config *arg)
{
	if (flag == 0) {
		/* Get nr config from current setup */
		if (atomisp_css_get_nr_config(asd, arg))
			return -EINVAL;
	} else {
		/* Set nr config to isp parameters */
		memcpy(&asd->params.css_param.nr_config, arg,
			sizeof(struct atomisp_css_nr_config));
		atomisp_css_set_nr_config(asd, &asd->params.css_param.nr_config);
		asd->params.css_update_params_needed = true;
	}
	return 0;
}

/*
 * Function to configure temporal noise reduction (TNR)
 */
int atomisp_tnr(struct atomisp_sub_device *asd, int flag,
		struct atomisp_tnr_config *config)
{
	/* Get tnr config from current setup */
	if (flag == 0) {
		/* Get tnr config from current setup */
		if (atomisp_css_get_tnr_config(asd, config))
			return -EINVAL;
	} else {
		/* Set tnr config to isp parameters */
		memcpy(&asd->params.css_param.tnr_config, config,
			sizeof(struct atomisp_css_tnr_config));
		atomisp_css_set_tnr_config(asd, &asd->params.css_param.tnr_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to configure black level compensation
 */
int atomisp_black_level(struct atomisp_sub_device *asd, int flag,
			struct atomisp_ob_config *config)
{
	if (flag == 0) {
		/* Get ob config from current setup */
		if (atomisp_css_get_ob_config(asd, config))
			return -EINVAL;
	} else {
		/* Set ob config to isp parameters */
		memcpy(&asd->params.css_param.ob_config, config,
			sizeof(struct atomisp_css_ob_config));
		atomisp_css_set_ob_config(asd, &asd->params.css_param.ob_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to configure edge enhancement
 */
int atomisp_ee(struct atomisp_sub_device *asd, int flag,
	       struct atomisp_ee_config *config)
{
	if (flag == 0) {
		/* Get ee config from current setup */
		if (atomisp_css_get_ee_config(asd, config))
			return -EINVAL;
	} else {
		/* Set ee config to isp parameters */
		memcpy(&asd->params.css_param.ee_config, config,
		       sizeof(asd->params.css_param.ee_config));
		atomisp_css_set_ee_config(asd, &asd->params.css_param.ee_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to update Gamma table for gamma, brightness and contrast config
 */
int atomisp_gamma(struct atomisp_sub_device *asd, int flag,
		  struct atomisp_gamma_table *config)
{
	if (flag == 0) {
		/* Get gamma table from current setup */
		if (atomisp_css_get_gamma_table(asd, config))
			return -EINVAL;
	} else {
		/* Set gamma table to isp parameters */
		memcpy(&asd->params.css_param.gamma_table, config,
		       sizeof(asd->params.css_param.gamma_table));
		atomisp_css_set_gamma_table(asd, &asd->params.css_param.gamma_table);
	}

	return 0;
}

/*
 * Function to update Ctc table for Chroma Enhancement
 */
int atomisp_ctc(struct atomisp_sub_device *asd, int flag,
		struct atomisp_ctc_table *config)
{
	if (flag == 0) {
		/* Get ctc table from current setup */
		if (atomisp_css_get_ctc_table(asd, config))
			return -EINVAL;
	} else {
		/* Set ctc table to isp parameters */
		memcpy(&asd->params.css_param.ctc_table, config,
			sizeof(asd->params.css_param.ctc_table));
		atomisp_css_set_ctc_table(asd, &asd->params.css_param.ctc_table);
	}

	return 0;
}

/*
 * Function to update gamma correction parameters
 */
int atomisp_gamma_correction(struct atomisp_sub_device *asd, int flag,
	struct atomisp_gc_config *config)
{
	if (flag == 0) {
		/* Get gamma correction params from current setup */
		if (atomisp_css_get_gc_config(asd, config))
			return -EINVAL;
	} else {
		/* Set gamma correction params to isp parameters */
		memcpy(&asd->params.css_param.gc_config, config,
			sizeof(asd->params.css_param.gc_config));
		atomisp_css_set_gc_config(asd, &asd->params.css_param.gc_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to update narrow gamma flag
 */
int atomisp_formats(struct atomisp_sub_device *asd, int flag,
		struct atomisp_formats_config *config)
{
	if (flag == 0) {
		/* Get narrow gamma flag from current setup */
		if (atomisp_css_get_formats_config(asd, config))
			return -EINVAL;
	} else {
		/* Set narrow gamma flag to isp parameters */
		memcpy(&asd->params.css_param.formats_config, config,
			sizeof(asd->params.css_param.formats_config));
		atomisp_css_set_formats_config(asd, &asd->params.css_param.formats_config);
	}

	return 0;
}

void atomisp_free_internal_buffers(struct atomisp_sub_device *asd)
{
	atomisp_free_css_parameters(&asd->params.css_param);

	if (asd->raw_output_frame) {
		atomisp_css_frame_free(asd->raw_output_frame);
		asd->raw_output_frame = NULL;
	}
}

static void atomisp_update_grid_info(struct atomisp_sub_device *asd,
				enum atomisp_css_pipe_id pipe_id, int source_pad)
{
	struct atomisp_device *isp = asd->isp;
	int err;
	uint16_t stream_id = atomisp_source_pad_to_stream_id(asd, source_pad);

	if (atomisp_css_get_grid_info(asd, pipe_id, source_pad))
		return;

	/* We must free all buffers because they no longer match
	   the grid size. */
	atomisp_css_free_stat_buffers(asd);

	err = atomisp_alloc_css_stat_bufs(asd, stream_id);
	if (err) {
		dev_err(isp->dev, "stat_buf allocate error\n");
		goto err;
	}

	if (atomisp_alloc_3a_output_buf(asd)) {
		/* Failure for 3A buffers does not influence DIS buffers */
		if (asd->params.s3a_output_bytes != 0) {
			/* For SOC sensor happens s3a_output_bytes == 0,
			 * using if condition to exclude false error log */
			dev_err(isp->dev, "Failed to allocate memory for 3A statistics\n");
		}
		goto err;
	}

	if (atomisp_alloc_dis_coef_buf(asd)) {
		dev_err(isp->dev,
			"Failed to allocate memory for DIS statistics\n");
		goto err;
	}

	if (atomisp_alloc_metadata_output_buf(asd)) {
		dev_err(isp->dev, "Failed to allocate memory for metadata\n");
		goto err;
	}

	return;

err:
	atomisp_css_free_stat_buffers(asd);
	return;
}

static void atomisp_curr_user_grid_info(struct atomisp_sub_device *asd,
				    struct atomisp_grid_info *info)
{
	memcpy(info, &asd->params.curr_grid_info.s3a_grid,
			sizeof(struct atomisp_css_3a_grid_info));
}

int atomisp_compare_grid(struct atomisp_sub_device *asd,
				struct atomisp_grid_info *atomgrid)
{
	struct atomisp_grid_info tmp = {0};

	atomisp_curr_user_grid_info(asd, &tmp);
	return memcmp(atomgrid, &tmp, sizeof(tmp));
}

/*
 * Function to update Gdc table for gdc
 */
int atomisp_gdc_cac_table(struct atomisp_sub_device *asd, int flag,
			  struct atomisp_morph_table *config)
{
	int ret;
	int i;
	struct atomisp_device *isp = asd->isp;

	if (flag == 0) {
		/* Get gdc table from current setup */
		struct atomisp_css_morph_table tab = {0};
		atomisp_css_get_morph_table(asd, &tab);

		config->width = tab.width;
		config->height = tab.height;

		for (i = 0; i < CSS_MORPH_TABLE_NUM_PLANES; i++) {
			ret = copy_to_user(config->coordinates_x[i],
				tab.coordinates_x[i], tab.height *
				tab.width * sizeof(*tab.coordinates_x[i]));
			if (ret) {
				dev_err(isp->dev,
					"Failed to copy to User for x\n");
				return -EFAULT;
			}
			ret = copy_to_user(config->coordinates_y[i],
				tab.coordinates_y[i], tab.height *
				tab.width * sizeof(*tab.coordinates_y[i]));
			if (ret) {
				dev_err(isp->dev,
					"Failed to copy to User for y\n");
				return -EFAULT;
			}
		}
	} else {
		struct atomisp_css_morph_table *tab =
			asd->params.css_param.morph_table;

		/* free first if we have one */
		if (tab) {
			atomisp_css_morph_table_free(tab);
			asd->params.css_param.morph_table = NULL;
		}

		/* allocate new one */
		tab = atomisp_css_morph_table_allocate(config->width,
						  config->height);

		if (!tab) {
			dev_err(isp->dev, "out of memory\n");
			return -EINVAL;
		}

		for (i = 0; i < CSS_MORPH_TABLE_NUM_PLANES; i++) {
			ret = copy_from_user(tab->coordinates_x[i],
				config->coordinates_x[i],
				config->height * config->width *
				sizeof(*config->coordinates_x[i]));
			if (ret) {
				dev_err(isp->dev,
				"Failed to copy from User for x, ret %d\n",
				ret);
				atomisp_css_morph_table_free(tab);
				return -EFAULT;
			}
			ret = copy_from_user(tab->coordinates_y[i],
				config->coordinates_y[i],
				config->height * config->width *
				sizeof(*config->coordinates_y[i]));
			if (ret) {
				dev_err(isp->dev,
				"Failed to copy from User for y, ret is %d\n",
				ret);
				atomisp_css_morph_table_free(tab);
				return -EFAULT;
			}
		}
		asd->params.css_param.morph_table = tab;
		if (asd->params.gdc_cac_en)
			atomisp_css_set_morph_table(asd, tab);
	}

	return 0;
}

int atomisp_macc_table(struct atomisp_sub_device *asd, int flag,
		       struct atomisp_macc_config *config)
{
	struct atomisp_css_macc_table *macc_table;

	switch (config->color_effect) {
	case V4L2_COLORFX_NONE:
		macc_table = &asd->params.css_param.macc_table;
		break;
	case V4L2_COLORFX_SKY_BLUE:
		macc_table = &blue_macc_table;
		break;
	case V4L2_COLORFX_GRASS_GREEN:
		macc_table = &green_macc_table;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_LOW:
		macc_table = &skin_low_macc_table;
		break;
	case V4L2_COLORFX_SKIN_WHITEN:
		macc_table = &skin_medium_macc_table;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_HIGH:
		macc_table = &skin_high_macc_table;
		break;
	default:
		return -EINVAL;
	}

	if (flag == 0) {
		/* Get macc table from current setup */
		memcpy(&config->table, macc_table,
		       sizeof(struct atomisp_css_macc_table));
	} else {
		memcpy(macc_table, &config->table,
		       sizeof(struct atomisp_css_macc_table));
		if (config->color_effect == asd->params.color_effect)
			atomisp_css_set_macc_table(asd, macc_table);
	}

	return 0;
}

int atomisp_set_dis_vector(struct atomisp_sub_device *asd,
			   struct atomisp_dis_vector *vector)
{
	atomisp_css_video_set_dis_vector(asd, vector);

	asd->params.dis_proj_data_valid = false;
	asd->params.css_update_params_needed = true;
	return 0;
}

/*
 * Function to set/get image stablization statistics
 */
int atomisp_get_dis_stat(struct atomisp_sub_device *asd,
			 struct atomisp_dis_statistics *stats)
{
	return atomisp_css_get_dis_stat(asd, stats);
}

/*
 * Function  set camrea_prefiles.xml current sensor pixel array size
 */
int atomisp_set_array_res(struct atomisp_sub_device *asd,
			 struct atomisp_resolution  *config)
{
	dev_dbg(asd->isp->dev, ">%s start\n", __func__);
	if (config == NULL || config->width < 0
		|| config->height < 0) {
		dev_err(asd->isp->dev, "Set sensor array size is not valid\n");
		return -EINVAL;
	}

	asd->sensor_array_res.width = config->width;
	asd->sensor_array_res.height = config->height;
	return 0;
}

/*
 * Function to get DVS2 BQ resolution settings
 */
int atomisp_get_dvs2_bq_resolutions(struct atomisp_sub_device *asd,
			 struct atomisp_dvs2_bq_resolutions *bq_res)
{
	struct ia_css_pipe_config *pipe_cfg =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipe_configs[CSS_PIPE_ID_VIDEO];
	struct ia_css_stream_config *stream_cfg =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.stream_config;
	struct ia_css_stream_input_config *input_config =
		&stream_cfg->input_config;

	if (!bq_res)
		return -EINVAL;

	/* the GDC output resolution */
	bq_res->output_bq.width_bq = pipe_cfg->output_info[0].res.width / 2;
	bq_res->output_bq.height_bq = pipe_cfg->output_info[0].res.height / 2;

	bq_res->envelope_bq.width_bq = 0;
	bq_res->envelope_bq.height_bq = 0;
	/* the GDC input resolution */
	if (!asd->continuous_mode->val) {
		bq_res->source_bq.width_bq = bq_res->output_bq.width_bq +
				pipe_cfg->dvs_envelope.width / 2;
		bq_res->source_bq.height_bq = bq_res->output_bq.height_bq +
				pipe_cfg->dvs_envelope.height / 2;
		/*
		 * Bad pixels caused by spatial filter processing
		 * ISP filter resolution should be given by CSS/FW, but for now
		 * there is not such API to query, and it is fixed value, so
		 * hardcoded here.
		 */
		bq_res->ispfilter_bq.width_bq = 12 / 2;
		bq_res->ispfilter_bq.height_bq = 12 / 2;
		/* spatial filter shift, always 4 pixels */
		bq_res->gdc_shift_bq.width_bq = 4 / 2;
		bq_res->gdc_shift_bq.height_bq = 4 / 2;

		if (asd->params.video_dis_en) {
			bq_res->envelope_bq.width_bq = pipe_cfg->dvs_envelope.width
					/ 2 - bq_res->ispfilter_bq.width_bq;
			bq_res->envelope_bq.height_bq = pipe_cfg->dvs_envelope.height
					/ 2 - bq_res->ispfilter_bq.height_bq;
		}
	} else {
		unsigned int w_padding;
		unsigned int gdc_effective_input = 0;

		/* For GDC:
		 * gdc_effective_input = effective_input + envelope
		 *
		 * From the comment and formula in BZ1786,
		 * we see the source_bq should be:
		 * effective_input / bayer_ds_ratio
		 */
		bq_res->source_bq.width_bq =
			(input_config->effective_res.width *
			 pipe_cfg->bayer_ds_out_res.width /
			 input_config->effective_res.width + 1) / 2;
		bq_res->source_bq.height_bq =
			(input_config->effective_res.height *
			 pipe_cfg->bayer_ds_out_res.height /
			 input_config->effective_res.height + 1) / 2;


		if (!asd->params.video_dis_en) {
			/*
			 * We adjust the ispfilter_bq to:
			 * ispfilter_bq = 128/BDS
			 * we still need firmware team to provide an offical
			 * formula for SDV.
			 */
			bq_res->ispfilter_bq.width_bq = 128 *
				pipe_cfg->bayer_ds_out_res.width /
				input_config->effective_res.width / 2;
			bq_res->ispfilter_bq.height_bq = 128 *
				pipe_cfg->bayer_ds_out_res.width /
				input_config->effective_res.width / 2;

			if (IS_HWREVISION(asd->isp, ATOMISP_HW_REVISION_ISP2401)) {
				/* No additional left padding for ISYS2401 */
				bq_res->gdc_shift_bq.width_bq = 4 / 2;
				bq_res->gdc_shift_bq.height_bq = 4 / 2;
			} else {
				/*
				 * For the w_padding and gdc_shift_bq cacluation
				 * Please see the BZ 1786 and 4358 for more info.
				 * Just test that this formula can work now,
				 * but we still have no offical formula.
				 *
				 * w_padding = ceiling(gdc_effective_input
				 *             /128, 1) * 128 - effective_width
				 * gdc_shift_bq = w_padding/BDS/2 + ispfilter_bq/2
				 */
				gdc_effective_input =
					input_config->effective_res.width +
					pipe_cfg->dvs_envelope.width;
				w_padding = roundup(gdc_effective_input, 128) -
					input_config->effective_res.width;
				w_padding = w_padding *
					pipe_cfg->bayer_ds_out_res.width /
					input_config->effective_res.width + 1;
				w_padding = roundup(w_padding/2, 1);

				bq_res->gdc_shift_bq.width_bq = bq_res->ispfilter_bq.width_bq / 2
					+ w_padding;
				bq_res->gdc_shift_bq.height_bq = 4 / 2;
			}
		} else {
			unsigned int dvs_w, dvs_h, dvs_w_max, dvs_h_max;

			bq_res->ispfilter_bq.width_bq = 8 / 2;
			bq_res->ispfilter_bq.height_bq = 8 / 2;

			if (IS_HWREVISION(asd->isp, ATOMISP_HW_REVISION_ISP2401)) {
				/* No additional left padding for ISYS2401 */
				bq_res->gdc_shift_bq.width_bq = 4 / 2;
				bq_res->gdc_shift_bq.height_bq = 4 / 2;
			} else {
				w_padding =
				    roundup(input_config->effective_res.width, 128) -
				    input_config->effective_res.width;
				if (w_padding < 12)
					w_padding = 12;
				bq_res->gdc_shift_bq.width_bq = 4 / 2 +
				    ((w_padding - 12) *
				     pipe_cfg->bayer_ds_out_res.width /
				input_config->effective_res.width + 1) / 2;
				bq_res->gdc_shift_bq.height_bq = 4 / 2;
			}

			dvs_w = pipe_cfg->bayer_ds_out_res.width -
				pipe_cfg->output_info[0].res.width;
			dvs_h = pipe_cfg->bayer_ds_out_res.height -
				pipe_cfg->output_info[0].res.height;
			dvs_w_max = rounddown(
					pipe_cfg->output_info[0].res.width / 5,
					ATOM_ISP_STEP_WIDTH);
			dvs_h_max = rounddown(
					pipe_cfg->output_info[0].res.height / 5,
					ATOM_ISP_STEP_HEIGHT);
			bq_res->envelope_bq.width_bq =
				min((dvs_w / 2), (dvs_w_max / 2)) -
				bq_res->ispfilter_bq.width_bq;
			bq_res->envelope_bq.height_bq =
				min((dvs_h / 2), (dvs_h_max / 2)) -
				bq_res->ispfilter_bq.height_bq;
		}
	}

	dev_dbg(asd->isp->dev, "source_bq.width_bq %d, source_bq.height_bq %d,\nispfilter_bq.width_bq %d, ispfilter_bq.height_bq %d,\ngdc_shift_bq.width_bq %d, gdc_shift_bq.height_bq %d,\nenvelope_bq.width_bq %d, envelope_bq.height_bq %d,\noutput_bq.width_bq %d, output_bq.height_bq %d\n",
	      bq_res->source_bq.width_bq, bq_res->source_bq.height_bq,
	      bq_res->ispfilter_bq.width_bq, bq_res->ispfilter_bq.height_bq,
	      bq_res->gdc_shift_bq.width_bq, bq_res->gdc_shift_bq.height_bq,
	      bq_res->envelope_bq.width_bq, bq_res->envelope_bq.height_bq,
	      bq_res->output_bq.width_bq, bq_res->output_bq.height_bq);

	return 0;
}

int atomisp_set_dis_coefs(struct atomisp_sub_device *asd,
			  struct atomisp_dis_coefficients *coefs)
{
	return atomisp_css_set_dis_coefs(asd, coefs);
}

/*
 * Function to set/get 3A stat from isp
 */
int atomisp_3a_stat(struct atomisp_sub_device *asd, int flag,
		    struct atomisp_3a_statistics *config)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_s3a_buf *s3a_buf;
	unsigned long ret;

	if (flag != 0)
		return -EINVAL;

	/* sanity check to avoid writing into unallocated memory. */
	if (asd->params.s3a_output_bytes == 0)
		return -EINVAL;

	if (atomisp_compare_grid(asd, &config->grid_info) != 0) {
		/* If the grid info in the argument differs from the current
		   grid info, we tell the caller to reset the grid size and
		   try again. */
		return -EAGAIN;
	}

	if (list_empty(&asd->s3a_stats_ready)) {
		dev_err(isp->dev, "3a statistics is not valid.\n");
		return -EAGAIN;
	}

	s3a_buf = list_entry(asd->s3a_stats_ready.next,
			struct atomisp_s3a_buf, list);
	if (s3a_buf->s3a_map)
		ia_css_translate_3a_statistics(
			asd->params.s3a_user_stat, s3a_buf->s3a_map);
	else
		ia_css_get_3a_statistics(asd->params.s3a_user_stat,
			s3a_buf->s3a_data);

	config->exp_id = s3a_buf->s3a_data->exp_id;
	config->isp_config_id = s3a_buf->s3a_data->isp_config_id;

	ret = copy_to_user(config->data, asd->params.s3a_user_stat->data,
			   asd->params.s3a_output_bytes);
	if (ret) {
		dev_err(isp->dev, "copy to user failed: copied %lu bytes\n",
				ret);
		return -EFAULT;
	}

	/* Move to free buffer list */
	list_del_init(&s3a_buf->list);
	list_add_tail(&s3a_buf->list, &asd->s3a_stats);
	dev_dbg(isp->dev, "%s: finish getting exp_id %d 3a stat, isp_config_id %d\n", __func__,
		config->exp_id, config->isp_config_id);
	return 0;
}

int atomisp_get_metadata(struct atomisp_sub_device *asd, int flag,
			 struct atomisp_metadata *md)
{
	struct atomisp_device *isp = asd->isp;
	struct ia_css_stream_config *stream_config;
	struct ia_css_stream_info *stream_info;
	struct camera_mipi_info *mipi_info;
	struct atomisp_metadata_buf *md_buf;
	enum atomisp_metadata_type md_type = ATOMISP_MAIN_METADATA;
	int ret, i;

	if (flag != 0)
		return -EINVAL;

	stream_config = &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].
		stream_config;
	stream_info = &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].
		stream_info;

	/* We always return the resolution and stride even if there is
	 * no valid metadata. This allows the caller to get the information
	 * needed to allocate user-space buffers. */
	md->width  = stream_info->metadata_info.resolution.width;
	md->height = stream_info->metadata_info.resolution.height;
	md->stride = stream_info->metadata_info.stride;

	/* sanity check to avoid writing into unallocated memory.
	 * This does not return an error because it is a valid way
	 * for applications to detect that metadata is not enabled. */
	if (md->width == 0 || md->height == 0 || !md->data)
		return 0;

	/* This is done in the atomisp_buf_done() */
	if (list_empty(&asd->metadata_ready[md_type])) {
		dev_warn(isp->dev, "Metadata queue is empty now!\n");
		return -EAGAIN;
	}

	mipi_info = atomisp_to_sensor_mipi_info(
		isp->inputs[asd->input_curr].camera);
	if (mipi_info == NULL)
		return -EINVAL;

	if (mipi_info->metadata_effective_width != NULL) {
		for (i = 0; i < md->height; i++)
			md->effective_width[i] =
				mipi_info->metadata_effective_width[i];
	}

	md_buf = list_entry(asd->metadata_ready[md_type].next,
			struct atomisp_metadata_buf, list);
	md->exp_id = md_buf->metadata->exp_id;
	if (md_buf->md_vptr) {
		ret = copy_to_user(md->data,
				md_buf->md_vptr,
				stream_info->metadata_info.size);
	} else {
		hrt_isp_css_mm_load(md_buf->metadata->address,
				asd->params.metadata_user[md_type],
				stream_info->metadata_info.size);

		ret = copy_to_user(md->data,
				asd->params.metadata_user[md_type],
				stream_info->metadata_info.size);
	}
	if (ret) {
		dev_err(isp->dev, "copy to user failed: copied %d bytes\n",
			ret);
		return -EFAULT;
	} else {
		list_del_init(&md_buf->list);
		list_add_tail(&md_buf->list, &asd->metadata[md_type]);
	}

	dev_dbg(isp->dev, "%s: HAL de-queued metadata type %d with exp_id %d\n",
		__func__, md_type, md->exp_id);
	return 0;
}

int atomisp_get_metadata_by_type(struct atomisp_sub_device *asd, int flag,
				 struct atomisp_metadata_with_type *md)
{
	struct atomisp_device *isp = asd->isp;
	struct ia_css_stream_config *stream_config;
	struct ia_css_stream_info *stream_info;
	struct camera_mipi_info *mipi_info;
	struct atomisp_metadata_buf *md_buf;
	enum atomisp_metadata_type md_type;
	int ret, i;

	if (flag != 0)
		return -EINVAL;

	stream_config = &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].
		stream_config;
	stream_info = &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].
		stream_info;

	/* We always return the resolution and stride even if there is
	 * no valid metadata. This allows the caller to get the information
	 * needed to allocate user-space buffers. */
	md->width  = stream_info->metadata_info.resolution.width;
	md->height = stream_info->metadata_info.resolution.height;
	md->stride = stream_info->metadata_info.stride;

	/* sanity check to avoid writing into unallocated memory.
	 * This does not return an error because it is a valid way
	 * for applications to detect that metadata is not enabled. */
	if (md->width == 0 || md->height == 0 || !md->data)
		return 0;

	md_type = md->type;
	if (md_type >= ATOMISP_METADATA_TYPE_NUM)
		return -EINVAL;

	/* This is done in the atomisp_buf_done() */
	if (list_empty(&asd->metadata_ready[md_type])) {
		dev_warn(isp->dev, "Metadata queue is empty now!\n");
		return -EAGAIN;
	}

	mipi_info = atomisp_to_sensor_mipi_info(
		isp->inputs[asd->input_curr].camera);
	if (mipi_info == NULL)
		return -EINVAL;

	if (mipi_info->metadata_effective_width != NULL) {
		for (i = 0; i < md->height; i++)
			md->effective_width[i] =
				mipi_info->metadata_effective_width[i];
	}

	md_buf = list_entry(asd->metadata_ready[md_type].next,
			struct atomisp_metadata_buf, list);
	md->exp_id = md_buf->metadata->exp_id;
	if (md_buf->md_vptr) {
		ret = copy_to_user(md->data,
				md_buf->md_vptr,
				stream_info->metadata_info.size);
	} else {
		hrt_isp_css_mm_load(md_buf->metadata->address,
				asd->params.metadata_user[md_type],
				stream_info->metadata_info.size);

		ret = copy_to_user(md->data,
				asd->params.metadata_user[md_type],
				stream_info->metadata_info.size);
	}
	if (ret) {
		dev_err(isp->dev, "copy to user failed: copied %d bytes\n",
			ret);
		return -EFAULT;
	} else {
		list_del_init(&md_buf->list);
		list_add_tail(&md_buf->list, &asd->metadata[md_type]);
	}
	dev_dbg(isp->dev, "%s: HAL de-queued metadata type %d with exp_id %d\n",
		__func__, md_type, md->exp_id);
	return 0;
}

/*
 * Function to calculate real zoom region for every pipe
 */
int atomisp_calculate_real_zoom_region(struct atomisp_sub_device *asd,
				struct ia_css_dz_config   *dz_config,
				enum atomisp_css_pipe_id css_pipe_id)

{
	struct atomisp_stream_env *stream_env =
			&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct atomisp_resolution  eff_res, out_res;

	memset(&eff_res, 0, sizeof(eff_res));
	memset(&out_res, 0, sizeof(out_res));

	if (dz_config->dx || dz_config->dy)
		return 0;

	if (css_pipe_id != IA_CSS_PIPE_ID_PREVIEW
		&& css_pipe_id != IA_CSS_PIPE_ID_CAPTURE) {
		dev_err(asd->isp->dev, "%s the set pipe no support crop region"
			, __func__);
		return -EINVAL;
	}

	eff_res.width =
		stream_env->stream_config.input_config.effective_res.width;
	eff_res.height =
		stream_env->stream_config.input_config.effective_res.height;
	if (eff_res.width == 0 || eff_res.height == 0) {
		dev_err(asd->isp->dev, "%s err effective resolution"
				, __func__);
		return -EINVAL;
	}

	if (dz_config->zoom_region.resolution.width
		== asd->sensor_array_res.width
		|| dz_config->zoom_region.resolution.height
		== asd->sensor_array_res.height) {
		/*no need crop region*/
		dz_config->zoom_region.origin.x = 0;
		dz_config->zoom_region.origin.y = 0;
		dz_config->zoom_region.resolution.width = eff_res.width;
		dz_config->zoom_region.resolution.height = eff_res.height;
		return 0;
	}

	/* FIXME:
	 * This is not the correct implementation with Google's definition, due
	 * to firmware limitation.
	 * map real crop region base on above calculating base max crop region.
	 */
	dz_config->zoom_region.origin.x =
			dz_config->zoom_region.origin.x
			* eff_res.width
			/ asd->sensor_array_res.width;
	dz_config->zoom_region.origin.y =
			dz_config->zoom_region.origin.y
			* eff_res.height
			/ asd->sensor_array_res.height;
	dz_config->zoom_region.resolution.width =
			dz_config->zoom_region.resolution.width
			* eff_res.width
			/ asd->sensor_array_res.width;
	dz_config->zoom_region.resolution.height =
			dz_config->zoom_region.resolution.height
			* eff_res.height
			/ asd->sensor_array_res.height;

	/*
	  * Set same ratio of crop region resolution and current pipe output
	  * resolution
	  */
	out_res.width =
		stream_env->pipe_configs[css_pipe_id].output_info[0].res.width;
	out_res.height =
		stream_env->pipe_configs[css_pipe_id].output_info[0].res.height;
	if (out_res.width == 0 || out_res.height == 0) {
		dev_err(asd->isp->dev, "%s err current pipe output resolution"
				, __func__);
		return -EINVAL;
	}

	if (out_res.width * dz_config->zoom_region.resolution.height
		> dz_config->zoom_region.resolution.width * out_res.height) {
		dz_config->zoom_region.resolution.height =
				dz_config->zoom_region.resolution.width
				* out_res.height / out_res.width;
	} else {
		dz_config->zoom_region.resolution.width =
				dz_config->zoom_region.resolution.height
				* out_res.width / out_res.height;
	}
	dev_dbg(asd->isp->dev, "%s crop region:(%d,%d),(%d,%d) eff_res(%d, %d) array_size(%d,%d) out_res(%d, %d)\n",
			__func__, dz_config->zoom_region.origin.x,
			dz_config->zoom_region.origin.y,
			dz_config->zoom_region.resolution.width,
			dz_config->zoom_region.resolution.height,
			eff_res.width, eff_res.height,
			asd->sensor_array_res.width,
			asd->sensor_array_res.height,
			out_res.width, out_res.height);


	if ((dz_config->zoom_region.origin.x +
		dz_config->zoom_region.resolution.width
		> eff_res.width) ||
		(dz_config->zoom_region.origin.y +
		dz_config->zoom_region.resolution.height
		> eff_res.height))
		return -EINVAL;

	return 0;
}


/*
 * Function to check the zoom region whether is effective
 */
static bool atomisp_check_zoom_region(
			struct atomisp_sub_device *asd,
			struct ia_css_dz_config *dz_config)
{
	struct atomisp_resolution  config;
	bool flag = false;
	unsigned int w , h;

	memset(&config, 0, sizeof(struct atomisp_resolution));

	if (dz_config->dx && dz_config->dy)
		return true;

	config.width = asd->sensor_array_res.width;
	config.height = asd->sensor_array_res.height;
	w = dz_config->zoom_region.origin.x +
		dz_config->zoom_region.resolution.width;
	h = dz_config->zoom_region.origin.y +
		dz_config->zoom_region.resolution.height;

	if ((w <= config.width) && (h <= config.height) && w > 0 && h > 0)
		flag = true;
	else
		/* setting error zoom region */
		dev_err(asd->isp->dev, "%s zoom region ERROR:dz_config:(%d,%d),(%d,%d)array_res(%d, %d)\n",
				__func__, dz_config->zoom_region.origin.x,
				dz_config->zoom_region.origin.y,
				dz_config->zoom_region.resolution.width,
				dz_config->zoom_region.resolution.height,
				config.width, config.height);

	return flag;
}

void atomisp_apply_css_parameters(
				struct atomisp_sub_device *asd,
				struct atomisp_parameters *arg,
				struct atomisp_css_params *css_param)
{
	if (arg->wb_config)
		atomisp_css_set_wb_config(asd, &css_param->wb_config);

	if (arg->ob_config)
		atomisp_css_set_ob_config(asd, &css_param->ob_config);

	if (arg->dp_config)
		atomisp_css_set_dp_config(asd, &css_param->dp_config);

	if (arg->nr_config)
		atomisp_css_set_nr_config(asd, &css_param->nr_config);

	if (arg->ee_config)
		atomisp_css_set_ee_config(asd, &css_param->ee_config);

	if (arg->tnr_config)
		atomisp_css_set_tnr_config(asd, &css_param->tnr_config);

	if (arg->a3a_config)
		atomisp_css_set_3a_config(asd, &css_param->s3a_config);

	if (arg->ctc_config)
		atomisp_css_set_ctc_config(asd, &css_param->ctc_config);

	if (arg->cnr_config)
		atomisp_css_set_cnr_config(asd, &css_param->cnr_config);

	if (arg->ecd_config)
		atomisp_css_set_ecd_config(asd, &css_param->ecd_config);

	if (arg->ynr_config)
		atomisp_css_set_ynr_config(asd, &css_param->ynr_config);

	if (arg->fc_config)
		atomisp_css_set_fc_config(asd, &css_param->fc_config);

	if (arg->macc_config)
		atomisp_css_set_macc_config(asd, &css_param->macc_config);

	if (arg->aa_config)
		atomisp_css_set_aa_config(asd, &css_param->aa_config);

	if (arg->anr_config)
		atomisp_css_set_anr_config(asd, &css_param->anr_config);

	if (arg->xnr_config)
		atomisp_css_set_xnr_config(asd, &css_param->xnr_config);

	if (arg->yuv2rgb_cc_config)
		atomisp_css_set_yuv2rgb_cc_config(asd,
					&css_param->yuv2rgb_cc_config);

	if (arg->rgb2yuv_cc_config)
		atomisp_css_set_rgb2yuv_cc_config(asd,
					&css_param->rgb2yuv_cc_config);

	if (arg->macc_table)
		atomisp_css_set_macc_table(asd, &css_param->macc_table);

	if (arg->xnr_table)
		atomisp_css_set_xnr_table(asd, &css_param->xnr_table);

	if (arg->r_gamma_table)
		atomisp_css_set_r_gamma_table(asd, &css_param->r_gamma_table);

	if (arg->g_gamma_table)
		atomisp_css_set_g_gamma_table(asd, &css_param->g_gamma_table);

	if (arg->b_gamma_table)
		atomisp_css_set_b_gamma_table(asd, &css_param->b_gamma_table);

	if (arg->anr_thres)
		atomisp_css_set_anr_thres(asd, &css_param->anr_thres);

	if (arg->shading_table)
		atomisp_css_set_shading_table(asd, css_param->shading_table);

	if (arg->morph_table && asd->params.gdc_cac_en)
		atomisp_css_set_morph_table(asd, css_param->morph_table);

	if (arg->dvs2_coefs) {
		struct atomisp_css_dvs_grid_info *dvs_grid_info =
			atomisp_css_get_dvs_grid_info(
				&asd->params.curr_grid_info);

		if (dvs_grid_info && dvs_grid_info->enable)
			atomisp_css_set_dvs2_coefs(asd, css_param->dvs2_coeff);
	}

	if (arg->dvs_6axis_config)
		atomisp_css_set_dvs_6axis(asd, css_param->dvs_6axis);

	atomisp_css_set_isp_config_id(asd, css_param->isp_config_id);
	/*
	 * These configurations are on used by ISP1.x, not for ISP2.x,
	 * so do not handle them. see comments of ia_css_isp_config.
	 * 1 cc_config
	 * 2 ce_config
	 * 3 de_config
	 * 4 gc_config
	 * 5 gamma_table
	 * 6 ctc_table
	 * 7 dvs_coefs
	 */
}

static int __atomisp_cp_general_isp_parameters(
					struct atomisp_sub_device *asd,
					struct atomisp_parameters *arg,
					struct atomisp_css_params *css_param)
{
	if (!arg || !asd || !css_param)
		return -EINVAL;

	if (arg->wb_config)
		if (copy_from_user(&css_param->wb_config, arg->wb_config,
				   sizeof(struct atomisp_css_wb_config)))
			return -EFAULT;

	if (arg->ob_config)
		if (copy_from_user(&css_param->ob_config, arg->ob_config,
				   sizeof(struct atomisp_css_ob_config)))
			return -EFAULT;

	if (arg->dp_config)
		if (copy_from_user(&css_param->dp_config, arg->dp_config,
				   sizeof(struct atomisp_css_dp_config)))
			return -EFAULT;
	if (asd->run_mode->val != ATOMISP_RUN_MODE_VIDEO) {
		if (arg->dz_config) {
			if (copy_from_user(&css_param->dz_config,
				arg->dz_config,
				sizeof(struct atomisp_css_dz_config)))
				return -EFAULT;
			if (!atomisp_check_zoom_region(asd,
						&css_param->dz_config)) {
				dev_err(asd->isp->dev, "crop region error!");
				return -EINVAL;
			}
		}
	}

	if (arg->nr_config)
		if (copy_from_user(&css_param->nr_config, arg->nr_config,
				   sizeof(struct atomisp_css_nr_config)))
			return -EFAULT;

	if (arg->ee_config)
		if (copy_from_user(&css_param->ee_config, arg->ee_config,
				   sizeof(struct atomisp_css_ee_config)))
			return -EFAULT;

	if (arg->tnr_config)
		if (copy_from_user(&css_param->tnr_config, arg->tnr_config,
				   sizeof(struct atomisp_css_tnr_config)))
			return -EFAULT;

	if (arg->a3a_config)
		if (copy_from_user(&css_param->s3a_config, arg->a3a_config,
				   sizeof(css_param->s3a_config)))
			return -EFAULT;

	if (arg->ctc_config)
		if (copy_from_user(&css_param->ctc_config, arg->ctc_config,
					sizeof(struct atomisp_css_ctc_config)))
			return -EFAULT;

	if (arg->cnr_config)
		if (copy_from_user(&css_param->cnr_config, arg->cnr_config,
				   sizeof(struct atomisp_css_cnr_config)))
			return -EFAULT;

	if (arg->ecd_config)
		if (copy_from_user(&css_param->ecd_config, arg->ecd_config,
				   sizeof(struct atomisp_css_ecd_config)))
			return -EFAULT;

	if (arg->ynr_config)
		if (copy_from_user(&css_param->ynr_config, arg->ynr_config,
				   sizeof(struct atomisp_css_ynr_config)))
			return -EFAULT;

	if (arg->fc_config)
		if (copy_from_user(&css_param->fc_config, arg->fc_config,
				   sizeof(struct atomisp_css_fc_config)))
			return -EFAULT;

	if (arg->macc_config)
		if (copy_from_user(&css_param->macc_config, arg->macc_config,
				   sizeof(struct atomisp_css_macc_config)))
			return -EFAULT;

	if (arg->aa_config)
		if (copy_from_user(&css_param->aa_config, arg->aa_config,
				   sizeof(struct atomisp_css_aa_config)))
			return -EFAULT;

	if (arg->anr_config)
		if (copy_from_user(&css_param->anr_config, arg->anr_config,
				   sizeof(struct atomisp_css_anr_config)))
			return -EFAULT;

	if (arg->xnr_config)
		if (copy_from_user(&css_param->xnr_config, arg->xnr_config,
				   sizeof(struct atomisp_css_xnr_config)))
			return -EFAULT;

	if (arg->yuv2rgb_cc_config)
		if (copy_from_user(&css_param->yuv2rgb_cc_config,
				   arg->yuv2rgb_cc_config,
				   sizeof(struct atomisp_css_cc_config)))
			return -EFAULT;

	if (arg->rgb2yuv_cc_config)
		if (copy_from_user(&css_param->rgb2yuv_cc_config,
				   arg->rgb2yuv_cc_config,
				   sizeof(struct atomisp_css_cc_config)))
			return -EFAULT;

	if (arg->macc_table)
		if (copy_from_user(&css_param->macc_table, arg->macc_table,
				   sizeof(struct atomisp_css_macc_table)))
			return -EFAULT;

	if (arg->xnr_table)
		if (copy_from_user(&css_param->xnr_table, arg->xnr_table,
				   sizeof(struct atomisp_css_xnr_table)))
			return -EFAULT;

	if (arg->r_gamma_table)
		if (copy_from_user(&css_param->r_gamma_table,
				   arg->r_gamma_table,
				   sizeof(struct atomisp_css_rgb_gamma_table)))
			return -EFAULT;

	if (arg->g_gamma_table)
		if (copy_from_user(&css_param->g_gamma_table,
				   arg->g_gamma_table,
				   sizeof(struct atomisp_css_rgb_gamma_table)))
			return -EFAULT;

	if (arg->b_gamma_table)
		if (copy_from_user(&css_param->b_gamma_table,
				   arg->b_gamma_table,
				   sizeof(struct atomisp_css_rgb_gamma_table)))
			return -EFAULT;

	if (arg->anr_thres)
		if (copy_from_user(&css_param->anr_thres, arg->anr_thres,
				   sizeof(struct atomisp_css_anr_thres)))
			return -EFAULT;

	css_param->isp_config_id = arg->isp_config_id;
	/*
	 * These configurations are on used by ISP1.x, not for ISP2.x,
	 * so do not handle them. see comments of ia_css_isp_config.
	 * 1 cc_config
	 * 2 ce_config
	 * 3 de_config
	 * 4 gc_config
	 * 5 gamma_table
	 * 6 ctc_table
	 * 7 dvs_coefs
	 */
	return 0;
}

static int __atomisp_cp_lsc_table(struct atomisp_sub_device *asd,
			struct atomisp_shading_table *user_st,
			struct atomisp_css_params *css_param)
{
	unsigned int i;
	unsigned int len_table;
	struct atomisp_css_shading_table *shading_table;
	struct atomisp_css_shading_table *old_table;

	if (!user_st)
		return 0;

	if (!css_param)
		return -EINVAL;

	old_table = css_param->shading_table;

	/* user config is to disable the shading table. */
	if (!user_st->enable) {
		/* Generate a minimum table with enable = 0. */
		shading_table = atomisp_css_shading_table_alloc(1, 1);
		if (!shading_table)
			return -ENOMEM;
		shading_table->enable = 0;
		goto set_lsc;
	}

	/* Setting a new table. Validate first - all tables must be set */
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (!user_st->data[i])
			return -EINVAL;
	}

	/* Shading table size per color */
	if (user_st->width > SH_CSS_MAX_SCTBL_WIDTH_PER_COLOR ||
		user_st->height > SH_CSS_MAX_SCTBL_HEIGHT_PER_COLOR)
		return -EINVAL;

	shading_table = atomisp_css_shading_table_alloc(user_st->width,
							user_st->height);
	if (!shading_table)
			return -ENOMEM;

	len_table = user_st->width * user_st->height * ATOMISP_SC_TYPE_SIZE;
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (copy_from_user(shading_table->data[i],
			user_st->data[i], len_table)) {
			atomisp_css_shading_table_free(shading_table);
			return -EFAULT;
		}

	}
	shading_table->sensor_width = user_st->sensor_width;
	shading_table->sensor_height = user_st->sensor_height;
	shading_table->fraction_bits = user_st->fraction_bits;
	shading_table->enable = user_st->enable;

	/* No need to update shading table if it is the same */
	if (old_table != NULL &&
		old_table->sensor_width == shading_table->sensor_width &&
		old_table->sensor_height == shading_table->sensor_height &&
		old_table->width == shading_table->width &&
		old_table->height == shading_table->height &&
		old_table->fraction_bits == shading_table->fraction_bits &&
		old_table->enable == shading_table->enable) {
		bool data_is_same = true;

		for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
			if (memcmp(shading_table->data[i], old_table->data[i],
				len_table) != 0) {
				data_is_same = false;
				break;
			}
		}

		if (data_is_same) {
			atomisp_css_shading_table_free(shading_table);
			return 0;
		}
	}

set_lsc:
	/* set LSC to CSS */
	css_param->shading_table = shading_table;
	asd->params.sc_en = shading_table != NULL;

	if (old_table)
		atomisp_css_shading_table_free(old_table);

	return 0;
}

static int __atomisp_css_cp_dvs2_coefs(struct atomisp_sub_device *asd,
				struct ia_css_dvs2_coefficients *coefs,
				struct atomisp_css_params *css_param)
{
	struct atomisp_css_dvs_grid_info *cur =
		atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);

	if (!coefs || !cur)
		return 0;

	if (sizeof(*cur) != sizeof(coefs->grid) ||
	    memcmp(&coefs->grid, cur, sizeof(*cur))) {
		dev_err(asd->isp->dev, "dvs grid mis-match!\n");
		/* If the grid info in the argument differs from the current
		   grid info, we tell the caller to reset the grid size and
		   try again. */
		return -EAGAIN;
	}

	if (coefs->hor_coefs.odd_real == NULL ||
	    coefs->hor_coefs.odd_imag == NULL ||
	    coefs->hor_coefs.even_real == NULL ||
	    coefs->hor_coefs.even_imag == NULL ||
	    coefs->ver_coefs.odd_real == NULL ||
	    coefs->ver_coefs.odd_imag == NULL ||
	    coefs->ver_coefs.even_real == NULL ||
	    coefs->ver_coefs.even_imag == NULL)
		return -EINVAL;

	if (!css_param->dvs2_coeff) {
		/* DIS coefficients. */
		css_param->dvs2_coeff = ia_css_dvs2_coefficients_allocate(cur);
		if (!css_param->dvs2_coeff)
			return -ENOMEM;
	}

	if (copy_from_user(css_param->dvs2_coeff->hor_coefs.odd_real,
	    coefs->hor_coefs.odd_real, asd->params.dvs_hor_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->hor_coefs.odd_imag,
	    coefs->hor_coefs.odd_imag, asd->params.dvs_hor_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->hor_coefs.even_real,
	    coefs->hor_coefs.even_real, asd->params.dvs_hor_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->hor_coefs.even_imag,
	    coefs->hor_coefs.even_imag, asd->params.dvs_hor_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->ver_coefs.odd_real,
	    coefs->ver_coefs.odd_real, asd->params.dvs_ver_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->ver_coefs.odd_imag,
	    coefs->ver_coefs.odd_imag, asd->params.dvs_ver_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->ver_coefs.even_real,
	    coefs->ver_coefs.even_real, asd->params.dvs_ver_coef_bytes) ||
	    copy_from_user(css_param->dvs2_coeff->ver_coefs.even_imag,
	    coefs->ver_coefs.even_imag, asd->params.dvs_ver_coef_bytes)) {
		ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
		css_param->dvs2_coeff = NULL;
		return -EFAULT;
	}

	return 0;
}

int atomisp_cp_dvs_6axis_config(struct atomisp_sub_device *asd,
			struct atomisp_dvs_6axis_config *user_6axis_config,
			struct atomisp_css_params *css_param)
{
	struct atomisp_css_dvs_6axis_config *dvs_6axis_config;
	struct atomisp_css_dvs_6axis_config *old_6axis_config;
	struct ia_css_stream *stream =
			asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream;
	struct atomisp_css_dvs_grid_info *dvs_grid_info =
		atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);
	int ret = -EFAULT;

	if (stream == NULL) {
		dev_err(asd->isp->dev, "%s: internal error!", __func__);
		return -EINVAL;
	}

	if (!user_6axis_config || !dvs_grid_info)
		return 0;

	if (!dvs_grid_info->enable)
		return 0;

	/* check whether need to reallocate for 6 axis config */
	old_6axis_config = css_param->dvs_6axis;
	dvs_6axis_config = old_6axis_config;
	if (old_6axis_config &&
	    (old_6axis_config->width_y != user_6axis_config->width_y ||
	     old_6axis_config->height_y != user_6axis_config->height_y ||
	     old_6axis_config->width_uv != user_6axis_config->width_uv ||
	     old_6axis_config->height_uv != user_6axis_config->height_uv)) {
		ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
		css_param->dvs_6axis = NULL;

		dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
		if (!dvs_6axis_config)
			return -ENOMEM;
	} else if (!dvs_6axis_config) {
		dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
		if (!dvs_6axis_config)
			return -ENOMEM;
	}

	dvs_6axis_config->exp_id = user_6axis_config->exp_id;

	if (copy_from_user(dvs_6axis_config->xcoords_y,
			   user_6axis_config->xcoords_y,
			   user_6axis_config->width_y *
			   user_6axis_config->height_y *
			   sizeof(*user_6axis_config->xcoords_y)))
		goto error;
	if (copy_from_user(dvs_6axis_config->ycoords_y,
			   user_6axis_config->ycoords_y,
			   user_6axis_config->width_y *
			   user_6axis_config->height_y *
			   sizeof(*user_6axis_config->ycoords_y)))
		goto error;
	if (copy_from_user(dvs_6axis_config->xcoords_uv,
			   user_6axis_config->xcoords_uv,
			   user_6axis_config->width_uv *
			   user_6axis_config->height_uv *
			   sizeof(*user_6axis_config->xcoords_uv)))
		goto error;
	if (copy_from_user(dvs_6axis_config->ycoords_uv,
			   user_6axis_config->ycoords_uv,
			   user_6axis_config->width_uv *
			   user_6axis_config->height_uv *
			   sizeof(*user_6axis_config->ycoords_uv)))
		goto error;

	css_param->dvs_6axis = dvs_6axis_config;
	return 0;

error:
	if (dvs_6axis_config)
		ia_css_dvs2_6axis_config_free(dvs_6axis_config);
	return ret;
}

static int __atomisp_cp_morph_table(struct atomisp_sub_device *asd,
				struct atomisp_morph_table *user_morph_table,
				struct atomisp_css_params *css_param)
{
	int ret = -EFAULT;
	unsigned int i;
	struct atomisp_css_morph_table *morph_table;
	struct atomisp_css_morph_table *old_morph_table;

	if (!user_morph_table)
		return 0;

	old_morph_table = css_param->morph_table;

	morph_table = atomisp_css_morph_table_allocate(user_morph_table->width,
				user_morph_table->height);
	if (!morph_table)
		return -ENOMEM;

	for (i = 0; i < CSS_MORPH_TABLE_NUM_PLANES; i++) {
		if (copy_from_user(morph_table->coordinates_x[i],
			user_morph_table->coordinates_x[i],
			user_morph_table->height * user_morph_table->width *
			sizeof(*user_morph_table->coordinates_x[i])))
			goto error;

		if (copy_from_user(morph_table->coordinates_y[i],
			user_morph_table->coordinates_y[i],
			user_morph_table->height * user_morph_table->width *
			sizeof(*user_morph_table->coordinates_y[i])))
			goto error;
	}

	css_param->morph_table = morph_table;
	if (old_morph_table)
		atomisp_css_morph_table_free(old_morph_table);

	return 0;

error:
	if (morph_table)
		atomisp_css_morph_table_free(morph_table);
	return ret;
}

void atomisp_free_css_parameters(struct atomisp_css_params *css_param)
{
	if (css_param->dvs_6axis) {
		ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
		css_param->dvs_6axis = NULL;
	}
	if (css_param->dvs2_coeff) {
		ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
		css_param->dvs2_coeff = NULL;
	}
	if (css_param->shading_table) {
		ia_css_shading_table_free(css_param->shading_table);
		css_param->shading_table = NULL;
	}
	if (css_param->morph_table) {
		ia_css_morph_table_free(css_param->morph_table);
		css_param->morph_table = NULL;
	}
}

/*
 * Check parameter queue list and buffer queue list to find out if matched items
 * and then set parameter to CSS and enqueue buffer to CSS.
 * Of course, if the buffer in buffer waiting list is not bound to a per-frame
 * parameter, it will be enqueued into CSS as long as the per-frame setting
 * buffers before it get enqueued.
 */
void atomisp_handle_parameter_and_buffer(struct atomisp_video_pipe *pipe)
{
	struct atomisp_sub_device *asd = pipe->asd;
	struct videobuf_buffer *vb = NULL, *vb_tmp;
	struct atomisp_css_params_with_list *param = NULL, *param_tmp;
	struct videobuf_vmalloc_memory *vm_mem = NULL;
	unsigned long irqflags;
	bool need_to_enqueue_buffer = false;

	if (atomisp_is_vf_pipe(pipe))
		return;

	/*
	 * CSS/FW requires set parameter and enqueue buffer happen after ISP
	 * is streamon.
	 */
	if (!asd->streaming == ATOMISP_DEVICE_STREAMING_ENABLED)
		return;

	if (list_empty(&pipe->per_frame_params) ||
	    list_empty(&pipe->buffers_waiting_for_param))
		return;

	list_for_each_entry_safe(vb, vb_tmp,
			&pipe->buffers_waiting_for_param, queue) {
		if (pipe->frame_request_config_id[vb->i]) {
			list_for_each_entry_safe(param, param_tmp,
				&pipe->per_frame_params, list) {
				if (pipe->frame_request_config_id[vb->i] !=
				    param->params.isp_config_id)
					continue;

				list_del(&param->list);
				list_del(&vb->queue);
				/*
				 * clear the request config id as the buffer
				 * will be handled and enqueued into CSS soon
				 */
				pipe->frame_request_config_id[vb->i] = 0;
				pipe->frame_params[vb->i] = param;
				vm_mem = vb->priv;
				BUG_ON(!vm_mem);
				break;
			}

			if (vm_mem) {
				spin_lock_irqsave(&pipe->irq_lock, irqflags);
				list_add_tail(&vb->queue, &pipe->activeq);
				spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
				vm_mem = NULL;
				need_to_enqueue_buffer = true;
			} else {
				/* The is the end, stop further loop */
				break;
			}
		} else {
			list_del(&vb->queue);
			pipe->frame_params[vb->i] = NULL;
			spin_lock_irqsave(&pipe->irq_lock, irqflags);
			list_add_tail(&vb->queue, &pipe->activeq);
			spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
			need_to_enqueue_buffer = true;
		}
	}

	if (need_to_enqueue_buffer) {
		atomisp_qbuffers_to_css(asd);
		if (!atomisp_is_wdt_running(asd) && atomisp_buffers_queued(asd))
			atomisp_wdt_start(asd);
	}
}

/*
* Function to configure ISP parameters
*/
int atomisp_set_parameters(struct video_device *vdev,
			struct atomisp_parameters *arg)
{
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_css_params_with_list *param = NULL;
	struct atomisp_css_params *css_param = &asd->params.css_param;
	int ret;

	if (asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream == NULL) {
		dev_err(asd->isp->dev, "%s: internal error!\n", __func__);
		return -EINVAL;
	}

	dev_dbg(asd->isp->dev, "%s: set parameter(per_frame_setting %d) for asd%d with isp_config_id %d of %s\n",
		__func__, arg->per_frame_setting, asd->index,
		arg->isp_config_id, vdev->name);
	if (arg->per_frame_setting && !atomisp_is_vf_pipe(pipe)) {
		/*
		 * Per-frame setting enabled, we allocate a new paramter
		 * buffer to cache the parameters and only when frame buffers
		 * are ready, the parameters will be set to CSS.
		 * per-frame setting only works for the main output frame.
		 */
		param = atomisp_kernel_zalloc(sizeof(*param), true);
		if (!param) {
			dev_err(asd->isp->dev, "%s: failed to alloc params buffer\n",
				__func__);
			return -ENOMEM;
		}
		memcpy(&param->us_params, arg, sizeof(*arg));
		css_param = &param->params;
	}

	ret = __atomisp_cp_general_isp_parameters(asd, arg, css_param);
	if (ret)
		goto apply_parameter_failed;

	ret = __atomisp_cp_lsc_table(asd, arg->shading_table, css_param);
	if (ret)
		goto apply_parameter_failed;

	ret = __atomisp_cp_morph_table(asd, arg->morph_table, css_param);
	if (ret)
		goto apply_parameter_failed;

	ret = __atomisp_css_cp_dvs2_coefs(asd,
			(struct ia_css_dvs2_coefficients *)arg->dvs2_coefs,
			css_param);
	if (ret)
		goto apply_parameter_failed;

	ret = atomisp_cp_dvs_6axis_config(asd, arg->dvs_6axis_config,
					  css_param);
	if (ret)
		goto apply_parameter_failed;

	if (!(arg->per_frame_setting && !atomisp_is_vf_pipe(pipe))) {
		atomisp_apply_css_parameters(asd, arg, css_param);
		/* indicate to CSS that we have parameters to be updated */
		asd->params.css_update_params_needed = true;

		if (asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream &&
		    asd->streaming == ATOMISP_DEVICE_STREAMING_ENABLED) {
			atomisp_css_update_isp_params(asd);
			asd->params.css_update_params_needed = false;
		}
	} else {
		list_add_tail(&param->list, &pipe->per_frame_params);
		atomisp_handle_parameter_and_buffer(pipe);
	}

	return 0;

apply_parameter_failed:
	if (css_param)
		atomisp_free_css_parameters(css_param);
	if (param)
		atomisp_kernel_free(param);

	return ret;
}

/*
 * Function to set/get isp parameters to isp
 */
int atomisp_param(struct atomisp_sub_device *asd, int flag,
		  struct atomisp_parm *config)
{
	struct atomisp_device *isp = asd->isp;
	struct ia_css_pipe_config *vp_cfg =
		&asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].
		pipe_configs[IA_CSS_PIPE_ID_VIDEO];

	/* Read parameter for 3A binary info */
	if (flag == 0) {
		struct atomisp_css_dvs_grid_info *dvs_grid_info =
			atomisp_css_get_dvs_grid_info(
				&asd->params.curr_grid_info);

		if (&config->info == NULL) {
			dev_err(isp->dev, "ERROR: NULL pointer in grid_info\n");
			return -EINVAL;
		}
		atomisp_curr_user_grid_info(asd, &config->info);

		/* We always return the resolution and stride even if there is
		 * no valid metadata. This allows the caller to get the
		 * information needed to allocate user-space buffers. */
		config->metadata_config.metadata_height = asd->
			stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream_info.
			metadata_info.resolution.height;
		config->metadata_config.metadata_stride = asd->
			stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream_info.
			metadata_info.stride;

		/* update dvs grid info */
		if (dvs_grid_info)
			memcpy(&config->dvs_grid,
				dvs_grid_info,
				sizeof(struct atomisp_css_dvs_grid_info));

		if (asd->run_mode->val != ATOMISP_RUN_MODE_VIDEO) {
			config->dvs_envelop.width = 0;
			config->dvs_envelop.height = 0;
			return 0;
		}

		/* update dvs envelop info */
		if (!asd->continuous_mode->val) {
			config->dvs_envelop.width = vp_cfg->dvs_envelope.width;
			config->dvs_envelop.height =
					vp_cfg->dvs_envelope.height;
		} else {
			unsigned int dvs_w, dvs_h, dvs_w_max, dvs_h_max;

			dvs_w = vp_cfg->bayer_ds_out_res.width -
				vp_cfg->output_info[0].res.width;
			dvs_h = vp_cfg->bayer_ds_out_res.height -
				vp_cfg->output_info[0].res.height;
			dvs_w_max = rounddown(
					vp_cfg->output_info[0].res.width / 5,
					ATOM_ISP_STEP_WIDTH);
			dvs_h_max = rounddown(
					vp_cfg->output_info[0].res.height / 5,
					ATOM_ISP_STEP_HEIGHT);

			config->dvs_envelop.width = min(dvs_w, dvs_w_max);
			config->dvs_envelop.height = min(dvs_h, dvs_h_max);
		}

		return 0;
	}

	memcpy(&asd->params.css_param.wb_config, &config->wb_config,
	       sizeof(struct atomisp_css_wb_config));
	memcpy(&asd->params.css_param.ob_config, &config->ob_config,
	       sizeof(struct atomisp_css_ob_config));
	memcpy(&asd->params.css_param.dp_config, &config->dp_config,
	       sizeof(struct atomisp_css_dp_config));
	memcpy(&asd->params.css_param.de_config, &config->de_config,
	       sizeof(struct atomisp_css_de_config));
	memcpy(&asd->params.css_param.dz_config, &config->dz_config,
	       sizeof(struct atomisp_css_dz_config));
	memcpy(&asd->params.css_param.ce_config, &config->ce_config,
	       sizeof(struct atomisp_css_ce_config));
	memcpy(&asd->params.css_param.nr_config, &config->nr_config,
	       sizeof(struct atomisp_css_nr_config));
	memcpy(&asd->params.css_param.ee_config, &config->ee_config,
	       sizeof(struct atomisp_css_ee_config));
	memcpy(&asd->params.css_param.tnr_config, &config->tnr_config,
	       sizeof(struct atomisp_css_tnr_config));

	if (asd->params.color_effect == V4L2_COLORFX_NEGATIVE) {
		asd->params.css_param.cc_config.matrix[3] = -config->cc_config.matrix[3];
		asd->params.css_param.cc_config.matrix[4] = -config->cc_config.matrix[4];
		asd->params.css_param.cc_config.matrix[5] = -config->cc_config.matrix[5];
		asd->params.css_param.cc_config.matrix[6] = -config->cc_config.matrix[6];
		asd->params.css_param.cc_config.matrix[7] = -config->cc_config.matrix[7];
		asd->params.css_param.cc_config.matrix[8] = -config->cc_config.matrix[8];
	}

	if (asd->params.color_effect != V4L2_COLORFX_SEPIA &&
	    asd->params.color_effect != V4L2_COLORFX_BW) {
		memcpy(&asd->params.css_param.cc_config, &config->cc_config,
		       sizeof(struct atomisp_css_cc_config));
		atomisp_css_set_cc_config(asd, &asd->params.css_param.cc_config);
	}

	atomisp_css_set_wb_config(asd, &asd->params.css_param.wb_config);
	atomisp_css_set_ob_config(asd, &asd->params.css_param.ob_config);
	atomisp_css_set_de_config(asd, &asd->params.css_param.de_config);
	atomisp_css_set_dz_config(asd, &asd->params.css_param.dz_config);
	atomisp_css_set_ce_config(asd, &asd->params.css_param.ce_config);
	atomisp_css_set_dp_config(asd, &asd->params.css_param.dp_config);
	atomisp_css_set_nr_config(asd, &asd->params.css_param.nr_config);
	atomisp_css_set_ee_config(asd, &asd->params.css_param.ee_config);
	atomisp_css_set_tnr_config(asd, &asd->params.css_param.tnr_config);
	asd->params.css_update_params_needed = true;

	return 0;
}

/*
 * Function to configure color effect of the image
 */
int atomisp_color_effect(struct atomisp_sub_device *asd, int flag,
			 __s32 *effect)
{
	struct atomisp_css_cc_config *cc_config = NULL;
	struct atomisp_css_macc_table *macc_table = NULL;
	struct atomisp_css_ctc_table *ctc_table = NULL;
	int ret = 0;
	struct v4l2_control control;
	struct atomisp_device *isp = asd->isp;

	if (flag == 0) {
		*effect = asd->params.color_effect;
		return 0;
	}


	control.id = V4L2_CID_COLORFX;
	control.value = *effect;
	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				core, s_ctrl, &control);
	/*
	 * if set color effect to sensor successfully, return
	 * 0 directly.
	 */
	if (!ret) {
		asd->params.color_effect = (u32)*effect;
		return 0;
	}

	if (*effect == asd->params.color_effect)
		return 0;

	/*
	 * isp_subdev->params.macc_en should be set to false.
	 */
	asd->params.macc_en = false;

	switch (*effect) {
	case V4L2_COLORFX_NONE:
		macc_table = &asd->params.css_param.macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SEPIA:
		cc_config = &sepia_cc_config;
		break;
	case V4L2_COLORFX_NEGATIVE:
		cc_config = &nega_cc_config;
		break;
	case V4L2_COLORFX_BW:
		cc_config = &mono_cc_config;
		break;
	case V4L2_COLORFX_SKY_BLUE:
		macc_table = &blue_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_GRASS_GREEN:
		macc_table = &green_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_LOW:
		macc_table = &skin_low_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SKIN_WHITEN:
		macc_table = &skin_medium_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_HIGH:
		macc_table = &skin_high_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_VIVID:
		ctc_table = &vivid_ctc_table;
		break;
	default:
		return -EINVAL;
	}
	atomisp_update_capture_mode(asd);

	if (cc_config)
		atomisp_css_set_cc_config(asd, cc_config);
	if (macc_table)
		atomisp_css_set_macc_table(asd, macc_table);
	if (ctc_table)
		atomisp_css_set_ctc_table(asd, ctc_table);
	asd->params.color_effect = (u32)*effect;
	asd->params.css_update_params_needed = true;
	return 0;
}

/*
 * Function to configure bad pixel correction
 */
int atomisp_bad_pixel(struct atomisp_sub_device *asd, int flag,
		      __s32 *value)
{

	if (flag == 0) {
		*value = asd->params.bad_pixel_en;
		return 0;
	}
	asd->params.bad_pixel_en = !!*value;

	return 0;
}

/*
 * Function to configure bad pixel correction params
 */
int atomisp_bad_pixel_param(struct atomisp_sub_device *asd, int flag,
			    struct atomisp_dp_config *config)
{
	if (flag == 0) {
		/* Get bad pixel from current setup */
		if (atomisp_css_get_dp_config(asd, config))
			return -EINVAL;
	} else {
		/* Set bad pixel to isp parameters */
		memcpy(&asd->params.css_param.dp_config, config,
			sizeof(asd->params.css_param.dp_config));
		atomisp_css_set_dp_config(asd, &asd->params.css_param.dp_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to enable/disable video image stablization
 */
int atomisp_video_stable(struct atomisp_sub_device *asd, int flag,
			 __s32 *value)
{
	if (flag == 0)
		*value = asd->params.video_dis_en;
	else
		asd->params.video_dis_en = !!*value;

	return 0;
}

/*
 * Function to configure fixed pattern noise
 */
int atomisp_fixed_pattern(struct atomisp_sub_device *asd, int flag,
			  __s32 *value)
{

	if (flag == 0) {
		*value = asd->params.fpn_en;
		return 0;
	}

	if (*value == 0) {
		asd->params.fpn_en = 0;
		return 0;
	}

	/* Add function to get black from from sensor with shutter off */
	return 0;
}

static unsigned int
atomisp_bytesperline_to_padded_width(unsigned int bytesperline,
				     enum atomisp_css_frame_format format)
{
	switch (format) {
	case CSS_FRAME_FORMAT_UYVY:
	case CSS_FRAME_FORMAT_YUYV:
	case CSS_FRAME_FORMAT_RAW:
	case CSS_FRAME_FORMAT_RGB565:
		return bytesperline/2;
	case CSS_FRAME_FORMAT_RGBA888:
		return bytesperline/4;
	/* The following cases could be removed, but we leave them
	   in to document the formats that are included. */
	case CSS_FRAME_FORMAT_NV11:
	case CSS_FRAME_FORMAT_NV12:
	case CSS_FRAME_FORMAT_NV16:
	case CSS_FRAME_FORMAT_NV21:
	case CSS_FRAME_FORMAT_NV61:
	case CSS_FRAME_FORMAT_YV12:
	case CSS_FRAME_FORMAT_YV16:
	case CSS_FRAME_FORMAT_YUV420:
	case CSS_FRAME_FORMAT_YUV420_16:
	case CSS_FRAME_FORMAT_YUV422:
	case CSS_FRAME_FORMAT_YUV422_16:
	case CSS_FRAME_FORMAT_YUV444:
	case CSS_FRAME_FORMAT_YUV_LINE:
	case CSS_FRAME_FORMAT_PLANAR_RGB888:
	case CSS_FRAME_FORMAT_QPLANE6:
	case CSS_FRAME_FORMAT_BINARY_8:
	default:
		return bytesperline;
	}
}

static int
atomisp_v4l2_framebuffer_to_css_frame(const struct v4l2_framebuffer *arg,
					 struct atomisp_css_frame **result)
{
	struct atomisp_css_frame *res;
	unsigned int padded_width;
	enum atomisp_css_frame_format sh_format;
	char *tmp_buf = NULL;
	int ret = 0;

	sh_format = v4l2_fmt_to_sh_fmt(arg->fmt.pixelformat);
	padded_width = atomisp_bytesperline_to_padded_width(
					arg->fmt.bytesperline, sh_format);

	/* Note: the padded width on an atomisp_css_frame is in elements, not in
	   bytes. The RAW frame we use here should always be a 16bit RAW
	   frame. This is why we bytesperline/2 is equal to the padded with */
	if (atomisp_css_frame_allocate(&res, arg->fmt.width, arg->fmt.height,
				  sh_format, padded_width, 0)) {
		ret = -ENOMEM;
		goto err;
	}

	tmp_buf = vmalloc(arg->fmt.sizeimage);
	if (!tmp_buf) {
		ret = -ENOMEM;
		goto err;
	}
	if (copy_from_user(tmp_buf, (void __user __force *)arg->base,
			   arg->fmt.sizeimage)) {
		ret = -EFAULT;
		goto err;
	}

	if (hmm_store(res->data, tmp_buf, arg->fmt.sizeimage)) {
		ret = -EINVAL;
		goto err;
	}

err:
	if (ret && res)
		atomisp_css_frame_free(res);
	if (tmp_buf)
		vfree(tmp_buf);
	if (ret == 0)
		*result = res;
	return ret;
}

/*
 * Function to configure fixed pattern noise table
 */
int atomisp_fixed_pattern_table(struct atomisp_sub_device *asd,
				struct v4l2_framebuffer *arg)
{
	struct atomisp_css_frame *raw_black_frame = NULL;
	int ret;

	if (arg == NULL)
		return -EINVAL;

	ret = atomisp_v4l2_framebuffer_to_css_frame(arg, &raw_black_frame);
	if (ret)
		return ret;
	if (atomisp_css_set_black_frame(asd, raw_black_frame))
		ret = -ENOMEM;

	atomisp_css_frame_free(raw_black_frame);
	return ret;
}

/*
 * Function to configure false color correction
 */
int atomisp_false_color(struct atomisp_sub_device *asd, int flag,
			__s32 *value)
{
	/* Get nr config from current setup */
	if (flag == 0) {
		*value = asd->params.false_color;
		return 0;
	}

	/* Set nr config to isp parameters */
	if (*value) {
		atomisp_css_set_default_de_config(asd);
	} else {
		asd->params.css_param.de_config.pixelnoise = 0;
		atomisp_css_set_de_config(asd, &asd->params.css_param.de_config);
	}
	asd->params.css_update_params_needed = true;
	asd->params.false_color = *value;
	return 0;
}

/*
 * Function to configure bad pixel correction params
 */
int atomisp_false_color_param(struct atomisp_sub_device *asd, int flag,
			      struct atomisp_de_config *config)
{
	if (flag == 0) {
		/* Get false color from current setup */
		if (atomisp_css_get_de_config(asd, config))
			return -EINVAL;
	} else {
		/* Set false color to isp parameters */
		memcpy(&asd->params.css_param.de_config, config,
				sizeof(asd->params.css_param.de_config));
		atomisp_css_set_de_config(asd, &asd->params.css_param.de_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to configure white balance params
 */
int atomisp_white_balance_param(struct atomisp_sub_device *asd, int flag,
	struct atomisp_wb_config *config)
{
	if (flag == 0) {
		/* Get white balance from current setup */
		if (atomisp_css_get_wb_config(asd, config))
			return -EINVAL;
	} else {
		/* Set white balance to isp parameters */
		memcpy(&asd->params.css_param.wb_config, config,
				sizeof(asd->params.css_param.wb_config));
		atomisp_css_set_wb_config(asd, &asd->params.css_param.wb_config);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

int atomisp_3a_config_param(struct atomisp_sub_device *asd, int flag,
			    struct atomisp_3a_config *config)
{
	struct atomisp_device *isp = asd->isp;

	dev_dbg(isp->dev, ">%s %d\n", __func__, flag);

	if (flag == 0) {
		/* Get white balance from current setup */
		if (atomisp_css_get_3a_config(asd, config))
			return -EINVAL;
	} else {
		/* Set white balance to isp parameters */
		memcpy(&asd->params.css_param.s3a_config, config,
				sizeof(asd->params.css_param.s3a_config));
		atomisp_css_set_3a_config(asd, &asd->params.css_param.s3a_config);
		asd->params.css_update_params_needed = true;
	}

	dev_dbg(isp->dev, "<%s %d\n", __func__, flag);
	return 0;
}

/*
 * Function to setup digital zoom
 */
int atomisp_digital_zoom(struct atomisp_sub_device *asd, int flag,
			 __s32 *value)
{
	u32 zoom;
	struct atomisp_device *isp = asd->isp;

	unsigned int max_zoom = MRFLD_MAX_ZOOM_FACTOR;

	if (flag == 0) {
		atomisp_css_get_zoom_factor(asd, &zoom);
		*value = max_zoom - zoom;
	} else {
		if (*value < 0)
			return -EINVAL;

		zoom = max_zoom - min_t(u32, max_zoom - 1, *value);
		atomisp_css_set_zoom_factor(asd, zoom);

		if (asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream &&
		    asd->streaming == ATOMISP_DEVICE_STREAMING_ENABLED) {
			dev_dbg(isp->dev, "%s, zoom: %d\n", __func__, zoom);
			atomisp_css_update_isp_params(asd);
		} else {
			asd->params.css_update_params_needed = true;
		}
	}

	return 0;
}

/*
 * Function to get sensor specific info for current resolution,
 * which will be used for auto exposure conversion.
 */
int atomisp_get_sensor_mode_data(struct atomisp_sub_device *asd,
				 struct atomisp_sensor_mode_data *config)
{
	struct camera_mipi_info *mipi_info;
	struct atomisp_device *isp = asd->isp;

	mipi_info = atomisp_to_sensor_mipi_info(
		isp->inputs[asd->input_curr].camera);
	if (mipi_info == NULL)
		return -EINVAL;

	memcpy(config, &mipi_info->data, sizeof(*config));
	return 0;
}

int atomisp_get_fmt(struct video_device *vdev, struct v4l2_format *f)
{
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);

	f->fmt.pix = pipe->pix;

	return 0;
}

static void __atomisp_update_stream_env(struct atomisp_sub_device *asd,
	uint16_t stream_index, struct atomisp_input_stream_info *stream_info)
{
	int i;

#if defined(ISP2401_NEW_INPUT_SYSTEM)
	/* assign virtual channel id return from sensor driver query */
	asd->stream_env[stream_index].ch_id = stream_info->ch_id;
#endif
	asd->stream_env[stream_index].isys_configs = stream_info->isys_configs;
	for (i = 0; i < stream_info->isys_configs; i++) {
		asd->stream_env[stream_index].isys_info[i].input_format =
			stream_info->isys_info[i].input_format;
		asd->stream_env[stream_index].isys_info[i].width =
			stream_info->isys_info[i].width;
		asd->stream_env[stream_index].isys_info[i].height =
			stream_info->isys_info[i].height;
	}
}

static void __atomisp_init_stream_info(uint16_t stream_index,
		struct atomisp_input_stream_info *stream_info)
{
	int i;

	stream_info->enable = 1;
	stream_info->stream = stream_index;
	stream_info->ch_id = 0;
	stream_info->isys_configs = 0;
	for (i = 0; i < MAX_STREAMS_PER_CHANNEL; i++) {
		stream_info->isys_info[i].input_format = 0;
		stream_info->isys_info[i].width = 0;
		stream_info->isys_info[i].height = 0;
	}
}

/* This function looks up the closest available resolution. */
int atomisp_try_fmt(struct video_device *vdev, struct v4l2_format *f,
						bool *res_overflow)
{
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct v4l2_mbus_framefmt snr_mbus_fmt;
	const struct atomisp_format_bridge *fmt;
	struct atomisp_input_stream_info *stream_info =
		(struct atomisp_input_stream_info *)snr_mbus_fmt.reserved;
	uint16_t stream_index;
	int source_pad = atomisp_subdev_source_pad(vdev);
	int ret;

	if (isp->inputs[asd->input_curr].camera == NULL)
		return -EINVAL;

	stream_index = atomisp_source_pad_to_stream_id(asd, source_pad);
	fmt = atomisp_get_format_bridge(f->fmt.pix.pixelformat);
	if (fmt == NULL) {
		dev_err(isp->dev, "unsupported pixelformat!\n");
		fmt = atomisp_output_fmts;
	}

	snr_mbus_fmt.code = fmt->mbus_code;
	snr_mbus_fmt.width = f->fmt.pix.width;
	snr_mbus_fmt.height = f->fmt.pix.height;

	__atomisp_init_stream_info(stream_index, stream_info);

	dev_dbg(isp->dev, "try_mbus_fmt: asking for %ux%u\n",
		snr_mbus_fmt.width, snr_mbus_fmt.height);

	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
			video, try_mbus_fmt, &snr_mbus_fmt);
	if (ret)
		return ret;

	dev_dbg(isp->dev, "try_mbus_fmt: got %ux%u\n",
		snr_mbus_fmt.width, snr_mbus_fmt.height);

	fmt = atomisp_get_format_bridge_from_mbus(snr_mbus_fmt.code);
	if (fmt == NULL) {
		dev_err(isp->dev, "unknown sensor format 0x%8.8x\n",
			snr_mbus_fmt.code);
		return -EINVAL;
	}

	f->fmt.pix.pixelformat = fmt->pixelformat;

	/*
	 * If the format is jpeg or custom RAW, then the width and height will
	 * not satisfy the normal atomisp requirements and no need to check
	 * the below conditions. So just assign to what is being returned from
	 * the sensor driver.
	 */
	if (f->fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG ||
	    f->fmt.pix.pixelformat == V4L2_PIX_FMT_CUSTOM_M10MO_RAW) {
		f->fmt.pix.width = snr_mbus_fmt.width;
		f->fmt.pix.height = snr_mbus_fmt.height;
		return 0;
	}

	if (snr_mbus_fmt.width < f->fmt.pix.width
	    && snr_mbus_fmt.height < f->fmt.pix.height) {
		f->fmt.pix.width = snr_mbus_fmt.width;
		f->fmt.pix.height = snr_mbus_fmt.height;
		/* Set the flag when resolution requested is
		 * beyond the max value supported by sensor
		 */
		if (res_overflow != NULL)
			*res_overflow = true;
	}

	/* app vs isp */
	f->fmt.pix.width = rounddown(
		clamp_t(u32, f->fmt.pix.width, ATOM_ISP_MIN_WIDTH,
			ATOM_ISP_MAX_WIDTH), ATOM_ISP_STEP_WIDTH);
	f->fmt.pix.height = rounddown(
		clamp_t(u32, f->fmt.pix.height, ATOM_ISP_MIN_HEIGHT,
			ATOM_ISP_MAX_HEIGHT), ATOM_ISP_STEP_HEIGHT);

	return 0;
}

static int
atomisp_try_fmt_file(struct atomisp_device *isp, struct v4l2_format *f)
{
	u32 width = f->fmt.pix.width;
	u32 height = f->fmt.pix.height;
	u32 pixelformat = f->fmt.pix.pixelformat;
	enum v4l2_field field = f->fmt.pix.field;
	u32 depth;

	if (!atomisp_get_format_bridge(pixelformat)) {
		dev_err(isp->dev, "Wrong output pixelformat\n");
		return -EINVAL;
	}

	depth = get_pixel_depth(pixelformat);

	if (!field || field == V4L2_FIELD_ANY)
		field = V4L2_FIELD_NONE;
	else if (field != V4L2_FIELD_NONE) {
		dev_err(isp->dev, "Wrong output field\n");
		return -EINVAL;
	}

	f->fmt.pix.field = field;
	f->fmt.pix.width = clamp_t(u32,
				   rounddown(width, (u32)ATOM_ISP_STEP_WIDTH),
				   ATOM_ISP_MIN_WIDTH, ATOM_ISP_MAX_WIDTH);
	f->fmt.pix.height = clamp_t(u32, rounddown(height,
						   (u32)ATOM_ISP_STEP_HEIGHT),
				    ATOM_ISP_MIN_HEIGHT, ATOM_ISP_MAX_HEIGHT);
	f->fmt.pix.bytesperline = (width * depth) >> 3;

	return 0;
}

mipi_port_ID_t __get_mipi_port(struct atomisp_device *isp,
				enum atomisp_camera_port port)
{
	switch (port) {
	case ATOMISP_CAMERA_PORT_PRIMARY:
		return MIPI_PORT0_ID;
	case ATOMISP_CAMERA_PORT_SECONDARY:
		return MIPI_PORT1_ID;
	case ATOMISP_CAMERA_PORT_TERTIARY:
		if (MIPI_PORT1_ID + 1 != N_MIPI_PORT_ID)
			return MIPI_PORT1_ID + 1;
		/* go through down for else case */
	default:
		dev_err(isp->dev, "unsupported port: %d\n", port);
		return MIPI_PORT0_ID;
	}
}

static inline int atomisp_set_sensor_mipi_to_isp(
				struct atomisp_sub_device *asd,
				enum atomisp_input_stream_id stream_id,
				struct camera_mipi_info *mipi_info)
{
	struct v4l2_control ctrl;
	struct atomisp_device *isp = asd->isp;
	const struct atomisp_in_fmt_conv *fc;
	int mipi_freq = 0;
	unsigned int input_format, bayer_order;

	ctrl.id = V4L2_CID_LINK_FREQ;
	if (v4l2_subdev_g_ctrl(isp->inputs[asd->input_curr].camera, &ctrl) == 0)
		mipi_freq = ctrl.value;

	if (asd->stream_env[stream_id].isys_configs == 1) {
		input_format =
			asd->stream_env[stream_id].isys_info[0].input_format;
		atomisp_css_isys_set_format(asd, stream_id,
				input_format, IA_CSS_STREAM_DEFAULT_ISYS_STREAM_IDX);
	} else if (asd->stream_env[stream_id].isys_configs == 2) {
		atomisp_css_isys_two_stream_cfg_update_stream1(
				asd, stream_id,
				asd->stream_env[stream_id].isys_info[0].input_format,
				asd->stream_env[stream_id].isys_info[0].width,
				asd->stream_env[stream_id].isys_info[0].height);

		atomisp_css_isys_two_stream_cfg_update_stream2(
				asd, stream_id,
				asd->stream_env[stream_id].isys_info[1].input_format,
				asd->stream_env[stream_id].isys_info[1].width,
				asd->stream_env[stream_id].isys_info[1].height);
	}

	/* Compatibility for sensors which provide no media bus code
	 * in s_mbus_framefmt() nor support pad formats. */
	if (mipi_info->input_format != -1) {
		bayer_order = mipi_info->raw_bayer_order;

		/* Input stream config is still needs configured */
		/* TODO: Check if this is necessary */
		fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(
						mipi_info->input_format);
		if (!fc)
			return -EINVAL;
		input_format = fc->css_stream_fmt;
	} else {
		struct v4l2_mbus_framefmt *sink;
		sink = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK);
		fc = atomisp_find_in_fmt_conv(sink->code);
		if (!fc)
			return -EINVAL;
		input_format = fc->css_stream_fmt;
		bayer_order = fc->bayer_order;
	}

	atomisp_css_input_set_format(asd, stream_id, input_format);
	atomisp_css_input_set_bayer_order(asd, stream_id, bayer_order);

	fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(
					mipi_info->metadata_format);
	if (!fc)
		return -EINVAL;
	input_format = fc->css_stream_fmt;
	atomisp_css_input_configure_port(asd,
				__get_mipi_port(asd->isp, mipi_info->port),
				mipi_info->num_lanes,
				0xffff4, mipi_freq,
				input_format,
				mipi_info->metadata_width,
				mipi_info->metadata_height);
	return 0;
}

static int __enable_continuous_mode(struct atomisp_sub_device *asd,
				    bool enable)
{
	struct atomisp_device *isp = asd->isp;

	dev_dbg(isp->dev,
		"continuous mode %d, raw buffers %d, stop preview %d\n",
		enable, asd->continuous_raw_buffer_size->val,
		!asd->continuous_viewfinder->val);
	atomisp_css_capture_set_mode(asd, CSS_CAPTURE_MODE_PRIMARY);
	/* in case of ANR, force capture pipe to offline mode */
	atomisp_css_capture_enable_online(asd, ATOMISP_INPUT_STREAM_GENERAL,
			asd->params.low_light ? false : !enable);
	atomisp_css_preview_enable_online(asd, ATOMISP_INPUT_STREAM_GENERAL,
			!enable);
	atomisp_css_enable_continuous(asd, enable);
	atomisp_css_enable_cvf(asd, asd->continuous_viewfinder->val);

	if (atomisp_css_continuous_set_num_raw_frames(asd,
			asd->continuous_raw_buffer_size->val)) {
		dev_err(isp->dev, "css_continuous_set_num_raw_frames failed\n");
		return -EINVAL;
	}

	if (!enable) {
		atomisp_css_enable_raw_binning(asd, false);
		atomisp_css_input_set_two_pixels_per_clock(asd, false);
	}

	if (isp->inputs[asd->input_curr].type != FILE_INPUT)
		atomisp_css_input_set_mode(asd, CSS_INPUT_MODE_SENSOR);

	return atomisp_update_run_mode(asd);
}

int configure_pp_input_nop(struct atomisp_sub_device *asd,
			   unsigned int width, unsigned int height)
{
	return 0;
}

int configure_output_nop(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format sh_fmt)
{
	return 0;
}

int get_frame_info_nop(struct atomisp_sub_device *asd,
				struct atomisp_css_frame_info *finfo)
{
	return 0;
}

/**
 * Resets CSS parameters that depend on input resolution.
 *
 * Update params like CSS RAW binning, 2ppc mode and pp_input
 * which depend on input size, but are not automatically
 * handled in CSS when the input resolution is changed.
 */
static int css_input_resolution_changed(struct atomisp_sub_device *asd,
		struct v4l2_mbus_framefmt *ffmt)
{
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf;
	unsigned int i;

	dev_dbg(asd->isp->dev, "css_input_resolution_changed to %ux%u\n",
		ffmt->width, ffmt->height);

#if defined(ISP2401_NEW_INPUT_SYSTEM)
	atomisp_css_input_set_two_pixels_per_clock(asd, false);
#else
	atomisp_css_input_set_two_pixels_per_clock(asd, true);
#endif
	if (asd->continuous_mode->val) {
		/* Note for all checks: ffmt includes pad_w+pad_h */
		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO ||
		    (ffmt->width >= 2048 || ffmt->height >= 1536)) {
			/*
			 * For preview pipe, enable only if resolution
			 * is >= 3M for ISP2400.
			 */
			atomisp_css_enable_raw_binning(asd, true);
		}
	}
	/*
	 * If sensor input changed, which means metadata resolution changed
	 * together. Release all metadata buffers here to let it re-allocated
	 * next time in reqbufs.
	 */
	for (i = 0; i < ATOMISP_METADATA_TYPE_NUM; i++) {
		list_for_each_entry_safe(md_buf, _md_buf, &asd->metadata[i],
					list) {
			atomisp_css_free_metadata_buffer(md_buf);
			list_del(&md_buf->list);
			kfree(md_buf);
		}
	}
	return 0;

	/*
	 * TODO: atomisp_css_preview_configure_pp_input() not
	 *       reset due to CSS bug tracked as PSI BZ 115124
	 */
}

static int atomisp_set_fmt_to_isp(struct video_device *vdev,
			struct atomisp_css_frame_info *output_info,
			struct atomisp_css_frame_info *raw_output_info,
			struct v4l2_pix_format *pix,
			unsigned int source_pad)
{
	struct camera_mipi_info *mipi_info;
	static enum atomisp_input_format input_format_temporary_keep;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	const struct atomisp_format_bridge *format;
	struct v4l2_rect *isp_sink_crop;
	enum atomisp_css_pipe_id pipe_id;
	struct v4l2_subdev_fh fh;
	int (*configure_output)(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum atomisp_css_frame_format sh_fmt) =
							configure_output_nop;
	int (*get_frame_info)(struct atomisp_sub_device *asd,
				struct atomisp_css_frame_info *finfo) =
							get_frame_info_nop;
	int (*configure_pp_input)(struct atomisp_sub_device *asd,
				  unsigned int width, unsigned int height) =
							configure_pp_input_nop;
	uint16_t stream_index = atomisp_source_pad_to_stream_id(asd, source_pad);
	const struct atomisp_in_fmt_conv *fc;
	int ret;

	v4l2_fh_init(&fh.vfh, vdev);

	isp_sink_crop = atomisp_subdev_get_rect(
		&asd->subdev, NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
		ATOMISP_SUBDEV_PAD_SINK, V4L2_SEL_TGT_CROP);

	format = atomisp_get_format_bridge(pix->pixelformat);
	if (format == NULL)
		return -EINVAL;

	if (isp->inputs[asd->input_curr].type != TEST_PATTERN &&
		isp->inputs[asd->input_curr].type != FILE_INPUT) {
		mipi_info = atomisp_to_sensor_mipi_info(
			isp->inputs[asd->input_curr].camera);
		if (!mipi_info) {
			dev_err(isp->dev, "mipi_info is NULL\n");
			return -EINVAL;
		}
		printk(KERN_INFO "ASUSBSP --- RAW10 hack started! \n");
		if (0 == strncmp(isp->inputs[asd->input_curr].camera->name, "m10mo", strlen("m10mo"))) {
			printk(KERN_INFO "ASUSBSP --- RAW10 hack started! [2]\n");
			if(pix->pixelformat == V4L2_PIX_FMT_SGBRG8) {
			    printk(KERN_INFO "ASUSBSP --- To capture RAW10, WA for m10mo/m12mo on ASUS project! \n");
			    input_format_temporary_keep = mipi_info->input_format;
				mipi_info->raw_bayer_order = atomisp_bayer_order_gbrg;
				mipi_info->input_format = ATOMISP_INPUT_FORMAT_RAW_8;
			} else if (pix->pixelformat == V4L2_PIX_FMT_NV12 && input_format_temporary_keep != 0) {
			    printk(KERN_INFO "ASUSBSP --- To preview after capturing RAW10, WA for m10mo/m12mo on ASUS project! \n");
				mipi_info->raw_bayer_order = 0;
				mipi_info->input_format = input_format_temporary_keep;
				input_format_temporary_keep = 0;
			}
		}
		if (atomisp_set_sensor_mipi_to_isp(asd, stream_index,
					mipi_info))
			return -EINVAL;
		fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(
				mipi_info->input_format);
		if (!fc)
			fc = atomisp_find_in_fmt_conv(
					atomisp_subdev_get_ffmt(&asd->subdev,
					NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
					ATOMISP_SUBDEV_PAD_SINK)->code);
		if (!fc)
			return -EINVAL;
		if (format->sh_fmt == CSS_FRAME_FORMAT_RAW &&
		     raw_output_format_match_input(fc->css_stream_fmt,
			pix->pixelformat))
			return -EINVAL;
	}

	/*
	 * Configure viewfinder also when vfpp is disabled: the
	 * CSS still requires viewfinder configuration.
	 */
	if (asd->fmt_auto->val ||
	    asd->vfpp->val != ATOMISP_VFPP_ENABLE) {
		struct v4l2_rect vf_size = {0};
		struct v4l2_mbus_framefmt vf_ffmt = {0};

		if (pix->width < 640 || pix->height < 480) {
			vf_size.width = pix->width;
			vf_size.height = pix->height;
		} else {
			vf_size.width = 640;
			vf_size.height = 480;
		}

		/* FIXME: proper format name for this one. See
		   atomisp_output_fmts[] in atomisp_v4l2.c */
		vf_ffmt.code = V4L2_MBUS_FMT_CUSTOM_YUV420;

		atomisp_subdev_set_selection(&asd->subdev, &fh,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     ATOMISP_SUBDEV_PAD_SOURCE_VF,
					     V4L2_SEL_TGT_COMPOSE, 0, &vf_size);
		atomisp_subdev_set_ffmt(&asd->subdev, &fh,
					V4L2_SUBDEV_FORMAT_ACTIVE,
					ATOMISP_SUBDEV_PAD_SOURCE_VF, &vf_ffmt);
		asd->video_out_vf.sh_fmt = CSS_FRAME_FORMAT_NV12;

		if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
			atomisp_css_video_configure_viewfinder(asd,
				vf_size.width, vf_size.height, 0,
				asd->video_out_vf.sh_fmt);
		} else if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
			if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW ||
			    source_pad == ATOMISP_SUBDEV_PAD_SOURCE_VIDEO)
				atomisp_css_video_configure_viewfinder(asd,
					vf_size.width, vf_size.height, 0,
					asd->video_out_vf.sh_fmt);
			else
				atomisp_css_capture_configure_viewfinder(asd,
					vf_size.width, vf_size.height, 0,
					asd->video_out_vf.sh_fmt);
		} else if (source_pad != ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW ||
			 asd->vfpp->val == ATOMISP_VFPP_DISABLE_LOWLAT) {
			atomisp_css_capture_configure_viewfinder(asd,
				vf_size.width, vf_size.height, 0,
				asd->video_out_vf.sh_fmt);
		}
	}

	if (asd->continuous_mode->val) {
		ret = __enable_continuous_mode(asd, true);
		if (ret)
			return -EINVAL;
	}

	atomisp_css_input_set_mode(asd, CSS_INPUT_MODE_SENSOR);
	atomisp_css_disable_vf_pp(asd,
			asd->vfpp->val != ATOMISP_VFPP_ENABLE);

	/* ISP2401 new input system need to use copy pipe */
	if (asd->copy_mode) {
		pipe_id = CSS_PIPE_ID_COPY;
		atomisp_css_capture_enable_online(asd, stream_index, false);
	} else if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
		/* video same in continuouscapture and online modes */
		configure_output = atomisp_css_video_configure_output;
		get_frame_info = atomisp_css_video_get_output_frame_info;
		pipe_id = CSS_PIPE_ID_VIDEO;
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		if (!asd->continuous_mode->val) {
			configure_output = atomisp_css_video_configure_output;
			get_frame_info =
				atomisp_css_video_get_output_frame_info;
			pipe_id = CSS_PIPE_ID_VIDEO;
		} else {
			if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW ||
			    source_pad == ATOMISP_SUBDEV_PAD_SOURCE_VIDEO) {
				configure_output =
					atomisp_css_video_configure_output;
				get_frame_info =
					atomisp_css_video_get_output_frame_info;
				configure_pp_input =
					atomisp_css_video_configure_pp_input;
				pipe_id = CSS_PIPE_ID_VIDEO;
			} else {
				configure_output =
					atomisp_css_capture_configure_output;
				get_frame_info =
					atomisp_css_capture_get_output_frame_info;
				configure_pp_input =
					atomisp_css_capture_configure_pp_input;
				pipe_id = CSS_PIPE_ID_CAPTURE;

				atomisp_update_capture_mode(asd);
				atomisp_css_capture_enable_online(asd, stream_index, false);
			}
		}
	} else if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW) {
		configure_output = atomisp_css_preview_configure_output;
		get_frame_info = atomisp_css_preview_get_output_frame_info;
		configure_pp_input = atomisp_css_preview_configure_pp_input;
		pipe_id = CSS_PIPE_ID_PREVIEW;
	} else {
		/* CSS doesn't support low light mode on SOC cameras, so disable
		 * it. FIXME: if this is done elsewhere, it gives corrupted
		 * colors into thumbnail image.
		 */
		if (isp->inputs[asd->input_curr].type == SOC_CAMERA)
			asd->params.low_light = false;

		if (format->sh_fmt == CSS_FRAME_FORMAT_RAW) {
			atomisp_css_capture_set_mode(asd, CSS_CAPTURE_MODE_RAW);
			atomisp_css_enable_dz(asd, false);
		} else {
			atomisp_update_capture_mode(asd);
		}

		if (!asd->continuous_mode->val)
			/* in case of ANR, force capture pipe to offline mode */
			atomisp_css_capture_enable_online(asd, stream_index,
					asd->params.low_light ?
					false : asd->params.online_process);

		configure_output = atomisp_css_capture_configure_output;
		get_frame_info = atomisp_css_capture_get_output_frame_info;
		configure_pp_input = atomisp_css_capture_configure_pp_input;
		pipe_id = CSS_PIPE_ID_CAPTURE;

		if (!asd->params.online_process &&
		    !asd->continuous_mode->val) {
			ret = atomisp_css_capture_get_output_raw_frame_info(asd,
							raw_output_info);
			if (ret)
				return ret;
		}
		if (!asd->continuous_mode->val && asd->run_mode->val
		    != ATOMISP_RUN_MODE_STILL_CAPTURE) {
			dev_err(isp->dev,
				    "Need to set the running mode first\n");
			asd->run_mode->val = ATOMISP_RUN_MODE_STILL_CAPTURE;
		}
	}

	/*
	 * to SOC camera, use yuvpp pipe.
	 */
	if (ATOMISP_USE_YUVPP(asd))
		pipe_id = CSS_PIPE_ID_YUVPP;

	if (asd->copy_mode)
		ret = atomisp_css_copy_configure_output(asd, stream_index,
				pix->width, pix->height,
				format->planar ? pix->bytesperline :
					pix->bytesperline * 8 / format->depth,
				format->sh_fmt);
	else
		ret = configure_output(asd, pix->width, pix->height,
				       format->planar ? pix->bytesperline :
				       pix->bytesperline * 8 / format->depth,
				       format->sh_fmt);
	if (ret) {
		dev_err(isp->dev, "configure_output %ux%u, format %8.8x\n",
			pix->width, pix->height, format->sh_fmt);
		return -EINVAL;
	}

	if (asd->continuous_mode->val &&
	    (configure_pp_input == atomisp_css_preview_configure_pp_input ||
	     configure_pp_input == atomisp_css_video_configure_pp_input)) {
		/* for isp 2.2, configure pp input is available for continuous
		 * mode */
		ret = configure_pp_input(asd, isp_sink_crop->width,
					 isp_sink_crop->height);
		if (ret) {
			dev_err(isp->dev, "configure_pp_input %ux%u\n",
				isp_sink_crop->width,
				isp_sink_crop->height);
			return -EINVAL;
		}
	} else {
		ret = configure_pp_input(asd, isp_sink_crop->width,
					 isp_sink_crop->height);
		if (ret) {
			dev_err(isp->dev, "configure_pp_input %ux%u\n",
				isp_sink_crop->width, isp_sink_crop->height);
			return -EINVAL;
		}
	}
	if (asd->copy_mode)
		ret = atomisp_css_copy_get_output_frame_info(asd, stream_index,
				output_info);
	else
		ret = get_frame_info(asd, output_info);
	if (ret) {
		dev_err(isp->dev, "get_frame_info %ux%u (padded to %u)\n",
			pix->width, pix->height, pix->bytesperline);
		return -EINVAL;
	}

	atomisp_update_grid_info(asd, pipe_id, source_pad);

	/* Free the raw_dump buffer first */
	atomisp_css_frame_free(asd->raw_output_frame);
	asd->raw_output_frame = NULL;

	if (!asd->continuous_mode->val &&
	    !asd->params.online_process && !isp->sw_contex.file_input &&
		atomisp_css_frame_allocate_from_info(&asd->raw_output_frame,
							raw_output_info))
		return -ENOMEM;

	return 0;
}

static void atomisp_get_dis_envelop(struct atomisp_sub_device *asd,
			    unsigned int width, unsigned int height,
			    unsigned int *dvs_env_w, unsigned int *dvs_env_h)
{
	struct atomisp_device *isp = asd->isp;

	/* if subdev type is SOC camera,we do not need to set DVS */
	if (isp->inputs[asd->input_curr].type == SOC_CAMERA)
		asd->params.video_dis_en = 0;

	if (asd->params.video_dis_en &&
	    asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO &&
	    !asd->continuous_mode->val) {
		/* envelope is 20% of the output resolution */
		/*
		 * dvs envelope cannot be round up.
		 * it would cause ISP timeout and color switch issue
		 */
		*dvs_env_w = rounddown(width / 5, ATOM_ISP_STEP_WIDTH);
		*dvs_env_h = rounddown(height / 5, ATOM_ISP_STEP_HEIGHT);
	}

	asd->params.dis_proj_data_valid = false;
	asd->params.css_update_params_needed = true;
}

static void atomisp_check_copy_mode(struct atomisp_sub_device *asd,
		int source_pad)
{
#if defined(ISP2401_NEW_INPUT_SYSTEM)
	/*
	 * For SOC camera we are using copy_mode all the time
	 * TDB: Do we have some cases where the copy_mode can't be used?
	 */
	if (((asd->isp->inputs[asd->input_curr].type == RAW_CAMERA) &&
	    (!atomisp_subdev_format_conversion(asd, source_pad))) ||
	    ((asd->isp->inputs[asd->input_curr].type == SOC_CAMERA) &&
	    (asd->isp->inputs[asd->input_curr].camera_caps->
		sensor[asd->sensor_curr].stream_num > 1)))
		asd->copy_mode = true;
	else
#endif
		/* Only used for the new input system */
		asd->copy_mode = false;

	dev_dbg(asd->isp->dev, "copy_mode: %d\n", asd->copy_mode);

#if defined(ISP2401_NEW_INPUT_SYSTEM)
	/*
	 * In copy mode we can still make a format conversion if formats
	 * doesn't match
	 */
	if (asd->copy_mode)
		asd->copy_mode_format_conv =
			atomisp_subdev_copy_format_conversion(asd, source_pad);
	dev_dbg(asd->isp->dev, "copy_mode_format_conv: %d\n", asd->copy_mode_format_conv);
#endif

}

static int atomisp_set_fmt_to_snr(struct video_device *vdev,
		struct v4l2_format *f, unsigned int pixelformat,
		unsigned int padding_w, unsigned int padding_h,
		unsigned int dvs_env_w, unsigned int dvs_env_h)
{
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	const struct atomisp_format_bridge *format;
	struct v4l2_mbus_framefmt ffmt, req_ffmt;
	struct atomisp_device *isp = asd->isp;
	struct atomisp_input_stream_info *stream_info =
		(struct atomisp_input_stream_info *)ffmt.reserved;
	uint16_t stream_index = ATOMISP_INPUT_STREAM_GENERAL;
	int source_pad = atomisp_subdev_source_pad(vdev);
	struct v4l2_subdev_fh fh;
	int ret;

	v4l2_fh_init(&fh.vfh, vdev);

	stream_index = atomisp_source_pad_to_stream_id(asd, source_pad);

	format = atomisp_get_format_bridge(pixelformat);
	if (format == NULL)
		return -EINVAL;

	v4l2_fill_mbus_format(&ffmt, &f->fmt.pix, format->mbus_code);
	ffmt.height += padding_h + dvs_env_h;
	ffmt.width += padding_w + dvs_env_w;

	dev_dbg(isp->dev, "s_mbus_fmt: ask %ux%u (padding %ux%u, dvs %ux%u)\n",
		ffmt.width, ffmt.height, padding_w, padding_h,
		dvs_env_w, dvs_env_h);

	__atomisp_init_stream_info(stream_index, stream_info);

	req_ffmt = ffmt;

	/* Disable dvs if resolution can't be supported by sensor */
	if (asd->params.video_dis_en &&
	    source_pad == ATOMISP_SUBDEV_PAD_SOURCE_VIDEO) {
		ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
			video, try_mbus_fmt, &ffmt);
		if (ret)
			return ret;
		if (ffmt.width < req_ffmt.width ||
				ffmt.height < req_ffmt.height) {
			req_ffmt.height -= dvs_env_h;
			req_ffmt.width -= dvs_env_w;
			ffmt = req_ffmt;
			dev_warn(isp->dev,
			  "can not enable video dis due to sensor limitation.");
			asd->params.video_dis_en = 0;
		}
	}
	dev_dbg(isp->dev, "sensor width: %d, height: %d\n",
		ffmt.width, ffmt.height);

	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera, video,
			       s_mbus_fmt, &ffmt);
	if (ret)
		return ret;

	__atomisp_update_stream_env(asd, stream_index, stream_info);

	dev_dbg(isp->dev, "sensor width: %d, height: %d\n",
		ffmt.width, ffmt.height);

	if (ffmt.width < ATOM_ISP_STEP_WIDTH ||
	    ffmt.height < ATOM_ISP_STEP_HEIGHT)
			return -EINVAL;

	if (asd->params.video_dis_en &&
	    source_pad == ATOMISP_SUBDEV_PAD_SOURCE_VIDEO &&
	    (ffmt.width < req_ffmt.width || ffmt.height < req_ffmt.height)) {
		dev_warn(isp->dev,
			 "can not enable video dis due to sensor limitation.");
		asd->params.video_dis_en = 0;
	}

	atomisp_subdev_set_ffmt(&asd->subdev, &fh,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK, &ffmt);

	return css_input_resolution_changed(asd, &ffmt);
}

int atomisp_set_fmt(struct video_device *vdev, struct v4l2_format *f)
{
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	const struct atomisp_format_bridge *format_bridge;
	struct atomisp_css_frame_info output_info, raw_output_info;
	struct v4l2_format snr_fmt = *f;
	struct v4l2_format backup_fmt = *f, s_fmt = *f;
	unsigned int dvs_env_w = 0, dvs_env_h = 0;
	unsigned int padding_w = pad_w, padding_h = pad_h;
	bool res_overflow = false, crop_needs_override = false;
	struct v4l2_mbus_framefmt isp_sink_fmt;
	struct v4l2_mbus_framefmt isp_source_fmt = {0};
	struct v4l2_rect isp_sink_crop;
	uint16_t source_pad = atomisp_subdev_source_pad(vdev);
	struct v4l2_subdev_fh fh;
	int ret;

	dev_dbg(isp->dev,
		"setting resolution %ux%u on pad %u for asd%d, bytesperline %u\n",
		f->fmt.pix.width, f->fmt.pix.height, source_pad,
		asd->index, f->fmt.pix.bytesperline);

	if (asd->streaming == ATOMISP_DEVICE_STREAMING_ENABLED) {
		dev_warn(isp->dev, "ISP does not support set format while at streaming!\n");
		return -EBUSY;
	}

	v4l2_fh_init(&fh.vfh, vdev);

	format_bridge = atomisp_get_format_bridge(f->fmt.pix.pixelformat);
	if (format_bridge == NULL)
		return -EINVAL;

	pipe->sh_fmt = format_bridge->sh_fmt;
	pipe->pix.pixelformat = f->fmt.pix.pixelformat;

	if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_VF ||
	    (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW
		&& asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)) {
		if (asd->fmt_auto->val) {
			struct v4l2_rect *capture_comp;
			struct v4l2_rect r = {0};

			r.width = f->fmt.pix.width;
			r.height = f->fmt.pix.height;

			if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW)
				capture_comp = atomisp_subdev_get_rect(
					&asd->subdev, NULL,
					V4L2_SUBDEV_FORMAT_ACTIVE,
					ATOMISP_SUBDEV_PAD_SOURCE_VIDEO,
					V4L2_SEL_TGT_COMPOSE);
			else
				capture_comp = atomisp_subdev_get_rect(
					&asd->subdev, NULL,
					V4L2_SUBDEV_FORMAT_ACTIVE,
					ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE,
					V4L2_SEL_TGT_COMPOSE);

			if (capture_comp->width < r.width
			    || capture_comp->height < r.height) {
				r.width = capture_comp->width;
				r.height = capture_comp->height;
			}

			atomisp_subdev_set_selection(
				&asd->subdev, &fh,
				V4L2_SUBDEV_FORMAT_ACTIVE, source_pad,
				V4L2_SEL_TGT_COMPOSE, 0, &r);

			f->fmt.pix.width = r.width;
			f->fmt.pix.height = r.height;
		}

		if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW &&
		    asd->copy_mode) {
			uint16_t video_index =
				atomisp_source_pad_to_stream_id(asd,
					ATOMISP_SUBDEV_PAD_SOURCE_VIDEO);

			ret = atomisp_css_copy_get_output_frame_info(asd,
				video_index, &output_info);
			if (ret) {
				dev_err(isp->dev,
				      "copy_get_output_frame_info ret %i", ret);
				return -EINVAL;
			}
			if (!asd->yuvpp_mode) {
				/*
				 * If viewfinder was configured into copy_mode,
				 * we switch to using yuvpp pipe instead.
				 */
				asd->yuvpp_mode = true;
				ret = atomisp_css_copy_configure_output(
					asd, video_index, 0, 0, 0, 0);
				if (ret) {
					dev_err(isp->dev,
						"failed to disable copy pipe");
					return -EINVAL;
				}
				ret = atomisp_css_yuvpp_configure_output(
					asd, video_index,
					output_info.res.width,
					output_info.res.height,
					output_info.padded_width,
					output_info.format);
				if (ret) {
					dev_err(isp->dev,
						"failed to set up yuvpp pipe\n");
					return -EINVAL;
				}
				atomisp_css_video_enable_online(asd, false);
				atomisp_css_preview_enable_online(asd,
					ATOMISP_INPUT_STREAM_GENERAL, false);
			}
			atomisp_css_yuvpp_configure_viewfinder(asd, video_index,
				f->fmt.pix.width, f->fmt.pix.height,
				format_bridge->planar ? f->fmt.pix.bytesperline
				: f->fmt.pix.bytesperline * 8
				/ format_bridge->depth, format_bridge->sh_fmt);
			atomisp_css_yuvpp_get_viewfinder_frame_info(
				asd, video_index, &output_info);
		} else if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW) {
			atomisp_css_video_configure_viewfinder(asd,
				f->fmt.pix.width, f->fmt.pix.height,
				format_bridge->planar ? f->fmt.pix.bytesperline
				: f->fmt.pix.bytesperline * 8
				/ format_bridge->depth,	format_bridge->sh_fmt);
			atomisp_css_video_get_viewfinder_frame_info(asd,
								&output_info);
		} else {
			atomisp_css_capture_configure_viewfinder(asd,
				f->fmt.pix.width, f->fmt.pix.height,
				format_bridge->planar ? f->fmt.pix.bytesperline
				: f->fmt.pix.bytesperline * 8
				/ format_bridge->depth,	format_bridge->sh_fmt);
			atomisp_css_capture_get_viewfinder_frame_info(asd,
								&output_info);
		}

		goto done;
	}
	/*
	 * Check whether main resolution configured smaller
	 * than snapshot resolution. If so, force main resolution
	 * to be the same as snapshot resolution
	 */
	if (source_pad == ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE) {
		struct v4l2_rect *r;

		r = atomisp_subdev_get_rect(
			&asd->subdev, NULL,
			V4L2_SUBDEV_FORMAT_ACTIVE,
			ATOMISP_SUBDEV_PAD_SOURCE_VF, V4L2_SEL_TGT_COMPOSE);

		if (r->width && r->height
		    && (r->width > f->fmt.pix.width
			|| r->height > f->fmt.pix.height))
			dev_warn(isp->dev,
				 "Main Resolution config smaller then Vf Resolution. Force to be equal with Vf Resolution.");
	}

	/* Pipeline configuration done through subdevs. Bail out now. */
	if (!asd->fmt_auto->val)
		goto set_fmt_to_isp;

	/* get sensor resolution and format */
	ret = atomisp_try_fmt(vdev, &snr_fmt, &res_overflow);
	if (ret)
		return ret;
	f->fmt.pix.width = snr_fmt.fmt.pix.width;
	f->fmt.pix.height = snr_fmt.fmt.pix.height;

	{
		/*
		 * Klockwork Nit - HSD-ES 1504053110: Check for NULL pointers in this preemptable code.
		 */
		const struct atomisp_format_bridge *format_bridge_ptr, *snr_format_bridge_ptr;
		struct v4l2_mbus_framefmt *isp_fmt_ptr;

	        format_bridge_ptr = atomisp_get_format_bridge(f->fmt.pix.pixelformat);
	        snr_format_bridge_ptr = atomisp_get_format_bridge(snr_fmt.fmt.pix.pixelformat);

		isp_fmt_ptr = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
								V4L2_SUBDEV_FORMAT_ACTIVE,
								ATOMISP_SUBDEV_PAD_SINK);

		if (snr_format_bridge_ptr == NULL || isp_fmt_ptr == NULL || format_bridge_ptr == NULL) {
			dev_err(isp->dev, "snr_format_bridge_ptr:%p == NULL || isp_fmt_ptr:%p == NULL || format_bridge_ptr:%p == NULL",
                                           snr_format_bridge_ptr,              isp_fmt_ptr,              format_bridge_ptr);
	                return EINVAL;
		}
		isp_fmt_ptr->code = snr_format_bridge_ptr->mbus_code;
		isp_sink_fmt = *isp_fmt_ptr;
		isp_source_fmt.code = format_bridge_ptr->mbus_code;
	}

	atomisp_subdev_set_ffmt(&asd->subdev, &fh,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				source_pad, &isp_source_fmt);

	if (!atomisp_subdev_format_conversion(asd, source_pad)) {
		padding_w = 0;
		padding_h = 0;
	} else if (IS_BYT) {
		padding_w = 12;
		padding_h = 12;
	}

	/* construct resolution supported by isp */
	if (res_overflow && !asd->continuous_mode->val) {
		f->fmt.pix.width = rounddown(
			clamp_t(u32, f->fmt.pix.width - padding_w,
				ATOM_ISP_MIN_WIDTH,
				ATOM_ISP_MAX_WIDTH), ATOM_ISP_STEP_WIDTH);
		f->fmt.pix.height = rounddown(
			clamp_t(u32, f->fmt.pix.height - padding_h,
				ATOM_ISP_MIN_HEIGHT,
				ATOM_ISP_MAX_HEIGHT), ATOM_ISP_STEP_HEIGHT);
	}

	atomisp_get_dis_envelop(asd, f->fmt.pix.width, f->fmt.pix.height,
				&dvs_env_w, &dvs_env_h);

	if (asd->continuous_mode->val) {
		struct v4l2_rect *r;

		r = atomisp_subdev_get_rect(
			&asd->subdev, NULL,
			V4L2_SUBDEV_FORMAT_ACTIVE,
			ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE,
			V4L2_SEL_TGT_COMPOSE);
		/*
		 * The ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE should get resolutions
		 * properly set otherwise, it should not be the capture_pad.
		 */
		if (r->width && r->height)
			asd->capture_pad = ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE;
		else
			asd->capture_pad = source_pad;
	} else {
		asd->capture_pad = source_pad;
	}
	/*
	 * set format info to sensor
	 * In continuous mode, resolution is set only if it is higher than
	 * existing value. This because preview pipe will be configured after
	 * capture pipe and usually has lower resolution than capture pipe.
	 */
	if (!asd->continuous_mode->val ||
	    isp_sink_fmt.width < (f->fmt.pix.width + padding_w + dvs_env_w) ||
	     isp_sink_fmt.height < (f->fmt.pix.height + padding_h +
				    dvs_env_h)) {
		/*
		 * For jpeg or custom raw format the sensor will return constant
		 * width and height. Because we already had quried try_mbus_fmt,
		 * f->fmt.pix.width and f->fmt.pix.height has been changed to
		 * this fixed width and height. So we cannot select the correct
		 * resolution with that information. So use the original width
		 * and height while set_mbus_fmt() so actual resolutions are
		 * being used in while set media bus format.
		 */
		s_fmt = *f;
		if (f->fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG ||
		    f->fmt.pix.pixelformat == V4L2_PIX_FMT_CUSTOM_M10MO_RAW) {
			s_fmt.fmt.pix.width = backup_fmt.fmt.pix.width;
			s_fmt.fmt.pix.height = backup_fmt.fmt.pix.height;
		}
		ret = atomisp_set_fmt_to_snr(vdev, &s_fmt,
					f->fmt.pix.pixelformat, padding_w,
					padding_h, dvs_env_w, dvs_env_h);
		if (ret)
			return -EINVAL;

		atomisp_csi_lane_config(isp);
		crop_needs_override = true;
	}

	atomisp_check_copy_mode(asd, source_pad);
	asd->yuvpp_mode = false;			/* Reset variable */

	isp_sink_crop = *atomisp_subdev_get_rect(&asd->subdev, NULL,
						 V4L2_SUBDEV_FORMAT_ACTIVE,
						 ATOMISP_SUBDEV_PAD_SINK,
						 V4L2_SEL_TGT_CROP);

	/* Try to enable YUV downscaling if ISP input is 10 % (either
	 * width or height) bigger than the desired result. */
	if (isp_sink_crop.width * 9 / 10 < f->fmt.pix.width ||
	    isp_sink_crop.height * 9 / 10 < f->fmt.pix.height ||
	    (atomisp_subdev_format_conversion(asd, source_pad) &&
	     ((asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO &&
	       !asd->continuous_mode->val) ||
	      asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER))) {
		/* for continuous mode, preview size might be smaller than
		 * still capture size. if preview size still needs crop,
		 * pick the larger one between crop size of preview and
		 * still capture.
		 */
		if (asd->continuous_mode->val
		    && source_pad == ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW
		    && !crop_needs_override) {
			isp_sink_crop.width =
				max_t(unsigned int, f->fmt.pix.width,
				      isp_sink_crop.width);
			isp_sink_crop.height =
				max_t(unsigned int, f->fmt.pix.height,
				      isp_sink_crop.height);
		} else {
			isp_sink_crop.width = f->fmt.pix.width;
			isp_sink_crop.height = f->fmt.pix.height;
		}

		atomisp_subdev_set_selection(&asd->subdev, &fh,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     ATOMISP_SUBDEV_PAD_SINK,
					     V4L2_SEL_TGT_CROP,
					     V4L2_SEL_FLAG_KEEP_CONFIG,
					     &isp_sink_crop);
		atomisp_subdev_set_selection(&asd->subdev, &fh,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     source_pad, V4L2_SEL_TGT_COMPOSE,
					     0, &isp_sink_crop);
	} else if (IS_MOFD) {
		struct v4l2_rect main_compose = {0};

		main_compose.width = isp_sink_crop.width;
		main_compose.height =
			DIV_ROUND_UP(main_compose.width * f->fmt.pix.height,
				     f->fmt.pix.width);
		if (main_compose.height > isp_sink_crop.height) {
			main_compose.height = isp_sink_crop.height;
			main_compose.width =
				DIV_ROUND_UP(main_compose.height *
					     f->fmt.pix.width,
					     f->fmt.pix.height);
		}

		atomisp_subdev_set_selection(&asd->subdev, &fh,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				source_pad,
				V4L2_SEL_TGT_COMPOSE, 0,
				&main_compose);
	} else {
		struct v4l2_rect sink_crop = {0};
		struct v4l2_rect main_compose = {0};

		main_compose.width = f->fmt.pix.width;
		main_compose.height = f->fmt.pix.height;

		if (crop_needs_override) {
			if (isp_sink_crop.width * main_compose.height >
			    isp_sink_crop.height * main_compose.width) {
				sink_crop.height = isp_sink_crop.height;
				sink_crop.width = DIV_NEAREST_STEP(
						sink_crop.height *
						f->fmt.pix.width,
						f->fmt.pix.height,
						ATOM_ISP_STEP_WIDTH);
			} else {
				sink_crop.width = isp_sink_crop.width;
				sink_crop.height = DIV_NEAREST_STEP(
						sink_crop.width *
						f->fmt.pix.height,
						f->fmt.pix.width,
						ATOM_ISP_STEP_HEIGHT);
			}
			atomisp_subdev_set_selection(&asd->subdev, &fh,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK,
				V4L2_SEL_TGT_CROP,
				V4L2_SEL_FLAG_KEEP_CONFIG,
				&sink_crop);
		}
		atomisp_subdev_set_selection(&asd->subdev, &fh,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				source_pad,
				V4L2_SEL_TGT_COMPOSE, 0,
				&main_compose);
	}

set_fmt_to_isp:
	ret = atomisp_set_fmt_to_isp(vdev, &output_info, &raw_output_info,
				     &f->fmt.pix, source_pad);
	if (ret)
		return -EINVAL;
done:
	pipe->pix.width = f->fmt.pix.width;
	pipe->pix.height = f->fmt.pix.height;
	pipe->pix.pixelformat = f->fmt.pix.pixelformat;
	if (format_bridge->planar) {
		pipe->pix.bytesperline = output_info.padded_width;
		pipe->pix.sizeimage = PAGE_ALIGN(f->fmt.pix.height *
			DIV_ROUND_UP(format_bridge->depth *
				     output_info.padded_width, 8));
	} else {
		pipe->pix.bytesperline =
			DIV_ROUND_UP(format_bridge->depth *
				     output_info.padded_width, 8);
		pipe->pix.sizeimage =
			PAGE_ALIGN(f->fmt.pix.height * pipe->pix.bytesperline);

	}
	if (f->fmt.pix.field == V4L2_FIELD_ANY)
		f->fmt.pix.field = V4L2_FIELD_NONE;
	pipe->pix.field = f->fmt.pix.field;

	f->fmt.pix = pipe->pix;
	f->fmt.pix.priv = PAGE_ALIGN(pipe->pix.width *
				     pipe->pix.height * 2);

	pipe->capq.field = f->fmt.pix.field;

	/*
	 * If in video 480P case, no GFX throttle
	 */
	if (asd->run_mode->val == ATOMISP_SUBDEV_PAD_SOURCE_VIDEO &&
	    f->fmt.pix.width == 720 && f->fmt.pix.height == 480)
		isp->need_gfx_throttle = false;
	else
		isp->need_gfx_throttle = true;

	return 0;
}

int atomisp_set_fmt_file(struct video_device *vdev, struct v4l2_format *f)
{
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct v4l2_mbus_framefmt ffmt = {0};
	const struct atomisp_format_bridge *format_bridge;
	struct v4l2_subdev_fh fh;
	int ret;

	v4l2_fh_init(&fh.vfh, vdev);

	dev_dbg(isp->dev, "setting fmt %ux%u 0x%x for file inject\n",
		f->fmt.pix.width, f->fmt.pix.height, f->fmt.pix.pixelformat);
	ret = atomisp_try_fmt_file(isp, f);
	if (ret) {
		dev_err(isp->dev, "atomisp_try_fmt_file err: %d\n", ret);
		return ret;
	}

	format_bridge = atomisp_get_format_bridge(f->fmt.pix.pixelformat);
	if (format_bridge == NULL) {
		dev_dbg(isp->dev, "atomisp_get_format_bridge err! fmt:0x%x\n",
				f->fmt.pix.pixelformat);
		return -EINVAL;
	}

	pipe->pix = f->fmt.pix;
	atomisp_css_input_set_mode(asd, CSS_INPUT_MODE_FIFO);
	atomisp_css_input_configure_port(asd,
		__get_mipi_port(isp, ATOMISP_CAMERA_PORT_PRIMARY), 2, 0xffff4,
		0, 0, 0, 0);
	ffmt.width = f->fmt.pix.width;
	ffmt.height = f->fmt.pix.height;
	ffmt.code = format_bridge->mbus_code;

	atomisp_subdev_set_ffmt(&asd->subdev, &fh, V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK, &ffmt);

	return 0;
}

int atomisp_set_shading_table(struct atomisp_sub_device *asd,
		struct atomisp_shading_table *user_shading_table)
{
	struct atomisp_css_shading_table *shading_table;
	struct atomisp_css_shading_table *free_table;
	unsigned int len_table;
	int i;
	int ret = 0;

	if (!user_shading_table)
		return -EINVAL;

	if (!user_shading_table->enable) {
		atomisp_css_set_shading_table(asd, NULL);
		asd->params.sc_en = 0;
		return 0;
	}

	/* If enabling, all tables must be set */
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (!user_shading_table->data[i])
			return -EINVAL;
	}

	/* Shading table size per color */
	if (user_shading_table->width > SH_CSS_MAX_SCTBL_WIDTH_PER_COLOR ||
	    user_shading_table->height > SH_CSS_MAX_SCTBL_HEIGHT_PER_COLOR)
		return -EINVAL;

	shading_table = atomisp_css_shading_table_alloc(
			user_shading_table->width, user_shading_table->height);
	if (!shading_table)
		return -ENOMEM;

	len_table = user_shading_table->width * user_shading_table->height *
		    ATOMISP_SC_TYPE_SIZE;
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		ret = copy_from_user(shading_table->data[i],
				     user_shading_table->data[i], len_table);
		if (ret) {
			free_table = shading_table;
			ret = -EFAULT;
			goto out;
		}
	}
	shading_table->sensor_width = user_shading_table->sensor_width;
	shading_table->sensor_height = user_shading_table->sensor_height;
	shading_table->fraction_bits = user_shading_table->fraction_bits;

	free_table = asd->params.css_param.shading_table;
	asd->params.css_param.shading_table = shading_table;
	atomisp_css_set_shading_table(asd, shading_table);
	asd->params.sc_en = 1;

out:
	if (free_table != NULL)
		atomisp_css_shading_table_free(free_table);

	return ret;
}

/*Turn off ISP dphy */
int atomisp_ospm_dphy_down(struct atomisp_device *isp)
{
	unsigned long flags;
	u32 reg;

	dev_dbg(isp->dev, "%s\n", __func__);

	/* if ISP timeout, we can force powerdown */
	if (isp->isp_timeout)
		goto done;

	if (!atomisp_dev_users(isp))
		goto done;

	spin_lock_irqsave(&isp->lock, flags);
	isp->sw_contex.power_state = ATOM_ISP_POWER_DOWN;
	spin_unlock_irqrestore(&isp->lock, flags);
done:
	/*
	 * MRFLD IUNIT DPHY is located in an always-power-on island
	 * MRFLD HW design need all CSI ports are disabled before
	 * powering down the IUNIT.
	 */
	pci_read_config_dword(isp->pdev, MRFLD_PCI_CSI_CONTROL, &reg);
	reg |= MRFLD_ALL_CSI_PORTS_OFF_MASK;
	pci_write_config_dword(isp->pdev, MRFLD_PCI_CSI_CONTROL, reg);
	return 0;
}

/*Turn on ISP dphy */
int atomisp_ospm_dphy_up(struct atomisp_device *isp)
{
	unsigned long flags;
	dev_dbg(isp->dev, "%s\n", __func__);

	spin_lock_irqsave(&isp->lock, flags);
	isp->sw_contex.power_state = ATOM_ISP_POWER_UP;
	spin_unlock_irqrestore(&isp->lock, flags);

	return 0;
}


int atomisp_exif_makernote(struct atomisp_sub_device *asd,
			   struct atomisp_makernote_info *config)
{
	struct v4l2_control ctrl;
	struct atomisp_device *isp = asd->isp;

	ctrl.id = V4L2_CID_FOCAL_ABSOLUTE;
	if (v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				 core, g_ctrl, &ctrl)) {
		dev_warn(isp->dev, "failed to g_ctrl for focal length\n");
		return -EINVAL;
	} else {
		config->focal_length = ctrl.value;
	}

	ctrl.id = V4L2_CID_FNUMBER_ABSOLUTE;
	if (v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				core, g_ctrl, &ctrl)) {
		dev_warn(isp->dev, "failed to g_ctrl for f-number\n");
		return -EINVAL;
	} else {
		config->f_number_curr = ctrl.value;
	}

	ctrl.id = V4L2_CID_FNUMBER_RANGE;
	if (v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				core, g_ctrl, &ctrl)) {
		dev_warn(isp->dev, "failed to g_ctrl for f number range\n");
		return -EINVAL;
	} else {
		config->f_number_range = ctrl.value;
	}

	return 0;
}

int atomisp_offline_capture_configure(struct atomisp_sub_device *asd,
			      struct atomisp_cont_capture_conf *cvf_config)
{
	struct v4l2_ctrl *c;

	/*
	* In case of M10MO ZSL capture case, we need to issue a separate
	* capture request to M10MO which will output captured jpeg image
	*/
	c = v4l2_ctrl_find(
		asd->isp->inputs[asd->input_curr].camera->ctrl_handler,
		V4L2_CID_START_ZSL_CAPTURE);
	if (c) {
		int ret;
		dev_dbg(asd->isp->dev, "%s trigger ZSL capture request\n",
			__func__);
		/* TODO: use the cvf_config */
		ret = v4l2_ctrl_s_ctrl(c, 1);
		if (ret)
			return ret;

		return v4l2_ctrl_s_ctrl(c, 0);
	}

	asd->params.offline_parm = *cvf_config;

	if (asd->params.offline_parm.num_captures) {
		if (asd->streaming == ATOMISP_DEVICE_STREAMING_DISABLED) {
			unsigned int init_raw_num;

			if (asd->enable_raw_buffer_lock->val) {
				init_raw_num =
					ATOMISP_CSS2_NUM_OFFLINE_INIT_CONTINUOUS_FRAMES_LOCK_EN;
				if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO &&
				    asd->params.video_dis_en)
					init_raw_num +=
					    ATOMISP_CSS2_NUM_DVS_FRAME_DELAY;
			} else {
				init_raw_num =
					ATOMISP_CSS2_NUM_OFFLINE_INIT_CONTINUOUS_FRAMES;
			}

			/* TODO: this can be removed once user-space
			 *       has been updated to use control API */
			asd->continuous_raw_buffer_size->val =
				max_t(int,
				      asd->continuous_raw_buffer_size->val,
				      asd->params.offline_parm.
				      num_captures + init_raw_num);
			asd->continuous_raw_buffer_size->val =
				min_t(int, ATOMISP_CONT_RAW_FRAMES,
				      asd->continuous_raw_buffer_size->val);
		}
		asd->continuous_mode->val = true;
	} else {
		asd->continuous_mode->val = false;
		__enable_continuous_mode(asd, false);
	}

	return 0;
}

/*
 * set auto exposure metering window to camera sensor
 */
int atomisp_s_ae_window(struct atomisp_sub_device *asd,
			struct atomisp_ae_window *arg)
{
	struct atomisp_device *isp = asd->isp;
	struct v4l2_subdev_selection sel;

	sel.r.left = arg->x_left;
	sel.r.top = arg->y_top;
	sel.r.width = arg->x_right - arg->x_left + 1;
	sel.r.height = arg->y_bottom - arg->y_top + 1;

	if (v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				 pad, set_selection, NULL, &sel)) {
		dev_err(isp->dev, "failed to call sensor set_selection.\n");
		return -EINVAL;
	}

	return 0;
}

int atomisp_flash_enable(struct atomisp_sub_device *asd, int num_frames)
{
	struct atomisp_device *isp = asd->isp;

	if (num_frames < 0) {
		dev_dbg(isp->dev, "%s ERROR: num_frames: %d\n", __func__,
				num_frames);
		return -EINVAL;
	}
	/* a requested flash is still in progress. */
	if (num_frames && asd->params.flash_state != ATOMISP_FLASH_IDLE) {
		dev_dbg(isp->dev, "%s flash busy: %d frames left: %d\n",
				__func__, asd->params.flash_state,
				asd->params.num_flash_frames);
		return -EBUSY;
	}

	asd->params.num_flash_frames = num_frames;
	asd->params.flash_state = ATOMISP_FLASH_REQUESTED;
	return 0;
}

int atomisp_source_pad_to_stream_id(struct atomisp_sub_device *asd,
					   uint16_t source_pad)
{
	int stream_id;
	struct atomisp_device *isp = asd->isp;

	if (isp->inputs[asd->input_curr].camera_caps->
			sensor[asd->sensor_curr].stream_num == 1)
		return ATOMISP_INPUT_STREAM_GENERAL;

	switch (source_pad) {
	case ATOMISP_SUBDEV_PAD_SOURCE_CAPTURE:
		stream_id = ATOMISP_INPUT_STREAM_CAPTURE;
		break;
	case ATOMISP_SUBDEV_PAD_SOURCE_VF:
		stream_id = ATOMISP_INPUT_STREAM_POSTVIEW;
		break;
	case ATOMISP_SUBDEV_PAD_SOURCE_PREVIEW:
		stream_id = ATOMISP_INPUT_STREAM_PREVIEW;
		break;
	case ATOMISP_SUBDEV_PAD_SOURCE_VIDEO:
		stream_id = ATOMISP_INPUT_STREAM_VIDEO;
		break;
	default:
		stream_id = ATOMISP_INPUT_STREAM_GENERAL;
	}

	return stream_id;
}

bool atomisp_is_vf_pipe(struct atomisp_video_pipe *pipe)
{
	struct atomisp_sub_device *asd = pipe->asd;

	if (pipe == &asd->video_out_vf)
		return true;

	if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO &&
	    pipe == &asd->video_out_preview)
		return true;

	return false;
}

static int __checking_exp_id(struct atomisp_sub_device *asd, int exp_id)
{
	struct atomisp_device *isp = asd->isp;

	if (!asd->enable_raw_buffer_lock->val) {
		dev_warn(isp->dev, "%s Raw Buffer Lock is disable.\n", __func__);
		return -EINVAL;
	}
	if (asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED) {
		dev_err(isp->dev, "%s streaming %d invalid exp_id %d.\n",
			__func__, exp_id, asd->streaming);
		return -EINVAL;
	}
	if ((exp_id > ATOMISP_MAX_EXP_ID) || (exp_id <= 0)) {
		dev_err(isp->dev, "%s exp_id %d invalid.\n", __func__, exp_id);
		return -EINVAL;
	}
	return 0;
}

void atomisp_init_raw_buffer_bitmap(struct atomisp_sub_device *asd)
{
	unsigned long flags;
	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	memset(asd->raw_buffer_bitmap, 0, sizeof(asd->raw_buffer_bitmap));
	asd->raw_buffer_locked_count = 0;
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);
}

int atomisp_set_raw_buffer_bitmap(struct atomisp_sub_device *asd, int exp_id)
{
	int *bitmap, bit;
	unsigned long flags;

	if (__checking_exp_id(asd, exp_id))
		return -EINVAL;

	bitmap = asd->raw_buffer_bitmap + exp_id / 32;
	bit = exp_id % 32;
	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	(*bitmap) |= (1 << bit);
	asd->raw_buffer_locked_count++;
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);

	dev_dbg(asd->isp->dev, "%s: exp_id %d,  raw_buffer_locked_count %d\n",
		__func__, exp_id, asd->raw_buffer_locked_count);

	/* Check if the raw buffer after next is still locked!!! */
	exp_id += 2;
	if (exp_id > ATOMISP_MAX_EXP_ID)
		exp_id -= ATOMISP_MAX_EXP_ID;
	bitmap = asd->raw_buffer_bitmap + exp_id / 32;
	bit = exp_id % 32;
	if ((*bitmap) & (1 << bit)) {
		int ret;

		/* WORKAROUND unlock the raw buffer compulsively */
		ret = atomisp_css_exp_id_unlock(asd, exp_id);
		if (ret) {
			dev_err(asd->isp->dev, "%s exp_id is wrapping back to %d but force unlock failed,, err %d.\n",
				__func__, exp_id, ret);
			return ret;
		}

		spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
		(*bitmap) &= ~(1 << bit);
		asd->raw_buffer_locked_count--;
		spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);
		dev_warn(asd->isp->dev, "%s exp_id is wrapping back to %d but it is still locked so force unlock it, raw_buffer_locked_count %d\n",
			__func__, exp_id, asd->raw_buffer_locked_count);
	}
	return 0;
}

static int __is_raw_buffer_locked(struct atomisp_sub_device *asd, int exp_id)
{
	int *bitmap, bit;
	unsigned long flags;
	int ret;

	if (__checking_exp_id(asd, exp_id))
		return -EINVAL;

	bitmap = asd->raw_buffer_bitmap + exp_id / 32;
	bit = exp_id % 32;
	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	ret = ((*bitmap) & (1 << bit));
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);
	return !ret;
}

static int __clear_raw_buffer_bitmap(struct atomisp_sub_device *asd, int exp_id)
{
	int *bitmap, bit;
	unsigned long flags;

	if (__is_raw_buffer_locked(asd, exp_id))
		return -EINVAL;

	bitmap = asd->raw_buffer_bitmap + exp_id / 32;
	bit = exp_id % 32;
	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	(*bitmap) &= ~(1 << bit);
	asd->raw_buffer_locked_count--;
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);

	dev_dbg(asd->isp->dev, "%s: exp_id %d,  raw_buffer_locked_count %d\n",
		__func__, exp_id, asd->raw_buffer_locked_count);
	return 0;
}

int atomisp_exp_id_capture(struct atomisp_sub_device *asd, int *exp_id)
{
	struct atomisp_device *isp = asd->isp;
	int value = *exp_id;
	int ret;

	ret = __is_raw_buffer_locked(asd, value);
	if (ret) {
		dev_err(isp->dev, "%s exp_id %d invalid %d.\n", __func__, value, ret);
		return -EINVAL;
	}

	dev_dbg(isp->dev, "%s exp_id %d\n", __func__, value);
	ret = atomisp_css_exp_id_capture(asd, value);
	if (ret) {
		dev_err(isp->dev, "%s exp_id %d failed.\n", __func__, value);
		return -EIO;
	}
	return 0;
}

int atomisp_exp_id_unlock(struct atomisp_sub_device *asd, int *exp_id)
{
	struct atomisp_device *isp = asd->isp;
	int value = *exp_id;
	int ret;

	ret = __clear_raw_buffer_bitmap(asd, value);
	if (ret) {
		dev_err(isp->dev, "%s exp_id %d invalid %d.\n", __func__, value, ret);
		return -EINVAL;
	}

	dev_dbg(isp->dev, "%s exp_id %d\n", __func__, value);
	ret = atomisp_css_exp_id_unlock(asd, value);
	if (ret)
		dev_err(isp->dev, "%s exp_id %d failed, err %d.\n",
			__func__, value, ret);

	return ret;
}

int atomisp_enable_dz_capt_pipe(struct atomisp_sub_device *asd,
					   unsigned int *enable)
{
	bool value;

	if (enable == NULL)
		return -EINVAL;

	value = *enable > 0 ? true : false;

	atomisp_en_dz_capt_pipe(asd, value);

	return 0;
}

int atomisp_inject_a_fake_event(struct atomisp_sub_device *asd, int *event)
{
	if (!event || asd->streaming != ATOMISP_DEVICE_STREAMING_ENABLED)
		return -EINVAL;

	dev_dbg(asd->isp->dev, "%s: trying to inject a fake event 0x%x\n",
		__func__, *event);

	switch (*event) {
	case V4L2_EVENT_FRAME_SYNC:
		atomisp_sof_event(asd);
		break;
	case V4L2_EVENT_FRAME_END:
		atomisp_eof_event(asd, 0);
		break;
	case V4L2_EVENT_ATOMISP_3A_STATS_READY:
		atomisp_3a_stats_ready_event(asd, 0);
		break;
	case V4L2_EVENT_ATOMISP_METADATA_READY:
		atomisp_metadata_ready_event(asd, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int atomisp_get_pipe_id(struct atomisp_video_pipe *pipe)
{
	struct atomisp_sub_device *asd = pipe->asd;

	if (ATOMISP_USE_YUVPP(asd))
		return CSS_PIPE_ID_YUVPP;
	else if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER)
		return CSS_PIPE_ID_VIDEO;
	else if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_LOWLAT)
		return CSS_PIPE_ID_CAPTURE;
	else if (pipe == &asd->video_out_video_capture)
		return CSS_PIPE_ID_VIDEO;
	else if (pipe == &asd->video_out_vf)
		return CSS_PIPE_ID_CAPTURE;
	else if (pipe == &asd->video_out_preview) {
		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO)
			return CSS_PIPE_ID_VIDEO;
		else
			return CSS_PIPE_ID_PREVIEW;
	} else if (pipe == &asd->video_out_capture) {
		if (asd->copy_mode && !asd->copy_mode_format_conv)
			return IA_CSS_PIPE_ID_COPY;
		else
			return CSS_PIPE_ID_CAPTURE;
	}

	/* fail through */
	dev_warn(asd->isp->dev, "%s failed to find proper pipe\n",
		__func__);
	return CSS_PIPE_ID_CAPTURE;
}

int atomisp_get_invalid_frame_num(struct video_device *vdev,
					int *invalid_frame_num)
{
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	enum atomisp_css_pipe_id pipe_id;
	struct ia_css_pipe_info p_info;
	int ret;

	if (asd->isp->inputs[asd->input_curr].camera_caps->
		sensor[asd->sensor_curr].stream_num > 1) {
		/* External ISP */
		*invalid_frame_num = 0;
		return 0;
	}

	pipe_id = atomisp_get_pipe_id(pipe);
	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].pipes[pipe_id]) {
		dev_warn(asd->isp->dev, "%s pipe %d has not been created yet, do SET_FMT first!\n",
			__func__, pipe_id);
		return -EINVAL;
	}

	ret = ia_css_pipe_get_info(
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		.pipes[pipe_id], &p_info);
	if (ret == IA_CSS_SUCCESS) {
		*invalid_frame_num = p_info.num_invalid_frames;
		return 0;
	} else {
		dev_warn(asd->isp->dev, "%s get pipe infor failed %d\n",
			 __func__, ret);
		return -EINVAL;
	}
}
