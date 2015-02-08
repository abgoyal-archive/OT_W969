

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bitmap.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/mm.h>
#include <linux/mutex.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-chip-ident.h>

#include "cx231xx.h"


static int afe_write_byte(struct cx231xx *dev, u16 saddr, u8 data)
{
	return cx231xx_write_i2c_data(dev, AFE_DEVICE_ADDRESS,
					saddr, 2, data, 1);
}

static int afe_read_byte(struct cx231xx *dev, u16 saddr, u8 *data)
{
	int status;
	u32 temp = 0;

	status = cx231xx_read_i2c_data(dev, AFE_DEVICE_ADDRESS,
					saddr, 2, &temp, 1);
	*data = (u8) temp;
	return status;
}

int cx231xx_afe_init_super_block(struct cx231xx *dev, u32 ref_count)
{
	int status = 0;
	u8 temp = 0;
	u8 afe_power_status = 0;
	int i = 0;

	/* super block initialize */
	temp = (u8) (ref_count & 0xff);
	status = afe_write_byte(dev, SUP_BLK_TUNE2, temp);
	if (status < 0)
		return status;

	status = afe_read_byte(dev, SUP_BLK_TUNE2, &afe_power_status);
	if (status < 0)
		return status;

	temp = (u8) ((ref_count & 0x300) >> 8);
	temp |= 0x40;
	status = afe_write_byte(dev, SUP_BLK_TUNE1, temp);
	if (status < 0)
		return status;

	status = afe_write_byte(dev, SUP_BLK_PLL2, 0x0f);
	if (status < 0)
		return status;

	/* enable pll     */
	while (afe_power_status != 0x18) {
		status = afe_write_byte(dev, SUP_BLK_PWRDN, 0x18);
		if (status < 0) {
			cx231xx_info(
			": Init Super Block failed in send cmd\n");
			break;
		}

		status = afe_read_byte(dev, SUP_BLK_PWRDN, &afe_power_status);
		afe_power_status &= 0xff;
		if (status < 0) {
			cx231xx_info(
			": Init Super Block failed in receive cmd\n");
			break;
		}
		i++;
		if (i == 10) {
			cx231xx_info(
			": Init Super Block force break in loop !!!!\n");
			status = -1;
			break;
		}
	}

	if (status < 0)
		return status;

	/* start tuning filter */
	status = afe_write_byte(dev, SUP_BLK_TUNE3, 0x40);
	if (status < 0)
		return status;

	msleep(5);

	/* exit tuning */
	status = afe_write_byte(dev, SUP_BLK_TUNE3, 0x00);

	return status;
}

int cx231xx_afe_init_channels(struct cx231xx *dev)
{
	int status = 0;

	/* power up all 3 channels, clear pd_buffer */
	status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1, 0x00);
	status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2, 0x00);
	status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3, 0x00);

	/* Enable quantizer calibration */
	status = afe_write_byte(dev, ADC_COM_QUANT, 0x02);

	/* channel initialize, force modulator (fb) reset */
	status = afe_write_byte(dev, ADC_FB_FRCRST_CH1, 0x17);
	status = afe_write_byte(dev, ADC_FB_FRCRST_CH2, 0x17);
	status = afe_write_byte(dev, ADC_FB_FRCRST_CH3, 0x17);

	/* start quantilizer calibration  */
	status = afe_write_byte(dev, ADC_CAL_ATEST_CH1, 0x10);
	status = afe_write_byte(dev, ADC_CAL_ATEST_CH2, 0x10);
	status = afe_write_byte(dev, ADC_CAL_ATEST_CH3, 0x10);
	msleep(5);

	/* exit modulator (fb) reset */
	status = afe_write_byte(dev, ADC_FB_FRCRST_CH1, 0x07);
	status = afe_write_byte(dev, ADC_FB_FRCRST_CH2, 0x07);
	status = afe_write_byte(dev, ADC_FB_FRCRST_CH3, 0x07);

	/* enable the pre_clamp in each channel for single-ended input */
	status = afe_write_byte(dev, ADC_NTF_PRECLMP_EN_CH1, 0xf0);
	status = afe_write_byte(dev, ADC_NTF_PRECLMP_EN_CH2, 0xf0);
	status = afe_write_byte(dev, ADC_NTF_PRECLMP_EN_CH3, 0xf0);

	/* use diode instead of resistor, so set term_en to 0, res_en to 0  */
	status = cx231xx_reg_mask_write(dev, AFE_DEVICE_ADDRESS, 8,
				   ADC_QGAIN_RES_TRM_CH1, 3, 7, 0x00);
	status = cx231xx_reg_mask_write(dev, AFE_DEVICE_ADDRESS, 8,
				   ADC_QGAIN_RES_TRM_CH2, 3, 7, 0x00);
	status = cx231xx_reg_mask_write(dev, AFE_DEVICE_ADDRESS, 8,
				   ADC_QGAIN_RES_TRM_CH3, 3, 7, 0x00);

	/* dynamic element matching off */
	status = afe_write_byte(dev, ADC_DCSERVO_DEM_CH1, 0x03);
	status = afe_write_byte(dev, ADC_DCSERVO_DEM_CH2, 0x03);
	status = afe_write_byte(dev, ADC_DCSERVO_DEM_CH3, 0x03);

	return status;
}

int cx231xx_afe_setup_AFE_for_baseband(struct cx231xx *dev)
{
	u8 c_value = 0;
	int status = 0;

	status = afe_read_byte(dev, ADC_PWRDN_CLAMP_CH2, &c_value);
	c_value &= (~(0x50));
	status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2, c_value);

	return status;
}

int cx231xx_afe_set_input_mux(struct cx231xx *dev, u32 input_mux)
{
	u8 ch1_setting = (u8) input_mux;
	u8 ch2_setting = (u8) (input_mux >> 8);
	u8 ch3_setting = (u8) (input_mux >> 16);
	int status = 0;
	u8 value = 0;

	if (ch1_setting != 0) {
		status = afe_read_byte(dev, ADC_INPUT_CH1, &value);
		value &= (!INPUT_SEL_MASK);
		value |= (ch1_setting - 1) << 4;
		value &= 0xff;
		status = afe_write_byte(dev, ADC_INPUT_CH1, value);
	}

	if (ch2_setting != 0) {
		status = afe_read_byte(dev, ADC_INPUT_CH2, &value);
		value &= (!INPUT_SEL_MASK);
		value |= (ch2_setting - 1) << 4;
		value &= 0xff;
		status = afe_write_byte(dev, ADC_INPUT_CH2, value);
	}

	/* For ch3_setting, the value to put in the register is
	   7 less than the input number */
	if (ch3_setting != 0) {
		status = afe_read_byte(dev, ADC_INPUT_CH3, &value);
		value &= (!INPUT_SEL_MASK);
		value |= (ch3_setting - 1) << 4;
		value &= 0xff;
		status = afe_write_byte(dev, ADC_INPUT_CH3, value);
	}

	return status;
}

int cx231xx_afe_set_mode(struct cx231xx *dev, enum AFE_MODE mode)
{
	int status = 0;

	/*
	* FIXME: We need to implement the AFE code for LOW IF and for HI IF.
	* Currently, only baseband works.
	*/

	switch (mode) {
	case AFE_MODE_LOW_IF:
		/* SetupAFEforLowIF();  */
		break;
	case AFE_MODE_BASEBAND:
		status = cx231xx_afe_setup_AFE_for_baseband(dev);
		break;
	case AFE_MODE_EU_HI_IF:
		/* SetupAFEforEuHiIF(); */
		break;
	case AFE_MODE_US_HI_IF:
		/* SetupAFEforUsHiIF(); */
		break;
	case AFE_MODE_JAPAN_HI_IF:
		/* SetupAFEforJapanHiIF(); */
		break;
	}

	if ((mode != dev->afe_mode) &&
		(dev->video_input == CX231XX_VMUX_TELEVISION))
		status = cx231xx_afe_adjust_ref_count(dev,
						     CX231XX_VMUX_TELEVISION);

	dev->afe_mode = mode;

	return status;
}

int cx231xx_afe_update_power_control(struct cx231xx *dev,
					enum AV_MODE avmode)
{
	u8 afe_power_status = 0;
	int status = 0;

	switch (dev->model) {
	case CX231XX_BOARD_CNXT_RDE_250:
	case CX231XX_BOARD_CNXT_RDU_250:
		if (avmode == POLARIS_AVMODE_ANALOGT_TV) {
			while (afe_power_status != (FLD_PWRDN_TUNING_BIAS |
						FLD_PWRDN_ENABLE_PLL)) {
				status = afe_write_byte(dev, SUP_BLK_PWRDN,
							FLD_PWRDN_TUNING_BIAS |
							FLD_PWRDN_ENABLE_PLL);
				status |= afe_read_byte(dev, SUP_BLK_PWRDN,
							&afe_power_status);
				if (status < 0)
					break;
			}

			status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1,
							0x00);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2,
							0x00);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3,
							0x00);
		} else if (avmode == POLARIS_AVMODE_DIGITAL) {
			status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1,
							0x70);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2,
							0x70);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3,
							0x70);

			status |= afe_read_byte(dev, SUP_BLK_PWRDN,
						  &afe_power_status);
			afe_power_status |= FLD_PWRDN_PD_BANDGAP |
						FLD_PWRDN_PD_BIAS |
						FLD_PWRDN_PD_TUNECK;
			status |= afe_write_byte(dev, SUP_BLK_PWRDN,
						   afe_power_status);
		} else if (avmode == POLARIS_AVMODE_ENXTERNAL_AV) {
			while (afe_power_status != (FLD_PWRDN_TUNING_BIAS |
						FLD_PWRDN_ENABLE_PLL)) {
				status = afe_write_byte(dev, SUP_BLK_PWRDN,
							FLD_PWRDN_TUNING_BIAS |
							FLD_PWRDN_ENABLE_PLL);
				status |= afe_read_byte(dev, SUP_BLK_PWRDN,
							&afe_power_status);
				if (status < 0)
					break;
			}

			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1,
						0x00);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2,
						0x00);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3,
						0x00);
		} else {
			cx231xx_info("Invalid AV mode input\n");
			status = -1;
		}
		break;
	default:
		if (avmode == POLARIS_AVMODE_ANALOGT_TV) {
			while (afe_power_status != (FLD_PWRDN_TUNING_BIAS |
						FLD_PWRDN_ENABLE_PLL)) {
				status = afe_write_byte(dev, SUP_BLK_PWRDN,
							FLD_PWRDN_TUNING_BIAS |
							FLD_PWRDN_ENABLE_PLL);
				status |= afe_read_byte(dev, SUP_BLK_PWRDN,
							&afe_power_status);
				if (status < 0)
					break;
			}

			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1,
							0x40);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2,
							0x40);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3,
							0x00);
		} else if (avmode == POLARIS_AVMODE_DIGITAL) {
			status = afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1,
							0x70);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2,
							0x70);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3,
							0x70);

			status |= afe_read_byte(dev, SUP_BLK_PWRDN,
						       &afe_power_status);
			afe_power_status |= FLD_PWRDN_PD_BANDGAP |
						FLD_PWRDN_PD_BIAS |
						FLD_PWRDN_PD_TUNECK;
			status |= afe_write_byte(dev, SUP_BLK_PWRDN,
							afe_power_status);
		} else if (avmode == POLARIS_AVMODE_ENXTERNAL_AV) {
			while (afe_power_status != (FLD_PWRDN_TUNING_BIAS |
						FLD_PWRDN_ENABLE_PLL)) {
				status = afe_write_byte(dev, SUP_BLK_PWRDN,
							FLD_PWRDN_TUNING_BIAS |
							FLD_PWRDN_ENABLE_PLL);
				status |= afe_read_byte(dev, SUP_BLK_PWRDN,
							&afe_power_status);
				if (status < 0)
					break;
			}

			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH1,
							0x00);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH2,
							0x00);
			status |= afe_write_byte(dev, ADC_PWRDN_CLAMP_CH3,
							0x40);
		} else {
			cx231xx_info("Invalid AV mode input\n");
			status = -1;
		}
	}			/* switch  */

	return status;
}

int cx231xx_afe_adjust_ref_count(struct cx231xx *dev, u32 video_input)
{
	u8 input_mode = 0;
	u8 ntf_mode = 0;
	int status = 0;

	dev->video_input = video_input;

	if (video_input == CX231XX_VMUX_TELEVISION) {
		status = afe_read_byte(dev, ADC_INPUT_CH3, &input_mode);
		status = afe_read_byte(dev, ADC_NTF_PRECLMP_EN_CH3,
					&ntf_mode);
	} else {
		status = afe_read_byte(dev, ADC_INPUT_CH1, &input_mode);
		status = afe_read_byte(dev, ADC_NTF_PRECLMP_EN_CH1,
					&ntf_mode);
	}

	input_mode = (ntf_mode & 0x3) | ((input_mode & 0x6) << 1);

	switch (input_mode) {
	case SINGLE_ENDED:
		dev->afe_ref_count = 0x23C;
		break;
	case LOW_IF:
		dev->afe_ref_count = 0x24C;
		break;
	case EU_IF:
		dev->afe_ref_count = 0x258;
		break;
	case US_IF:
		dev->afe_ref_count = 0x260;
		break;
	default:
		break;
	}

	status = cx231xx_afe_init_super_block(dev, dev->afe_ref_count);

	return status;
}

static int vid_blk_write_byte(struct cx231xx *dev, u16 saddr, u8 data)
{
	return cx231xx_write_i2c_data(dev, VID_BLK_I2C_ADDRESS,
					saddr, 2, data, 1);
}

static int vid_blk_read_byte(struct cx231xx *dev, u16 saddr, u8 *data)
{
	int status;
	u32 temp = 0;

	status = cx231xx_read_i2c_data(dev, VID_BLK_I2C_ADDRESS,
					saddr, 2, &temp, 1);
	*data = (u8) temp;
	return status;
}

static int vid_blk_write_word(struct cx231xx *dev, u16 saddr, u32 data)
{
	return cx231xx_write_i2c_data(dev, VID_BLK_I2C_ADDRESS,
					saddr, 2, data, 4);
}

static int vid_blk_read_word(struct cx231xx *dev, u16 saddr, u32 *data)
{
	return cx231xx_read_i2c_data(dev, VID_BLK_I2C_ADDRESS,
					saddr, 2, data, 4);
}

int cx231xx_set_video_input_mux(struct cx231xx *dev, u8 input)
{
	int status = 0;

	switch (INPUT(input)->type) {
	case CX231XX_VMUX_COMPOSITE1:
	case CX231XX_VMUX_SVIDEO:
		if ((dev->current_pcb_config.type == USB_BUS_POWER) &&
		    (dev->power_mode != POLARIS_AVMODE_ENXTERNAL_AV)) {
			/* External AV */
			status = cx231xx_set_power_mode(dev,
					POLARIS_AVMODE_ENXTERNAL_AV);
			if (status < 0) {
				cx231xx_errdev("%s: set_power_mode : Failed to"
						" set Power - errCode [%d]!\n",
						__func__, status);
				return status;
			}
		}
		status = cx231xx_set_decoder_video_input(dev,
							 INPUT(input)->type,
							 INPUT(input)->vmux);
		break;
	case CX231XX_VMUX_TELEVISION:
	case CX231XX_VMUX_CABLE:
		if ((dev->current_pcb_config.type == USB_BUS_POWER) &&
		    (dev->power_mode != POLARIS_AVMODE_ANALOGT_TV)) {
			/* Tuner */
			status = cx231xx_set_power_mode(dev,
						POLARIS_AVMODE_ANALOGT_TV);
			if (status < 0) {
				cx231xx_errdev("%s: set_power_mode:Failed"
					" to set Power - errCode [%d]!\n",
					__func__, status);
				return status;
			}
		}
		status = cx231xx_set_decoder_video_input(dev,
							CX231XX_VMUX_COMPOSITE1,
							INPUT(input)->vmux);
		break;
	default:
		cx231xx_errdev("%s: set_power_mode : Unknown Input %d !\n",
		     __func__, INPUT(input)->type);
		break;
	}

	/* save the selection */
	dev->video_input = input;

	return status;
}

int cx231xx_set_decoder_video_input(struct cx231xx *dev,
				u8 pin_type, u8 input)
{
	int status = 0;
	u32 value = 0;

	if (pin_type != dev->video_input) {
		status = cx231xx_afe_adjust_ref_count(dev, pin_type);
		if (status < 0) {
			cx231xx_errdev("%s: adjust_ref_count :Failed to set"
				"AFE input mux - errCode [%d]!\n",
				__func__, status);
			return status;
		}
	}

	/* call afe block to set video inputs */
	status = cx231xx_afe_set_input_mux(dev, input);
	if (status < 0) {
		cx231xx_errdev("%s: set_input_mux :Failed to set"
				" AFE input mux - errCode [%d]!\n",
				__func__, status);
		return status;
	}

	switch (pin_type) {
	case CX231XX_VMUX_COMPOSITE1:
		status = vid_blk_read_word(dev, AFE_CTRL, &value);
		value |= (0 << 13) | (1 << 4);
		value &= ~(1 << 5);

		/* set [24:23] [22:15] to 0  */
		value &= (~(0x1ff8000));
		/* set FUNC_MODE[24:23] = 2 IF_MOD[22:15] = 0  */
		value |= 0x1000000;
		status = vid_blk_write_word(dev, AFE_CTRL, value);

		status = vid_blk_read_word(dev, OUT_CTRL1, &value);
		value |= (1 << 7);
		status = vid_blk_write_word(dev, OUT_CTRL1, value);

		/* Set vip 1.1 output mode */
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							OUT_CTRL1,
							FLD_OUT_MODE,
							OUT_MODE_VIP11);

		/* Tell DIF object to go to baseband mode  */
		status = cx231xx_dif_set_standard(dev, DIF_USE_BASEBAND);
		if (status < 0) {
			cx231xx_errdev("%s: cx231xx_dif set to By pass"
						   " mode- errCode [%d]!\n",
				__func__, status);
			return status;
		}

		/* Read the DFE_CTRL1 register */
		status = vid_blk_read_word(dev, DFE_CTRL1, &value);

		/* enable the VBI_GATE_EN */
		value |= FLD_VBI_GATE_EN;

		/* Enable the auto-VGA enable */
		value |= FLD_VGA_AUTO_EN;

		/* Write it back */
		status = vid_blk_write_word(dev, DFE_CTRL1, value);

		/* Disable auto config of registers */
		status = cx231xx_read_modify_write_i2c_dword(dev,
					VID_BLK_I2C_ADDRESS,
					MODE_CTRL, FLD_ACFG_DIS,
					cx231xx_set_field(FLD_ACFG_DIS, 1));

		/* Set CVBS input mode */
		status = cx231xx_read_modify_write_i2c_dword(dev,
			VID_BLK_I2C_ADDRESS,
			MODE_CTRL, FLD_INPUT_MODE,
			cx231xx_set_field(FLD_INPUT_MODE, INPUT_MODE_CVBS_0));
		break;
	case CX231XX_VMUX_SVIDEO:
		/* Disable the use of  DIF */

		status = vid_blk_read_word(dev, AFE_CTRL, &value);

		/* set [24:23] [22:15] to 0 */
		value &= (~(0x1ff8000));
		/* set FUNC_MODE[24:23] = 2
		IF_MOD[22:15] = 0 DCR_BYP_CH2[4:4] = 1; */
		value |= 0x1000010;
		status = vid_blk_write_word(dev, AFE_CTRL, value);

		/* Tell DIF object to go to baseband mode */
		status = cx231xx_dif_set_standard(dev, DIF_USE_BASEBAND);
		if (status < 0) {
			cx231xx_errdev("%s: cx231xx_dif set to By pass"
						   " mode- errCode [%d]!\n",
				__func__, status);
			return status;
		}

		/* Read the DFE_CTRL1 register */
		status = vid_blk_read_word(dev, DFE_CTRL1, &value);

		/* enable the VBI_GATE_EN */
		value |= FLD_VBI_GATE_EN;

		/* Enable the auto-VGA enable */
		value |= FLD_VGA_AUTO_EN;

		/* Write it back */
		status = vid_blk_write_word(dev, DFE_CTRL1, value);

		/* Disable auto config of registers  */
		status =  cx231xx_read_modify_write_i2c_dword(dev,
					VID_BLK_I2C_ADDRESS,
					MODE_CTRL, FLD_ACFG_DIS,
					cx231xx_set_field(FLD_ACFG_DIS, 1));

		/* Set YC input mode */
		status = cx231xx_read_modify_write_i2c_dword(dev,
			VID_BLK_I2C_ADDRESS,
			MODE_CTRL,
			FLD_INPUT_MODE,
			cx231xx_set_field(FLD_INPUT_MODE, INPUT_MODE_YC_1));

		/* Chroma to ADC2 */
		status = vid_blk_read_word(dev, AFE_CTRL, &value);
		value |= FLD_CHROMA_IN_SEL;	/* set the chroma in select */

		/* Clear VGA_SEL_CH2 and VGA_SEL_CH3 (bits 7 and 8)
		   This sets them to use video
		   rather than audio.  Only one of the two will be in use. */
		value &= ~(FLD_VGA_SEL_CH2 | FLD_VGA_SEL_CH3);

		status = vid_blk_write_word(dev, AFE_CTRL, value);

		status = cx231xx_afe_set_mode(dev, AFE_MODE_BASEBAND);
		break;
	case CX231XX_VMUX_TELEVISION:
	case CX231XX_VMUX_CABLE:
	default:
		switch (dev->model) {
		case CX231XX_BOARD_CNXT_RDE_250:
		case CX231XX_BOARD_CNXT_RDU_250:
			/* Disable the use of  DIF   */

			status = vid_blk_read_word(dev, AFE_CTRL, &value);
			value |= (0 << 13) | (1 << 4);
			value &= ~(1 << 5);

			/* set [24:23] [22:15] to 0 */
			value &= (~(0x1FF8000));
			/* set FUNC_MODE[24:23] = 2 IF_MOD[22:15] = 0 */
			value |= 0x1000000;
			status = vid_blk_write_word(dev, AFE_CTRL, value);

			status = vid_blk_read_word(dev, OUT_CTRL1, &value);
			value |= (1 << 7);
			status = vid_blk_write_word(dev, OUT_CTRL1, value);

			/* Set vip 1.1 output mode */
			status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							OUT_CTRL1, FLD_OUT_MODE,
							OUT_MODE_VIP11);

			/* Tell DIF object to go to baseband mode */
			status = cx231xx_dif_set_standard(dev,
							  DIF_USE_BASEBAND);
			if (status < 0) {
				cx231xx_errdev("%s: cx231xx_dif set to By pass"
						" mode- errCode [%d]!\n",
						__func__, status);
				return status;
			}

			/* Read the DFE_CTRL1 register */
			status = vid_blk_read_word(dev, DFE_CTRL1, &value);

			/* enable the VBI_GATE_EN */
			value |= FLD_VBI_GATE_EN;

			/* Enable the auto-VGA enable */
			value |= FLD_VGA_AUTO_EN;

			/* Write it back */
			status = vid_blk_write_word(dev, DFE_CTRL1, value);

			/* Disable auto config of registers */
			status = cx231xx_read_modify_write_i2c_dword(dev,
					VID_BLK_I2C_ADDRESS,
					MODE_CTRL, FLD_ACFG_DIS,
					cx231xx_set_field(FLD_ACFG_DIS, 1));

			/* Set CVBS input mode */
			status = cx231xx_read_modify_write_i2c_dword(dev,
				VID_BLK_I2C_ADDRESS,
				MODE_CTRL, FLD_INPUT_MODE,
				cx231xx_set_field(FLD_INPUT_MODE,
						INPUT_MODE_CVBS_0));
			break;
		default:
			/* Enable the DIF for the tuner */

			/* Reinitialize the DIF */
			status = cx231xx_dif_set_standard(dev, dev->norm);
			if (status < 0) {
				cx231xx_errdev("%s: cx231xx_dif set to By pass"
						" mode- errCode [%d]!\n",
						__func__, status);
				return status;
			}

			/* Make sure bypass is cleared */
			status = vid_blk_read_word(dev, DIF_MISC_CTRL, &value);

			/* Clear the bypass bit */
			value &= ~FLD_DIF_DIF_BYPASS;

			/* Enable the use of the DIF block */
			status = vid_blk_write_word(dev, DIF_MISC_CTRL, value);

			/* Read the DFE_CTRL1 register */
			status = vid_blk_read_word(dev, DFE_CTRL1, &value);

			/* Disable the VBI_GATE_EN */
			value &= ~FLD_VBI_GATE_EN;

			/* Enable the auto-VGA enable, AGC, and
			   set the skip count to 2 */
			value |= FLD_VGA_AUTO_EN | FLD_AGC_AUTO_EN | 0x00200000;

			/* Write it back */
			status = vid_blk_write_word(dev, DFE_CTRL1, value);

			/* Wait until AGC locks up */
			msleep(1);

			/* Disable the auto-VGA enable AGC */
			value &= ~(FLD_VGA_AUTO_EN);

			/* Write it back */
			status = vid_blk_write_word(dev, DFE_CTRL1, value);

			/* Enable Polaris B0 AGC output */
			status = vid_blk_read_word(dev, PIN_CTRL, &value);
			value |= (FLD_OEF_AGC_RF) |
				 (FLD_OEF_AGC_IFVGA) |
				 (FLD_OEF_AGC_IF);
			status = vid_blk_write_word(dev, PIN_CTRL, value);

			/* Set vip 1.1 output mode */
			status = cx231xx_read_modify_write_i2c_dword(dev,
						VID_BLK_I2C_ADDRESS,
						OUT_CTRL1, FLD_OUT_MODE,
						OUT_MODE_VIP11);

			/* Disable auto config of registers */
			status = cx231xx_read_modify_write_i2c_dword(dev,
					VID_BLK_I2C_ADDRESS,
					MODE_CTRL, FLD_ACFG_DIS,
					cx231xx_set_field(FLD_ACFG_DIS, 1));

			/* Set CVBS input mode */
			status = cx231xx_read_modify_write_i2c_dword(dev,
				VID_BLK_I2C_ADDRESS,
				MODE_CTRL, FLD_INPUT_MODE,
				cx231xx_set_field(FLD_INPUT_MODE,
						INPUT_MODE_CVBS_0));

			/* Set some bits in AFE_CTRL so that channel 2 or 3
			 * is ready to receive audio */
			/* Clear clamp for channels 2 and 3      (bit 16-17) */
			/* Clear droop comp                      (bit 19-20) */
			/* Set VGA_SEL (for audio control)       (bit 7-8) */
			status = vid_blk_read_word(dev, AFE_CTRL, &value);

			value |= FLD_VGA_SEL_CH3 | FLD_VGA_SEL_CH2;

			status = vid_blk_write_word(dev, AFE_CTRL, value);
			break;

		}
		break;
	}

	/* Set raw VBI mode */
	status = cx231xx_read_modify_write_i2c_dword(dev,
				VID_BLK_I2C_ADDRESS,
				OUT_CTRL1, FLD_VBIHACTRAW_EN,
				cx231xx_set_field(FLD_VBIHACTRAW_EN, 1));

	status = vid_blk_read_word(dev, OUT_CTRL1, &value);
	if (value & 0x02) {
		value |= (1 << 19);
		status = vid_blk_write_word(dev, OUT_CTRL1, value);
	}

	return status;
}

int cx231xx_do_mode_ctrl_overrides(struct cx231xx *dev)
{
	int status = 0;

	cx231xx_info("do_mode_ctrl_overrides : 0x%x\n",
		     (unsigned int)dev->norm);

	/* Change the DFE_CTRL3 bp_percent to fix flagging */
	status = vid_blk_write_word(dev, DFE_CTRL3, 0xCD3F0280);

	if (dev->norm & (V4L2_STD_NTSC | V4L2_STD_PAL_M)) {
		cx231xx_info("do_mode_ctrl_overrides NTSC\n");

		/* Move the close caption lines out of active video,
		   adjust the active video start point */
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							VERT_TIM_CTRL,
							FLD_VBLANK_CNT, 0x18);
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							VERT_TIM_CTRL,
							FLD_VACTIVE_CNT,
							0x1E6000);
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							VERT_TIM_CTRL,
							FLD_V656BLANK_CNT,
							0x1E000000);

		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							HORIZ_TIM_CTRL,
							FLD_HBLANK_CNT,
							cx231xx_set_field
							(FLD_HBLANK_CNT, 0x79));
	} else if (dev->norm & V4L2_STD_SECAM) {
		cx231xx_info("do_mode_ctrl_overrides SECAM\n");
		status =  cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							VERT_TIM_CTRL,
							FLD_VBLANK_CNT, 0x24);
		/* Adjust the active video horizontal start point */
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							HORIZ_TIM_CTRL,
							FLD_HBLANK_CNT,
							cx231xx_set_field
							(FLD_HBLANK_CNT, 0x85));
	} else {
		cx231xx_info("do_mode_ctrl_overrides PAL\n");
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							VERT_TIM_CTRL,
							FLD_VBLANK_CNT, 0x24);
		/* Adjust the active video horizontal start point */
		status = cx231xx_read_modify_write_i2c_dword(dev,
							VID_BLK_I2C_ADDRESS,
							HORIZ_TIM_CTRL,
							FLD_HBLANK_CNT,
							cx231xx_set_field
							(FLD_HBLANK_CNT, 0x85));
	}

	return status;
}

int cx231xx_set_audio_input(struct cx231xx *dev, u8 input)
{
	int status = 0;
	enum AUDIO_INPUT ainput = AUDIO_INPUT_LINE;

	switch (INPUT(input)->amux) {
	case CX231XX_AMUX_VIDEO:
		ainput = AUDIO_INPUT_TUNER_TV;
		break;
	case CX231XX_AMUX_LINE_IN:
		status = cx231xx_i2s_blk_set_audio_input(dev, input);
		ainput = AUDIO_INPUT_LINE;
		break;
	default:
		break;
	}

	status = cx231xx_set_audio_decoder_input(dev, ainput);

	return status;
}

int cx231xx_set_audio_decoder_input(struct cx231xx *dev,
				    enum AUDIO_INPUT audio_input)
{
	u32 dwval;
	int status;
	u8 gen_ctrl;
	u32 value = 0;

	/* Put it in soft reset   */
	status = vid_blk_read_byte(dev, GENERAL_CTL, &gen_ctrl);
	gen_ctrl |= 1;
	status = vid_blk_write_byte(dev, GENERAL_CTL, gen_ctrl);

	switch (audio_input) {
	case AUDIO_INPUT_LINE:
		/* setup AUD_IO control from Merlin paralle output */
		value = cx231xx_set_field(FLD_AUD_CHAN1_SRC,
					  AUD_CHAN_SRC_PARALLEL);
		status = vid_blk_write_word(dev, AUD_IO_CTRL, value);

		/* setup input to Merlin, SRC2 connect to AC97
		   bypass upsample-by-2, slave mode, sony mode, left justify
		   adr 091c, dat 01000000 */
		status = vid_blk_read_word(dev, AC97_CTL, &dwval);

		status = vid_blk_write_word(dev, AC97_CTL,
					   (dwval | FLD_AC97_UP2X_BYPASS));

		/* select the parallel1 and SRC3 */
		status = vid_blk_write_word(dev, BAND_OUT_SEL,
				cx231xx_set_field(FLD_SRC3_IN_SEL, 0x0) |
				cx231xx_set_field(FLD_SRC3_CLK_SEL, 0x0) |
				cx231xx_set_field(FLD_PARALLEL1_SRC_SEL, 0x0));

		/* unmute all, AC97 in, independence mode
		   adr 08d0, data 0x00063073 */
		status = vid_blk_write_word(dev, PATH1_CTL1, 0x00063073);

		/* set AVC maximum threshold, adr 08d4, dat ffff0024 */
		status = vid_blk_read_word(dev, PATH1_VOL_CTL, &dwval);
		status = vid_blk_write_word(dev, PATH1_VOL_CTL,
					   (dwval | FLD_PATH1_AVC_THRESHOLD));

		/* set SC maximum threshold, adr 08ec, dat ffffb3a3 */
		status = vid_blk_read_word(dev, PATH1_SC_CTL, &dwval);
		status = vid_blk_write_word(dev, PATH1_SC_CTL,
					   (dwval | FLD_PATH1_SC_THRESHOLD));
		break;

	case AUDIO_INPUT_TUNER_TV:
	default:

		/* Setup SRC sources and clocks */
		status = vid_blk_write_word(dev, BAND_OUT_SEL,
			cx231xx_set_field(FLD_SRC6_IN_SEL, 0x00)         |
			cx231xx_set_field(FLD_SRC6_CLK_SEL, 0x01)        |
			cx231xx_set_field(FLD_SRC5_IN_SEL, 0x00)         |
			cx231xx_set_field(FLD_SRC5_CLK_SEL, 0x02)        |
			cx231xx_set_field(FLD_SRC4_IN_SEL, 0x02)         |
			cx231xx_set_field(FLD_SRC4_CLK_SEL, 0x03)        |
			cx231xx_set_field(FLD_SRC3_IN_SEL, 0x00)         |
			cx231xx_set_field(FLD_SRC3_CLK_SEL, 0x00)        |
			cx231xx_set_field(FLD_BASEBAND_BYPASS_CTL, 0x00) |
			cx231xx_set_field(FLD_AC97_SRC_SEL, 0x03)        |
			cx231xx_set_field(FLD_I2S_SRC_SEL, 0x00)         |
			cx231xx_set_field(FLD_PARALLEL2_SRC_SEL, 0x02)   |
			cx231xx_set_field(FLD_PARALLEL1_SRC_SEL, 0x01));

		/* Setup the AUD_IO control */
		status = vid_blk_write_word(dev, AUD_IO_CTRL,
			cx231xx_set_field(FLD_I2S_PORT_DIR, 0x00)  |
			cx231xx_set_field(FLD_I2S_OUT_SRC, 0x00)   |
			cx231xx_set_field(FLD_AUD_CHAN3_SRC, 0x00) |
			cx231xx_set_field(FLD_AUD_CHAN2_SRC, 0x00) |
			cx231xx_set_field(FLD_AUD_CHAN1_SRC, 0x03));

		status = vid_blk_write_word(dev, PATH1_CTL1, 0x1F063870);

		/* setAudioStandard(_audio_standard); */

		status = vid_blk_write_word(dev, PATH1_CTL1, 0x00063870);
		switch (dev->model) {
		case CX231XX_BOARD_CNXT_RDE_250:
		case CX231XX_BOARD_CNXT_RDU_250:
			status = cx231xx_read_modify_write_i2c_dword(dev,
					VID_BLK_I2C_ADDRESS,
					CHIP_CTRL,
					FLD_SIF_EN,
					cx231xx_set_field(FLD_SIF_EN, 1));
			break;
		default:
			break;
		}
		break;

	case AUDIO_INPUT_TUNER_FM:
		/*  use SIF for FM radio
		   setupFM();
		   setAudioStandard(_audio_standard);
		 */
		break;

	case AUDIO_INPUT_MUTE:
		status = vid_blk_write_word(dev, PATH1_CTL1, 0x1F011012);
		break;
	}

	/* Take it out of soft reset */
	status = vid_blk_read_byte(dev, GENERAL_CTL, &gen_ctrl);
	gen_ctrl &= ~1;
	status = vid_blk_write_byte(dev, GENERAL_CTL, gen_ctrl);

	return status;
}

/* Set resolution of the video */
int cx231xx_resolution_set(struct cx231xx *dev)
{
	/* set horzontal scale */
	int status = vid_blk_write_word(dev, HSCALE_CTRL, dev->hscale);
	if (status)
		return status;

	/* set vertical scale */
	return vid_blk_write_word(dev, VSCALE_CTRL, dev->vscale);
}

int cx231xx_init_ctrl_pin_status(struct cx231xx *dev)
{
	u32 value;
	int status = 0;

	status = vid_blk_read_word(dev, PIN_CTRL, &value);
	value |= (~dev->board.ctl_pin_status_mask);
	status = vid_blk_write_word(dev, PIN_CTRL, value);

	return status;
}

int cx231xx_set_agc_analog_digital_mux_select(struct cx231xx *dev,
					      u8 analog_or_digital)
{
	int status = 0;

	/* first set the direction to output */
	status = cx231xx_set_gpio_direction(dev,
					    dev->board.
					    agc_analog_digital_select_gpio, 1);

	/* 0 - demod ; 1 - Analog mode */
	status = cx231xx_set_gpio_value(dev,
				   dev->board.agc_analog_digital_select_gpio,
				   analog_or_digital);

	return status;
}

int cx231xx_enable_i2c_for_tuner(struct cx231xx *dev, u8 I2CIndex)
{
	u8 value[4] = { 0, 0, 0, 0 };
	int status = 0;

	cx231xx_info("Changing the i2c port for tuner to %d\n", I2CIndex);

	status = cx231xx_read_ctrl_reg(dev, VRT_GET_REGISTER,
				       PWR_CTL_EN, value, 4);
	if (status < 0)
		return status;

	if (I2CIndex == I2C_1) {
		if (value[0] & I2C_DEMOD_EN) {
			value[0] &= ~I2C_DEMOD_EN;
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						   PWR_CTL_EN, value, 4);
		}
	} else {
		if (!(value[0] & I2C_DEMOD_EN)) {
			value[0] |= I2C_DEMOD_EN;
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						   PWR_CTL_EN, value, 4);
		}
	}

	return status;

}

int cx231xx_dif_configure_C2HH_for_low_IF(struct cx231xx *dev, u32 mode,
					  u32 function_mode, u32 standard)
{
	int status = 0;

	if (mode == V4L2_TUNER_RADIO) {
		/* C2HH */
		/* lo if big signal */
		status = cx231xx_reg_mask_write(dev,
				VID_BLK_I2C_ADDRESS, 32,
				AFE_CTRL_C2HH_SRC_CTRL, 30, 31, 0x1);
		/* FUNC_MODE = DIF */
		status = cx231xx_reg_mask_write(dev,
				VID_BLK_I2C_ADDRESS, 32,
				AFE_CTRL_C2HH_SRC_CTRL, 23, 24, function_mode);
		/* IF_MODE */
		status = cx231xx_reg_mask_write(dev,
				VID_BLK_I2C_ADDRESS, 32,
				AFE_CTRL_C2HH_SRC_CTRL, 15, 22, 0xFF);
		/* no inv */
		status = cx231xx_reg_mask_write(dev,
				VID_BLK_I2C_ADDRESS, 32,
				AFE_CTRL_C2HH_SRC_CTRL, 9, 9, 0x1);
	} else if (standard != DIF_USE_BASEBAND) {
		if (standard & V4L2_STD_MN) {
			/* lo if big signal */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 30, 31, 0x1);
			/* FUNC_MODE = DIF */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 23, 24,
					function_mode);
			/* IF_MODE */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 15, 22, 0xb);
			/* no inv */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 9, 9, 0x1);
			/* 0x124, AUD_CHAN1_SRC = 0x3 */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AUD_IO_CTRL, 0, 31, 0x00000003);
		} else if ((standard == V4L2_STD_PAL_I) |
			(standard & V4L2_STD_SECAM)) {
			/* C2HH setup */
			/* lo if big signal */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 30, 31, 0x1);
			/* FUNC_MODE = DIF */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 23, 24,
					function_mode);
			/* IF_MODE */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 15, 22, 0xF);
			/* no inv */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 9, 9, 0x1);
		} else {
			/* default PAL BG */
			/* C2HH setup */
			/* lo if big signal */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 30, 31, 0x1);
			/* FUNC_MODE = DIF */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 23, 24,
					function_mode);
			/* IF_MODE */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 15, 22, 0xE);
			/* no inv */
			status = cx231xx_reg_mask_write(dev,
					VID_BLK_I2C_ADDRESS, 32,
					AFE_CTRL_C2HH_SRC_CTRL, 9, 9, 0x1);
		}
	}

	return status;
}

int cx231xx_dif_set_standard(struct cx231xx *dev, u32 standard)
{
	int status = 0;
	u32 dif_misc_ctrl_value = 0;
	u32 func_mode = 0;

	cx231xx_info("%s: setStandard to %x\n", __func__, standard);

	status = vid_blk_read_word(dev, DIF_MISC_CTRL, &dif_misc_ctrl_value);
	if (standard != DIF_USE_BASEBAND)
		dev->norm = standard;

	switch (dev->model) {
	case CX231XX_BOARD_CNXT_RDE_250:
	case CX231XX_BOARD_CNXT_RDU_250:
		func_mode = 0x03;
		break;
	default:
		func_mode = 0x01;
	}

	status = cx231xx_dif_configure_C2HH_for_low_IF(dev, dev->active_mode,
						  func_mode, standard);

	if (standard == DIF_USE_BASEBAND) {	/* base band */
		/* There is a different SRC_PHASE_INC value
		   for baseband vs. DIF */
		status = vid_blk_write_word(dev, DIF_SRC_PHASE_INC, 0xDF7DF83);
		status = vid_blk_read_word(dev, DIF_MISC_CTRL,
						&dif_misc_ctrl_value);
		dif_misc_ctrl_value |= FLD_DIF_DIF_BYPASS;
		status = vid_blk_write_word(dev, DIF_MISC_CTRL,
						dif_misc_ctrl_value);
	} else if (standard & V4L2_STD_PAL_D) {
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL, 0, 31, 0x6503bc0c);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL1, 0, 31, 0xbd038c85);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL2, 0, 31, 0x1db4640a);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL3, 0, 31, 0x00008800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_REF, 0, 31, 0x444C1380);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_IF, 0, 31, 0xDA302600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_INT, 0, 31, 0xDA261700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_RF, 0, 31, 0xDA262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_INT_CURRENT, 0, 31,
					   0x26001700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_RF_CURRENT, 0, 31,
					   0x00002660);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VIDEO_AGC_CTRL, 0, 31,
					   0x72500800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VID_AUD_OVERRIDE, 0, 31,
					   0x27000100);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AV_SEP_CTRL, 0, 31, 0x3F3934EA);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_COMP_FLT_CTRL, 0, 31,
					   0x00000000);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_PHASE_INC, 0, 31,
					   0x1befbf06);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_GAIN_CONTROL, 0, 31,
					   0x000035e8);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_RPT_VARIANCE, 0, 31, 0x00000000);
		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3a023F11;
	} else if (standard & V4L2_STD_PAL_I) {
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL, 0, 31, 0x6503bc0c);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL1, 0, 31, 0xbd038c85);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL2, 0, 31, 0x1db4640a);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL3, 0, 31, 0x00008800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_REF, 0, 31, 0x444C1380);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_IF, 0, 31, 0xDA302600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_INT, 0, 31, 0xDA261700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_RF, 0, 31, 0xDA262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_INT_CURRENT, 0, 31,
					   0x26001700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_RF_CURRENT, 0, 31,
					   0x00002660);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VIDEO_AGC_CTRL, 0, 31,
					   0x72500800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VID_AUD_OVERRIDE, 0, 31,
					   0x27000100);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AV_SEP_CTRL, 0, 31, 0x5F39A934);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_COMP_FLT_CTRL, 0, 31,
					   0x00000000);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_PHASE_INC, 0, 31,
					   0x1befbf06);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_GAIN_CONTROL, 0, 31,
					   0x000035e8);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_RPT_VARIANCE, 0, 31, 0x00000000);
		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3a033F11;
	} else if (standard & V4L2_STD_PAL_M) {
		/* improved Low Frequency Phase Noise */
		status = vid_blk_write_word(dev, DIF_PLL_CTRL, 0xFF01FF0C);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL1, 0xbd038c85);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL2, 0x1db4640a);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL3, 0x00008800);
		status = vid_blk_write_word(dev, DIF_AGC_IF_REF, 0x444C1380);
		status = vid_blk_write_word(dev, DIF_AGC_IF_INT_CURRENT,
						0x26001700);
		status = vid_blk_write_word(dev, DIF_AGC_RF_CURRENT,
						0x00002660);
		status = vid_blk_write_word(dev, DIF_VIDEO_AGC_CTRL,
						0x72500800);
		status = vid_blk_write_word(dev, DIF_VID_AUD_OVERRIDE,
						0x27000100);
		status = vid_blk_write_word(dev, DIF_AV_SEP_CTRL, 0x012c405d);
		status = vid_blk_write_word(dev, DIF_COMP_FLT_CTRL,
						0x009f50c1);
		status = vid_blk_write_word(dev, DIF_SRC_PHASE_INC,
						0x1befbf06);
		status = vid_blk_write_word(dev, DIF_SRC_GAIN_CONTROL,
						0x000035e8);
		status = vid_blk_write_word(dev, DIF_SOFT_RST_CTRL_REVB,
						0x00000000);
		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3A0A3F10;
	} else if (standard & (V4L2_STD_PAL_N | V4L2_STD_PAL_Nc)) {
		/* improved Low Frequency Phase Noise */
		status = vid_blk_write_word(dev, DIF_PLL_CTRL, 0xFF01FF0C);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL1, 0xbd038c85);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL2, 0x1db4640a);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL3, 0x00008800);
		status = vid_blk_write_word(dev, DIF_AGC_IF_REF, 0x444C1380);
		status = vid_blk_write_word(dev, DIF_AGC_IF_INT_CURRENT,
						0x26001700);
		status = vid_blk_write_word(dev, DIF_AGC_RF_CURRENT,
						0x00002660);
		status = vid_blk_write_word(dev, DIF_VIDEO_AGC_CTRL,
						0x72500800);
		status = vid_blk_write_word(dev, DIF_VID_AUD_OVERRIDE,
						0x27000100);
		status = vid_blk_write_word(dev, DIF_AV_SEP_CTRL,
						0x012c405d);
		status = vid_blk_write_word(dev, DIF_COMP_FLT_CTRL,
						0x009f50c1);
		status = vid_blk_write_word(dev, DIF_SRC_PHASE_INC,
						0x1befbf06);
		status = vid_blk_write_word(dev, DIF_SRC_GAIN_CONTROL,
						0x000035e8);
		status = vid_blk_write_word(dev, DIF_SOFT_RST_CTRL_REVB,
						0x00000000);
		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value = 0x3A093F10;
	} else if (standard &
		  (V4L2_STD_SECAM_B | V4L2_STD_SECAM_D | V4L2_STD_SECAM_G |
		   V4L2_STD_SECAM_K | V4L2_STD_SECAM_K1)) {

		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL, 0, 31, 0x6503bc0c);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL1, 0, 31, 0xbd038c85);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL2, 0, 31, 0x1db4640a);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL3, 0, 31, 0x00008800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_REF, 0, 31, 0x888C0380);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_IF, 0, 31, 0xe0262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_INT, 0, 31, 0xc2171700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_RF, 0, 31, 0xc2262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_INT_CURRENT, 0, 31,
					   0x26001700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_RF_CURRENT, 0, 31,
					   0x00002660);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VID_AUD_OVERRIDE, 0, 31,
					   0x27000100);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AV_SEP_CTRL, 0, 31, 0x3F3530ec);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_COMP_FLT_CTRL, 0, 31,
					   0x00000000);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_PHASE_INC, 0, 31,
					   0x1befbf06);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_GAIN_CONTROL, 0, 31,
					   0x000035e8);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_RPT_VARIANCE, 0, 31, 0x00000000);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VIDEO_AGC_CTRL, 0, 31,
					   0xf4000000);

		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3a023F11;
	} else if (standard & (V4L2_STD_SECAM_L | V4L2_STD_SECAM_LC)) {
		/* Is it SECAM_L1? */
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL, 0, 31, 0x6503bc0c);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL1, 0, 31, 0xbd038c85);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL2, 0, 31, 0x1db4640a);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL3, 0, 31, 0x00008800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_REF, 0, 31, 0x888C0380);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_IF, 0, 31, 0xe0262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_INT, 0, 31, 0xc2171700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_RF, 0, 31, 0xc2262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_INT_CURRENT, 0, 31,
					   0x26001700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_RF_CURRENT, 0, 31,
					   0x00002660);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VID_AUD_OVERRIDE, 0, 31,
					   0x27000100);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AV_SEP_CTRL, 0, 31, 0x3F3530ec);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_COMP_FLT_CTRL, 0, 31,
					   0x00000000);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_PHASE_INC, 0, 31,
					   0x1befbf06);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_GAIN_CONTROL, 0, 31,
					   0x000035e8);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_RPT_VARIANCE, 0, 31, 0x00000000);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VIDEO_AGC_CTRL, 0, 31,
					   0xf2560000);

		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3a023F11;

	} else if (standard & V4L2_STD_NTSC_M) {
		/* V4L2_STD_NTSC_M (75 IRE Setup) Or
		   V4L2_STD_NTSC_M_JP (Japan,  0 IRE Setup) */

		/* For NTSC the centre frequency of video coming out of
		   sidewinder is around 7.1MHz or 3.6MHz depending on the
		   spectral inversion. so for a non spectrally inverted channel
		   the pll freq word is 0x03420c49
		 */

		status = vid_blk_write_word(dev, DIF_PLL_CTRL, 0x6503BC0C);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL1, 0xBD038C85);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL2, 0x1DB4640A);
		status = vid_blk_write_word(dev, DIF_PLL_CTRL3, 0x00008800);
		status = vid_blk_write_word(dev, DIF_AGC_IF_REF, 0x444C0380);
		status = vid_blk_write_word(dev, DIF_AGC_IF_INT_CURRENT,
						0x26001700);
		status = vid_blk_write_word(dev, DIF_AGC_RF_CURRENT,
						0x00002660);
		status = vid_blk_write_word(dev, DIF_VIDEO_AGC_CTRL,
						0x04000800);
		status = vid_blk_write_word(dev, DIF_VID_AUD_OVERRIDE,
						0x27000100);
		status = vid_blk_write_word(dev, DIF_AV_SEP_CTRL, 0x01296e1f);

		status = vid_blk_write_word(dev, DIF_COMP_FLT_CTRL,
						0x009f50c1);
		status = vid_blk_write_word(dev, DIF_SRC_PHASE_INC,
						0x1befbf06);
		status = vid_blk_write_word(dev, DIF_SRC_GAIN_CONTROL,
						0x000035e8);

		status = vid_blk_write_word(dev, DIF_AGC_CTRL_IF, 0xC2262600);
		status = vid_blk_write_word(dev, DIF_AGC_CTRL_INT,
						0xC2262600);
		status = vid_blk_write_word(dev, DIF_AGC_CTRL_RF, 0xC2262600);

		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3a003F10;
	} else {
		/* default PAL BG */
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL, 0, 31, 0x6503bc0c);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL1, 0, 31, 0xbd038c85);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL2, 0, 31, 0x1db4640a);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_PLL_CTRL3, 0, 31, 0x00008800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_REF, 0, 31, 0x444C1380);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_IF, 0, 31, 0xDA302600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_INT, 0, 31, 0xDA261700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_CTRL_RF, 0, 31, 0xDA262600);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_IF_INT_CURRENT, 0, 31,
					   0x26001700);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AGC_RF_CURRENT, 0, 31,
					   0x00002660);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VIDEO_AGC_CTRL, 0, 31,
					   0x72500800);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_VID_AUD_OVERRIDE, 0, 31,
					   0x27000100);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_AV_SEP_CTRL, 0, 31, 0x3F3530EC);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_COMP_FLT_CTRL, 0, 31,
					   0x00A653A8);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_PHASE_INC, 0, 31,
					   0x1befbf06);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_SRC_GAIN_CONTROL, 0, 31,
					   0x000035e8);
		status = cx231xx_reg_mask_write(dev, VID_BLK_I2C_ADDRESS, 32,
					   DIF_RPT_VARIANCE, 0, 31, 0x00000000);
		/* Save the Spec Inversion value */
		dif_misc_ctrl_value &= FLD_DIF_SPEC_INV;
		dif_misc_ctrl_value |= 0x3a013F11;
	}

	/* The AGC values should be the same for all standards,
	   AUD_SRC_SEL[19] should always be disabled    */
	dif_misc_ctrl_value &= ~FLD_DIF_AUD_SRC_SEL;

	/* It is still possible to get Set Standard calls even when we
	   are in FM mode.
	   This is done to override the value for FM. */
	if (dev->active_mode == V4L2_TUNER_RADIO)
		dif_misc_ctrl_value = 0x7a080000;

	/* Write the calculated value for misc ontrol register      */
	status = vid_blk_write_word(dev, DIF_MISC_CTRL, dif_misc_ctrl_value);

	return status;
}

int cx231xx_tuner_pre_channel_change(struct cx231xx *dev)
{
	int status = 0;
	u32 dwval;

	/* Set the RF and IF k_agc values to 3 */
	status = vid_blk_read_word(dev, DIF_AGC_IF_REF, &dwval);
	dwval &= ~(FLD_DIF_K_AGC_RF | FLD_DIF_K_AGC_IF);
	dwval |= 0x33000000;

	status = vid_blk_write_word(dev, DIF_AGC_IF_REF, dwval);

	return status;
}

int cx231xx_tuner_post_channel_change(struct cx231xx *dev)
{
	int status = 0;
	u32 dwval;

	/* Set the RF and IF k_agc values to 4 for PAL/NTSC and 8 for
	 * SECAM L/B/D standards */
	status = vid_blk_read_word(dev, DIF_AGC_IF_REF, &dwval);
	dwval &= ~(FLD_DIF_K_AGC_RF | FLD_DIF_K_AGC_IF);

	if (dev->norm & (V4L2_STD_SECAM_L | V4L2_STD_SECAM_B |
			 V4L2_STD_SECAM_D))
		dwval |= 0x88000000;
	else
		dwval |= 0x44000000;

	status = vid_blk_write_word(dev, DIF_AGC_IF_REF, dwval);

	return status;
}

int cx231xx_i2s_blk_initialize(struct cx231xx *dev)
{
	int status = 0;
	u32 value;

	status = cx231xx_read_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
				       CH_PWR_CTRL1, 1, &value, 1);
	/* enables clock to delta-sigma and decimation filter */
	value |= 0x80;
	status = cx231xx_write_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
					CH_PWR_CTRL1, 1, value, 1);
	/* power up all channel */
	status = cx231xx_write_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
					CH_PWR_CTRL2, 1, 0x00, 1);

	return status;
}

int cx231xx_i2s_blk_update_power_control(struct cx231xx *dev,
					enum AV_MODE avmode)
{
	int status = 0;
	u32 value = 0;

	if (avmode != POLARIS_AVMODE_ENXTERNAL_AV) {
		status = cx231xx_read_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
					  CH_PWR_CTRL2, 1, &value, 1);
		value |= 0xfe;
		status = cx231xx_write_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
						CH_PWR_CTRL2, 1, value, 1);
	} else {
		status = cx231xx_write_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
						CH_PWR_CTRL2, 1, 0x00, 1);
	}

	return status;
}

/* set i2s_blk for audio input types */
int cx231xx_i2s_blk_set_audio_input(struct cx231xx *dev, u8 audio_input)
{
	int status = 0;

	switch (audio_input) {
	case CX231XX_AMUX_LINE_IN:
		status = cx231xx_write_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
						CH_PWR_CTRL2, 1, 0x00, 1);
		status = cx231xx_write_i2c_data(dev, I2S_BLK_DEVICE_ADDRESS,
						CH_PWR_CTRL1, 1, 0x80, 1);
		break;
	case CX231XX_AMUX_VIDEO:
	default:
		break;
	}

	dev->ctl_ainput = audio_input;

	return status;
}

int cx231xx_set_power_mode(struct cx231xx *dev, enum AV_MODE mode)
{
	u8 value[4] = { 0, 0, 0, 0 };
	u32 tmp = 0;
	int status = 0;

	if (dev->power_mode != mode)
		dev->power_mode = mode;
	else {
		cx231xx_info(" setPowerMode::mode = %d, No Change req.\n",
			     mode);
		return 0;
	}

	cx231xx_info(" setPowerMode::mode = %d\n", mode);

	status = cx231xx_read_ctrl_reg(dev, VRT_GET_REGISTER, PWR_CTL_EN, value,
				       4);
	if (status < 0)
		return status;

	tmp = *((u32 *) value);

	switch (mode) {
	case POLARIS_AVMODE_ENXTERNAL_AV:

		tmp &= (~PWR_MODE_MASK);

		tmp |= PWR_AV_EN;
		value[0] = (u8) tmp;
		value[1] = (u8) (tmp >> 8);
		value[2] = (u8) (tmp >> 16);
		value[3] = (u8) (tmp >> 24);
		status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						PWR_CTL_EN, value, 4);
		msleep(PWR_SLEEP_INTERVAL);

		tmp |= PWR_ISO_EN;
		value[0] = (u8) tmp;
		value[1] = (u8) (tmp >> 8);
		value[2] = (u8) (tmp >> 16);
		value[3] = (u8) (tmp >> 24);
		status =
		    cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER, PWR_CTL_EN,
					   value, 4);
		msleep(PWR_SLEEP_INTERVAL);

		tmp |= POLARIS_AVMODE_ENXTERNAL_AV;
		value[0] = (u8) tmp;
		value[1] = (u8) (tmp >> 8);
		value[2] = (u8) (tmp >> 16);
		value[3] = (u8) (tmp >> 24);
		status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						PWR_CTL_EN, value, 4);

		/* reset state of xceive tuner */
		dev->xc_fw_load_done = 0;
		break;

	case POLARIS_AVMODE_ANALOGT_TV:

		tmp &= (~PWR_DEMOD_EN);
		tmp |= (I2C_DEMOD_EN);
		value[0] = (u8) tmp;
		value[1] = (u8) (tmp >> 8);
		value[2] = (u8) (tmp >> 16);
		value[3] = (u8) (tmp >> 24);
		status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						PWR_CTL_EN, value, 4);
		msleep(PWR_SLEEP_INTERVAL);

		if (!(tmp & PWR_TUNER_EN)) {
			tmp |= (PWR_TUNER_EN);
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}

		if (!(tmp & PWR_AV_EN)) {
			tmp |= PWR_AV_EN;
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}
		if (!(tmp & PWR_ISO_EN)) {
			tmp |= PWR_ISO_EN;
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}

		if (!(tmp & POLARIS_AVMODE_ANALOGT_TV)) {
			tmp |= POLARIS_AVMODE_ANALOGT_TV;
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}

		if ((dev->model == CX231XX_BOARD_CNXT_RDE_250) ||
		    (dev->model == CX231XX_BOARD_CNXT_RDU_250)) {
			/* tuner path to channel 1 from port 3 */
			cx231xx_enable_i2c_for_tuner(dev, I2C_3);

			if (dev->cx231xx_reset_analog_tuner)
				dev->cx231xx_reset_analog_tuner(dev);
		}
		break;

	case POLARIS_AVMODE_DIGITAL:
		if (!(tmp & PWR_TUNER_EN)) {
			tmp |= (PWR_TUNER_EN);
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}
		if (!(tmp & PWR_AV_EN)) {
			tmp |= PWR_AV_EN;
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}
		if (!(tmp & PWR_ISO_EN)) {
			tmp |= PWR_ISO_EN;
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}

		tmp |= POLARIS_AVMODE_DIGITAL | I2C_DEMOD_EN;
		value[0] = (u8) tmp;
		value[1] = (u8) (tmp >> 8);
		value[2] = (u8) (tmp >> 16);
		value[3] = (u8) (tmp >> 24);
		status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						PWR_CTL_EN, value, 4);
		msleep(PWR_SLEEP_INTERVAL);

		if (!(tmp & PWR_DEMOD_EN)) {
			tmp |= PWR_DEMOD_EN;
			value[0] = (u8) tmp;
			value[1] = (u8) (tmp >> 8);
			value[2] = (u8) (tmp >> 16);
			value[3] = (u8) (tmp >> 24);
			status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
							PWR_CTL_EN, value, 4);
			msleep(PWR_SLEEP_INTERVAL);
		}

		if ((dev->model == CX231XX_BOARD_CNXT_RDE_250) ||
		    (dev->model == CX231XX_BOARD_CNXT_RDU_250)) {
			/* tuner path to channel 1 from port 3 */
			cx231xx_enable_i2c_for_tuner(dev, I2C_3);

			if (dev->cx231xx_reset_analog_tuner)
				dev->cx231xx_reset_analog_tuner(dev);
		}
		break;

	default:
		break;
	}

	msleep(PWR_SLEEP_INTERVAL);

	/* For power saving, only enable Pwr_resetout_n
	   when digital TV is selected. */
	if (mode == POLARIS_AVMODE_DIGITAL) {
		tmp |= PWR_RESETOUT_EN;
		value[0] = (u8) tmp;
		value[1] = (u8) (tmp >> 8);
		value[2] = (u8) (tmp >> 16);
		value[3] = (u8) (tmp >> 24);
		status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER,
						PWR_CTL_EN, value, 4);
		msleep(PWR_SLEEP_INTERVAL);
	}

	/* update power control for afe */
	status = cx231xx_afe_update_power_control(dev, mode);

	/* update power control for i2s_blk */
	status = cx231xx_i2s_blk_update_power_control(dev, mode);

	status = cx231xx_read_ctrl_reg(dev, VRT_GET_REGISTER, PWR_CTL_EN, value,
				       4);
	cx231xx_info(" The data of PWR_CTL_EN register 0x74"
				 "=0x%0x,0x%0x,0x%0x,0x%0x\n",
		     value[0], value[1], value[2], value[3]);

	return status;
}

int cx231xx_power_suspend(struct cx231xx *dev)
{
	u8 value[4] = { 0, 0, 0, 0 };
	u32 tmp = 0;
	int status = 0;

	status = cx231xx_read_ctrl_reg(dev, VRT_GET_REGISTER, PWR_CTL_EN,
				       value, 4);
	if (status > 0)
		return status;

	tmp = *((u32 *) value);
	tmp &= (~PWR_MODE_MASK);

	value[0] = (u8) tmp;
	value[1] = (u8) (tmp >> 8);
	value[2] = (u8) (tmp >> 16);
	value[3] = (u8) (tmp >> 24);
	status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER, PWR_CTL_EN,
					value, 4);

	return status;
}

int cx231xx_start_stream(struct cx231xx *dev, u32 ep_mask)
{
	u8 value[4] = { 0x0, 0x0, 0x0, 0x0 };
	u32 tmp = 0;
	int status = 0;

	cx231xx_info("cx231xx_start_stream():: ep_mask = %x\n", ep_mask);
	status = cx231xx_read_ctrl_reg(dev, VRT_GET_REGISTER, EP_MODE_SET,
				       value, 4);
	if (status < 0)
		return status;

	tmp = *((u32 *) value);
	tmp |= ep_mask;
	value[0] = (u8) tmp;
	value[1] = (u8) (tmp >> 8);
	value[2] = (u8) (tmp >> 16);
	value[3] = (u8) (tmp >> 24);

	status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER, EP_MODE_SET,
					value, 4);

	return status;
}

int cx231xx_stop_stream(struct cx231xx *dev, u32 ep_mask)
{
	u8 value[4] = { 0x0, 0x0, 0x0, 0x0 };
	u32 tmp = 0;
	int status = 0;

	cx231xx_info("cx231xx_stop_stream():: ep_mask = %x\n", ep_mask);
	status =
	    cx231xx_read_ctrl_reg(dev, VRT_GET_REGISTER, EP_MODE_SET, value, 4);
	if (status < 0)
		return status;

	tmp = *((u32 *) value);
	tmp &= (~ep_mask);
	value[0] = (u8) tmp;
	value[1] = (u8) (tmp >> 8);
	value[2] = (u8) (tmp >> 16);
	value[3] = (u8) (tmp >> 24);

	status = cx231xx_write_ctrl_reg(dev, VRT_SET_REGISTER, EP_MODE_SET,
					value, 4);

	return status;
}

int cx231xx_initialize_stream_xfer(struct cx231xx *dev, u32 media_type)
{
	int status = 0;

	if (dev->udev->speed == USB_SPEED_HIGH) {
		switch (media_type) {
		case 81: /* audio */
			cx231xx_info("%s: Audio enter HANC\n", __func__);
			status =
			    cx231xx_mode_register(dev, TS_MODE_REG, 0x9300);
			break;

		case 2:	/* vbi */
			cx231xx_info("%s: set vanc registers\n", __func__);
			status = cx231xx_mode_register(dev, TS_MODE_REG, 0x300);
			break;

		case 3:	/* sliced cc */
			cx231xx_info("%s: set hanc registers\n", __func__);
			status =
			    cx231xx_mode_register(dev, TS_MODE_REG, 0x1300);
			break;

		case 0:	/* video */
			cx231xx_info("%s: set video registers\n", __func__);
			status = cx231xx_mode_register(dev, TS_MODE_REG, 0x100);
			break;

		case 4:	/* ts1 */
			cx231xx_info("%s: set ts1 registers\n", __func__);
			status = cx231xx_mode_register(dev, TS_MODE_REG, 0x101);
			status = cx231xx_mode_register(dev, TS1_CFG_REG, 0x400);
			break;
		case 6:	/* ts1 parallel mode */
			cx231xx_info("%s: set ts1 parrallel mode registers\n",
				     __func__);
			status = cx231xx_mode_register(dev, TS_MODE_REG, 0x100);
			status = cx231xx_mode_register(dev, TS1_CFG_REG, 0x400);
			break;
		}
	} else {
		status = cx231xx_mode_register(dev, TS_MODE_REG, 0x101);
	}

	return status;
}

int cx231xx_capture_start(struct cx231xx *dev, int start, u8 media_type)
{
	int rc = -1;
	u32 ep_mask = -1;
	struct pcb_config *pcb_config;

	/* get EP for media type */
	pcb_config = (struct pcb_config *)&dev->current_pcb_config;

	if (pcb_config->config_num == 1) {
		switch (media_type) {
		case 0:	/* Video */
			ep_mask = ENABLE_EP4;	/* ep4  [00:1000] */
			break;
		case 1:	/* Audio */
			ep_mask = ENABLE_EP3;	/* ep3  [00:0100] */
			break;
		case 2:	/* Vbi */
			ep_mask = ENABLE_EP5;	/* ep5 [01:0000] */
			break;
		case 3:	/* Sliced_cc */
			ep_mask = ENABLE_EP6;	/* ep6 [10:0000] */
			break;
		case 4:	/* ts1 */
		case 6:	/* ts1 parallel mode */
			ep_mask = ENABLE_EP1;	/* ep1 [00:0001] */
			break;
		case 5:	/* ts2 */
			ep_mask = ENABLE_EP2;	/* ep2 [00:0010] */
			break;
		}

	} else if (pcb_config->config_num > 1) {
		switch (media_type) {
		case 0:	/* Video */
			ep_mask = ENABLE_EP4;	/* ep4  [00:1000] */
			break;
		case 1:	/* Audio */
			ep_mask = ENABLE_EP3;	/* ep3  [00:0100] */
			break;
		case 2:	/* Vbi */
			ep_mask = ENABLE_EP5;	/* ep5 [01:0000] */
			break;
		case 3:	/* Sliced_cc */
			ep_mask = ENABLE_EP6;	/* ep6 [10:0000] */
			break;
		case 4:	/* ts1 */
		case 6:	/* ts1 parallel mode */
			ep_mask = ENABLE_EP1;	/* ep1 [00:0001] */
			break;
		case 5:	/* ts2 */
			ep_mask = ENABLE_EP2;	/* ep2 [00:0010] */
			break;
		}

	}

	if (start) {
		rc = cx231xx_initialize_stream_xfer(dev, media_type);

		if (rc < 0)
			return rc;

		/* enable video capture */
		if (ep_mask > 0)
			rc = cx231xx_start_stream(dev, ep_mask);
	} else {
		/* disable video capture */
		if (ep_mask > 0)
			rc = cx231xx_stop_stream(dev, ep_mask);
	}

	if (dev->mode == CX231XX_ANALOG_MODE)
		;/* do any in Analog mode */
	else
		;/* do any in digital mode */

	return rc;
}
EXPORT_SYMBOL_GPL(cx231xx_capture_start);

int cx231xx_set_gpio_bit(struct cx231xx *dev, u32 gpio_bit, u8 * gpio_val)
{
	int status = 0;

	status = cx231xx_send_gpio_cmd(dev, gpio_bit, gpio_val, 4, 0, 0);

	return status;
}

int cx231xx_get_gpio_bit(struct cx231xx *dev, u32 gpio_bit, u8 * gpio_val)
{
	int status = 0;

	status = cx231xx_send_gpio_cmd(dev, gpio_bit, gpio_val, 4, 0, 1);

	return status;
}

int cx231xx_set_gpio_direction(struct cx231xx *dev,
			       int pin_number, int pin_value)
{
	int status = 0;
	u32 value = 0;

	/* Check for valid pin_number - if 32 , bail out */
	if (pin_number >= 32)
		return -EINVAL;

	/* input */
	if (pin_value == 0)
		value = dev->gpio_dir & (~(1 << pin_number));	/* clear */
	else
		value = dev->gpio_dir | (1 << pin_number);

	status = cx231xx_set_gpio_bit(dev, value, (u8 *) &dev->gpio_val);

	/* cache the value for future */
	dev->gpio_dir = value;

	return status;
}

int cx231xx_set_gpio_value(struct cx231xx *dev, int pin_number, int pin_value)
{
	int status = 0;
	u32 value = 0;

	/* Check for valid pin_number - if 0xFF , bail out */
	if (pin_number >= 32)
		return -EINVAL;

	/* first do a sanity check - if the Pin is not output, make it output */
	if ((dev->gpio_dir & (1 << pin_number)) == 0x00) {
		/* It was in input mode */
		value = dev->gpio_dir | (1 << pin_number);
		dev->gpio_dir = value;
		status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
					      (u8 *) &dev->gpio_val);
		value = 0;
	}

	if (pin_value == 0)
		value = dev->gpio_val & (~(1 << pin_number));
	else
		value = dev->gpio_val | (1 << pin_number);

	/* store the value */
	dev->gpio_val = value;

	/* toggle bit0 of GP_IO */
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	return status;
}

int cx231xx_gpio_i2c_start(struct cx231xx *dev)
{
	int status = 0;

	/* set SCL to output 1 ; set SDA to output 1 */
	dev->gpio_dir |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_dir |= 1 << dev->board.tuner_sda_gpio;
	dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_val |= 1 << dev->board.tuner_sda_gpio;

	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);
	if (status < 0)
		return -EINVAL;

	/* set SCL to output 1; set SDA to output 0 */
	dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);

	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);
	if (status < 0)
		return -EINVAL;

	/* set SCL to output 0; set SDA to output 0      */
	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);

	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);
	if (status < 0)
		return -EINVAL;

	return status;
}

int cx231xx_gpio_i2c_end(struct cx231xx *dev)
{
	int status = 0;

	/* set SCL to output 0; set SDA to output 0      */
	dev->gpio_dir |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_dir |= 1 << dev->board.tuner_sda_gpio;

	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);

	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);
	if (status < 0)
		return -EINVAL;

	/* set SCL to output 1; set SDA to output 0      */
	dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);

	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);
	if (status < 0)
		return -EINVAL;

	/* set SCL to input ,release SCL cable control
	   set SDA to input ,release SDA cable control */
	dev->gpio_dir &= ~(1 << dev->board.tuner_scl_gpio);
	dev->gpio_dir &= ~(1 << dev->board.tuner_sda_gpio);

	status =
	    cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);
	if (status < 0)
		return -EINVAL;

	return status;
}

int cx231xx_gpio_i2c_write_byte(struct cx231xx *dev, u8 data)
{
	int status = 0;
	u8 i;

	/* set SCL to output ; set SDA to output */
	dev->gpio_dir |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_dir |= 1 << dev->board.tuner_sda_gpio;

	for (i = 0; i < 8; i++) {
		if (((data << i) & 0x80) == 0) {
			/* set SCL to output 0; set SDA to output 0     */
			dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
			dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);
			status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
						      (u8 *)&dev->gpio_val);

			/* set SCL to output 1; set SDA to output 0     */
			dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
			status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
						      (u8 *)&dev->gpio_val);

			/* set SCL to output 0; set SDA to output 0     */
			dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
			status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
						      (u8 *)&dev->gpio_val);
		} else {
			/* set SCL to output 0; set SDA to output 1     */
			dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
			dev->gpio_val |= 1 << dev->board.tuner_sda_gpio;
			status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
						      (u8 *)&dev->gpio_val);

			/* set SCL to output 1; set SDA to output 1     */
			dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
			status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
						      (u8 *)&dev->gpio_val);

			/* set SCL to output 0; set SDA to output 1     */
			dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
			status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
						      (u8 *)&dev->gpio_val);
		}
	}
	return status;
}

int cx231xx_gpio_i2c_read_byte(struct cx231xx *dev, u8 * buf)
{
	u8 value = 0;
	int status = 0;
	u32 gpio_logic_value = 0;
	u8 i;

	/* read byte */
	for (i = 0; i < 8; i++) {	/* send write I2c addr */

		/* set SCL to output 0; set SDA to input */
		dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
		status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
					      (u8 *)&dev->gpio_val);

		/* set SCL to output 1; set SDA to input */
		dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
		status = cx231xx_set_gpio_bit(dev, dev->gpio_dir,
					      (u8 *)&dev->gpio_val);

		/* get SDA data bit */
		gpio_logic_value = dev->gpio_val;
		status = cx231xx_get_gpio_bit(dev, dev->gpio_dir,
					      (u8 *)&dev->gpio_val);
		if ((dev->gpio_val & (1 << dev->board.tuner_sda_gpio)) != 0)
			value |= (1 << (8 - i - 1));

		dev->gpio_val = gpio_logic_value;
	}

	/* set SCL to output 0,finish the read latest SCL signal.
	   !!!set SDA to input, never to modify SDA direction at
	   the same times */
	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* store the value */
	*buf = value & 0xff;

	return status;
}

int cx231xx_gpio_i2c_read_ack(struct cx231xx *dev)
{
	int status = 0;
	u32 gpio_logic_value = 0;
	int nCnt = 10;
	int nInit = nCnt;

	/* clock stretch; set SCL to input; set SDA to input;
	   get SCL value till SCL = 1 */
	dev->gpio_dir &= ~(1 << dev->board.tuner_sda_gpio);
	dev->gpio_dir &= ~(1 << dev->board.tuner_scl_gpio);

	gpio_logic_value = dev->gpio_val;
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	do {
		msleep(2);
		status = cx231xx_get_gpio_bit(dev, dev->gpio_dir,
					      (u8 *)&dev->gpio_val);
		nCnt--;
	} while (((dev->gpio_val &
			  (1 << dev->board.tuner_scl_gpio)) == 0) &&
			 (nCnt > 0));

	if (nCnt == 0)
		cx231xx_info("No ACK after %d msec -GPIO I2C failed!",
			     nInit * 10);

	/*
	 * readAck
	 * through clock stretch, slave has given a SCL signal,
	 * so the SDA data can be directly read.
	 */
	status = cx231xx_get_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	if ((dev->gpio_val & 1 << dev->board.tuner_sda_gpio) == 0) {
		dev->gpio_val = gpio_logic_value;
		dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);
		status = 0;
	} else {
		dev->gpio_val = gpio_logic_value;
		dev->gpio_val |= (1 << dev->board.tuner_sda_gpio);
	}

	/* read SDA end, set the SCL to output 0, after this operation,
	   SDA direction can be changed. */
	dev->gpio_val = gpio_logic_value;
	dev->gpio_dir |= (1 << dev->board.tuner_scl_gpio);
	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	return status;
}

int cx231xx_gpio_i2c_write_ack(struct cx231xx *dev)
{
	int status = 0;

	/* set SDA to ouput */
	dev->gpio_dir |= 1 << dev->board.tuner_sda_gpio;
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* set SCL = 0 (output); set SDA = 0 (output) */
	dev->gpio_val &= ~(1 << dev->board.tuner_sda_gpio);
	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* set SCL = 1 (output); set SDA = 0 (output) */
	dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* set SCL = 0 (output); set SDA = 0 (output) */
	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* set SDA to input,and then the slave will read data from SDA. */
	dev->gpio_dir &= ~(1 << dev->board.tuner_sda_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	return status;
}

int cx231xx_gpio_i2c_write_nak(struct cx231xx *dev)
{
	int status = 0;

	/* set scl to output ; set sda to input */
	dev->gpio_dir |= 1 << dev->board.tuner_scl_gpio;
	dev->gpio_dir &= ~(1 << dev->board.tuner_sda_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* set scl to output 0; set sda to input */
	dev->gpio_val &= ~(1 << dev->board.tuner_scl_gpio);
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	/* set scl to output 1; set sda to input */
	dev->gpio_val |= 1 << dev->board.tuner_scl_gpio;
	status = cx231xx_set_gpio_bit(dev, dev->gpio_dir, (u8 *)&dev->gpio_val);

	return status;
}

int cx231xx_gpio_i2c_read(struct cx231xx *dev, u8 dev_addr, u8 * buf, u8 len)
{
	int status = 0;
	int i = 0;

	/* get the lock */
	mutex_lock(&dev->gpio_i2c_lock);

	/* start */
	status = cx231xx_gpio_i2c_start(dev);

	/* write dev_addr */
	status = cx231xx_gpio_i2c_write_byte(dev, (dev_addr << 1) + 1);

	/* readAck */
	status = cx231xx_gpio_i2c_read_ack(dev);

	/* read data */
	for (i = 0; i < len; i++) {
		/* read data */
		buf[i] = 0;
		status = cx231xx_gpio_i2c_read_byte(dev, &buf[i]);

		if ((i + 1) != len) {
			/* only do write ack if we more length */
			status = cx231xx_gpio_i2c_write_ack(dev);
		}
	}

	/* write NAK - inform reads are complete */
	status = cx231xx_gpio_i2c_write_nak(dev);

	/* write end */
	status = cx231xx_gpio_i2c_end(dev);

	/* release the lock */
	mutex_unlock(&dev->gpio_i2c_lock);

	return status;
}

int cx231xx_gpio_i2c_write(struct cx231xx *dev, u8 dev_addr, u8 * buf, u8 len)
{
	int status = 0;
	int i = 0;

	/* get the lock */
	mutex_lock(&dev->gpio_i2c_lock);

	/* start */
	status = cx231xx_gpio_i2c_start(dev);

	/* write dev_addr */
	status = cx231xx_gpio_i2c_write_byte(dev, dev_addr << 1);

	/* read Ack */
	status = cx231xx_gpio_i2c_read_ack(dev);

	for (i = 0; i < len; i++) {
		/* Write data */
		status = cx231xx_gpio_i2c_write_byte(dev, buf[i]);

		/* read Ack */
		status = cx231xx_gpio_i2c_read_ack(dev);
	}

	/* write End */
	status = cx231xx_gpio_i2c_end(dev);

	/* release the lock */
	mutex_unlock(&dev->gpio_i2c_lock);

	return 0;
}
