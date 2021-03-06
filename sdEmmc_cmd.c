/*
 * Copyright (c) 2006 Uwe Stuehler <uwe@openbsd.org>
 * Adaptations to ESP-IDF Copyright (c) 2016 Espressif Systems (Shanghai) PTE LTD
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <string.h>
#include "esp32-hal-log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdEmmc_defs.h"
#include "sdEmmc_types.h"
#include "sdEmmc_cmd.h"
#include "sys/param.h"
#include "soc/soc_memory_layout.h"





static esp_err_t sdmmc_send_cmd(sdmmc_card_t* card, sdmmc_command_t* cmd);
static esp_err_t sdmmc_send_app_cmd(sdmmc_card_t* card, sdmmc_command_t* cmd);
static esp_err_t sdmmc_send_cmd_go_idle_state(sdmmc_card_t* card);
static esp_err_t sdmmc_send_cmd_send_if_cond(sdmmc_card_t* card, uint32_t ocr);
static esp_err_t sdmmc_send_cmd_send_op_cond(sdmmc_card_t* card, uint32_t ocr, uint32_t *ocrp);
static esp_err_t sdmmc_send_cmd_read_ocr(sdmmc_card_t *card, uint32_t *ocrp);
static esp_err_t sdmmc_send_cmd_send_cid(sdmmc_card_t *card, sdmmc_cid_t *out_cid);
static esp_err_t sdmmc_decode_cid(sdmmc_response_t resp, sdmmc_cid_t* out_cid);
static esp_err_t sdmmc_send_cmd_all_send_cid(sdmmc_card_t* card, sdmmc_cid_t* out_cid);
static esp_err_t sdmmc_send_cmd_set_relative_addr(sdmmc_card_t* card, uint16_t* out_rca);
static esp_err_t sdmmc_send_cmd_set_blocklen(sdmmc_card_t* card, sdmmc_csd_t* csd);
//static esp_err_t sdmmc_send_cmd_switch_func(sdmmc_card_t* card,        uint32_t mode, uint32_t group, uint32_t function,        sdmmc_switch_func_rsp_t* resp);
//static esp_err_t sdmmc_enable_hs_mode(sdmmc_card_t* card);
static esp_err_t mmc_decode_csd(sdmmc_response_t response, sdmmc_csd_t* out_csd);
static esp_err_t sd_decode_csd(sdmmc_response_t response, sdmmc_csd_t* out_csd);
static esp_err_t sdmmc_send_cmd_send_csd(sdmmc_card_t* card, sdmmc_csd_t* out_csd);
static esp_err_t sdmmc_mem_send_cxd_data(sdmmc_card_t* card , int opcode, void *data, size_t datalen);
static esp_err_t sdmmc_send_cmd_select_card(sdmmc_card_t* card, uint32_t rca);
//static esp_err_t sdmmc_decode_scr(uint32_t *raw_scr, sdmmc_scr_t* out_scr);
//static esp_err_t sdmmc_send_cmd_send_scr(sdmmc_card_t* card, sdmmc_scr_t *out_scr);
//static esp_err_t sdmmc_send_cmd_set_bus_width(sdmmc_card_t* card, int width);
//static esp_err_t sdmmc_mmc_command_set(sdmmc_card_t* card, uint8_t set);
static esp_err_t sdmmc_mmc_switch(sdmmc_card_t* card, uint8_t set, uint8_t index, uint8_t value);
//static esp_err_t sdmmc_send_cmd_stop_transmission(sdmmc_card_t* card, uint32_t* status);
static esp_err_t sdmmc_send_cmd_send_status(sdmmc_card_t* card, uint32_t* out_status);
static esp_err_t sdmmc_send_cmd_crc_on_off(sdmmc_card_t* card, bool crc_enable);
static uint32_t  get_host_ocr(float voltage);
static void flip_byte_order(uint32_t* response, size_t size);



#include "soc/sdmmc_struct.h"
static bool host_is_spi(const sdmmc_card_t* card)
{
    return (card->host.flags & SDMMC_HOST_FLAG_SPI) != 0;
}

esp_err_t sdEmmc_card_init(const sdmmc_host_t* config, sdmmc_card_t* card)
{
    log_d( "%s", __func__);
    memset(card, 0, sizeof(*card));
    memcpy(&card->host, config, sizeof(*config));
    const bool is_spi = host_is_spi(card);

    /* GO_IDLE_STATE (CMD0) command resets the card */
    esp_err_t err = sdmmc_send_cmd_go_idle_state(card);
    if (err != ESP_OK) {
        log_e( "%s: go_idle_state (1) returned 0x%x", __func__, err);
        return err;
    }
    vTaskDelay(SDMMC_GO_IDLE_DELAY_MS / portTICK_PERIOD_MS);
    sdmmc_send_cmd_go_idle_state(card);
    vTaskDelay(SDMMC_GO_IDLE_DELAY_MS / portTICK_PERIOD_MS);

    /* SEND_IF_COND (CMD8) command is used to identify SDHC/SDXC cards.
     * SD v1 and non-SD cards will not respond to this command.
     */
    uint32_t host_ocr = get_host_ocr(config->io_voltage);
    err = sdmmc_send_cmd_send_if_cond(card, host_ocr);
    if (err == ESP_OK) {
        log_d( "SDHC/SDXC card");
        host_ocr |= SD_OCR_SDHC_CAP;
    } else if (err == ESP_ERR_TIMEOUT) {
        log_d( "CMD8 timeout; not an SDHC/SDXC card");
    } else {
        log_e( "%s: send_if_cond (1) returned 0x%x", __func__, err);
        return err;
    }

    /* In SPI mode, READ_OCR (CMD58) command is used to figure out which voltage
     * ranges the card can support. This step is skipped since 1.8V isn't
     * supported on the ESP32.
     */

    /* In SD mode, CRC checks of data transfers are mandatory and performed
     * by the hardware. In SPI mode, CRC16 of data transfers is optional and
     * needs to be enabled.
     */
    if (is_spi) {
        err = sdmmc_send_cmd_crc_on_off(card, true);
        if (err != ESP_OK) {
            log_e( "%s: sdmmc_send_cmd_crc_on_off returned 0x%x", __func__, err);
            return err;
        }
    }
    
    /* Send SEND_OP_COND (ACMD41) command to the card until it becomes ready. */
    err = sdmmc_send_cmd_send_op_cond(card, host_ocr, &card->ocr);

    //if time-out try switching from SD to MMC and vice-versa
    if (err == ESP_ERR_TIMEOUT){
        if (card->host.flags & SDMMC_HOST_MMC_CARD) {   
            card->host.flags &= ~((uint32_t)(SDMMC_HOST_MMC_CARD)); 
            log_d( "Clear SDMMC_HOST_MMC_CARD");
        } else {
            card->host.flags |= SDMMC_HOST_MMC_CARD;              
            log_d( "Set SDMMC_HOST_MMC_CARD");            
        }
        //retry SEND_OP_COND operation
        err = sdmmc_send_cmd_send_op_cond(card, host_ocr, &card->ocr);
    }
    if (err != ESP_OK) {
        log_e( "%s: send_op_cond (1) returned 0x%x", __func__, err);
        return err;
    }

    
    if (is_spi) {
        err = sdmmc_send_cmd_read_ocr(card, &card->ocr);
        if (err != ESP_OK) {
            log_e( "%s: read_ocr returned 0x%x", __func__, err);
            return err;
        }
    }
    log_d( "host_ocr=0x%x card_ocr=0x%x", host_ocr, card->ocr);

    /* Clear all voltage bits in host's OCR which the card doesn't support.
     * Don't touch CCS bit because in SPI mode cards don't report CCS in ACMD41
     * response.
     */
    host_ocr &= (card->ocr | (~SD_OCR_VOL_MASK));
    log_d( "sdEmmc_card_init: host_ocr=%08x, card_ocr=%08x", host_ocr, card->ocr);

    /* Read and decode the contents of CID register */
    if (!is_spi) {
        err = sdmmc_send_cmd_all_send_cid(card, &card->cid);
        if (err != ESP_OK) {
            log_e( "%s: all_send_cid returned 0x%x", __func__, err);
            return err;
        }
        err = sdmmc_send_cmd_set_relative_addr(card, &card->rca);
        if (err != ESP_OK) {
            log_e( "%s: set_relative_addr returned 0x%x", __func__, err);
            return err;
        }
    } else {
        err = sdmmc_send_cmd_send_cid(card, &card->cid);
        if (err != ESP_OK) {
            log_e( "%s: send_cid returned 0x%x", __func__, err);
            return err;
        }
    }

    /* Get and decode the contents of CSD register. Determine card capacity. */
    err = sdmmc_send_cmd_send_csd(card, &card->csd);
    if (err != ESP_OK) {
        log_e( "%s: send_csd (1) returned 0x%x", __func__, err);
        return err;
    }
    const size_t max_sdsc_capacity = UINT32_MAX / card->csd.sector_size + 1;
    if (!(card->ocr & SD_OCR_SDHC_CAP) &&
         card->csd.capacity > max_sdsc_capacity) {
        log_w( "%s: SDSC card reports capacity=%u. Limiting to %u.",
                __func__, card->csd.capacity, max_sdsc_capacity);
        card->csd.capacity = max_sdsc_capacity;
    }

    /* Switch the card from stand-by mode to data transfer mode (not needed if
     * SPI interface is used). This is needed to issue SET_BLOCKLEN and
     * SEND_SCR commands.
     */
    if (!is_spi) {
        err = sdmmc_send_cmd_select_card(card, card->rca);
        if (err != ESP_OK) {
            log_e( "%s: select_card returned 0x%x", __func__, err);
            return err;
        }
    }

    /* SDSC cards support configurable data block lengths.
     * We don't use this feature and set the block length to 512 bytes,
     * same as the block length for SDHC cards.
     */
    if ((card->ocr & SD_OCR_SDHC_CAP) == 0) {
        err = sdmmc_send_cmd_set_blocklen(card, &card->csd);
        if (err != ESP_OK) {
            log_e( "%s: set_blocklen returned 0x%x", __func__, err);
            return err;
        }
    }

    if (card->host.flags & SDMMC_HOST_MMC_CARD) {
        log_d( "Using MMC protocol");
        /* sdmmc_mem_mmc_init */
        int width, width_value;
        int card_type;
        uint8_t ext_csd[512];
        uint8_t powerclass = 0;
		int speed_supported = MMC_FREQ_PROBING_400K;
		                
        uint32_t sectors = 0;

        if (card->csd.mmc_ver < MMC_CSD_MMCVER_4_0){
          log_d( "card->csd.mmc_ver < MMC_CSD_MMCVER_4_0"); 
          return ESP_FAIL;
        } 

        
		/* read EXT_CSD */
		err = sdmmc_mem_send_cxd_data(card,
				MMC_SEND_EXT_CSD, ext_csd, sizeof(ext_csd));
		if (err != ESP_OK) {
			//SET(card->flags, SFF_ERROR);
			log_e( "%s: can't read EXT_CSD\n", __func__);
			return err;
		}

		card_type = ext_csd[EXT_CSD_CARD_TYPE];

		//NOTE: ESP32 doesn't support DDR
		if (card_type & EXT_CSD_CARD_TYPE_F_52M_1_8V) {
			log_d( "EXT_CSD_CARD_TYPE_F_52M_1_8V");
			speed_supported = MMC_FREQ_HIGHSPEED_SDR_52M;
		} else if (card_type & EXT_CSD_CARD_TYPE_F_52M) {
			log_d( "EXT_CSD_CARD_TYPE_F_52M");
			speed_supported = MMC_FREQ_HIGHSPEED_SDR_52M;
		} else if (card_type & EXT_CSD_CARD_TYPE_F_26M) {
			log_d( "EXT_CSD_CARD_TYPE_F_26M");
			speed_supported = MMC_FREQ_DEFAULT_26M;
		} else {
			log_e( "%s: unknown CARD_TYPE 0x%x\n", __func__,
					ext_csd[EXT_CSD_CARD_TYPE]);
		}
		
		int speed = speed_supported;
		if(config->max_freq_khz < speed ) speed = config->max_freq_khz;

		if (speed > MMC_FREQ_DEFAULT_26M) {
			/* switch to high speed timing */
			log_d( "switch to high speed timing ");
			err = sdmmc_mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_HS_TIMING, EXT_CSD_HS_TIMING_HS);
			if (err != ESP_OK) {
				log_e( "%s: can't change high speed",
						__func__);
				return err;
			}
			ets_delay_us(10000);
			
			/* read EXT_CSD again */
			err = sdmmc_mem_send_cxd_data(card,
					MMC_SEND_EXT_CSD, ext_csd, sizeof(ext_csd));
			if (err != ESP_OK) {
				log_e( "%s: can't re-read EXT_CSD\n", __func__);
				return err;
			}
			if (ext_csd[EXT_CSD_HS_TIMING] != EXT_CSD_HS_TIMING_HS) {
				log_e( "%s, HS_TIMING set failed\n", __func__);
				return ESP_ERR_INVALID_RESPONSE;
			}			
		}

		log_d( "switching speed to:%u",speed);
		err = (*config->set_card_clk)(config->slot, speed);
		if (err != ESP_OK) {
			log_e( "failed to switch speed");
			return err;
		}
        

		if (card->host.flags & SDMMC_HOST_FLAG_8BIT) {
			width = 8;
			width_value = EXT_CSD_BUS_WIDTH_8;
			powerclass = ext_csd[(speed > MMC_FREQ_DEFAULT_26M) ? EXT_CSD_PWR_CL_52_360 : EXT_CSD_PWR_CL_26_360] >> 4;
		} else if (card->host.flags & SDMMC_HOST_FLAG_4BIT) {
			width = 4;
			width_value = EXT_CSD_BUS_WIDTH_4;
			powerclass = ext_csd[(speed > MMC_FREQ_DEFAULT_26M) ? EXT_CSD_PWR_CL_52_360 : EXT_CSD_PWR_CL_26_360] & 0x0f;
		} else {
			width = 1;
			width_value = EXT_CSD_BUS_WIDTH_1;
			powerclass = 0; //card must be able to do full rate at powerclass 0 in 1-bit mode
		}
		
		if (powerclass != 0) {
			log_d("setting powerclass:%X",powerclass);
			err = sdmmc_mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_POWER_CLASS, powerclass);
			if (err != ESP_OK) {
				log_e( "%s: can't change power class"
							" (%d bit)\n", __func__, powerclass);
				return err;
			}
            ets_delay_us(10000);
		}
		if (width != 1) {
			log_d("setting bus width:%u width_value:%X",width,width_value);
			err = sdmmc_mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_BUS_WIDTH, width_value);
			if (err == ESP_OK) {
				err = (*config->set_bus_width)(config->slot, width);
				if (err != ESP_OK) {
					log_e( "slot->set_bus_width failed");
					return err;
				}
			} else {
				log_e( "%s: can't change bus width"
							" (%d bit)\n", __func__, width);
				return err;
			}

			/* XXXX: need bus test? (using by CMD14 & CMD19) */
			ets_delay_us(10000);
		}

		sectors = ext_csd[EXT_CSD_SEC_COUNT + 0] << 0 |
			ext_csd[EXT_CSD_SEC_COUNT + 1] << 8  |
			ext_csd[EXT_CSD_SEC_COUNT + 2] << 16 |
			ext_csd[EXT_CSD_SEC_COUNT + 3] << 24;

		if (sectors > (2u * 1024 * 1024 * 1024) / 512) {
			//card->flags |= SFF_SDHC;
			card->csd.capacity = sectors;
		}

        log_d( "MMC width:%d card_type:%d speed:%d powerclass:%d  sectors:%lu",   
            width,card_type,speed, powerclass,  sectors 
        );
        
    } else {
        log_d( "Using SD protocol");
    }
    return ESP_OK;
}

void sdEmmc_card_print_info(FILE* stream, const sdmmc_card_t* card)
{
    fprintf(stream, "Name: %s\n", card->cid.name);
    fprintf(stream, "Speed: %u\n", card->csd.tr_speed);
    fprintf(stream, "Size: %lluMB\n", ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    fprintf(stream, "CSD: ver=%d, sector_size=%d, capacity=%d read_bl_len=%d\n",
            card->csd.csd_ver,
            card->csd.sector_size, card->csd.capacity, card->csd.read_block_len);
    fprintf(stream, "SCR: sd_spec=%d, bus_width=%d\n", card->scr.sd_spec, card->scr.bus_width);
}

static esp_err_t sdmmc_send_cmd(sdmmc_card_t* card, sdmmc_command_t* cmd)
{
    if (card->host.command_timeout_ms != 0) {
        cmd->timeout_ms = card->host.command_timeout_ms;
    } else if (cmd->timeout_ms == 0) {
        cmd->timeout_ms = SDMMC_DEFAULT_CMD_TIMEOUT_MS;
    }

    int slot = card->host.slot;
    log_v( "sending cmd slot=%d op=%d arg=%x flags=%x data=%p blklen=%d datalen=%d timeout=%d",
            slot, cmd->opcode, cmd->arg, cmd->flags, cmd->data, cmd->blklen, cmd->datalen, cmd->timeout_ms);
    esp_err_t err = (*card->host.do_transaction)(slot, cmd);
    if (err != 0) {
        log_d( "sdmmc_req_run returned 0x%x", err);
        return err;
    }
    
    log_v( "cmd response %08x %08x %08x %08x err=0x%x state=%d",
               cmd->response[0],
               cmd->response[1],
               cmd->response[2],
               cmd->response[3],
               cmd->error,
               (int)(MMC_R1_CURRENT_STATE(cmd->response)));
    return cmd->error;
}

static esp_err_t sdmmc_send_app_cmd(sdmmc_card_t* card, sdmmc_command_t* cmd)
{
    sdmmc_command_t app_cmd = {
        .opcode = MMC_APP_CMD,
        .flags = SCF_CMD_AC | SCF_RSP_R1,
        .arg = MMC_ARG_RCA(card->rca),
    };
    esp_err_t err = sdmmc_send_cmd(card, &app_cmd);
    if (err != ESP_OK) {
        return err;
    }
    // Check APP_CMD status bit (only in SD mode)
    if (!host_is_spi(card) && !(MMC_R1(app_cmd.response) & MMC_R1_APP_CMD)) {
        log_w( "card doesn't support APP_CMD");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return sdmmc_send_cmd(card, cmd);
}


static esp_err_t sdmmc_send_cmd_go_idle_state(sdmmc_card_t* card)
{
    sdmmc_command_t cmd = {
        .opcode = MMC_GO_IDLE_STATE,
        .flags = SCF_CMD_BC | SCF_RSP_R0,
    };
    return sdmmc_send_cmd(card, &cmd);
}


static esp_err_t sdmmc_send_cmd_send_if_cond(sdmmc_card_t* card, uint32_t ocr)
{
    const uint8_t pattern = 0xaa; /* any pattern will do here */
    sdmmc_command_t cmd = {
        .opcode = SD_SEND_IF_COND,
        .arg = (((ocr & SD_OCR_VOL_MASK) != 0) << 8) | pattern,
        .flags = SCF_CMD_BCR | SCF_RSP_R7,
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t response = cmd.response[0] & 0xff;
    if (response != pattern) {
        log_d( "%s: received=0x%x expected=0x%x", __func__, response, pattern);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t sdmmc_send_cmd_send_op_cond(sdmmc_card_t* card, uint32_t ocr, uint32_t *ocrp)
{
    esp_err_t err;

    sdmmc_command_t cmd = {
            .arg = ocr,
            .flags = SCF_CMD_BCR | SCF_RSP_R3,
            .opcode = SD_APP_OP_COND
    };
    int nretries = 100;   // arbitrary, BSD driver uses this value
    for (; nretries != 0; --nretries)  {
        bzero(&cmd, sizeof cmd);
        cmd.arg = ocr;
        cmd.flags = SCF_CMD_BCR | SCF_RSP_R3;
        if (card->host.flags & SDMMC_HOST_MMC_CARD) { /* MMC mode */
            cmd.arg &= ~MMC_OCR_ACCESS_MODE_MASK;
            cmd.arg |= MMC_OCR_SECTOR_MODE;
            cmd.opcode = MMC_SEND_OP_COND;
            err = sdmmc_send_cmd(card, &cmd);
        } else { /* SD mode */
            cmd.opcode = SD_APP_OP_COND;
            err = sdmmc_send_app_cmd(card, &cmd);
        }
        if (err != ESP_OK) {
            return err;
        }
        // In SD protocol, card sets MEM_READY bit in OCR when it is ready.
        // In SPI protocol, card clears IDLE_STATE bit in R1 response.
        if (!host_is_spi(card)) {
            if ((MMC_R3(cmd.response) & MMC_OCR_MEM_READY) ||
                ocr == 0) {
                break;
            }
        } else {
            if ((SD_SPI_R1(cmd.response) & SD_SPI_R1_IDLE_STATE) == 0) {
                break;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (nretries == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (ocrp) {
        *ocrp = MMC_R3(cmd.response);
    }
    return ESP_OK;
}

static esp_err_t sdmmc_send_cmd_read_ocr(sdmmc_card_t *card, uint32_t *ocrp)
{
    assert(ocrp);
    sdmmc_command_t cmd = {
        .opcode = SD_READ_OCR,
        .flags = SCF_CMD_BCR | SCF_RSP_R2
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    *ocrp = SD_SPI_R3(cmd.response);
    return ESP_OK;
}

static esp_err_t sdmmc_decode_cid(sdmmc_response_t resp, sdmmc_cid_t* out_cid)
{
    out_cid->mfg_id = SD_CID_MID(resp);
    out_cid->oem_id = SD_CID_OID(resp);
    SD_CID_PNM_CPY(resp, out_cid->name);
    out_cid->revision = SD_CID_REV(resp);
    out_cid->serial = SD_CID_PSN(resp);
    out_cid->date = SD_CID_MDT(resp);
    return ESP_OK;
}

static esp_err_t sdmmc_send_cmd_all_send_cid(sdmmc_card_t* card, sdmmc_cid_t* out_cid)
{
    assert(out_cid);
    sdmmc_command_t cmd = {
            .opcode = MMC_ALL_SEND_CID,
            .flags = SCF_CMD_BCR | SCF_RSP_R2
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    return sdmmc_decode_cid(cmd.response, out_cid);
}

static esp_err_t sdmmc_send_cmd_send_cid(sdmmc_card_t *card, sdmmc_cid_t *out_cid)
{
    assert(out_cid);
    assert(host_is_spi(card) && "SEND_CID should only be used in SPI mode");
    sdmmc_response_t buf;
    sdmmc_command_t cmd = {
        .opcode = MMC_SEND_CID,
        .flags = SCF_CMD_READ | SCF_CMD_ADTC,
        .arg = 0,
        .data = &buf[0],
        .datalen = sizeof(buf)
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    flip_byte_order(buf, sizeof(buf));
    return sdmmc_decode_cid(buf, out_cid);
}


static esp_err_t sdmmc_send_cmd_set_relative_addr(sdmmc_card_t* card, uint16_t* out_rca)
{
    static uint16_t next_rca_mmc = 0;
    assert(out_rca);
    sdmmc_command_t cmd = {
            .opcode = SD_SEND_RELATIVE_ADDR,
            .flags = SCF_CMD_BCR | SCF_RSP_R6
    };
    if (card->host.flags & SDMMC_HOST_MMC_CARD) {
        // MMC cards expect you to set the RCA, so just keep a counter of them
        next_rca_mmc++;
        if (next_rca_mmc == 0) /* 0 means deselcted, so can't use that for an RCA */
            next_rca_mmc++;
        cmd.arg = MMC_ARG_RCA(next_rca_mmc);
    }

    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    *out_rca = (card->host.flags & SDMMC_HOST_MMC_CARD) ? next_rca_mmc : SD_R6_RCA(cmd.response);
    return ESP_OK;
}


static esp_err_t sdmmc_send_cmd_set_blocklen(sdmmc_card_t* card, sdmmc_csd_t* csd)
{
    sdmmc_command_t cmd = {
            .opcode = MMC_SET_BLOCKLEN,
            .arg = csd->sector_size,
            .flags = SCF_CMD_AC | SCF_RSP_R1
    };
    return sdmmc_send_cmd(card, &cmd);
}

static esp_err_t mmc_decode_csd(sdmmc_response_t response, sdmmc_csd_t* out_csd)
{
    out_csd->csd_ver = MMC_CSD_CSDVER(response);
    if (out_csd->csd_ver == MMC_CSD_CSDVER_1_0 ||
            out_csd->csd_ver == MMC_CSD_CSDVER_2_0 ||
            out_csd->csd_ver == MMC_CSD_CSDVER_EXT_CSD) {
        out_csd->mmc_ver = MMC_CSD_MMCVER(response);
        out_csd->capacity = MMC_CSD_CAPACITY(response);
        out_csd->read_block_len = MMC_CSD_READ_BL_LEN(response);
    } else {
        log_e( "unknown MMC CSD structure version 0x%x\n", out_csd->csd_ver);
        return 1;
    }
    int read_bl_size = 1 << out_csd->read_block_len;
    out_csd->sector_size = MIN(read_bl_size, 512);
    if (out_csd->sector_size < read_bl_size) {
        out_csd->capacity *= read_bl_size / out_csd->sector_size;
    }
    /* MMC special handling? */
    int speed = SD_CSD_SPEED(response);
    if (speed == SD_CSD_SPEED_50_MHZ) {
        out_csd->tr_speed = 50000000;
    } else {
        out_csd->tr_speed = 25000000;
    }
    return ESP_OK;
}

static esp_err_t sd_decode_csd(sdmmc_response_t response, sdmmc_csd_t* out_csd)
{
    out_csd->csd_ver = SD_CSD_CSDVER(response);
    switch (out_csd->csd_ver) {
    case SD_CSD_CSDVER_2_0:
        out_csd->capacity = SD_CSD_V2_CAPACITY(response);
        out_csd->read_block_len = SD_CSD_V2_BL_LEN;
        break;
    case SD_CSD_CSDVER_1_0:
        out_csd->capacity = SD_CSD_CAPACITY(response);
        out_csd->read_block_len = SD_CSD_READ_BL_LEN(response);
        break;
    default:
        log_e( "unknown SD CSD structure version 0x%x", out_csd->csd_ver);
        return ESP_ERR_NOT_SUPPORTED;
    }
    out_csd->card_command_class = SD_CSD_CCC(response);
    int read_bl_size = 1 << out_csd->read_block_len;
    out_csd->sector_size = MIN(read_bl_size, 512);
    if (out_csd->sector_size < read_bl_size) {
        out_csd->capacity *= read_bl_size / out_csd->sector_size;
    }
    int speed = SD_CSD_SPEED(response);
    if (speed == SD_CSD_SPEED_50_MHZ) {
        out_csd->tr_speed = 50000000;
    } else {
        out_csd->tr_speed = 25000000;
    }
    return ESP_OK;
}

static esp_err_t sdmmc_send_cmd_send_csd(sdmmc_card_t* card, sdmmc_csd_t* out_csd)
{
    /* The trick with SEND_CSD is that in SPI mode, it acts as a data read
     * command, while in SD mode it is an AC command with R2 response.
     */
    sdmmc_response_t spi_buf;
    const bool is_spi = host_is_spi(card);
    sdmmc_command_t cmd = {
            .opcode = MMC_SEND_CSD,
            .arg = is_spi ? 0 : MMC_ARG_RCA(card->rca),
            .flags = is_spi ? (SCF_CMD_READ | SCF_CMD_ADTC | SCF_RSP_R1) :
                    (SCF_CMD_AC | SCF_RSP_R2),
            .data = is_spi ? &spi_buf[0] : 0,
            .datalen = is_spi ? sizeof(spi_buf) : 0,
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    
    if (is_spi) {
        flip_byte_order(spi_buf,  sizeof(spi_buf));
    }

    if (card->host.flags & SDMMC_HOST_MMC_CARD) /* MMC mode */
        err = mmc_decode_csd(cmd.response, out_csd);
    else /* SD mode */
        err = sd_decode_csd(cmd.response, out_csd);
    return err;
}

static esp_err_t sdmmc_mem_send_cxd_data(sdmmc_card_t* card , int opcode, void *data, size_t datalen)
{
    sdmmc_command_t cmd;
    void *ptr = NULL;
    esp_err_t error = ESP_OK;

    ptr = malloc(datalen);
    if (ptr == NULL) {
        error = ESP_ERR_NO_MEM;
        goto out;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.data = ptr;
    cmd.datalen = datalen;
    cmd.blklen = datalen;
    cmd.opcode = opcode;
    cmd.arg = 0;
    cmd.flags = SCF_CMD_ADTC | SCF_CMD_READ;
    if (opcode == MMC_SEND_EXT_CSD)
        cmd.flags |= SCF_RSP_R1;
    else
        cmd.flags |= SCF_RSP_R2;

    error = sdmmc_send_cmd(card, &cmd);
    if (error == 0)
        memcpy(data, ptr, datalen);

out:
    if (ptr != NULL)
        free(ptr);

    return error;
}

static esp_err_t sdmmc_send_cmd_select_card(sdmmc_card_t* card, uint32_t rca)
{
    /* Don't expect to see a response when de-selecting a card */
    uint32_t response = (rca == 0) ? 0 : SCF_RSP_R1;
    sdmmc_command_t cmd = {
            .opcode = MMC_SELECT_CARD,
            .arg = MMC_ARG_RCA(rca),
            .flags = SCF_CMD_AC | response
    };
    return sdmmc_send_cmd(card, &cmd);
}

/*static esp_err_t sdmmc_decode_scr(uint32_t *raw_scr, sdmmc_scr_t* out_scr)
{
    sdmmc_response_t resp = {0xabababab, 0xabababab, 0x12345678, 0x09abcdef};
    resp[1] = __builtin_bswap32(raw_scr[0]);
    resp[0] = __builtin_bswap32(raw_scr[1]);
    int ver = SCR_STRUCTURE(resp);
    if (ver != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    out_scr->sd_spec = SCR_SD_SPEC(resp);
    out_scr->bus_width = SCR_SD_BUS_WIDTHS(resp);
    return ESP_OK;
}*/

/*static esp_err_t sdmmc_send_cmd_send_scr(sdmmc_card_t* card, sdmmc_scr_t *out_scr)
{
    size_t datalen = 8;
    uint32_t* buf = (uint32_t*) heap_caps_malloc(datalen, MALLOC_CAP_DMA);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sdmmc_command_t cmd = {
            .data = buf,
            .datalen = datalen,
            .blklen = datalen,
            .flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1,
            .opcode = SD_APP_SEND_SCR
    };
    esp_err_t err = sdmmc_send_app_cmd(card, &cmd);
    if (err == ESP_OK) {
        err = sdmmc_decode_scr(buf, out_scr);
    }
    free(buf);
    return err;
}*/

/*static esp_err_t sdmmc_send_cmd_set_bus_width(sdmmc_card_t* card, int width)
{
    uint8_t ignored[8];
    sdmmc_command_t cmd = {
            .opcode = SD_APP_SET_BUS_WIDTH,
            .flags = SCF_RSP_R1 | SCF_CMD_AC,
            .arg = (width == 4) ? SD_ARG_BUS_WIDTH_4 : SD_ARG_BUS_WIDTH_1,
            .data = ignored,
            .datalen = 8,
            .blklen = 4,
    };

    return sdmmc_send_app_cmd(card, &cmd);
}*/

/*static esp_err_t sdmmc_mmc_command_set(sdmmc_card_t* card, uint8_t set)
{
    sdmmc_command_t cmd = {
            .opcode = MMC_SWITCH,
            .arg = (MMC_SWITCH_MODE_CMD_SET << 24) | set,
            .flags = SCF_RSP_R1B | SCF_CMD_AC,
            .data = 0,
            .datalen = 0,
            .blklen = 0,
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err == ESP_OK) {
        //check response bit to see that switch was accepted
        if (cmd.response[0] & MMC_R1_SWITCH_ERROR)
            err = ESP_ERR_INVALID_RESPONSE;
    }

    return err;
}*/
static esp_err_t sdmmc_mmc_switch(sdmmc_card_t* card, uint8_t set, uint8_t index, uint8_t value)
{
    sdmmc_command_t cmd = {
            .opcode = MMC_SWITCH,
            .arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) | (index << 16) | (value << 8) | set,
            .flags = SCF_RSP_R1B | SCF_CMD_AC,
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err == ESP_OK) {
        //check response bit to see that switch was accepted
        if (MMC_R1(cmd.response) & MMC_R1_SWITCH_ERROR)
            err = ESP_ERR_INVALID_RESPONSE;
    }

    return err;
}

/*static esp_err_t sdmmc_send_cmd_stop_transmission(sdmmc_card_t* card, uint32_t* status)
{
    sdmmc_command_t cmd = {
            .opcode = MMC_STOP_TRANSMISSION,
            .arg = 0,
            .flags = SCF_RSP_R1B | SCF_CMD_AC
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err == 0) {
        *status = MMC_R1(cmd.response);
    }
    return err;
}*/

static esp_err_t sdmmc_send_cmd_crc_on_off(sdmmc_card_t* card, bool crc_enable)
{
    assert(host_is_spi(card) && "CRC_ON_OFF can only be used in SPI mode");
    sdmmc_command_t cmd = {
            .opcode = SD_CRC_ON_OFF,
            .arg = crc_enable ? 1 : 0,
            .flags = SCF_CMD_AC | SCF_RSP_R1
    };
    return sdmmc_send_cmd(card, &cmd);
}

static uint32_t get_host_ocr(float voltage)
{
    // TODO: report exact voltage to the card
    // For now tell that the host has 2.8-3.6V voltage range
    (void) voltage;
    return SD_OCR_VOL_MASK;
}

static void flip_byte_order(uint32_t* response, size_t size)
{
    assert(size % (2 * sizeof(uint32_t)) == 0);
    const size_t n_words = size / sizeof(uint32_t);
    for (int i = 0; i < n_words / 2; ++i) {
        uint32_t left = __builtin_bswap32(response[i]);
        uint32_t right = __builtin_bswap32(response[n_words - i - 1]);
        response[i] = right;
        response[n_words - i - 1] = left;
    }
}

static esp_err_t sdmmc_send_cmd_send_status(sdmmc_card_t* card, uint32_t* out_status)
{
    sdmmc_command_t cmd = {
            .opcode = MMC_SEND_STATUS,
            .arg = MMC_ARG_RCA(card->rca),
            .flags = SCF_CMD_AC | SCF_RSP_R1
    };
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        return err;
    }
    if (out_status) {
        *out_status = MMC_R1(cmd.response);
    }
    return ESP_OK;
}

esp_err_t sdEmmc_write_sectors(sdmmc_card_t* card, const void* src,
        size_t start_block, size_t block_count)
{
    esp_err_t err = ESP_OK;
    size_t block_size = card->csd.sector_size;
    if (esp_ptr_dma_capable(src) && (intptr_t)src % 4 == 0) {
        err = sdEmmc_write_sectors_dma(card, src, start_block, block_count);
    } else {
        // SDMMC peripheral needs DMA-capable buffers. Split the write into
        // separate single block writes, if needed, and allocate a temporary
        // DMA-capable buffer.
        void* tmp_buf = heap_caps_malloc(block_size, MALLOC_CAP_DMA);
        if (tmp_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        const uint8_t* cur_src = (const uint8_t*) src;
        for (size_t i = 0; i < block_count; ++i) {
            memcpy(tmp_buf, cur_src, block_size);
            cur_src += block_size;
            err = sdEmmc_write_sectors_dma(card, tmp_buf, start_block + i, 1);
            if (err != ESP_OK) {
                log_d( "%s: error 0x%x writing block %d+%d",
                        __func__, err, start_block, i);
                break;
            }
        }
        free(tmp_buf);
    }
    return err;
}
#define sdEmmc_MILLIS() ( (uint32_t ) (esp_timer_get_time() / 1000) )

esp_err_t sdEmmc_wait_ready(sdmmc_card_t* card, uint32_t timeout_ms){
    uint32_t status = 0;
    size_t count = 0;
    uint32_t t0 =  sdEmmc_MILLIS();
    if(host_is_spi(card)) return ESP_OK;
    do {
        esp_err_t err = sdmmc_send_cmd_send_status(card, &status);
        if (err != ESP_OK) {
            return err;
        }
        if (++count % 10 == 0) {
            log_v( "waiting for card to become ready (%d)", count);
        }
    }while(!(status & MMC_R1_READY_FOR_DATA) && (sdEmmc_MILLIS() - t0 < timeout_ms));
    return ESP_OK;
}

esp_err_t sdEmmc_write_sectors_dma(sdmmc_card_t* card, const void* src,
        size_t start_block, size_t block_count){

        esp_err_t err = sdEmmc_write_sectors_dma_no_wait(card,src,start_block,block_count);    
        
        if(ESP_OK != err) return err;
        
        return sdEmmc_wait_ready(card,SDMMC_DEFAULT_CMD_TIMEOUT_MS);        
        
}

esp_err_t sdEmmc_write_sectors_dma_no_wait(sdmmc_card_t* card, const void* src,
        size_t start_block, size_t block_count)
{
    if (start_block + block_count > card->csd.capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t block_size = card->csd.sector_size;
    sdmmc_command_t cmd = {
            .flags = SCF_CMD_ADTC | SCF_RSP_R1,
            .blklen = block_size,
            .data = (void*) src,
            .datalen = block_count * block_size,
            .timeout_ms = SDMMC_WRITE_CMD_TIMEOUT_MS
    };
    if (block_count == 1) {
        cmd.opcode = MMC_WRITE_BLOCK_SINGLE;
    } else {
        cmd.opcode = MMC_WRITE_BLOCK_MULTIPLE;
    }
    if (card->ocr & SD_OCR_SDHC_CAP) {
        cmd.arg = start_block;
    } else {
        cmd.arg = start_block * block_size;
    }
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        log_e( "%s: sdmmc_send_cmd returned 0x%x", __func__, err);
        return err;
    }

    return ESP_OK;
}

/*
esp_err_t sdEmmc_read_sectors(sdmmc_card_t* card, void* dst,
        size_t start_block, size_t block_count)
{
    esp_err_t err = ESP_OK;
    size_t block_size = card->csd.sector_size;
    if (esp_ptr_dma_capable(dst) && (intptr_t)dst % 4 == 0) {
        err = sdEmmc_read_sectors_dma(card, dst, start_block, block_count);
    } else {
        // SDMMC peripheral needs DMA-capable buffers. Split the read into
        // separate single block reads, if needed, and allocate a temporary
        // DMA-capable buffer.
        void* tmp_buf = heap_caps_malloc(block_size, MALLOC_CAP_DMA);
        if (tmp_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        uint8_t* cur_dst = (uint8_t*) dst;
        for (size_t i = 0; i < block_count; ++i) {
            err = sdEmmc_read_sectors_dma(card, tmp_buf, start_block + i, 1);
            if (err != ESP_OK) {
                log_d( "%s: error 0x%x writing block %d+%d",
                        __func__, err, start_block, i);
                break;
            }
            memcpy(cur_dst, tmp_buf, block_size);
            cur_dst += block_size;
        }
        free(tmp_buf);
    }
    return err;
}
*/

esp_err_t sdEmmc_read_sectors_dma(sdmmc_card_t* card, void* dst,
        size_t start_block, size_t block_count)
{
	if (!esp_ptr_dma_capable(dst) || (intptr_t)dst % 4 != 0) {
		log_e( "%s: dst must be a DMA, dword aligned buffer", __func__);
		return ESP_ERR_INVALID_ARG; 
	}		
	
    if (start_block + block_count > card->csd.capacity) {
		log_e( "%s: sector range would exceed card capacity", __func__);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t block_size = card->csd.sector_size;
    sdmmc_command_t cmd = {
            .flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1,
            .blklen = block_size,
            .data = (void*) dst,
            .datalen = block_count * block_size
    };
    if (block_count == 1) {
        cmd.opcode = MMC_READ_BLOCK_SINGLE;
    } else {
        cmd.opcode = MMC_READ_BLOCK_MULTIPLE;
    }
    if (card->ocr & SD_OCR_SDHC_CAP) {
        cmd.arg = start_block;
    } else {
        cmd.arg = start_block * block_size;
    }
    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        log_e( "%s: sdmmc_send_cmd returned 0x%x", __func__, err);
        return err;
    }
    uint32_t status = 0;
    size_t count = 0;
    while (!host_is_spi(card) && !(status & MMC_R1_READY_FOR_DATA)) {
        // TODO: add some timeout here
        err = sdmmc_send_cmd_send_status(card, &status);
        if (err != ESP_OK) {
            return err;
        }
        if (++count % 10 == 0) {
            log_v( "waiting for card to become ready (%d)", count);
        }
    }
    return ESP_OK;
}

/*static esp_err_t sdmmc_send_cmd_switch_func(sdmmc_card_t* card,
        uint32_t mode, uint32_t group, uint32_t function,
        sdmmc_switch_func_rsp_t* resp)
{
    if (card->scr.sd_spec < SCR_SD_SPEC_VER_1_10 ||
        ((card->csd.card_command_class & SD_CSD_CCC_SWITCH) == 0)) {
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (group == 0 ||
        group > SD_SFUNC_GROUP_MAX ||
        function > SD_SFUNC_FUNC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode > 1) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t group_shift = (group - 1) << 2;
    // all functions which should not be affected are set to 0xf (no change) 
    uint32_t other_func_mask = (0x00ffffff & ~(0xf << group_shift));
    uint32_t func_val = (function << group_shift) | other_func_mask;

    sdmmc_command_t cmd = {
            .opcode = MMC_SWITCH,
            .flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1,
            .blklen = sizeof(sdmmc_switch_func_rsp_t),
            .data = resp->data,
            .datalen = sizeof(sdmmc_switch_func_rsp_t),
            .arg = (!!mode << 31) | func_val
    };

    esp_err_t err = sdmmc_send_cmd(card, &cmd);
    if (err != ESP_OK) {
        log_e( "%s: sdmmc_send_cmd returned 0x%x", __func__, err);
        return err;
    }
    flip_byte_order(resp->data, sizeof(sdmmc_switch_func_rsp_t));
    uint32_t resp_ver = SD_SFUNC_VER(resp->data);
    if (resp_ver == 0) {
        // busy response is never sent 
    } else if (resp_ver == 1) {
        if (SD_SFUNC_BUSY(resp->data, group) & (1 << function)) {
            log_d( "%s: response indicates function %d:%d is busy",
                    __func__, group, function);
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        log_d( "%s: got an invalid version of SWITCH_FUNC response: 0x%02x",
                __func__, resp_ver);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}*/

/*static esp_err_t sdmmc_enable_hs_mode(sdmmc_card_t* card)
{
    if (card->scr.sd_spec < SCR_SD_SPEC_VER_1_10 ||
        ((card->csd.card_command_class & SD_CSD_CCC_SWITCH) == 0)) {
            return ESP_ERR_NOT_SUPPORTED;
    }
    sdmmc_switch_func_rsp_t* response = (sdmmc_switch_func_rsp_t*)
            heap_caps_malloc(sizeof(*response), MALLOC_CAP_DMA);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sdmmc_send_cmd_switch_func(card, 0, SD_ACCESS_MODE, 0, response);
    if (err != ESP_OK) {
        log_d( "%s: sdmmc_send_cmd_switch_func (1) returned 0x%x", __func__, err);
        goto out;
    }
    uint32_t supported_mask = SD_SFUNC_SUPPORTED(response->data, 1);
    if ((supported_mask & BIT(SD_ACCESS_MODE_SDR25)) == 0) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    err = sdmmc_send_cmd_switch_func(card, 1, SD_ACCESS_MODE, SD_ACCESS_MODE_SDR25, response);
    if (err != ESP_OK) {
        log_d( "%s: sdmmc_send_cmd_switch_func (2) returned 0x%x", __func__, err);
        goto out;
    }

out:
    free(response);
    return err;
}*/
