#include <linux/sysfs.h>
#include <linux/m10mo.h>
#include <media/m10mo_atomisp.h>

#ifndef ZX551ML_USER_BUILD //#ifdef CONFIG_ASUS_FACTORY_MODE
static u32 password_debug;
static bool CacCalibrationStatus = false;
#endif

static bool isFlashFwFail = false;

static int dump_fw(struct m10mo_device *dev)
{
	int ret = 0;
	mutex_lock(&dev->input_lock);
	if (dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	__m10mo_s_power(&dev->sd, 1, true);
	m10mo_dump_fw(dev);
	__m10mo_s_power(&dev->sd, 0, true);
leave:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int dump_cali_data(struct m10mo_device *dev)
{
	int ret = 0;
	mutex_lock(&dev->input_lock);
	if (dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	__m10mo_s_power(&dev->sd, 1, true);
	m10mo_dump_cali_data(dev);
	__m10mo_s_power(&dev->sd, 0, true);
leave:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int read_fw_checksum(struct m10mo_device *dev, u16 *result)
{
	int ret;

	mutex_lock(&dev->input_lock);
	if (dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	__m10mo_s_power(&dev->sd, 1, true);
	ret = m10mo_fw_checksum(dev, result);
	__m10mo_s_power(&dev->sd, 0, true);
leave:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int read_fw_version(struct m10mo_device *dev, char *buf)
{
	int ret;

	mutex_lock(&dev->input_lock);
	if (dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	__m10mo_s_power(&dev->sd, 1, true);
	ret = m10mo_get_isp_fw_version_string(dev, buf, M10MO_MAX_FW_ID_STRING,
			dev->fw_addr_id);
	__m10mo_s_power(&dev->sd, 0, true);
leave:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int update_fw(struct m10mo_device *dev, int erase_all)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	int ret = 0;
	u16 result;

	mutex_lock(&dev->input_lock);
	if (dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	__m10mo_s_power(&dev->sd, 1, true);
	if(erase_all == 0)
		ret = m10mo_program_device(dev, 0, (fw_version_val1 << 8 |  fw_version_val2), DIT_version);
	else if(erase_all == 1)
		ret = m10mo_program_device_erase_all_flash_partial(dev);
	else if(erase_all == 2)
		ret = m10mo_program_device_erase_all_flash_all(dev);
	else{
		__m10mo_s_power(&dev->sd, 0, true);
		goto leave;
	}
	__m10mo_s_power(&dev->sd, 0, true);
	if (ret)
		goto leave;
	/* Power cycle chip and re-identify the version */
	__m10mo_s_power(&dev->sd, 1, true);
	ret = m10mo_identify_fw_type(&dev->sd);
	__m10mo_s_power(&dev->sd, 0, true);
	if (ret)
		goto leave;

	__m10mo_s_power(&dev->sd, 1, true);
	ret = m10mo_fw_checksum(dev, &result);
	__m10mo_s_power(&dev->sd, 0, true);
	dev_info(&client->dev, "m10mo FW checksum: %d\n", result);

leave:
	if (ret || result!=0) isFlashFwFail = true;
	else isFlashFwFail = false;

	mutex_unlock(&dev->input_lock);
	return ret;
}

static ssize_t m10mo_flash_rom_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "flash\n");
}

static ssize_t m10mo_flash_rom_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t len)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	int ret=0;
	wake_lock_timeout(&m10mo_dev->m10mo_update_wake_lock, msecs_to_jiffies(180000));
	if (!strncmp(buf, "flash", 5) || !strncmp(buf, "eraseall_flash", 14) || !strncmp(buf, "erase_flash_all", 15)) {

	    m10mo_USB_status(1);

	    if (!strncmp(buf, "flash", 5)){
		printk("m10mo flash\n");
		ret=update_fw(m10mo_dev, 0);
	    }else if (!strncmp(buf, "eraseall_flash", 14)){
		printk("m10mo eraseall_flash\n");
		ret=update_fw(m10mo_dev, 1);
	    }else if (!strncmp(buf, "erase_flash_all", 15)){
		printk("m10mo erase_flash_all\n");
		ret=update_fw(m10mo_dev, 2);
	    }else{
		printk("m10mo EINVAL\n");
		wake_unlock(&m10mo_dev->m10mo_update_wake_lock);
		return -EINVAL;
	    }
	    msleep(200);
	    if(!ret){

		m10mo_s_power_fac(1);
		DIT_version = writeDummyVersion(m10mo_dev);
		m10mo_s_power_fac(0);

		mdelay(200);
		__m10mo_s_power(sdd, 1, false);

		m10mo_set_zsl_monitor(sdd);
		msleep(3000);
		__m10mo_s_power(sdd, 0, false);

		//read fw version
		msleep(200);
		m10mo_s_power_fac(1);
		DIT_version = readDitVersion(m10mo_dev);
		m10mo_s_power_fac(0);
	    }
		m10mo_USB_status(0);
		wake_unlock(&m10mo_dev->m10mo_update_wake_lock);
		return len;
	}
    	wake_unlock(&m10mo_dev->m10mo_update_wake_lock);
	return -EINVAL;
}
static DEVICE_ATTR(isp_flashfw, S_IRUGO | S_IWUSR, m10mo_flash_rom_show,
		   m10mo_flash_rom_store);

static ssize_t m10mo_flash_spi_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE,
			 "%d\n", m10mo_get_spi_state(m10mo_dev));
}

static ssize_t m10mo_flash_spi_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t len)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	unsigned long value;
	int ret;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	ret = m10mo_set_spi_state(m10mo_dev, value);
	if (ret)
		return ret;
	return len;
}
static DEVICE_ATTR(isp_spi, S_IRUGO | S_IWUSR, m10mo_flash_spi_show,
		   m10mo_flash_spi_store);

static ssize_t m10mo_flash_checksum_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10_dev = dev_get_drvdata(dev);
	ssize_t ret;
	u16 result;

	ret  = read_fw_checksum(m10_dev, &result);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%04x\n", result);
}
static DEVICE_ATTR(isp_checksum, S_IRUGO, m10mo_flash_checksum_show, NULL);

static ssize_t m10mo_flash_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10_dev = dev_get_drvdata(dev);
	ssize_t ret;

	ret  = read_fw_version(m10_dev, buf);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%s\n", buf);
}
static DEVICE_ATTR(isp_fw_id, S_IRUGO, m10mo_flash_version_show, NULL);


static ssize_t m10mo_flash_customer_project_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10_dev = dev_get_drvdata(dev);
	int ret=0;

	mutex_lock(&m10_dev->input_lock);
	if (m10_dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	m10mo_s_power_fac(1);

	ret  = m10mo_detect(&m10_dev->sd);
	if (ret){
		m10mo_s_power_fac(0);
		goto leave;
	}
	m10mo_s_power_fac(0);

leave:
	mutex_unlock(&m10_dev->input_lock);

	return scnprintf(buf, PAGE_SIZE, "%s\n", buf);
}
static DEVICE_ATTR(isp_fw_customer_project, S_IRUGO, m10mo_flash_customer_project_show, NULL);


static ssize_t m10mo_flash_dump_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10_dev = dev_get_drvdata(dev);
	dump_fw(m10_dev);
	return scnprintf(buf, PAGE_SIZE, "done\n");
}
static DEVICE_ATTR(isp_fw_dump, S_IRUGO, m10mo_flash_dump_show, NULL);

static ssize_t m10mo_flash_cali_data_dump_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10_dev = dev_get_drvdata(dev);
	dump_cali_data(m10_dev);
	return scnprintf(buf, PAGE_SIZE, "done\n");
}
static DEVICE_ATTR(isp_cali_data_dump, S_IRUGO, m10mo_flash_cali_data_dump_show, NULL);

static ssize_t m10mo_debug(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 val=0;

	sscanf(buf, "%x", &val);
	m10mo_break_log_loop = val;

	return len;
}
static DEVICE_ATTR(isp_fw_debug, S_IRUGO | S_IWUSR, NULL, m10mo_debug);

static ssize_t m10mo_read_Flag(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	int ret= 0;
	mutex_lock(&m10mo_dev->input_lock);
    	(void) m10mo_s_power_fac(1);
	ASUS_flag = ret = readFwForceUpdateFlag(m10mo_dev);
    	(void) m10mo_s_power_fac(0);
	mutex_unlock(&m10mo_dev->input_lock);
	return scnprintf(buf, PAGE_SIZE, "\n\
		flag is %x\n", ret);
}


static ssize_t m10mo_write_Flag(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	int ret=0, res=0, timeout=5000;
    	u32 val=0;

	sscanf(buf, "%x", &val);

	mutex_lock(&m10mo_dev->input_lock);
    	(void) m10mo_s_power_fac(1);

	(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_OFFSET_H, 0xf4);


	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, val);

	if(ret == 1){
		pr_err("timeout while waiting for chip op to finish\n");
		return 0xffff;
	}
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_MODE, 0x09);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_FLASH_MODE, 0x03);

	do {
		msleep(10);
		m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, SFLASH_SPI_STATUS, &res);
	} while ((res != 0) && --timeout);

	if (!timeout) {
		pr_err("timeout while waiting for chip op to finish\n");
		return 0xffff;
	}
	printk("flash done\n");


    (void) m10mo_s_power_fac(0);
	mutex_unlock(&m10mo_dev->input_lock);


	return len;
}
static DEVICE_ATTR(isp_fw_default_flag, S_IRUGO | S_IWUSR, m10mo_read_Flag, m10mo_write_Flag);

static ssize_t m10mo_read_download_trace_log(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d", m10mo_trace_log_lock);
}


static ssize_t m10mo_write_download_trace_log(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    //=== Only tow kinds of value could be set in this file node. ===//
	u32 val;
	sscanf(buf, "%d", &val);
	if(val != DOWNLOAD_AVAILABLE
	     && val != DOWNLOAD_PROTECT) {
	    return len;
	}
	m10mo_trace_log_lock = val;

	return len;
}
static DEVICE_ATTR(m10mo_download_trace_log_when_time_out, S_IRUGO | S_IWUSR, m10mo_read_download_trace_log, m10mo_write_download_trace_log);

static ssize_t m10mo_read_asus_camera_flag(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d", asus_camera_flag);
}

static ssize_t m10mo_write_asus_camera_flag(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	u32 val;
	sscanf(buf, "%d", &val);
	asus_camera_flag = val;

	return len;
}
static DEVICE_ATTR(m10mo_asus_camera_flag, S_IRUGO, m10mo_read_asus_camera_flag, m10mo_write_asus_camera_flag);



static ssize_t m10mo_shading_table_checksum_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	mutex_lock(&m10mo_dev->input_lock);
	if (m10mo_dev->power == 1) {
		mutex_unlock(&m10mo_dev->input_lock);
		return -EBUSY;
	}

	__m10mo_s_power(&m10mo_dev->sd, 1, true);
	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, 0x69, &val);
	__m10mo_s_power(&m10mo_dev->sd, 0, true);

	if (ret) {
		mutex_unlock(&m10mo_dev->input_lock);
        return -EINVAL;
	}

	mutex_unlock(&m10mo_dev->input_lock);
        printk("@%s %d, shading table checksum = %x\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "shading table checksum = %x\n", val);
}

static DEVICE_ATTR(isp_shading_table_checksum, S_IRUGO | S_IWUSR, m10mo_shading_table_checksum_read, NULL);

#ifndef ZX551ML_USER_BUILD //#ifdef CONFIG_ASUS_FACTORY_MODE

static ssize_t m10mo_digital_zoom_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_MONITOR, DIGIT_ZOOM, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, digital zoom position = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "digital zoom position = %d\n", val);
}
static ssize_t m10mo_digital_zoom_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
    printk("@%s %d, digital zoom position %d\n", __func__, __LINE__, val);

    if(0x0 < val && val < 0x20 ){
        ret = m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_START_DIGITAL_ZOOM, &val);
        if (ret < 0)
            return ret;
        return len;
    }else{
	    	printk("@%s %d, wrong zoom position %d\n", __func__, __LINE__, val);
		return -EINVAL;
	}


}
static DEVICE_ATTR(isp_digital_zoom, S_IRUGO | S_IWUSR, m10mo_digital_zoom_read, m10mo_digital_zoom_write);

static ssize_t m10mo_optical_zoom_cancel_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_MONITOR, DIGIT_ZOOM, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, digital zoom position = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "digital zoom position = %d\n", val);
}

static ssize_t m10mo_optical_zoom_cancel_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
    printk("@%s %d, m10mo_optical_zoom_cancel_write: %d\n", __func__, __LINE__, val);

    ret = m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_ASUS_ZOOM_CANCEL, NULL);
    if (ret)
        return ret;

	return len;

}
static DEVICE_ATTR(m10mo_optical_zoom_cancel, S_IRUGO | S_IWUSR, m10mo_optical_zoom_cancel_read, m10mo_optical_zoom_cancel_write);

static ssize_t m10mo_auto_focus_cancel_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_MONITOR, DIGIT_ZOOM, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, digital zoom position = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "digital zoom position = %d\n", val);
}

static ssize_t m10mo_auto_focus_cancel_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
    printk("@%s %d, m10mo_auto_focus_cancel_write: %d\n", __func__, __LINE__, val);
    m10mo_dev->cmd = m10mo_dev->requested_cmd = M10MO_START_AF;
	wake_up(&m10mo_dev->irq_waitq);
    ret = m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_ASUS_FOCUS_CANCEL, NULL);
    if (ret)
        return ret;

	return len;

}
static DEVICE_ATTR(m10mo_auto_focus_cancel, S_IRUGO | S_IWUSR, m10mo_auto_focus_cancel_read, m10mo_auto_focus_cancel_write);

static ssize_t m10mo_i2c_debug_byte_write_cat(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	if(password_debug != 0x3296) return -1;

	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, zoom step = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}
static ssize_t m10mo_i2c_debug_byte_write_echo(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    int ret = 0;
    u8 cat, reg_addr;
	u32 val, reg_val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	if(password_debug != 0x3296) return -1;

	sscanf(buf, "%x", &val);
    reg_val  = val & 0xFF;
	reg_addr = (val >> 8) & 0xFF;
	cat      = (val >> 16) & 0xFF;
    printk(KERN_INFO "ASUSBSP --- @m10mo_i2c_debug_byte_write_echo, cat is 0x%x, reg_addr is 0x%x, reg_val is 0x%x \n",
                                  cat, reg_addr, reg_val);

    ret = m10mo_writeb(&m10mo_dev->sd, cat, reg_addr, reg_val);
    if (!ret){
	    printk(KERN_INFO "ASUSBSP --- @m10mo_i2c_debug_byte_write_echo \n");
        return len;
	}else{
	    return -EINVAL;
    }
}
static DEVICE_ATTR(m10mo_i2c_debug_byte_write_change_name_by_jevian, S_IRUGO | S_IWUSR, m10mo_i2c_debug_byte_write_cat, m10mo_i2c_debug_byte_write_echo);
static ssize_t m10mo_debug_state_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	printk("@%s %d, \n", __func__, __LINE__);
	return scnprintf(buf, PAGE_SIZE, "dev->requested_cmd is %d, dev->cmd is %d, m10mo_capture_pre_flag is %d \n", m10mo_dev->requested_cmd, m10mo_dev->cmd, m10mo_capture_pre_flag);
}

static ssize_t m10mo_debug_state_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%d", &val);
    m10mo_dev->cmd = val;

    return len;
}
static DEVICE_ATTR(m10mo_i2c_debug_state, S_IRUGO | S_IWUSR, m10mo_debug_state_read, m10mo_debug_state_write);

static ssize_t m10mo_i2c_debug_byte_read_cat(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, zoom step = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}
static ssize_t m10mo_i2c_debug_byte_read_echo(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    int ret = 0;
    u8 cat, reg_addr;
	u32 val;
	u32 reg_val = 0;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
//    reg_val  = val & 0xFF;
	reg_addr = (val >> 8) & 0xFF;
	cat      = (val >> 16) & 0xFF;
    printk(KERN_INFO "ASUSBSP --- @m10mo_i2c_debug_byte_read_echo, cat is 0x%x, reg_addr is 0x%x \n",
                                  cat, reg_addr);

    ret = m10mo_readb(&m10mo_dev->sd, cat, reg_addr, &reg_val);
    if (!ret){
	    printk(KERN_INFO "ASUSBSP --- @m10mo_i2c_debug_byte_read_echo, reg_val is 0x%x\n", reg_val);
        return len;
	}else{
	    return -EINVAL;
    }
}
static DEVICE_ATTR(m10mo_i2c_debug_byte_read_change_name_by_jevian, S_IRUGO | S_IWUSR, m10mo_i2c_debug_byte_read_cat, m10mo_i2c_debug_byte_read_echo);


static ssize_t m10mo_read_IQ_calibration_data(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	mutex_lock(&m10mo_dev->input_lock);
    (void) m10mo_s_power_fac(1);
    	(void)__m10mo_param_mode_set(&m10mo_dev->sd);
	m10mo_isp_fw_IQ_R(m10mo_dev);
    (void) m10mo_s_power_fac(0);
	mutex_unlock(&m10mo_dev->input_lock);
	return 0;
}


static ssize_t m10mo_write_IQ_calibration_data(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	int res=0, timeout=5000;

	mutex_lock(&m10mo_dev->input_lock);
    	(void) m10mo_s_power_fac(1);

    	(void)__m10mo_param_mode_set(&m10mo_dev->sd);

	m10mo_isp_fw_IQ_W(m10mo_dev);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_FLASH_MODE, 0x30);

	do {
		msleep(10);
		m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, SFLASH_SPI_STATUS, &res);
	} while ((res != 0) && --timeout);

	if (!timeout) {
		printk("timeout while waiting for chip op to finish\n");
		return len;
	}
	printk("flash done\n");

    	(void) m10mo_s_power_fac(0);
	mutex_unlock(&m10mo_dev->input_lock);

	return len;
}
static DEVICE_ATTR(isp_fw_iq_RW, S_IRUGO | S_IWUSR, m10mo_read_IQ_calibration_data, m10mo_write_IQ_calibration_data);



static ssize_t m10mo_asus_awb_control_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val1, reg_val2, ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	printk( "ASUS AWB balance CONTROL \n \
			Byte 71: AWB_DEBUG1 \n \
			Byte 72: AWB_DEBUG2 \n \
			Byte 73: AWB_DEBUG3 \n \
			Byte 74: AWB_DEBUG4 \n \
			Byte 75: AWB_DEBUG5 \n \
			Byte 76: AWB_DEBUG6 \n \
			Byte 77: AWB_DEBUG7 \n \
			Byte 78: AWB_DEBUG8 \n \
					\n");

	ret = m10mo_readl(&m10mo_dev->sd, CATEGORY_WB, REG_AWB_DEBUG1, &reg_val1);
        if(ret)
            	goto out;
	printk("@%s %d, REG_AWB_DEBUG1 ~ REG_AWB_DEBUG4 = %x\n", __func__, __LINE__, reg_val1);

	ret = m10mo_readl(&m10mo_dev->sd, CATEGORY_WB, REG_AWB_DEBUG5, &reg_val2);
        if(ret)
            	goto out;
	printk("@%s %d, REG_AWB_DEBUG5 ~ REG_AWB_DEBUG8 = %x\n", __func__, __LINE__, reg_val2);

	return scnprintf(buf, PAGE_SIZE, "\n\
		ASUS AWB balance CONTROL \n \
			REG_AWB_DEBUG1 ~ REG_AWB_DEBUG4 = %x\n \
			REG_AWB_DEBUG5 ~ REG_AWB_DEBUG8 = %x\n \
					\n\n\n ", reg_val1, reg_val2);
out:
	return -EINVAL;
}

static DEVICE_ATTR(isp_asus_awb_control, S_IRUGO | S_IWUSR, m10mo_asus_awb_control_read, NULL);


static ssize_t m10mo_asus_awb_control_read_set(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "\n\
		set ASUS AWB balance CONTROL read Byte\n");
}

static DEVICE_ATTR(isp_asus_awb_control_set, S_IRUGO | S_IWUSR, m10mo_asus_awb_control_read_set, NULL);


static ssize_t m10mo_exif_info_exptime_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val[2],ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
#if 0
Unit : numerator / denominator (sec)
*For Example

numerator = 600(0x0000258)
denominator = 20000(0x00004E20)
600 / 2000 = 0.03sec
0x0000000 -> 30ms
#endif
    	ret = m10mo_readl(&m10mo_dev->sd, CATEGORY_EXIF, EXIF_INFO_EXPTIME_NU, &reg_val[0]);
        if(ret)
            goto out;
    	ret = m10mo_readl(&m10mo_dev->sd, CATEGORY_EXIF, EXIF_INFO_EXPTIME_DE, &reg_val[1]);
        if(ret)
            goto out;

	printk( "exif_info_exptime.\n\
		 numerator = %x\n \
		 denominatorF = %x\n \
		exposure time = %x\n", reg_val[0],reg_val[1],reg_val[0]/reg_val[1]);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 exif_info_exptime.\n\
		 numerator = %x\n \
		 denominatorF = %x\n\
		 exposure time = 0x%x\n" , reg_val[0],reg_val[1],reg_val[0]/reg_val[1]);
out:
	return -EINVAL;
}


static DEVICE_ATTR(isp_exif_info_exptime, S_IRUGO | S_IWUSR, m10mo_exif_info_exptime_read, NULL);



static ssize_t m10mo_exif_info_iso_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

    	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_EXIF, EXIF_INFO_ISO, &reg_val);
        if(ret)
            goto out;

	printk( "exif_info_ISO = %x\n", reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 exif_info_ISO = %x\n" , reg_val);
out:
	return -EINVAL;
}


static DEVICE_ATTR(isp_exif_info_iso, S_IRUGO | S_IWUSR, m10mo_exif_info_iso_read, NULL);

static ssize_t m10mo_read_power_ctrl(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk("@%s %d, \n", __func__, __LINE__);
	return scnprintf(buf, PAGE_SIZE, "m10mo_boot_state is %d, power is %d, m10mo_trace_log_lock is %d \n", m10mo_boot_state, m10mo_dev->power, m10mo_trace_log_lock);
}


static ssize_t m10mo_write_power_ctrl(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	mutex_lock(&m10mo_dev->input_lock);
	sscanf(buf, "%d", &val);
    (void) m10mo_s_power_fac(val);
	mutex_unlock(&m10mo_dev->input_lock);

	return len;
}
static DEVICE_ATTR(m10mo_power_ctrl, S_IRUGO | S_IWUSR, m10mo_read_power_ctrl, m10mo_write_power_ctrl);

static ssize_t m10mo_debug_password_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
	printk("@%s %d, \n", __func__, __LINE__);
	return scnprintf(buf, PAGE_SIZE, "password_debug is 0x%x \n", password_debug);
}

static ssize_t m10mo_debug_password_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	sscanf(buf, "%x", &password_debug);
    return len;
}

static DEVICE_ATTR(m10mo_debug_password, S_IRUGO | S_IWUSR, m10mo_debug_password_read, m10mo_debug_password_write);

static ssize_t m10mo_optical_zoom_cur_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_MONITOR, OPTICAL_ZOOM_CR, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, optical zoom cur = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "optical zoom cur = %d\n", val);
}

static DEVICE_ATTR(isp_optical_zoom_cur, S_IRUGO | S_IWUSR, m10mo_optical_zoom_cur_read, NULL);

static ssize_t m10mo_optical_zoom_read(struct device *dev,	struct device_attribute *attr, char *buf)
{
    int ret = 0;
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_MONITOR, OPTICAL_ZOOM, &val);
	if (ret)
        return -EINVAL;

    printk("@%s %d, optical zoom position = %d\n", __func__, __LINE__, val);
	return scnprintf(buf, PAGE_SIZE, "optical zoom position = %d\n", val);
}
static ssize_t m10mo_optical_zoom_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	int ret = 0;
    	u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
    		if(val < 33 && val > 0){
        		ret = m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_START_OPTICAL_ZOOM, &val);
        		if (ret) return ret;
    		}else
		        return len;

        return len;
}
static DEVICE_ATTR(isp_optical_zoom, S_IRUGO | S_IWUSR, m10mo_optical_zoom_read, m10mo_optical_zoom_write);

static ssize_t m10mo_apk_capture_mode_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	val = m10mo_dev->capture_mode;
	printk("@%s %d, m10mo_dev->capture_mode = %d\n", __func__, __LINE__, val);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}
static ssize_t m10mo_apk_capture_mode_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	m10mo_dev->capture_mode = val;
    (void)__m10mo_param_mode_set(&m10mo_dev->sd);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_CAPTURE_CTRL, REQUEST_MULTI_CAP_FRAMES, val);
    (void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
    printk("@%s %d, m10mo_dev->capture_mode %d\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_apk_capture_mode, S_IRUGO | S_IWUSR, m10mo_apk_capture_mode_read, m10mo_apk_capture_mode_write);

static ssize_t m10mo_home_searching_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

    (void)__m10mo_param_mode_set(&m10mo_dev->sd);
    (void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_HEAD_HALF_HOME_SEARCHING_MODE, NULL);
    printk("@%s %d, home_searching done.\n", __func__, __LINE__);

	return len;
}
static DEVICE_ATTR(isp_home_searching, S_IRUGO | S_IWUSR, NULL, m10mo_home_searching_write);

static ssize_t m10mo_distirtion_mode_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val, ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_PARAM, DISTORTION, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, distortion_mode = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "distortion_mode = %x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_distirtion_mode_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

    	(void)__m10mo_param_mode_set(&m10mo_dev->sd);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_PARAM, DISTORTION, val);
    	(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
    	printk("@%s %d, distortion_mode %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_distortion_mode, S_IRUGO | S_IWUSR, m10mo_distirtion_mode_read, m10mo_distirtion_mode_write);

static ssize_t m10mo_flash1_test_brightness_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "0x00 250mA\n \
		0x01 300mA\n \
		0x02 350mA\n \
		0x03 400mA\n \
		0x04 450mA\n \
		0x05 500mA\n \
		0x06 550mA\n \
		0x07 600mA\n \
		0x08 650mA\n \
		0x09 700mA\n \
		0x0a 750mA\n \
		0x0b 800mA\n \
		0x0c 850mA\n \
		0x0d 900mA\n \
		0x0e 950mA\n \
		0x0f 1000mA (default)\n \
		0x10 1100mA\n \
		0x11 1200mA\n \
		0x12 1300mA\n \
		0x13 1500mA\n \
		0x14 1400mA\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, FLASH1_TEST_BRIGHTNESS, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, flash1_test_brightness = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		0x00 250mA\n \
		0x01 300mA\n \
		0x02 350mA\n \
		0x03 400mA\n \
		0x04 450mA\n \
		0x05 500mA\n \
		0x06 550mA\n \
		0x07 600mA\n \
		0x08 650mA\n \
		0x09 700mA\n \
		0x0a 750mA\n \
		0x0b 800mA\n \
		0x0c 850mA\n \
		0x0d 900mA\n \
		0x0e 950mA\n \
		0x0f 1000mA (default)\n \
		0x10 1100mA\n \
		0x11 1200mA\n \
		0x12 1300mA\n \
		0x13 1500mA\n \
		0x14 1400mA\n \
		flash1_test_brightness = 0x%x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_flash1_test_brightness_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x14) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, FLASH1_TEST_BRIGHTNESS, val);
    	printk("@%s %d, flash1_test_brightness %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_flash1_test_brightness, S_IRUGO | S_IWUSR, m10mo_flash1_test_brightness_read, m10mo_flash1_test_brightness_write);

static ssize_t m10mo_flash2_test_brightness_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "0x00 250mA\n \
		0x01 300mA\n \
		0x02 350mA\n \
		0x03 400mA\n \
		0x04 450mA\n \
		0x05 500mA\n \
		0x06 550mA\n \
		0x07 600mA\n \
		0x08 650mA\n \
		0x09 700mA\n \
		0x0a 750mA\n \
		0x0b 800mA\n \
		0x0c 850mA\n \
		0x0d 900mA\n \
		0x0e 950mA\n \
		0x0f 1000mA (default)\n \
		0x10 1100mA\n \
		0x11 1200mA\n \
		0x12 1300mA\n \
		0x13 1500mA\n \
		0x14 1400mA\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, FLASH2_TEST_BRIGHTNESS, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, flash2_test_brightness = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		0x00 250mA\n \
		0x01 300mA\n \
		0x02 350mA\n \
		0x03 400mA\n \
		0x04 450mA\n \
		0x05 500mA\n \
		0x06 550mA\n \
		0x07 600mA\n \
		0x08 650mA\n \
		0x09 700mA\n \
		0x0a 750mA\n \
		0x0b 800mA\n \
		0x0c 850mA\n \
		0x0d 900mA\n \
		0x0e 950mA\n \
		0x0f 1000mA (default)\n \
		0x10 1100mA\n \
		0x11 1200mA\n \
		0x12 1300mA\n \
		0x13 1500mA\n \
		0x14 1400mA\n \
		flash2_test_brightness = 0x%x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_flash2_test_brightness_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x14) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, FLASH2_TEST_BRIGHTNESS, val);
    	printk("@%s %d, flash2_test_brightness %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_flash2_test_brightness, S_IRUGO | S_IWUSR, m10mo_flash2_test_brightness_read, m10mo_flash2_test_brightness_write);


static ssize_t m10mo_torch1_test_brightness_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "0x00 25mA\n \
		 0x01 50mA\n \
		 0x02 75mA\n \
		 0x03 100mA\n \
		 0x04 125mA\n \
		 0x05 150mA\n \
		 0x06 175mA\n \
		 0x07 200mA (default)\n \
		 0x08 225mA\n \
		 0x09 250mA\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, TORCH1_TEST_BRIGHTNESS, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, torch1_test_brightness = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 0x00 25mA\n \
		 0x01 50mA\n \
		 0x02 75mA\n \
		 0x03 100mA\n \
		 0x04 125mA\n \
		 0x05 150mA\n \
		 0x06 175mA\n \
		 0x07 200mA (default)\n \
		 0x08 225mA\n \
		 0x09 250mA\n \
		 torch1_test_brightness = 0x%x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_torch1_test_brightness_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x09) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, TORCH1_TEST_BRIGHTNESS, val);
    	printk("@%s %d, torch1_test_brightness %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_torch1_test_brightness, S_IRUGO | S_IWUSR, m10mo_torch1_test_brightness_read, m10mo_torch1_test_brightness_write);


static ssize_t m10mo_torch2_test_brightness_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "0x00 25mA\n \
		 0x01 50mA\n \
		 0x02 75mA\n \
		 0x03 100mA\n \
		 0x04 125mA\n \
		 0x05 150mA\n \
		 0x06 175mA\n \
		 0x07 200mA (default)\n \
		 0x08 225mA\n \
		 0x09 250mA\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, TORCH2_TEST_BRIGHTNESS, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, torch2_test_brightness = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 0x00 25mA\n \
		 0x01 50mA\n \
		 0x02 75mA\n \
		 0x03 100mA\n \
		 0x04 125mA\n \
		 0x05 150mA\n \
		 0x06 175mA\n \
		 0x07 200mA (default)\n \
		 0x08 225mA\n \
		 0x09 250mA\n \
		 torch2_test_brightness = 0x%x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_torch2_test_brightness_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x09) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, TORCH2_TEST_BRIGHTNESS, val);
    	printk("@%s %d, torch2_test_brightness %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_torch2_test_brightness, S_IRUGO | S_IWUSR, m10mo_torch2_test_brightness_read, m10mo_torch2_test_brightness_write);

static ssize_t m10mo_flash1_test_timeout_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "0x00 off\n \
		 0x01 95ms\n \
		 0x02 190ms\n \
		 0x03 285ms\n \
		 0x04 380ms\n \
		 0x05 475ms\n \
		 0x06 570ms\n \
		 0x07 665ms\n \
		 0x08 760ms\n \
		 0x09 855ms\n \
		 0x0a 950ms\n \
		 0x0b 1045ms (default)\n \
		 0x0c 1140ms\n \
		 0x0d 1235ms\n \
		 0x0e 1330ms\n \
		 0x0f 1425ms\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, FLASH1_TEST_TIMEOUT, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, flash1_test_timeout = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 0x00 off\n \
		 0x01 95ms\n \
		 0x02 190ms\n \
		 0x03 285ms\n \
		 0x04 380ms\n \
		 0x05 475ms\n \
		 0x06 570ms\n \
		 0x07 665ms\n \
		 0x08 760ms\n \
		 0x09 855ms\n \
		 0x0a 950ms\n \
		 0x0b 1045ms (default)\n \
		 0x0c 1140ms\n \
		 0x0d 1235ms\n \
		 0x0e 1330ms\n \
		 0x0f 1425ms\n \
		 flash1_test_timeout = 0x%x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_flash1_test_timeout_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x0f) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, FLASH1_TEST_TIMEOUT, val);
    	printk("@%s %d, flash1_test_timeout %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_flash1_test_timeout, S_IRUGO | S_IWUSR, m10mo_flash1_test_timeout_read, m10mo_flash1_test_timeout_write);

static ssize_t m10mo_flash2_test_timeout_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "0x00 off\n \
		 0x01 95ms\n \
		 0x02 190ms\n \
		 0x03 285ms\n \
		 0x04 380ms\n \
		 0x05 475ms\n \
		 0x06 570ms\n \
		 0x07 665ms\n \
		 0x08 760ms\n \
		 0x09 855ms\n \
		 0x0a 950ms\n \
		 0x0b 1045ms (default)\n \
		 0x0c 1140ms\n \
		 0x0d 1235ms\n \
		 0x0e 1330ms\n \
		 0x0f 1425ms\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, FLASH2_TEST_TIMEOUT, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, flash2_test_timeout = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 0x00 off\n \
		 0x01 95ms\n \
		 0x02 190ms\n \
		 0x03 285ms\n \
		 0x04 380ms\n \
		 0x05 475ms\n \
		 0x06 570ms\n \
		 0x07 665ms\n \
		 0x08 760ms\n \
		 0x09 855ms\n \
		 0x0a 950ms\n \
		 0x0b 1045ms (default)\n \
		 0x0c 1140ms\n \
		 0x0d 1235ms\n \
		 0x0e 1330ms\n \
		 0x0f 1425ms\n \
		 flash2_test_timeout = 0x%x\n", reg_val);
out:
	return -EINVAL;
}
static ssize_t m10mo_flash2_test_timeout_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x0f) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, FLASH2_TEST_TIMEOUT, val);
    	printk("@%s %d, flash2_test_timeout %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_flash2_test_timeout, S_IRUGO | S_IWUSR, m10mo_flash2_test_timeout_read, m10mo_flash2_test_timeout_write);

static ssize_t m10mo_led_test_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "LED Control\n \
		 0x01 LED init\n \
		 0x02 Torch On (281.25mA)\n \
		 0x03 Torch Off\n \
		 0x04 Flash (375mA)\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, LED_TEST, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, LED_TEST = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 LED Control\n \
		 0x01 LED init\n \
		 0x02 Torch On (281.25mA)\n \
		 0x03 Torch Off\n \
		 0x04 Flash (375mA)\n\n\n \
		 LED_TEST = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_led_test_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x01 || val > 0x04) goto out;

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, LED_TEST, val);
    	printk("@%s %d, LED_TEST %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_led_test, S_IRUGO | S_IWUSR, m10mo_led_test_read, m10mo_led_test_write);


static ssize_t m10mo_ois_data_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 ret, reg_val[9];
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA7_0, &reg_val[0]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data0 = %x\n", __func__, __LINE__, reg_val[0]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA15_8, &reg_val[1]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data1 = %x\n", __func__, __LINE__, reg_val[1]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA23_16, &reg_val[2]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data2 = %x\n", __func__, __LINE__, reg_val[2]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA31_24, &reg_val[3]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data3 = %x\n", __func__, __LINE__, reg_val[3]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA39_32, &reg_val[4]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data4 = %x\n", __func__, __LINE__, reg_val[4]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA47_40, &reg_val[5]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data5 = %x\n", __func__, __LINE__, reg_val[5]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA55_48, &reg_val[6]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data6 = %x\n", __func__, __LINE__, reg_val[6]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA63_56, &reg_val[7]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data7 = %x\n", __func__, __LINE__, reg_val[7]);

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA71_64, &reg_val[8]);
        if(ret)
            goto out;
	printk("@%s %d, ois_data8 = %x\n", __func__, __LINE__, reg_val[8]);


	return scnprintf(buf, PAGE_SIZE, "\n%x %x %x %x %x %x %x %x %x\n", reg_val[0]\
			, reg_val[1], reg_val[2], reg_val[3], reg_val[4], reg_val[5]\
			, reg_val[6], reg_val[7], reg_val[8]);
out:
	return -EINVAL;
}
static ssize_t m10mo_ois_data_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 reg_val[10];
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x %x %x %x %x %x %x %x %x", &reg_val[0]\
			, &reg_val[1], &reg_val[2], &reg_val[3], &reg_val[4], &reg_val[5]\
			, &reg_val[6], &reg_val[7], &reg_val[8]);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA7_0, reg_val[0]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA15_8, reg_val[1]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA23_16, reg_val[2]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA31_24, reg_val[3]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA39_32, reg_val[4]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA47_40, reg_val[5]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA55_48, reg_val[6]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA63_56, reg_val[7]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_DATA71_64, reg_val[8]);

    	printk("@%s %d, ois_data %x %x %x %x %x %x %x %x %x\n", __func__, __LINE__, reg_val[0]\
			, reg_val[1], reg_val[2], reg_val[3], reg_val[4], reg_val[5]\
			, reg_val[6], reg_val[7], reg_val[8]);

	return len;
}
static DEVICE_ATTR(isp_ois_data, S_IRUGO | S_IWUSR, m10mo_ois_data_read, m10mo_ois_data_write);



static ssize_t m10mo_ois_write_read_trig_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "OIS Write Read Start Triger\n \
		 0x00: non\n \
		 0x01: 1 byte Read\n \
		 0x02: 2 byte Read\n \
		 0x04: 4 byte Read\n \
		 0x11: 1 byte Write\n \
		 0x12: 2 byte Write\n \
		 0x14: 4 byte Write\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ois_write_read_trig = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 OIS Write Read Start Triger\n \
		 0x00: non\n \
		 0x01: 1 byte Read\n \
		 0x02: 2 byte Read\n \
		 0x04: 4 byte Read\n \
		 0x11: 1 byte Write\n \
		 0x12: 2 byte Write\n \
		 0x14: 4 byte Write\n \
		 ois_write_read_trig = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ois_write_read_trig_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	if(val < 0x00 || val > 0x14) goto out;
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_WRITE_READ_TRIG, val);
    	printk("@%s %d, ois_write_read_trig %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_ois_write_read_trig, S_IRUGO | S_IWUSR, m10mo_ois_write_read_trig_read, m10mo_ois_write_read_trig_write);

static ssize_t m10mo_ois_ramreg_addr_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "OIS Write Read Address H.\n");

    	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ois_ramreg_addr_h = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "ois_ramreg_addr_h = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ois_ramreg_addr_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_TEST, OIS_RAMREG_ADDR_H, val);
    	printk("@%s %d, ois_ramreg_addr_h %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_ois_ramreg_addr, S_IRUGO | S_IWUSR, m10mo_ois_ramreg_addr_read, m10mo_ois_ramreg_addr_write);


static ssize_t m10mo_ois_lib_api_start_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "Start the OIS API library.\n\
		 0x01: Start api library 1 (IniSet)\n\
		 0x02: Start api library 2 (OscAdj)\n\
		 0x03: Start api library 3 (TneRun)\n\
		 0x04: OIS OFF (RtnCen(0))\n\
		 0x05: OIS ON  (OisEna)\n\
		 0x06: SetOpt (x refer to byte 0x72-0x73, y refer to byte 0x74-0x75)\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_LIB_API_START, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ois_lib_api_start = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 Start the OIS API library.\n\
		 0x01: Start api library 1 (IniSet)\n\
		 0x02: Start api library 2 (OscAdj)\n\
		 0x03: Start api library 3 (TneRun)\n\
		 0x04: OIS OFF (RtnCen(0))\n\
		 0x05: OIS ON  (OisEna)\n\
		 0x06: SetOpt (x refer to byte 0x72-0x73, y refer to byte 0x74-0x75)\n \
		 ois_lib_api_start = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ois_lib_api_start_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x01 || val > 0x06) goto out;
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_LIB_API_START, val);
    	printk("@%s %d, ois_lib_api_start %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_ois_lib_api_start, S_IRUGO | S_IWUSR, m10mo_ois_lib_api_start_read, m10mo_ois_lib_api_start_write);

static ssize_t m10mo_ois_cali_start_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "OIS Calibration Request Start.\n\
		 1: Start\n\
		 0: Invaild\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_CALI_API_START, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ois_cali_start = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 OIS Calibration Request Start.\n\
		 1: Start\n\
		 0: Invaild\n \
		 ois_cali_start = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ois_cali_start_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x01) goto out;
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_CALI_API_START, val);
    	printk("@%s %d, ois_cali_start %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_ois_cali_start, S_IRUGO | S_IWUSR, m10mo_ois_cali_start_read, m10mo_ois_cali_start_write);


static ssize_t m10mo_ois_cali_result_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val[2],ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "please input data[15-0], eg, echo 0x5 0x6 --> 0x5 is H byte, 0x6 is L byte\n\
		 bit 0 = 1 : Serious error\n\
		 bit 1 = 1 : Process end\n\
		 bit 2 = 1 : Hall X offset/bias NG\n\
		 bit 3 = 1 : Hall Y offset/bias NG\n\
		 bit 4 = 1 : X Actuator gain NG\n\
		 bit 5 = 1 : Y Actuator gain NG\n\
		 bit 6 = 1 : X Gyro Offset NG\n\
		 bit 7 = 1 : Y Gyro Offset NG\n\
		 bit 8 = 1 : NG\n\
		 bit 9 = 1 : NG\n\
		 bit 10 = 1 : NG\n\
		 bit 11 = 1 : Hall X direction NG\n\
		 bit 11 = 1 : Hall Y direction NG\n\
		 All Pass = 0x0002\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_CALI_RESULT_7_0, &reg_val[1]);
        if(ret)
            goto out;
    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, OIS_CALI_RESULT_15_8, &reg_val[0]);
        if(ret)
            goto out;
	printk("@%s %d, ois_cali_result = %x%x\n", __func__, __LINE__, reg_val[0],reg_val[1]);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 please input data[15-0], eg, echo \"0x5 0x6\" > isp_ois_cali_resul --> 0x5 is H byte, 0x6 is L byte\n\
		 bit 0 = 1 : Serious error\n\
		 bit 1 = 1 : Process end\n\
		 bit 2 = 1 : Hall X offset/bias NG\n\
		 bit 3 = 1 : Hall Y offset/bias NG\n\
		 bit 4 = 1 : X Actuator gain NG\n\
		 bit 5 = 1 : Y Actuator gain NG\n\
		 bit 6 = 1 : X Gyro Offset NG\n\
		 bit 7 = 1 : Y Gyro Offset NG\n\
		 bit 8 = 1 : NG\n\
		 bit 9 = 1 : NG\n\
		 bit 10 = 1 : NG\n\
		 bit 11 = 1 : Hall X direction NG\n\
		 bit 11 = 1 : Hall Y direction NG\n\
		 All Pass = 0x0002\n \
		 ois_cali_result = 0x%x%x\n", reg_val[0],reg_val[1]);
out:
	return -EINVAL;
}


static ssize_t m10mo_ois_cali_result_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 reg_val[2];
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x %x", &reg_val[1], &reg_val[0]);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_CALI_RESULT_7_0, reg_val[1]);
    	printk("@%s %d, OIS_CALI_RESULT_7_0 %x\n", __func__, __LINE__, reg_val[1]);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, OIS_CALI_RESULT_15_8, reg_val[0]);
    	printk("@%s %d, OIS_CALI_RESULT_15_8 %x\n", __func__, __LINE__, reg_val[0]);

	return len;
}
static DEVICE_ATTR(isp_ois_cali_result, S_IRUGO | S_IWUSR, m10mo_ois_cali_result_read, m10mo_ois_cali_result_write);

static ssize_t m10mo_cap_test_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "Capture LDC SPR ON/OFF SW.\n\
		 0x00: LDC ON / SPR ON\n\
		 0x01: LDC OFF / SPR ON\n\
		 0x02: LDC OFF / SPR OFF\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, CAP_TEST, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, cap_test = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 Capture LDC SPR ON/OFF SW.\n\
		 0x00: LDC ON / SPR ON\n\
		 0x01: LDC OFF / SPR ON\n\
		 0x02: LDC OFF / SPR OFF\n\n\n\
		 cap_test = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_cap_test_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x02) goto out;
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, CAP_TEST, val);
    	printk("@%s %d, cap_test %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_cap_test, S_IRUGO | S_IWUSR, m10mo_cap_test_read, m10mo_cap_test_write);

static ssize_t m10mo_pr_test_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "PR Test for LiveView.\n\
		 0x01: Start PR Detection\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, PR_TEST, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, pr_test = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 PR Test for LiveView.\n\
		 0x01: Start PR Detection\n\
		 pr_test = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_pr_test_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val < 0x00 || val > 0x01) goto out;
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, PR_TEST, val);
    	printk("@%s %d, pr_test %x\n", __func__, __LINE__, val);

	return len;
out:
	return -EINVAL;
}
static DEVICE_ATTR(isp_pr_test, S_IRUGO | S_IWUSR, m10mo_pr_test_read, m10mo_pr_test_write);


static ssize_t m10mo_pr_led_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "LED On/Off for PR.\n\
		 0x00: LED2(zoom) OFF & LED1 (Focus) OFF\n \
		 0x01: LED2(zoom) OFF & LED1 (Focus) ON\n \
		 0x10: LED2(zoom) ON  & LED1 (Focus) OFF\n \
		 0x11: LED2(zoom) ON  & LED1 (Focus) ON\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, PR_LED, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, pr_led = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 LED On/Off for PR.\n\
		 0x00: LED2(zoom) OFF & LED1 (Focus) OFF\n \
		 0x01: LED2(zoom) OFF & LED1 (Focus) ON\n \
		 0x10: LED2(zoom) ON  & LED1 (Focus) OFF\n \
		 0x11: LED2(zoom) ON  & LED1 (Focus) ON\n\
		 pr_led = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_pr_led_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, PR_LED, val);
    	printk("@%s %d, pr_led %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_pr_led, S_IRUGO | S_IWUSR, m10mo_pr_led_read, m10mo_pr_led_write);


static ssize_t m10mo_led_status_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_data, reg_status,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);



	//set address
    	ret = m10mo_writeb(&m10mo_dev->sd, CATEGORY_ASUS, LED_ERROR_ADDR, 0x09);
        if(ret)
            goto out;

	//read data from sky81296
    	ret = m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, LED_ERROR_WRITE_READ_TRIG, 0x1);
        if(ret)
            goto out;

	//get data from m10mo
    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, LED_ERROR_DATA, &reg_data);
        if(ret)
            goto out;


	//get status from m10mo
    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, LED_ERROR_STATUS, &reg_status);
        if(ret)
            goto out;

	printk("@%s %d, addr = 0x09\n", __func__, __LINE__);
	printk("@%s %d, data = %x\n", __func__, __LINE__, reg_data);
	printk("@%s %d, status = %x\n", __func__, __LINE__, reg_status);


	return scnprintf(buf, PAGE_SIZE, "\n\
		 addr = 0x09\n \
		 data = %x\n \
		 status = %x\n", reg_data, reg_status);
out:
	return -EINVAL;
}


static DEVICE_ATTR(isp_led_status, S_IRUGO | S_IWUSR, m10mo_led_status_read, NULL);



static ssize_t m10mo_maunal_focus_ctrl_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "setting Range: 0x0002-0x1fff, \n");

    	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_TEST, MANUAL_FOCUS_CTRL_H, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, maunal_focus_ctrl = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		setting Range: 0x0002-0x1fff, \n\n\n\
		maunal_focus_ctrl = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_maunal_focus_ctrl_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 reg_val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &reg_val);

    	(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MANUAL_FOCUS, &reg_val);

	return len;
}
static DEVICE_ATTR(isp_maunal_focus_ctrl, S_IRUGO | S_IWUSR, m10mo_maunal_focus_ctrl_read, m10mo_maunal_focus_ctrl_write);


static ssize_t m10mo_maunal_zoom_ctrl_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "setting Range: 0x0002-0x1fff, \n");


    	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_TEST, MANUAL_ZOOM_CTRL_H, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, maunal_zoom_ctrl = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		setting Range: 0x0002-0x1fff, \n\n\n\
		maunal_zoom_ctrl = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_maunal_zoom_ctrl_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 reg_val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &reg_val);
    	(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MANUAL_ZOOM, &reg_val);

	return len;
}
static DEVICE_ATTR(isp_maunal_zoom_ctrl, S_IRUGO | S_IWUSR, m10mo_maunal_zoom_ctrl_read, m10mo_maunal_zoom_ctrl_write);


static ssize_t m10mo_sensor_nr_en_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "set NR enable.\n\
		 0x00: sensor NR OFF\n \
		 0x01: sensor NR ON\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, SENSOR_NR_EN, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, sensor_nr_en = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 set NR enable.\n\
		 0x00: sensor NR OFF\n \
		 0x01: sensor NR ON\n\
		 sensor_nr_en = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_sensor_nr_en_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, SENSOR_NR_EN, val);
    	printk("@%s %d, sensor_nr_en %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_sensor_nr_en, S_IRUGO | S_IWUSR, m10mo_sensor_nr_en_read, m10mo_sensor_nr_en_write);


static ssize_t m10mo_sensor_update_en_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "set update enable.\n\
		 0x00: sensor update OFF / OIS tempcorrection OFF\n \
		 0x01: sensor update ON / OIS tempcorrection OFF\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_TEST, SENSOR_UPDATE_EN, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, sensor_update_en = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		 set update enable.\n\
		 0x00: sensor update OFF / OIS tempcorrection OFF\n \
		 0x01: sensor update ON / OIS tempcorrection OFF\n\
		 sensor_update_en = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_sensor_update_en_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_TEST, SENSOR_UPDATE_EN, val);
    	printk("@%s %d, sensor_update_en %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_sensor_update_en, S_IRUGO | S_IWUSR, m10mo_sensor_update_en_read, m10mo_sensor_update_en_write);


static ssize_t m10mo_ae_mode_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "AE Metering mode setting\n \
			0x00: METERING_CENTER\n \
			0x01: METERING_SPOT\n \
			0x02: METERING_AVERAGE\n \
			0xFF: AE OFF\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_AE, AE_MODE, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ae_mode = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		AE Metering mode setting\n \
			0x00: METERING_CENTER\n \
			0x01: METERING_SPOT\n \
			0x02: METERING_AVERAGE\n \
			0xFF: AE OFF\n\n\n \
		ae_mode = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ae_mode_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val == 0x0 || val == 0x01 || val == 0x02 || val == 0xFF){
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_AE, AE_MODE, val);
    		printk("@%s %d, ae_mode %x\n", __func__, __LINE__, val);
		return len;
	}else{
    		printk("@%s %d, arg error\n", __func__, __LINE__);
		return -EINVAL;
	}

}
static DEVICE_ATTR(isp_ae_mode, S_IRUGO | S_IWUSR, m10mo_ae_mode_read, m10mo_ae_mode_write);

static ssize_t m10mo_ae_target_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "AE TARGET (0x01~0x5A)\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_AE, AE_TARGET, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ae_target = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		AE TARGET (0x01~0x5A)\n\n\n \
		ae_target = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ae_target_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val > 0x00 && val < 0x5b){
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_AE, AE_TARGET, val);
    		printk("@%s %d, ae_target %x\n", __func__, __LINE__, val);
		return len;
	}else{
    		printk("@%s %d, arg error\n", __func__, __LINE__);
		return -EINVAL;
	}

}
static DEVICE_ATTR(isp_ae_target, S_IRUGO | S_IWUSR, m10mo_ae_target_read, m10mo_ae_target_write);

static ssize_t m10mo_ae_speed_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "AE_SPEED (0x01~0x64)\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_AE, AE_SPEED, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, ae_speed = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		AE_SPEED (0x01~0x64)\n\n\n \
		ae_speed = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_ae_speed_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val > 0x00 && val < 0x65){
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_AE, AE_SPEED, val);
    		printk("@%s %d, ae_speed %x\n", __func__, __LINE__, val);
		return len;
	}else{
    		printk("@%s %d, arg error\n", __func__, __LINE__);
		return -EINVAL;
	}

}
static DEVICE_ATTR(isp_ae_speed, S_IRUGO | S_IWUSR, m10mo_ae_speed_read, m10mo_ae_speed_write);

static ssize_t m10mo_af_start_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "Starting AF operation\n \
		<Write>\n \
			0x09: Focus Calibration\n \
			0x13: Single AF (Simple)\n \
			0x14: Single AF\n \
		<Read>\n \
			0x00: AF stop by force or AF released\n \
			0x01: AF starting\n \
			0x02: AF done\n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_LENS, AF_START, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, af_start = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		Starting AF operation\n \
		<Write>\n \
			0x09: Focus Calibration\n \
			0x13: Single AF (Simple)\n \
			0x14: Single AF\n \
		<Read>\n \
			0x00: AF stop by force or AF released\n \
			0x01: AF starting\n \
			0x02: AF done\n\n\n \
		af_start = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_af_start_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;//,timeout=0;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);
	if(val == 0x13){
		(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_START_AF, NULL);
		return len;
	}else if(val == 0x9 || val == 0x14){
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LENS, AF_START, val);
    		printk("@%s %d, af_start %x\n", __func__, __LINE__, val);
		return len;
	}else{
    		printk("@%s %d, arg error\n", __func__, __LINE__);
		return -EINVAL;
	}

}
static DEVICE_ATTR(isp_af_start, S_IRUGO | S_IWUSR, m10mo_af_start_read, m10mo_af_start_write);


static ssize_t m10mo_af_operation_result_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "AF operation result \n \
			0x00: AF OFF \n \
			0x01: Focus operation success \n \
			0x02: Focus operation fail \n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_LENS, AF_RESULT, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, af_operation_result = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		AF operation result \n \
			0x00: AF OFF \n \
			0x01: Focus operation success \n \
			0x02: Focus operation fail \n \n\n\
		af_operation_result = %x\n", reg_val);
out:
	return -EINVAL;
}

static DEVICE_ATTR(isp_af_operation_result, S_IRUGO | S_IWUSR, m10mo_af_operation_result_read, NULL);

static ssize_t m10mo_af_laser_start_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "Laser Sensor Start \n \
			0x01: Simple Distance \n \
			0x02: Distance (not support) \n");

    	ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_LENS, AF_LASER_START, &reg_val);
        if(ret)
            goto out;
	printk("@%s %d, af_laser_start = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		Laser Sensor Start \n \
			0x01: Simple Distance \n \
			0x02: Distance (not support) \n\n\n \
		af_laser_start = %x\n", reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_af_laser_start_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LENS, AF_LASER_START, val);
    	printk("@%s %d, af_laser_start %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_af_laser_start, S_IRUGO | S_IWUSR, m10mo_af_laser_start_read, m10mo_af_laser_start_write);


static ssize_t m10mo_af_laser_distance_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	printk( "Unit:mm, \n \
		 Distance: 0mm ~ 500mm, \n");

    	ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_LENS, AF_LASER_DISTANCE_H, &reg_val);
        if(ret)
            goto out;

	printk("@%s %d, af_laser_distance = %x\n", __func__, __LINE__, reg_val);

	return scnprintf(buf, PAGE_SIZE, "\n\
		Unit:mm, \n \
		Distance: 0mm ~ 500mm, \n\n\n\
		af_laser_distance = 0x%x\n", reg_val);
out:
	return -EINVAL;
}


static DEVICE_ATTR(isp_af_laser_distance, S_IRUGO | S_IWUSR, m10mo_af_laser_distance_read, NULL);

static ssize_t m10mo_asus_exposure_control_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    	u32 reg_val,ret;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	switch(m10mo_dev->asus_exposure_control_mode){

	case ASUS_SATURATION:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SATURATION, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SATURATION = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_CONTRAST:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_CONTRAST, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_CONTRAST = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FACTORY_MODE:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FACTORY_MODE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FACTORY_MODE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_ZOOM_CANCEL:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_ZOOM_CANCEL, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_ZOOM_CANCEL = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FOCUS_CANCEL:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FOCUS_CANCEL, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FOCUS_CANCEL = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FLASH_NEEDED:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FLASH_NEEDED, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FLASH_NEEDED = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FOCUS_RESULT:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FOCUS_RESULT, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FOCUS_RESULT = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_3A_LOCK:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_3A_LOCK, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_3A_LOCK = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_BATTERY_CAPACITY:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_BATTERY_CAPACITY, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_BATTERY_CAPACITY = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_EFFECT:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_EFFECT, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_EFFECT = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_ISO:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_ISO, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_ISO = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_EV:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_EV, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_EV = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SHUTTER_SPEED:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SHUTTER_SPEED, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SHUTTER_SPEED = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FLICKER:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FLICKER, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FLICKER = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AE_SCENE_MODE:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AE_SCENE_MODE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AE_SCENE_MODE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FLASH:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FLASH, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FLASH = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_METERING_MODE:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_METERING_MODE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_METERING_MODE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_TOUCH_POSITION_X:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_TOUCH_POSITION_X, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_TOUCH_POSITION_X = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_TOUCH_POSITION_Y:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_TOUCH_POSITION_Y, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_TOUCH_POSITION_Y = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FULL_SEARCH_TARGET:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FULL_SEARCH_TARGET, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FULL_SEARCH_TARGET = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FULL_SEARCH_RANGE:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FULL_SEARCH_RANGE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FULL_SEARCH_RANGE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FULL_SEARCH_STEP:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FULL_SEARCH_STEP, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FULL_SEARCH_STEP = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LONG_EXP_CAP:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LONG_EXP_CAP, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LONG_EXP_CAP = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AUTOLL:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AUTOLL, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AUTOLL = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AT_SCENE:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AT_SCENE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AT_SCENE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_TOUCH_ROI_LEFT_UPPER_X:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_TOUCH_ROI_LEFT_UPPER_X, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_TOUCH_ROI_LEFT_UPPER_X = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_TOUCH_ROI_LEFT_UPPER_Y:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_TOUCH_ROI_LEFT_UPPER_Y, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_TOUCH_ROI_LEFT_UPPER_Y = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_TOUCH_WIDTH:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_TOUCH_WIDTH, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_TOUCH_WIDTH = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_TOUCH_HEIGHT:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_TOUCH_HEIGHT, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_TOUCH_HEIGHT = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_PHONE_DIRECTION:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_PHONE_DIRECTION, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_PHONE_DIRECTION = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_ZOOM_POSITION:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_ZOOM_POSITION, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_ZOOM_POSITION = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FOCUS_STEP:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FOCUS_STEP, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FOCUS_STEP = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_FOCUS_MODE:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_FOCUS_MODE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_FOCUS_MODE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_G_SENSOR_X:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_G_SENSOR_X, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_G_SENSOR_X = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_G_SENSOR_Y:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_G_SENSOR_Y, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_G_SENSOR_Y = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_G_SENSOR_Z:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_G_SENSOR_Z, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_G_SENSOR_Z = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_WB_MANUAL:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_WB_MANUAL, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_WB_MANUAL = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_COLOR_TEMPERATURE:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_COLOR_TEMPERATURE, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_COLOR_TEMPERATURE = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_GAIN1_RGAIN:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_GAIN1_RGAIN, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_GAIN1_RGAIN = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_GAIN1_BGAIN:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_GAIN1_BGAIN, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_GAIN1_BGAIN = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_GAIN2_RGAIN:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_GAIN2_RGAIN, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_GAIN2_RGAIN = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_GAIN2_BGAIN:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_GAIN2_BGAIN, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_GAIN2_BGAIN = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LV:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LV, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LV = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LSC_ZOOM_INDEX_A:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LSC_ZOOM_INDEX_A, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LSC_ZOOM_INDEX_A = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LSC_ZOOM_INDEX_B:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LSC_ZOOM_INDEX_B, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LSC_ZOOM_INDEX_B = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LSC_LIGHTSOURCE_INDEX_A:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LSC_LIGHTSOURCE_INDEX_A, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LSC_LIGHTSOURCE_INDEX_A = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LSC_LIGHTSOURCE_INDEX_B:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LSC_LIGHTSOURCE_INDEX_B, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LSC_LIGHTSOURCE_INDEX_B = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_LSC_BLEND_RATIO:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_LSC_BLEND_RATIO, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_LSC_BLEND_RATIO = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG9:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG9, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG9 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG10:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG10, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG10 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG11:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG11, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG11 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG12:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG12, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG12 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG13:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG13, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG13 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG14:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG14, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG14 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_AWB_DEBUG15:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_AWB_DEBUG15, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_AWB_DEBUG15 = %x\n", __func__, __LINE__, reg_val);
	break;
	case LED_ERROR_ADDR:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, LED_ERROR_ADDR, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, LED_ERROR_ADDR = %x\n", __func__, __LINE__, reg_val);
	break;
	case LED_ERROR_STATUS:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, LED_ERROR_STATUS, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, LED_ERROR_STATUS = %x\n", __func__, __LINE__, reg_val);
	break;
	case LED_ERROR_DATA:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, LED_ERROR_DATA, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, LED_ERROR_DATA = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_INFINITY_OFFSET:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_INFINITY_OFFSET, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_INFINITY_OFFSET = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MARCO_OFFSET:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MARCO_OFFSET, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MARCO_OFFSET = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_PREVIEW_FRAME_WIDTH:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_PREVIEW_FRAME_WIDTH, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_PREVIEW_FRAME_WIDTH = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_PREVIEW_FRAME_HEIGHT:
		ret = m10mo_readw(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_PREVIEW_FRAME_HEIGHT, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_PREVIEW_FRAME_HEIGHT = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR0:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR0, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR0 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR1:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR1, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR1 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR2:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR2, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR2 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR3:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR3, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR3 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR4:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR4, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR4 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR5:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR5, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR5 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR6:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR6, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR6 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR7:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR7, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR7 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR8:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR8, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR8 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR9:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR9, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR9 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR10:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR10, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR10 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR11:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR11, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR11 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR12:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR12, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR12 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR13:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR13, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR13 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR14:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR14, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR14 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR15:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR15, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR15 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR16:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR16, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR16 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR17:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR17, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR17 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR18:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR18, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR18 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_MOTOR19:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_MOTOR19, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_MOTOR19 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE0:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE0, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE0 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE1:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE1, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE1 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE2:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE2, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE2 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE3:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE3, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE3 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE4:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE4, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE4 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE5:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE5, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE5 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE6:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE6, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE6 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE7:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE7, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE7 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE8:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE8, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE8 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE9:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE9, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE9 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE10:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE10, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE10 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE11:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE11, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE11 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE12:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE12, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE12 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE13:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE13, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE13 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE14:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE14, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE14 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE15:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE15, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE15 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE16:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE16, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE16 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE17:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE17, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE17 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE18:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE18, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE18 = %x\n", __func__, __LINE__, reg_val);
	break;
	case ASUS_SCORE19:
		ret = m10mo_readb(&m10mo_dev->sd, CATEGORY_ASUS, ASUS_SCORE19, &reg_val);
        	if(ret)
            		goto out;
		printk("@%s %d, ASUS_SCORE19 = %x\n", __func__, __LINE__, reg_val);
	break;

	default:
		printk("@%s %d, Category error = %x\n", __func__, __LINE__, m10mo_dev->asus_exposure_control_mode);
	break;
	}


	return scnprintf(buf, PAGE_SIZE, "\n\
			ASUS EXPOSURE CONTROL Byte = %x data = %x\n", m10mo_dev->asus_exposure_control_mode, reg_val);
out:
	return -EINVAL;
}


static ssize_t m10mo_asus_exposure_control_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 val[2];
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x %x", &val[0], &val[1]);
	if(ASUS_TOUCH_POSITION_X == val[0] ||\
	   ASUS_TOUCH_POSITION_Y == val[0] ||\
	   ASUS_FULL_SEARCH_TARGET == val[0] ||\
	   ASUS_AT_SCENE == val[0] ||\
	   ASUS_TOUCH_ROI_LEFT_UPPER_X == val[0] ||\
	   ASUS_TOUCH_ROI_LEFT_UPPER_Y == val[0] ||\
	   ASUS_TOUCH_WIDTH == val[0] ||\
	   ASUS_TOUCH_HEIGHT == val[0] ||\
	   ASUS_G_SENSOR_X == val[0] ||\
	   ASUS_G_SENSOR_Y == val[0] ||\
	   ASUS_G_SENSOR_Z == val[0] ||\
	   ASUS_COLOR_TEMPERATURE == val[0] ||\
	   ASUS_GAIN1_RGAIN == val[0] ||\
	   ASUS_GAIN1_BGAIN == val[0] ||\
	   ASUS_GAIN2_RGAIN == val[0] ||\
	   ASUS_GAIN2_BGAIN == val[0] ||\
	   ASUS_LV == val[0] ||\
	   ASUS_LSC_BLEND_RATIO == val[0] ||\
	   ASUS_INFINITY_OFFSET == val[0] ||\
	   ASUS_MARCO_OFFSET == val[0] ||\
	   ASUS_PREVIEW_FRAME_WIDTH == val[0] ||\
	   ASUS_PREVIEW_FRAME_HEIGHT == val[0])
	(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_ASUS, val[0], val[1]);
	else
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_ASUS, val[0], val[1]);

    	printk("@%s %d, ASUS EXPOSURE CONTROL Byte = %x data = %x\n", __func__, __LINE__, val[0], val[1]);

	return len;
}
static DEVICE_ATTR(isp_asus_exposure_control, S_IRUGO | S_IWUSR, m10mo_asus_exposure_control_read, m10mo_asus_exposure_control_write);


static ssize_t m10mo_asus_exposure_control_read_set(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "\n\
		set ASUS EXPOSURE CONTROL read Byte\n");
}

static ssize_t m10mo_asus_exposure_control_write_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    u32 val;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	m10mo_dev->asus_exposure_control_mode = val;
    	printk("@%s %d, ASUS EXPOSURE CONTROL read Byte = %x\n", __func__, __LINE__, val);

	return len;
}
static DEVICE_ATTR(isp_asus_exposure_control_set, S_IRUGO | S_IWUSR, m10mo_asus_exposure_control_read_set, m10mo_asus_exposure_control_write_set);



static ssize_t m10mo_read_calibration_data(struct device *dev, struct device_attribute *attr, char *buf)
{

	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

    	u32 reg_val[4]={0x0,0x0,0x0,0x0};
	int ret=0, res=0, timeout=5000;
	printk( "read/write calibration data\n\
			arg1: 	0x01: 1 byte read\n\
				0x02: 2 byte read\n\
				0x04: 4 byte read\n\
				0x09: 1 byte write\n\
				0x0a: 2 byte write\n\
				0x0c: 4 byte write\n\
			      others: setting mode\n\
			arg2: offset\n\
			arg3: data0\n\
			arg4: data1\n\
			arg5: data2\n\
			arg6: data3\n");
	printk("@%s %d,start read calibration!\n",__func__, __LINE__);

    if(m10mo_dev->fadj_command == 0x01 || m10mo_dev->fadj_command == 0x02 || m10mo_dev->fadj_command == 0x04){

    	(void)__m10mo_param_mode_set(&m10mo_dev->sd);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_FLASH_MODE, 0x01);
	do {
		msleep(10);
		m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, SFLASH_SPI_STATUS, &res);
	} while ((res != 0) && --timeout);

	if (!timeout) {
		printk("@%s %d,timeout while waiting for chip op to finish\n", __func__, __LINE__);
		return 0;
	}
	(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_OFFSET_H, m10mo_dev->fadj_offset);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_MODE, m10mo_dev->fadj_command);
	switch(m10mo_dev->fadj_command){
	    case 0x01://1 byte read
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, &reg_val[0]);
		ret = scnprintf(buf, PAGE_SIZE, "\n \
		read/write calibration data\n\
			arg1: 	0x01: 1 byte read\n\
				0x02: 2 byte read\n\
				0x04: 4 byte read\n\
				0x09: 1 byte write\n\
				0x0a: 2 byte write\n\
				0x0c: 4 byte write\n\
			      others: setting mode\n\
			arg2: offset\n\
			arg3: data0\n\
			arg4: data1\n\
			arg5: data2\n\
			arg6: data3\n\n \
			command %x offset %x data0=%x\n", m10mo_dev->fadj_command,m10mo_dev->fadj_offset,reg_val[0]);
	    break;
	    case 0x02://2 byte read
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, &reg_val[0]);
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE1, &reg_val[1]);
		ret = scnprintf(buf, PAGE_SIZE, "\n \
		read/write calibration data\n\
			arg1: 	0x01: 1 byte read\n\
				0x02: 2 byte read\n\
				0x04: 4 byte read\n\
				0x09: 1 byte write\n\
				0x0a: 2 byte write\n\
				0x0c: 4 byte write\n\
			      others: setting mode\n\
			arg2: offset\n\
			arg3: data0\n\
			arg4: data1\n\
			arg5: data2\n\
			arg6: data3\n\n \
			command %x offset %x data0=%x data1=%x\n", m10mo_dev->fadj_command,m10mo_dev->fadj_offset,reg_val[0], reg_val[1]);
	    break;
	    case 0x04://4 byte read
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, &reg_val[0]);
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE1, &reg_val[1]);
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE2, &reg_val[2]);
		(void) m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE3, &reg_val[3]);
		ret = scnprintf(buf, PAGE_SIZE, "\n \
		read/write calibration data\n\
			arg1: 	0x01: 1 byte read\n\
				0x02: 2 byte read\n\
				0x04: 4 byte read\n\
				0x09: 1 byte write\n\
				0x0a: 2 byte write\n\
				0x0c: 4 byte write\n\
			      others: setting mode\n\
			arg2: offset\n\
			arg3: data0\n\
			arg4: data1\n\
			arg5: data2\n\
			arg6: data3\n\n \
			command %x offset %x data0=%x data1=%x data2=%x data3=%x\n", m10mo_dev->fadj_command,m10mo_dev->fadj_offset,reg_val[0], reg_val[1],reg_val[2], reg_val[3]);
	    break;
	}
    	(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
	printk("@%s %d,finish read calibration!\n",__func__, __LINE__);
	return ret;
    }else{
	printk("@%s %d, command %x is not read command\n", __func__, __LINE__,m10mo_dev->fadj_command);
	return -1;
    }
}


static ssize_t m10mo_write_calibration_data(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 reg_val[4]={0x0,0x0,0x0,0x0};
	int ret=0, res=0, timeout=5000;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x %x %x %x %x %x", &m10mo_dev->fadj_command, &m10mo_dev->fadj_offset, &reg_val[0], &reg_val[1], &reg_val[2], &reg_val[3]);

	printk("@%s %d,start write calibration!\n",__func__, __LINE__);

	switch(m10mo_dev->fadj_command){
	    case 0x09://1 byte write
		if(CacCalibrationStatus == false)(void)__m10mo_param_mode_set(&m10mo_dev->sd);
		(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_OFFSET_H, m10mo_dev->fadj_offset);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, reg_val[0]);
	    break;
	    case 0x0a://2 byte write
		if(CacCalibrationStatus == false)(void)__m10mo_param_mode_set(&m10mo_dev->sd);
		(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_OFFSET_H, m10mo_dev->fadj_offset);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, reg_val[0]);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE1, reg_val[1]);
	    break;
	    case 0x0c://4 byte write
		if(CacCalibrationStatus == false)(void)__m10mo_param_mode_set(&m10mo_dev->sd);
		(void) m10mo_writew(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_OFFSET_H, m10mo_dev->fadj_offset);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE0, reg_val[0]);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE1, reg_val[1]);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE2, reg_val[2]);
		(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_DATA_BYTE3, reg_val[3]);
	    break;
	    default:
		ret = 1;
	    break;
	}
	if(ret == 1){
		printk("@%s %d,set command=%x offset=%x\n",__func__, __LINE__,m10mo_dev->fadj_command, m10mo_dev->fadj_offset);
		return len;
	}
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_RW_MODE, m10mo_dev->fadj_command);
	(void) m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, FADJ_FLASH_MODE, 0x03);

	do {
		msleep(10);
		m10mo_readb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, SFLASH_SPI_STATUS, &res);
	} while ((res != 0) && --timeout);

	if (!timeout) {
		printk("@%s %d,timeout while waiting for chip op to finish\n",__func__, __LINE__);
		return len;
	}
	printk("@%s %d,finish write calibration!\n",__func__, __LINE__);
	if(CacCalibrationStatus == false)
    	(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);
	return len;

}
static DEVICE_ATTR(isp_fw_RW, S_IRUGO | S_IWUSR, m10mo_read_calibration_data, m10mo_write_calibration_data);


static ssize_t m10mo_read_SHD_calibration_data(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	mutex_lock(&m10mo_dev->input_lock);
    (void) m10mo_s_power_fac(1);
	m10mo_isp_fw_SHD_R(m10mo_dev);
    (void) m10mo_s_power_fac(0);
	mutex_unlock(&m10mo_dev->input_lock);
	return 0;
}


static ssize_t m10mo_write_SHD_calibration_data(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);
	mutex_lock(&m10mo_dev->input_lock);
    (void) m10mo_s_power_fac(1);
	m10mo_isp_fw_SHD_W(m10mo_dev);

	(void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_WRITE_SHD_TABLE, NULL);
    (void) m10mo_s_power_fac(0);
	mutex_unlock(&m10mo_dev->input_lock);
//    (void) m10mo_request_cmd_effect(&m10mo_dev->sd, M10MO_MONITOR_MODE_ZSL_REQUEST_CMD, NULL);

	return len;
}
static DEVICE_ATTR(isp_fw_SHD_RW, S_IRUGO | S_IWUSR, m10mo_read_SHD_calibration_data, m10mo_write_SHD_calibration_data);

static ssize_t m10mo_open_m10mo_fac(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 val=0;

	sscanf(buf, "%x", &val);

	if(val == 0){
    	    (void) m10mo_s_power_fac(0);
	    CacCalibrationStatus = false;
	}else{
	    CacCalibrationStatus = true;
    	    (void) m10mo_s_power_fac(1);
	}
	return len;
}
static DEVICE_ATTR(isp_fw_power_fac, S_IRUGO | S_IWUSR, NULL, m10mo_open_m10mo_fac);
#endif

static ssize_t m10mo_isp_update_status(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%x", isp_update_status);;
}
static DEVICE_ATTR(isp_update_status, S_IRUGO | S_IWUSR, m10mo_isp_update_status, NULL);


static int m10mo_ispd1(struct m10mo_device *dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	struct v4l2_subdev *sd = &dev->sd;
	int ret = 0;

	mutex_lock(&dev->input_lock);

	dev_info(&client->dev, "ispd1 start\n");
	ret = m10mo_dump_string_log1(sd);
	if (ret < 0)
		dev_err(&client->dev, "isp log 1 error\n");

	dev_info(&client->dev, "ispd1 finished\n");
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int m10mo_ispd2(struct m10mo_device *dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	struct v4l2_subdev *sd = &dev->sd;
	int ret = 0;

	mutex_lock(&dev->input_lock);

	dev_info(&client->dev, "ispd2 start\n");

	ret = m10mo_dump_string_log2_1(sd);
	if (ret != 0)
		dev_err(&client->dev, "m10mo_dump_string_log2_1 error\n");

	dev_info(&client->dev, "log2_1 finished\n");

	ret = m10mo_dump_string_log2_2(sd);
	if (ret != 0)
		dev_err(&client->dev, "m10mo_dump_string_log2_2 error\n");

	dev_info(&client->dev, "ispd2_2 finish\n");

	ret = m10mo_dump_string_log2_3(sd);
	if (ret != 0)
		dev_err(&client->dev, "m10mo_dump_string_log2_3 error\n");

	dev_info(&client->dev, "ispd2_3 finish\n");

	mutex_unlock(&dev->input_lock);
	return ret;
}

static int m10mo_ispd3(struct m10mo_device *dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	struct v4l2_subdev *sd = &dev->sd;
	int ret = 0;

	mutex_lock(&dev->input_lock);

	/**ISP RESET**/
	ret = dev->pdata->common.gpio_ctrl(sd, 0);

	msleep(10);

	/**ISP RESET**/
	ret = dev->pdata->common.gpio_ctrl(sd, 1);

	msleep(50);

	dev_info(&client->dev, "ispd3 start\n");

	ret = m10mo_dump_string_log2_1(sd);
	if (ret != 0)
		dev_err(&client->dev, "m10mo_dump_string_log2_1 error\n");

	dev_info(&client->dev, "log2_1 finished\n");

	ret = m10mo_dump_string_log2_2(sd);
	if (ret != 0)
		dev_err(&client->dev, "m10mo_dump_string_log2_2 error\n");

	dev_info(&client->dev, "ispd2_2 finish\n");

	ret = m10mo_dump_string_log2_3(sd);
	if (ret != 0)
		dev_err(&client->dev, "m10mo_dump_string_log2_3 error\n");

	dev_info(&client->dev, "ispd2_3 finish\n");

	dev_info(&client->dev, "ispd3 finished\n");

	mutex_unlock(&dev->input_lock);
	return ret;
}

int m10mo_ispd4(struct m10mo_device *dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	struct v4l2_subdev *sd = &dev->sd;
	int ret = 0;

	mutex_lock(&dev->input_lock);

	dev_info(&client->dev, "ispd4 start\n");
	ret = m10mo_dump_string_log3(sd);
	if (ret < 0)
		dev_err(&client->dev, "isp log 4 error\n");

	dev_info(&client->dev, "ispd4 finished\n");
	mutex_unlock(&dev->input_lock);
	return ret;
}

void m10mo_dump_log(struct v4l2_subdev *sd)
{
	struct m10mo_device *m10mo_dev = to_m10mo_sensor(sd);

	/*
	 * dbglvl: bit0 to dump m10mo_ispd1
	 * dbglvl: bit1 to dump m10mo_ispd2
	 * dbglvl: bit2 to dump m10mo_ispd4
	 * Debug log 3 is most important, so dump first
	 */
	if (dbglvl & 4)
		m10mo_ispd4(m10mo_dev);

	if (dbglvl & 2)
		m10mo_ispd2(m10mo_dev);

	if (dbglvl & 1)
		m10mo_ispd1(m10mo_dev);
}

static ssize_t m10mo_isp_log_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	if (m10mo_dev->power == 0) {
		goto leave;
	}

	if (!strncmp(buf, "1", 1))
		m10mo_ispd1(m10mo_dev);
	else if (!strncmp(buf, "4", 1))
		m10mo_ispd4(m10mo_dev);
	else if (!strncmp(buf, "2", 1))
		m10mo_ispd2(m10mo_dev);
	else if (!strncmp(buf, "3", 1))
		m10mo_ispd3(m10mo_dev);
	else
		m10mo_ispd4(m10mo_dev);

leave:
	return len;
}

static DEVICE_ATTR(isp_log, S_IRUGO | S_IWUSR, NULL, m10mo_isp_log_store);


static ssize_t m10mo_fw_version_read(struct device *dev, struct device_attribute *attr, char *buf)
{
    u32 val=0;
	int ret=0;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	mutex_lock(&m10mo_dev->input_lock);
	if (m10mo_dev->power == 1) {
		ret = -EBUSY;
		goto leave;
	}
	m10mo_s_power_fac(1);

	ret = m10mo_readb(&m10mo_dev->sd, 0x00, 0x02, &fw_version_val1);
        if (ret){
		m10mo_s_power_fac(0);
        goto  leave;
	}
	ret =  m10mo_readb(&m10mo_dev->sd, 0x00, 0x03, &fw_version_val2);
        if (ret){
		m10mo_s_power_fac(0);
        goto  leave;
	}

	m10mo_s_power_fac(0);

	val = fw_version_val1;
	val = val << 8 |  fw_version_val2;
	printk(KERN_INFO "m10mo, FW version is %X%X-%x  \n", fw_version_val1, fw_version_val2,DIT_version);

leave:
	mutex_unlock(&m10mo_dev->input_lock);

	if (ret)
		return scnprintf(buf, PAGE_SIZE, "0xfff0\n");
	else if(isFlashFwFail == true)
		return scnprintf(buf, PAGE_SIZE, "0xffff\n");
	else
		return scnprintf(buf, PAGE_SIZE, "0x%x-%x\n", val,DIT_version);
}

static DEVICE_ATTR(isp_fw_version, S_IRUGO | S_IWUSR, m10mo_fw_version_read, NULL);

static ssize_t m10mo_log_enable(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
    	u32 val=0;
	int ret=0;
	struct m10mo_device *m10mo_dev = dev_get_drvdata(dev);

	sscanf(buf, "%x", &val);

	if(val == 1){
		ret = m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, LOG_ACT, LOG_ACT_ENABLE);
		if (ret < 0)
			printk("%s, m10mo_writeb error\n", __func__);
	}else if(val == 0){
		ret = m10mo_writeb(&m10mo_dev->sd, CATEGORY_LOGLEDFLASH, LOG_ACT, LOG_ACT_DISABLE);
		if (ret < 0)
			printk("%s, m10mo_writeb error\n", __func__);
	}
	return len;
}
static DEVICE_ATTR(m10mo_log_enable, S_IRUGO | S_IWUSR, NULL, m10mo_log_enable);

static struct attribute *sysfs_attrs_ctrl[] = {
	&dev_attr_isp_checksum.attr,
	&dev_attr_isp_flashfw.attr,
	&dev_attr_isp_fw_dump.attr,
	&dev_attr_isp_cali_data_dump.attr,
	&dev_attr_isp_update_status.attr,
	&dev_attr_isp_spi.attr,
	&dev_attr_isp_fw_id.attr,
	&dev_attr_isp_log.attr,
    	&dev_attr_isp_fw_version.attr,
    	&dev_attr_m10mo_log_enable.attr,
    	&dev_attr_isp_fw_customer_project.attr,
    	&dev_attr_isp_fw_debug.attr,
	&dev_attr_isp_fw_default_flag.attr,
	&dev_attr_m10mo_download_trace_log_when_time_out.attr,
	&dev_attr_m10mo_asus_camera_flag.attr,
	&dev_attr_isp_shading_table_checksum.attr,
#ifndef ZX551ML_USER_BUILD //#ifdef CONFIG_ASUS_FACTORY_MODE
	&dev_attr_isp_optical_zoom.attr,
	&dev_attr_isp_digital_zoom.attr,
	&dev_attr_m10mo_i2c_debug_byte_write_change_name_by_jevian.attr,
	&dev_attr_m10mo_i2c_debug_state.attr,
	&dev_attr_isp_apk_capture_mode.attr,
	&dev_attr_m10mo_i2c_debug_byte_read_change_name_by_jevian.attr,
	&dev_attr_isp_distortion_mode.attr,
	&dev_attr_isp_flash1_test_brightness.attr,
	&dev_attr_isp_flash2_test_brightness.attr,
	&dev_attr_isp_torch1_test_brightness.attr,
	&dev_attr_isp_torch2_test_brightness.attr,
	&dev_attr_isp_flash1_test_timeout.attr,
	&dev_attr_isp_flash2_test_timeout.attr,
	&dev_attr_isp_ois_data.attr,
	&dev_attr_isp_led_test.attr,
	&dev_attr_isp_ois_write_read_trig.attr,
	&dev_attr_isp_ois_ramreg_addr.attr,
	&dev_attr_isp_ois_lib_api_start.attr,
	&dev_attr_isp_ois_cali_start.attr,
	&dev_attr_isp_ois_cali_result.attr,
	&dev_attr_isp_cap_test.attr,
	&dev_attr_isp_pr_test.attr,
	&dev_attr_isp_pr_led.attr,
	&dev_attr_isp_maunal_focus_ctrl.attr,
	&dev_attr_isp_maunal_zoom_ctrl.attr,
	&dev_attr_isp_led_status.attr,
	&dev_attr_isp_sensor_update_en.attr,
	&dev_attr_isp_sensor_nr_en.attr,
	&dev_attr_isp_af_start.attr,
	&dev_attr_isp_af_operation_result.attr,
	&dev_attr_isp_af_laser_start.attr,
	&dev_attr_isp_af_laser_distance.attr,
	&dev_attr_isp_asus_exposure_control_set.attr,
	&dev_attr_isp_asus_exposure_control.attr,
	&dev_attr_isp_fw_RW.attr,
	&dev_attr_isp_optical_zoom_cur.attr,
	&dev_attr_isp_ae_mode.attr,
	&dev_attr_isp_ae_target.attr,
	&dev_attr_isp_ae_speed.attr,
	&dev_attr_isp_asus_awb_control_set.attr,
	&dev_attr_isp_asus_awb_control.attr,
	&dev_attr_isp_exif_info_exptime.attr,
	&dev_attr_isp_exif_info_iso.attr,
	&dev_attr_isp_home_searching.attr,
	&dev_attr_isp_fw_SHD_RW.attr,
	&dev_attr_m10mo_power_ctrl.attr,
	&dev_attr_m10mo_optical_zoom_cancel.attr,
	&dev_attr_m10mo_auto_focus_cancel.attr,
	&dev_attr_m10mo_debug_password.attr,
	&dev_attr_isp_fw_iq_RW.attr,
	&dev_attr_isp_fw_power_fac.attr,
#endif
	NULL
};

struct attribute_group m10mo_attribute_group[1] = {
	{.attrs = sysfs_attrs_ctrl },
};