/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 Øyvind Harboe                                      *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/image.h>
#include <target/register.h>
#include <target/esp108.h>

/* Regarding performance:
 *
 * Short story - it might be best to leave the performance at
 * current levels.
 *
 * You may see a jump in speed if you change to using
 * 32bit words for the block programming.
 *
 * Its a shame you cannot use the double word as its
 * even faster - but you require external VPP for that mode.
 *
 * Having said all that 16bit writes give us the widest vdd
 * operating range, so may be worth adding a note to that effect.
 *
 */

/* Danger!!!! The STM32F1x and STM32F2x series actually have
 * quite different flash controllers.
 *
 * What's more scary is that the names of the registers and their
 * addresses are the same, but the actual bits and what they do are
 * can be very different.
 *
 * To reduce testing complexity and dangers of regressions,
 * a seperate file is used for stm32fx2x.
 *
 * Sector sizes in kiBytes:
 * 1 MiByte part with 4 x 16, 1 x 64, 7 x 128.
 * 2 MiByte part with 4 x 16, 1 x 64, 7 x 128, 4 x 16, 1 x 64, 7 x 128.
 * 1 MiByte STM32F42x/43x part with DB1M Option set:
 *                    4 x 16, 1 x 64, 3 x 128, 4 x 16, 1 x 64, 3 x 128.
 *
 * STM32F7[4|5]
 * 1 MiByte part with 4 x 32, 1 x 128, 3 x 256.
 *
 * STM32F7[6|7]
 * 1 MiByte part in single bank mode with 4 x 32, 1 x 128, 3 x 256.
 * 1 MiByte part in dual-bank mode two banks with 4 x 16, 1 x 64, 3 x 128 each.
 * 2 MiByte part in single-bank mode with 4 x 32, 1 x 128, 7 x 256.
 * 2 MiByte part in dual-bank mode two banks with 4 x 16, 1 x 64, 7 x 128 each.
 *
 * Protection size is sector size.
 *
 * Tested with STM3220F-EVAL board.
 *
 * STM32F4xx series for reference.
 *
 * RM0090
 * http://www.st.com/web/en/resource/technical/document/reference_manual/DM00031020.pdf
 *
 * PM0059
 * www.st.com/internet/com/TECHNICAL_RESOURCES/TECHNICAL_LITERATURE/
 * PROGRAMMING_MANUAL/CD00233952.pdf
 *
 * STM32F7xx series for reference.
 *
 * RM0385
 * http://www.st.com/web/en/resource/technical/document/reference_manual/DM00124865.pdf
 *
 * RM0410
 * http://www.st.com/resource/en/reference_manual/dm00224583.pdf
 *
 * STM32F1x series - notice that this code was copy, pasted and knocked
 * into a stm32f2x driver, so in case something has been converted or
 * bugs haven't been fixed, here are the original manuals:
 *
 * RM0008 - Reference manual
 *
 * RM0042, the Flash programming manual for low-, medium- high-density and
 * connectivity line STM32F10x devices
 *
 * PM0068, the Flash programming manual for XL-density STM32F10x devices.
 *
 */

#define ESP32_STUB_ASYNC_WRITE_ALGO   0

#define ESP32_STUB_STACK_SZ		(1024)
#define ESP32_STUB_STACK_STAMP	0xCE
#define ESP32_STUB_STACK_DEBUG	1

#define ESP32_STUB_ALGO_TMO		1000 //ms

#define SPI_FLASH_SEC_SIZE  	4096    /**< SPI Flash sector size */

#define STUB_ERR_OK             0
#define STUB_ERR_FAIL           (-1)
#define STUB_ERR_NOT_SUPPORTED  (-2)

#define STUB_CMD_TEST         	0
#define STUB_CMD_FLASH_READ   	1
#define STUB_CMD_FLASH_WRITE  	2
#define STUB_CMD_FLASH_ERASE  	3
#define STUB_CMD_FLASH_TEST   	4

#define STUB_ARGS_MAX         	10
#define STUB_ARGS_FUNC_START  	5

#define ELF_PHF_EXEC			0x1

//TODO: remove hard coded path
#define STUB_PATH  			"/home/alexey/projects/esp/openocd-esp32/contrib/loaders/flash/esp32/build/stub_flasher.elf"

struct esp32_flash_bank {
	int 		probed;
	uint32_t 	user_bank_size;
};

struct esp32_flash_stub {
	struct working_area *code;
	struct working_area *data;
	struct working_area *algo;
	struct working_area *stack;
	struct xtensa_algorithm ainfo;
	struct reg_param reg_params[STUB_ARGS_MAX];
	uint32_t reg_params_count;
};

static int esp32_stub_load(struct target *target, struct esp32_flash_stub *stub)
{
	static const uint8_t esp32_stub_wrapper[] = {
		#include "esp32_stub_wrapper.inc"
	};
	struct image image;
	uint8_t buf[512];

	image.base_address_set = 1;
	image.base_address = 0;
	image.start_address_set = 0;
	int retval = image_open(&image, STUB_PATH, NULL);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to open stub image (%d)!", retval);
		return retval;
	}
	stub->code = NULL;
	stub->data = NULL;
	stub->stack = NULL;
	stub->algo = NULL;
	LOG_INFO("stub: base 0x%x, start 0x%x, %d sections", (unsigned)image.base_address, image.start_address, image.num_sections);
	for (int i = 0; i < image.num_sections; i++) {
		struct imagesection *section = &image.sections[i];
		LOG_INFO("addr %x, sz %d, flags %x", section->base_address, section->size, section->flags);
		if (section->flags & ELF_PHF_EXEC) {
			if (target_alloc_working_area(target, section->size, &stub->code) != ERROR_OK) {
				LOG_ERROR("no working area available, can't alloc space for stub code!");
				image_close(&image);
				retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
				goto _on_error;
			}
			// sanity check, stub is compiled to be run from working area
			if (stub->code->address != section->base_address) {
				LOG_ERROR("working area 0x%x and stub code section 0x%x address mismatch!", section->base_address, stub->code->address);
				image_close(&image);
				retval = ERROR_FAIL;
				goto _on_error;
			}
		} else {
			if (target_alloc_alt_working_area(target, section->size, &stub->data) != ERROR_OK) {
				LOG_ERROR("no working area available, can't alloc space for stub data!");
				image_close(&image);
				retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
				goto _on_error;
			}
			// sanity check, stub is compiled to be run from working area
			if (stub->data->address != section->base_address) {
				LOG_ERROR("working area 0x%x and stub data section 0x%x address mismatch!", section->base_address, stub->data->address);
				image_close(&image);
				retval = ERROR_FAIL;
				goto _on_error;
			}
		}
		uint32_t sec_wr = 0;
		while (sec_wr < section->size) {
			uint32_t nb = section->size - sec_wr > sizeof(buf) ? sizeof(buf) : section->size - sec_wr;
			size_t size_read = 0;
			retval = image_read_section(&image, i, sec_wr, nb, buf, &size_read);
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to read stub section (%d)!", retval);
				image_close(&image);
				goto _on_error;
			}
			retval = target_write_buffer(target, section->base_address + sec_wr, size_read, buf);
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to write stub section!");
				image_close(&image);
				goto _on_error;
			}
			sec_wr += size_read;
		}
	}
	image_close(&image);

	if (target_alloc_alt_working_area(target, ESP32_STUB_STACK_SZ, &stub->stack) != ERROR_OK) {
		LOG_ERROR("no working area available, can't alloc stub stack!");
		image_close(&image);
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto _on_error;
	}
	uint32_t stack_addr = stub->stack->address + ESP32_STUB_STACK_SZ;
	if (stack_addr % 16) {
		LOG_INFO("Adjust stack addr 0x%x", stack_addr);
		stack_addr &= ~0xFUL;
	}

	init_reg_param(&stub->reg_params[0], "a0", 			32, PARAM_OUT);
	init_reg_param(&stub->reg_params[1], "a1", 			32, PARAM_OUT);
	init_reg_param(&stub->reg_params[2], "windowbase", 	32, PARAM_OUT);
	init_reg_param(&stub->reg_params[3], "windowstart", 32, PARAM_OUT);
	init_reg_param(&stub->reg_params[4], "ps", 			32, PARAM_OUT);

	buf_set_u32(stub->reg_params[0].value, 0, 32, image.start_address);
	buf_set_u32(stub->reg_params[1].value, 0, 32, stack_addr);
	buf_set_u32(stub->reg_params[2].value, 0, 32, 0x0); // initial window base
	buf_set_u32(stub->reg_params[3].value, 0, 32, 0x1); // initial window start
	buf_set_u32(stub->reg_params[4].value, 0, 32, 0x60021); // enable WOE, UM and debug interrupts level

	if (target_alloc_working_area(target, sizeof(esp32_stub_wrapper), &stub->algo) != ERROR_OK) {
		LOG_ERROR("no working area available, can't alloc space for stub jumper!");
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		goto _on_error;
	}

	retval = target_write_buffer(target, stub->algo->address, sizeof(esp32_stub_wrapper), esp32_stub_wrapper);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to write stub jumper!");
		goto _on_error;
	}

	return ERROR_OK;

_on_error:
	if (stub->code) {
		target_free_working_area(target, stub->code);
	}
	if (stub->algo) {
		target_free_working_area(target, stub->algo);
	}
	if (stub->data) {
		target_free_alt_working_area(target, stub->data);
	}
	if (stub->stack) {
		target_free_alt_working_area(target, stub->stack);
	}

	return retval;
}

static void esp32_stub_cleanup(struct target *target, struct esp32_flash_stub *stub)
{
	target_free_working_area(target, stub->algo);
	target_free_working_area(target, stub->code);
	target_free_alt_working_area(target, stub->stack);
	target_free_alt_working_area(target, stub->data);
	destroy_reg_param(&stub->reg_params[4]);
	destroy_reg_param(&stub->reg_params[3]);
	destroy_reg_param(&stub->reg_params[2]);
	destroy_reg_param(&stub->reg_params[1]);
	destroy_reg_param(&stub->reg_params[0]);
}

#if ESP32_STUB_STACK_DEBUG
static int esp32_stub_fill_stack(struct target *target, uint32_t stack_addr)
{
	uint8_t buf[256];

	// fill stub stack with canary bytes
	memset(buf, ESP32_STUB_STACK_STAMP, sizeof(buf));
	for (uint32_t i = 0; i < ESP32_STUB_STACK_SZ;) {
		uint32_t wr_sz = ESP32_STUB_STACK_SZ - i >= sizeof(buf) ? sizeof(buf) : ESP32_STUB_STACK_SZ - i;
		int retval = target_write_buffer(target, stack_addr + i, wr_sz, buf);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to init stub stack (%d)!", retval);
			return retval;
		}
		i += wr_sz;
	}
	return ERROR_OK;
}

static int esp32_stub_check_stack(struct target *target, uint32_t stack_addr)
{
	int retval = ERROR_OK;
	uint8_t buf[256];

	// check stub stack for overflow
	for (uint32_t i = 0; i < ESP32_STUB_STACK_SZ;) {
		uint32_t rd_sz = ESP32_STUB_STACK_SZ - i >= sizeof(buf) ? sizeof(buf) : ESP32_STUB_STACK_SZ - i;
		retval = target_read_buffer(target, stack_addr + i, rd_sz, buf);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to read stub stack (%d)!", retval);
			return retval;
		}
		int checked = 0;
		for (uint32_t j = 0; j < rd_sz; j++) {
			if (buf[j] != ESP32_STUB_STACK_STAMP) {
				if (i+j > 0) {
					LOG_ERROR("Stub stack bytes unused %d out of %d", i+j, ESP32_STUB_STACK_SZ);
				} else {
					LOG_ERROR("Stub stack OVF!!!");
					retval = ERROR_FAIL;
				}
				checked = 1;
				break;
			}
		}
		if (checked) {
			break;
		}
		i += rd_sz;
	}
	return retval;
}
#endif

#if ESP32_STUB_ASYNC_WRITE_ALGO
static int esp32_run_flash_async_algo(struct target *target, struct esp32_flash_stub *stub,
		const uint8_t *buffer, uint32_t count, uint32_t block_sz, struct working_area *target_buf)
{
	int retval;

#if ESP32_STUB_STACK_DEBUG
	retval = esp32_stub_fill_stack(target, stub->stack->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to init stub stack (%d)!", retval);
		return retval;
	}
#endif

	LOG_INFO("Algorithm async run @ 0x%x, stack %d bytes @ 0x%x ", stub->algo->address,
			ESP32_STUB_STACK_SZ, stub->stack->address + ESP32_STUB_STACK_SZ);
	retval = target_run_flash_async_algorithm(target, buffer, count, block_sz,
			0, NULL,
			stub->reg_params_count, stub->reg_params,
			target_buf->address, target_buf->size,
			stub->algo->address, 0,
			&stub->ainfo);
	if (retval != ERROR_OK) {
		LOG_ERROR("Algorithm failed!");
		return retval;
	}

#if ESP32_STUB_STACK_DEBUG
	retval = esp32_stub_check_stack(target, stub->stack->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to check stub stack (%d)!", retval);
		return retval;
	}
#endif
	return retval;
}
#endif

static int esp32_run_algo(struct target *target, struct esp32_flash_stub *stub)
{
	int retval;

#if ESP32_STUB_STACK_DEBUG
	retval = esp32_stub_fill_stack(target, stub->stack->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to init stub stack (%d)!", retval);
		return retval;
	}
#endif

	LOG_INFO("Algorithm run @ 0x%x, stack %d bytes @ 0x%x ", stub->algo->address,
			ESP32_STUB_STACK_SZ, stub->stack->address + ESP32_STUB_STACK_SZ);
	retval = target_run_algorithm(target,
			0, NULL,
			stub->reg_params_count, stub->reg_params,
			stub->algo->address, 0,
			ESP32_STUB_ALGO_TMO, &stub->ainfo);
	if (retval != ERROR_OK) {
		LOG_ERROR("Algorithm failed!");
		return retval;
	}

#if ESP32_STUB_STACK_DEBUG
	retval = esp32_stub_check_stack(target, stub->stack->address);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to check stub stack (%d)!", retval);
		return retval;
	}
#endif
	return retval;
}

#if 0
static int esp32_stub_cmd_do(struct target *target, int cmd, ...)
{
	va_list ap;
	struct esp32_flash_stub stub;
	struct working_area *target_buf = NULL;
	uint32_t total_count = 0;
	uint32_t offset = 0;
	uint32_t count = 0;
	uint8_t *buffer = NULL;

	va_start(ap, cmd);

	switch (cmd)
	{
		case STUB_CMD_FLASH_READ:
		case STUB_CMD_FLASH_WRITE:
			offset = va_arg(ap, uint32_t);
			count = va_arg(ap, uint32_t);
			buffer = va_arg(ap, uint8_t *);
			break;
		case STUB_CMD_FLASH_ERASE:
			offset = va_arg(ap, uint32_t);
			count = va_arg(ap, uint32_t);
			break;
		default:
			break;
	}

	stub.algorithm_info.core_mode = XT_MODE_ANY; //TODO: run in ring0 mode

	/* memory buffer */
	uint32_t buffer_size = count;
	if (cmd == STUB_CMD_FLASH_READ || cmd == STUB_CMD_FLASH_WRITE) {
		while (target_alloc_alt_working_area_try(target, buffer_size, &target_buf) != ERROR_OK) {
			buffer_size /= 2;
			if (buffer_size == 0) {
				LOG_ERROR("Failed to alloc target buffer for flash data!");
				va_end(ap);
				return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			}
		}
	}

	int retval = esp32_stub_load(target, &stub);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load stub (%d)!", retval);
		if (target_buf) {
			target_free_alt_working_area(target, target_buf);
		}
		return retval;
	}

	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0], "a2", 32, PARAM_IN_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1], "a3", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2], "a4", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3], "a5", 32, PARAM_OUT);

	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32, cmd);
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+1].value, 0, 32, offset); // flash addr
	if (cmd == STUB_CMD_FLASH_READ || cmd == STUB_CMD_FLASH_WRITE) {
		buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+3].value, 0, 32, target_buf->address); // buffer
	}

	while (total_count < count) {
		uint32_t data_sz = target_buf && target_buf->size < (count - total_count) ? target_buf->size : count - total_count;
		buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+2].value, 0, 32, data_sz); // size

		if (cmd == STUB_CMD_FLASH_WRITE) {
			/*retval = target_write_buffer(target, target_buf->address, data_sz, &buffer[total_count]);
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to write flash data to target's memory (%d)!", retval);
				goto _on_error;
			}*/
			retval = esp32_run_flash_async_algo(target, &stub, STUB_ARGS_FUNC_START+4, buffer, );
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to run algo (%d)!", retval);
				goto _on_error;
			}
		} else {
			retval = esp32_run_algo(target, &stub, STUB_ARGS_FUNC_START+4);
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to run algo (%d)!", retval);
				goto _on_error;
			}
		}
		int flasher_rc = buf_get_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32);
		if (flasher_rc != STUB_ERR_OK) {
			LOG_ERROR("Failed to read flash (%d)!", flasher_rc);
			retval = ERROR_FAIL;
			goto _on_error;
		}
		if (cmd == STUB_CMD_FLASH_READ) {
			retval = target_read_buffer(target, target_buf->address, data_sz, &buffer[total_count]);
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to read flash data from target's memory (%d)!", retval);
				goto _on_error;
			}
		}
		total_count += data_sz;
	}

_on_error:
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3]);
	esp32_stub_cleanup(target, &stub);
	if (target_buf) {
		target_free_alt_working_area(target, target_buf);
	}
	return  retval;
}

static int esp32_size_get(struct target *target, struct esp32_flash_stub *stub, uint32_t *size)
{
	//TODO: implement
	*size = 0xFFFFFFFF;
	return ERROR_OK;
}
#endif

/* flash bank stm32x <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(esp32_flash_bank_command)
{
	struct esp32_flash_bank *esp32_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	esp32_info = malloc(sizeof(struct esp32_flash_bank));
	bank->driver_priv = esp32_info;

	esp32_info->probed = 0;
	esp32_info->user_bank_size = bank->size;

	return ERROR_OK;
}

static int esp32_protect_check(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int esp32_blank_check(struct flash_bank *bank)
{
	return ERROR_OK;
}

static int esp32_erase(struct flash_bank *bank, int first, int last)
{
	int retval = ERROR_OK;
	struct esp32_flash_stub stub;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
	assert((0 <= first) && (first <= last) && (last < bank->num_sectors));
	//TODO: check range
	// int retval = esp32_stub_cmd_do(bank->target, STUB_CMD_FLASH_ERASE,
	// 		first*SPI_FLASH_SEC_SIZE, (last-first+1)*SPI_FLASH_SEC_SIZE);
	// if (retval != ERROR_OK) {
	// 	LOG_ERROR("Failed to exec erase flash cmd (%d)!", retval);
	// 	return retval;
	// }
	stub.ainfo.core_mode = XT_MODE_ANY; //TODO: run in ring0 mode
	stub.reg_params_count = STUB_ARGS_FUNC_START+3;

	retval = esp32_stub_load(bank->target, &stub);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load stub (%d)!", retval);
		return retval;
	}

	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0], "a2", 32, PARAM_IN_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1], "a3", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2], "a4", 32, PARAM_OUT);

	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32, STUB_CMD_FLASH_ERASE);
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+1].value, 0, 32, first*SPI_FLASH_SEC_SIZE); // flash addr
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+2].value, 0, 32, (last-first+1)*SPI_FLASH_SEC_SIZE); // size

	retval = esp32_run_algo(bank->target, &stub);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to run algo (%d)!", retval);
		goto _on_error;
	}
	int flasher_rc = buf_get_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32);
	if (flasher_rc != STUB_ERR_OK) {
		LOG_ERROR("Failed to erase flash (%d)!", flasher_rc);
		retval = ERROR_FAIL;
		goto _on_error;
	}

_on_error:
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2]);
	esp32_stub_cleanup(bank->target, &stub);

	return  retval;
}

static int esp32_protect(struct flash_bank *bank, int set, int first, int last)
{
	return ERROR_OK;
}

static int esp32_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	int retval = ERROR_OK;
	struct esp32_flash_stub stub;
	struct working_area *target_buf = NULL;
#if ESP32_STUB_ASYNC_WRITE_ALGO == 0
	uint32_t total_count = 0;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
	//TODO: check range
	// int retval = esp32_stub_cmd_do(bank->target, STUB_CMD_FLASH_READ, offset, count, buffer);
	// if (retval != ERROR_OK) {
	// 	LOG_ERROR("Failed to exec read flash cmd (%d)!", retval);
	// 	return retval;
	// }
	stub.ainfo.core_mode = XT_MODE_ANY; //TODO: run in ring0 mode
	stub.reg_params_count = STUB_ARGS_FUNC_START+4;

	retval = esp32_stub_load(bank->target, &stub);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load stub (%d)!", retval);
		return retval;
	}

	/* memory buffer */
	uint32_t buffer_size = count;
	while (target_alloc_alt_working_area_try(bank->target, buffer_size, &target_buf) != ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size == 0) {
			LOG_ERROR("Failed to alloc target buffer for flash data!");
			esp32_stub_cleanup(bank->target, &stub);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0], "a2", 32, PARAM_IN_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1], "a3", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2], "a4", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3], "a5", 32, PARAM_OUT);

	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32, STUB_CMD_FLASH_WRITE);
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+1].value, 0, 32, offset); // flash addr
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+3].value, 0, 32, target_buf->address); // buffer

	while (total_count < count) {
		uint32_t data_sz = target_buf->size < (count - total_count) ? target_buf->size : count - total_count;
		buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+2].value, 0, 32, data_sz); // size

		retval = target_write_buffer(bank->target, target_buf->address, data_sz, &buffer[total_count]);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to write flash data to target's memory (%d)!", retval);
			goto _on_error;
		}
		retval = esp32_run_algo(bank->target, &stub);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to run algo (%d)!", retval);
			goto _on_error;
		}
		int flasher_rc = buf_get_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32);
		if (flasher_rc != STUB_ERR_OK) {
			LOG_ERROR("Failed to write flash (%d)!", flasher_rc);
			retval = ERROR_FAIL;
			goto _on_error;
		}
		total_count += data_sz;
	}

_on_error:
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3]);
	esp32_stub_cleanup(bank->target, &stub);
	target_free_alt_working_area(bank->target, target_buf);
#else
	uint8_t *new_buffer = NULL;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x3UL) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-byte alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* If there's an odd number of bytes, the data has to be padded. Duplicate
	 * the buffer and use the normal code path with a single block write since
	 * it's probably cheaper than to special case the last odd write using
	 * discrete accesses. */
	if (count & 0x3UL) {
		new_buffer = malloc((count + 0x3UL) & ~0x3UL);
		if (new_buffer == NULL) {
			LOG_ERROR("no memory for padding buffer");
			return ERROR_FAIL;
		}
		LOG_INFO("odd number of bytes to write, padding with 0xff");
		buffer = memcpy(new_buffer, buffer, count);
		while (count & 0x3UL) { // pad
			new_buffer[count++] = 0xff;
		}
	}

	//TODO: check range
	// int retval = esp32_stub_cmd_do(bank->target, STUB_CMD_FLASH_WRITE, offset, count, buffer);
	// if (retval != ERROR_OK) {
	// 	LOG_ERROR("Failed to exec write flash cmd (%d)!", retval);
	// 	return retval;
	// }
	stub.ainfo.core_mode = XT_MODE_ANY; //TODO: run in ring0 mode
	stub.reg_params_count = STUB_ARGS_FUNC_START+5;

	retval = esp32_stub_load(bank->target, &stub);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load stub (%d)!", retval);
		if (new_buffer) {
			free(new_buffer);
		}
		return retval;
	}

	/* memory buffer */
	uint32_t buffer_size = 16384;
	while (target_alloc_alt_working_area_try(bank->target, buffer_size, &target_buf) != ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size <= 32) { //TODO: avoid hardcoding
			LOG_ERROR("Failed to alloc target buffer for flash data!");
			esp32_stub_cleanup(bank->target, &stub);
			if (new_buffer) {
				free(new_buffer);
			}
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0], "a2", 32, PARAM_IN_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1], "a3", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2], "a4", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3], "a5", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+4], "a6", 32, PARAM_OUT);

	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32, STUB_CMD_FLASH_WRITE);
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+1].value, 0, 32, offset); // flash addr
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+2].value, 0, 32, count); // size
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+3].value, 0, 32, target_buf->address);
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+4].value, 0, 32, target_buf->address + target_buf->size);

	retval = esp32_run_flash_async_algo(bank->target, &stub, buffer, count/4, 4, target_buf);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to run algo (%d)!", retval);
		goto _on_error;
	}
	int flasher_rc = buf_get_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32);
	if (flasher_rc != STUB_ERR_OK) {
		LOG_ERROR("Failed to write flash (%d)!", flasher_rc);
		retval = ERROR_FAIL;
		goto _on_error;
	}

 _on_error:
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+4]);
	esp32_stub_cleanup(bank->target, &stub);
	if (new_buffer) {
		free(new_buffer);
	}
#endif
	return  retval;
}

static int esp32_read(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	int retval = ERROR_OK;
	struct esp32_flash_stub stub;
	struct working_area *target_buf = NULL;
	uint32_t total_count = 0;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
	//TODO: check range
	// int retval = esp32_stub_cmd_do(bank->target, STUB_CMD_FLASH_READ, offset, count, buffer);
	// if (retval != ERROR_OK) {
	// 	LOG_ERROR("Failed to exec read flash cmd (%d)!", retval);
	// 	return retval;
	// }
	stub.ainfo.core_mode = XT_MODE_ANY; //TODO: run in ring0 mode
	stub.reg_params_count = STUB_ARGS_FUNC_START+4;

	retval = esp32_stub_load(bank->target, &stub);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to load stub (%d)!", retval);
		return retval;
	}

	/* memory buffer */
	uint32_t buffer_size = count;
	while (target_alloc_alt_working_area_try(bank->target, buffer_size, &target_buf) != ERROR_OK) {
		buffer_size /= 2;
		if (buffer_size == 0) {
			LOG_ERROR("Failed to alloc target buffer for flash data!");
			esp32_stub_cleanup(bank->target, &stub);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0], "a2", 32, PARAM_IN_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1], "a3", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2], "a4", 32, PARAM_OUT);
	init_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3], "a5", 32, PARAM_OUT);

	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32, STUB_CMD_FLASH_READ);
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+1].value, 0, 32, offset); // flash addr
	buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+3].value, 0, 32, target_buf->address); // buffer

	while (total_count < count) {
		uint32_t data_sz = target_buf->size < (count - total_count) ? target_buf->size : count - total_count;
		buf_set_u32(stub.reg_params[STUB_ARGS_FUNC_START+2].value, 0, 32, data_sz); // size

		retval = esp32_run_algo(bank->target, &stub);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to run algo (%d)!", retval);
			goto _on_error;
		}
		int flasher_rc = buf_get_u32(stub.reg_params[STUB_ARGS_FUNC_START+0].value, 0, 32);
		if (flasher_rc != STUB_ERR_OK) {
			LOG_ERROR("Failed to read flash (%d)!", flasher_rc);
			retval = ERROR_FAIL;
			goto _on_error;
		}
		retval = target_read_buffer(bank->target, target_buf->address, data_sz, &buffer[total_count]);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to read flash data from target's memory (%d)!", retval);
			goto _on_error;
		}
		total_count += data_sz;
	}

_on_error:
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+0]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+1]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+2]);
	destroy_reg_param(&stub.reg_params[STUB_ARGS_FUNC_START+3]);
	esp32_stub_cleanup(bank->target, &stub);
	target_free_alt_working_area(bank->target, target_buf);

	return  retval;
}

static int esp32_probe(struct flash_bank *bank)
{
	struct esp32_flash_bank *esp32_info = bank->driver_priv;

	esp32_info->probed = 0;

	LOG_INFO("flash size = %d KB @ 0x%x", bank->size/1024, bank->base);

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->num_sectors = bank->size / SPI_FLASH_SEC_SIZE;
	bank->sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	for (int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].offset = i*SPI_FLASH_SEC_SIZE;
		bank->sectors[i].size = SPI_FLASH_SEC_SIZE;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}
	LOG_DEBUG("allocated %d sectors", bank->num_sectors);

	//int res = esp32_run_stub(bank->target);

	esp32_info->probed = 1;

	return ERROR_OK;
}

static int esp32_auto_probe(struct flash_bank *bank)
{
	struct esp32_flash_bank *esp32_info = bank->driver_priv;
	if (esp32_info->probed)
		return ERROR_OK;
	return esp32_probe(bank);
}

static int get_esp32_info(struct flash_bank *bank, char *buf, int buf_size)
{
	snprintf(buf, buf_size, "ESP32");
	return ERROR_OK;
}

#if 0
COMMAND_HANDLER(esp32_handle_flash_read_command)
{
	struct flash_bank *bank;
	struct esp32_flash_stub stub;
	uint8_t* buf;
	uint32_t buf_sz = 0, flash_addr = 0x0, flash_sz = 0x0;

	if (CMD_ARGC < 3) {
		command_print(CMD_CTX, "esp32 flash_read <bank> <addr> <size>");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	flash_addr = strtoul(CMD_ARGV[1], NULL, 16);
	if (flash_addr == UINT_MAX) {
		command_print(CMD_CTX, "Invalid flash address!");
		return retval;
	}

	buf_sz = strtoul(CMD_ARGV[2], NULL, 10);
	if (buf_sz == UINT_MAX) {
		command_print(CMD_CTX, "Invalid read size!");
		return retval;
	}

	buf = malloc(buf_sz);
	if (!buf) {
		command_print(CMD_CTX, "failed to alloc data buf!");
		return ERROR_FAIL;
	}

	retval = esp32_stub_load(bank->target, &stub);
	if (retval != ERROR_OK) {
		command_print(CMD_CTX, "Failed to load stub (%d)!", retval);
		free(buf);
		return retval;
	}

	retval = esp32_size_get(bank->target, &stub, &flash_sz);
	if (retval != ERROR_OK) {
		command_print(CMD_CTX, "Failed to get flash size (%d)!", retval);
		goto _on_error;
	}
	if ((flash_addr + buf_sz) > flash_sz) {
		command_print(CMD_CTX, "Invalid read size!");
		goto _on_error;
	}

	retval = esp32_read(bank->target, &stub, flash_addr, buf, buf_sz);
	if (retval != ERROR_OK) {
		command_print(CMD_CTX, "Failed to read flash (%d)!", retval);
		goto _on_error;
	}

	for (uint32_t i = 0; i < buf_sz; i++) {
		command_print_sameline(CMD_CTX, "%x ", buf[i]);
		if (i > 0 && (i % 80) == 0) {
			command_print_sameline(CMD_CTX, "\n");
		}
	}
	command_print_sameline(CMD_CTX, "\n");

_on_error:
	free(buf);
	esp32_stub_cleanup(bank->target, &stub);

	return retval;
}
#endif

static const struct command_registration esp32_exec_command_handlers[] = {
/*
	{
		.name = "flash_read",
		.handler = esp32_handle_flash_read_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id addr size",
		.help = "Read flash device.",
	},
*/
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration esp32_command_handlers[] = {
	{
		.name = "esp32",
		.mode = COMMAND_ANY,
		.help = "esp32 flash command group",
		.usage = "",
		.chain = esp32_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver esp32_flash = {
	.name = "esp32",
	.commands = esp32_command_handlers,
	.flash_bank_command = esp32_flash_bank_command,
	.erase = esp32_erase,
	.protect = esp32_protect,
	.write = esp32_write,
	.read = esp32_read,
	.probe = esp32_probe,
	.auto_probe = esp32_auto_probe,
	.erase_check = esp32_blank_check,
	.protect_check = esp32_protect_check,
	.info = get_esp32_info,
};
