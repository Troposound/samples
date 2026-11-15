#ifndef TAC5211_REGS_H_
#define TAC5211_REGS_H_

#include <zephyr/sys/util.h>

/* Register pages. */
#define TAC5211_PAGE_CONTROL        0x00U
#define TAC5211_PAGE_DAC_BQ_1_TO_6  0x0FU
#define TAC5211_PAGE_DAC_BQ_7_TO_12 0x10U

/* Page 0 control registers. */
#define TAC5211_REG_PAGE_SEL        0x00U
#define TAC5211_REG_SW_RESET        0x01U
#define TAC5211_REG_DEV_MISC_CFG    0x02U
#define TAC5211_REG_GPIO1_CFG0      0x0AU
#define TAC5211_REG_GPIO2_CFG0      0x0BU
#define TAC5211_REG_GPO1_CFG0       0x0CU
#define TAC5211_REG_INTF_CFG2       0x11U
#define TAC5211_REG_INTF_CFG4       0x13U
#define TAC5211_REG_ASI_CFG1        0x19U
#define TAC5211_REG_PASI_CFG0       0x1AU
#define TAC5211_REG_PASI_RX_CH1_CFG 0x28U
#define TAC5211_REG_PASI_RX_CH2_CFG 0x29U
#define TAC5211_REG_CNT_CLK_CFG0    0x35U
#define TAC5211_REG_ADC_CH1_CFG0    0x50U
#define TAC5211_REG_ADC_CH1_CFG2    0x52U
#define TAC5211_REG_OUT1X_CFG0      0x64U
#define TAC5211_REG_OUT1X_CFG1      0x65U
#define TAC5211_REG_OUT1X_CFG2      0x66U
#define TAC5211_REG_DAC_CH1A_CFG0   0x67U
#define TAC5211_REG_DAC_CH1B_CFG0   0x69U
#define TAC5211_REG_DSP_CFG1        0x73U
#define TAC5211_REG_CH_EN           0x76U
#define TAC5211_REG_DYN_PUPD_CFG    0x77U
#define TAC5211_REG_PWR_CFG         0x78U

/* DEV_MISC_CFG (P0_R2). */
#define TAC5211_DEV_MISC_SLEEP_EXIT_VREF_EN BIT(3)
#define TAC5211_DEV_MISC_SLEEP_ENZ          BIT(0)
#define TAC5211_DEV_MISC_ACTIVE_WITH_VREF                                                          \
	(TAC5211_DEV_MISC_SLEEP_EXIT_VREF_EN | TAC5211_DEV_MISC_SLEEP_ENZ)

/* Multifunction pins. */
#define TAC5211_PIN_DISABLED 0x00U

/* Audio serial and microphone interfaces. */
#define TAC5211_INTF_CFG2_PASI_DIN_ENABLE BIT(7)
#define TAC5211_INTF_CFG4_ANALOG_CH1      0x00U
#define TAC5211_ASI_CFG1_SINGLE_DIN_DOUT  0x00U
#define TAC5211_PASI_CFG0_I2S_16_BIT      0x40U
#define TAC5211_PASI_RX_CH1_LEFT_SLOT0    0x20U
#define TAC5211_PASI_RX_CH2_RIGHT_SLOT0   0x30U
#define TAC5211_CNT_CLK_CFG0_DEFAULT      0x00U

/* ADC and DAC signal paths. */
#define TAC5211_ADC_CH1_SINGLE_ENDED_10K  0x50U

/* ADC_CH1_CFG2 (P0_R82) channel digital volume, datasheet Table 8-72:
 * 0d mute, 1d = -80 dB, 0.5 dB per step, 161d = 0 dB, 255d = +47 dB. 161 is
 * also the register's reset value.
 *
 * Decimal on purpose. This is NOT the DAC's codepoint -- TAC5211_DAC_DVOL_0_DB
 * is 201, because that control starts at -100 dB instead of -80 dB. Written as
 * 0xA1 next to 201 the two look like they contradict each other; they do not.
 *
 * This is the only real gain control in the capture path. The part has no
 * analog PGA (P0_R81 is IADC_CH_CFG, not a gain register), MICBIAS is already
 * at its maximum, and the full-scale bit in ADC_CH1_CFG0 only ever loses
 * sensitivity. Everything above 0 dB comes from here.
 */
#define TAC5211_ADC_DVOL_MUTE      0U
#define TAC5211_ADC_DVOL_0_DB      161U /* 0xA1 */
#define TAC5211_ADC_DVOL_MAX_STEPS 94   /* +47 dB */

#define TAC5211_OUT1X_DAC_STEREO_SINGLE_ENDED 0x24U
#define TAC5211_OUT1P_HEADPHONE_0_DB          0x60U
#define TAC5211_OUT1M_HEADPHONE_0_DB          0x60U
#define TAC5211_DSP_CFG1_DAC_3_BIQUADS        0x1CU

/* DYN_PUPD_CFG (P0_R119). */
#define TAC5211_DYN_PUPD_ADC_DYN_PUPD_EN BIT(7)
#define TAC5211_DYN_PUPD_ADC_CH_SWAP     BIT(1)

/* Channel and block power control. */
#define TAC5211_CH_EN_INPUT1_OUTPUT1_OUTPUT2 0x8CU
#define TAC5211_PWR_ADC                      BIT(7)
#define TAC5211_PWR_DAC                      BIT(6)
#define TAC5211_PWR_MICBIAS                  BIT(5)
#define TAC5211_PWR_PLAYBACK_AND_MICBIAS \
	(TAC5211_PWR_DAC | TAC5211_PWR_MICBIAS)
#define TAC5211_PWR_FULL_DUPLEX \
	(TAC5211_PWR_ADC | TAC5211_PWR_DAC | TAC5211_PWR_MICBIAS)

#endif /* TAC5211_REGS_H_ */
