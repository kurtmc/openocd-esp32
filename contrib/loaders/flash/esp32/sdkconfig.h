#ifndef _STUB_SDKCONFIG_H_
#define _STUB_SDKCONFIG_H_

#define CONFIG_SPI_FLASH_ROM_DRIVER_PATCH	1
#define CONFIG_ESP32_APPTRACE_LOCK_ENABLE	0
#define CONFIG_ESP32_APPTRACE_ENABLE		1
#define CONFIG_ESP32_APPTRACE_DEST_TRAX		1
#define CONFIG_ESP32_RTC_CLOCK_SOURCE_EXTERNAL_CRYSTAL	0
#define CONFIG_CONSOLE_UART_NONE			0
#define CONFIG_CONSOLE_UART_NUM				0
#define CONFIG_CONSOLE_UART_CUSTOM			0
#define CONFIG_CONSOLE_UART_BAUDRATE		115200
#define CONFIG_ESP32_XTAL_FREQ				0

#define ESP_APPTRACE_DOWN_BUF_SIZE			16384


// needed due to various checks in IDF headers
#define CONFIG_FREERTOS_MAX_TASK_NAME_LEN	16
// TODO: get clk from PLL settings
#define CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ	240

#endif //_STUB_SDKCONFIG_H_