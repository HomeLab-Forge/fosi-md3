#include "usb_msc.h"

#include <string.h>

#include "class/msc/msc.h"
#include "class/msc/msc_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"
#include "tinyusb.h"
#include "usb_ctl.h"

static const char *TAG = "usb_msc";

#define MSC_ASC_MEDIUM_NOT_PRESENT 0x3A
#define MSC_ASC_INVALID_COMMAND_OPERATION_CODE 0x20
#define MSC_ASC_MEDIUM_CHANGED 0x28  // NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED
#define MSC_ASCQ 0x00

// A volume the firmware mounted for itself goes back to the host after this much local idle time.
#define VOLUME_IDLE_US (2 * 1000 * 1000)

static bool s_installed;
static volatile int64_t s_last_poll_us;

esp_err_t usb_msc_init(void)
{
    if (s_installed) {
        return ESP_OK;
    }
    if (!storage_sector_count() || storage_sector_size() != 4096) {
        return ESP_ERR_INVALID_STATE;
    }
    const tinyusb_config_t config = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
        .self_powered = false,
        .vbus_monitor_io = -1,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&config), TAG, "driver install");
    s_installed = true;
    if (usb_ctl_init() != ESP_OK) {
        ESP_LOGW(TAG, "vendor control channel unavailable");
    }
    return ESP_OK;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, "Fosi Aud", 8);
    memcpy(product_id, "Fosi Audio MD3", 14);
    memcpy(product_rev, "1.0", 3);
}

int64_t usb_msc_last_poll_us(void)
{
    return s_last_poll_us;
}

void usb_msc_reconnect(void)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    tud_connect();
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    s_last_poll_us = esp_timer_get_time();
    if (!storage_sector_count()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, MSC_ASC_MEDIUM_NOT_PRESENT, MSC_ASCQ);
        return false;
    }
    // A locally mounted volume stays offline while something holds it or uses it; once idle it
    // goes back to the host, so an eject (by an agent, Finder or macOS at wake) never leaves
    // the drive dark until a replug.
    const esp_err_t released = storage_release_to_host(VOLUME_IDLE_US);
    if (released == ESP_OK) {
        // The firmware may have changed the volume while it held it: make the host re-read.
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, MSC_ASC_MEDIUM_CHANGED, MSC_ASCQ);
        return false;
    }
    if (released != ESP_ERR_NOT_FOUND) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, MSC_ASC_MEDIUM_NOT_PRESENT, MSC_ASCQ);
        return false;
    }
    if (storage_unmount() != ESP_OK) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, MSC_ASC_MEDIUM_NOT_PRESENT, MSC_ASCQ);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = storage_sector_count();
    *block_size = (uint16_t)storage_sector_size();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (load_eject && !start && storage_mount() != ESP_OK) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, MSC_ASC_MEDIUM_NOT_PRESENT, MSC_ASCQ);
        return false;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (storage_read_sector(lba, offset, buffer, bufsize) != ESP_OK) {
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (storage_write_sector(lba, offset, buffer, bufsize) != ESP_OK) {
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (scsi_cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) {
        return 0;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, MSC_ASC_INVALID_COMMAND_OPERATION_CODE, MSC_ASCQ);
    return -1;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return storage_sector_count() > 0;
}

void tud_mount_cb(void)
{
    // The album keeps its volume through a re-enumeration; TEST UNIT READY returns it afterwards.
    if (storage_holds() & STORAGE_HOLD_ALBUM) {
        return;
    }
    if (storage_unmount() != ESP_OK) {
        ESP_LOGW(TAG, "could not detach FAT for USB host");
    }
}

void tud_umount_cb(void)
{
    if (storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "could not remount FAT after USB host");
    }
}
