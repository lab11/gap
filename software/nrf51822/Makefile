PROJECT_NAME = $(shell basename "$(realpath ./)")

APPLICATION_SRCS = $(notdir $(wildcard ./*.c))
#APPLICATION_SRCS += system_nrf51.c
APPLICATION_SRCS += softdevice_handler.c
APPLICATION_SRCS += ble_advdata.c
APPLICATION_SRCS += ble_conn_params.c
APPLICATION_SRCS += app_timer.c
APPLICATION_SRCS += ble_srv_common.c
APPLICATION_SRCS += ble_db_discovery.c
APPLICATION_SRCS += device_manager_central.c
APPLICATION_SRCS += pstorage.c
APPLICATION_SRCS += nrf_delay.c
APPLICATION_SRCS += spi_slave.c

DEVICE = NRF51

ifndef TARGET
	TARGET = GAP
endif

TARGET_UPPER = $(shell echo $(TARGET) | tr a-z A-Z)
BOARD = BOARD_$(TARGET_UPPER)

USE_SOFTDEVICE = s120

SDK_PATH ?= $(HOME)/nrf51822_sdk/nrf51822/
TEMPLATE_PATH ?= $(HOME)/git/nrf51-pure-gcc-setup/template/

LIBRARY_PATHS += ./

CFLAGS = -Os
GDB_PORT_NUMBER = 2331

include $(TEMPLATE_PATH)Makefile
