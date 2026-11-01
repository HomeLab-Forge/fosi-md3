#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb_msc.c"

static uint32_t fake_count = 5584;
static uint32_t fake_size = 4096;
static esp_err_t fake_mount_result;
static esp_err_t fake_io_result;
static esp_err_t fake_install_result;
static int mount_calls;
static int unmount_calls;
static int install_calls;
static int sense_calls;
static bool fake_mounted;
static uint8_t last_sense_key;
static uint8_t last_sense_code;
static uint32_t last_lba;
static uint32_t fake_holds;
static bool fake_idle;
static int connect_calls;
static int disconnect_calls;
static int ctl_init_calls;
static uint32_t last_offset;
static size_t last_size;

esp_err_t storage_mount(void)
{
    mount_calls++;
    if (fake_mount_result == ESP_OK) {
        fake_mounted = true;
    }
    return fake_mount_result;
}

esp_err_t storage_unmount(void)
{
    unmount_calls++;
    if (fake_mount_result == ESP_OK) {
        fake_mounted = false;
    }
    return fake_mount_result;
}

bool storage_mounted(void)
{
    return fake_mounted;
}

uint32_t storage_sector_count(void)
{
    return fake_count;
}

uint32_t storage_sector_size(void)
{
    return fake_size;
}

esp_err_t storage_read_sector(uint32_t lba, uint32_t offset, void *buffer, size_t size)
{
    (void)buffer;
    last_lba = lba;
    last_offset = offset;
    last_size = size;
    if (lba >= fake_count || size % fake_size || (uint64_t)lba * fake_size + offset + size > (uint64_t)fake_count * fake_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return fake_io_result;
}

esp_err_t storage_write_sector(uint32_t lba, uint32_t offset, const void *buffer, size_t size)
{
    (void)buffer;
    last_lba = lba;
    last_offset = offset;
    last_size = size;
    if (lba >= fake_count || size % fake_size || (uint64_t)lba * fake_size + offset + size > (uint64_t)fake_count * fake_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return fake_io_result;
}

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    (void)config;
    install_calls++;
    return fake_install_result;
}

bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier)
{
    (void)lun;
    (void)add_sense_qualifier;
    sense_calls++;
    last_sense_key = sense_key;
    last_sense_code = add_sense_code;
    return true;
}

int64_t esp_timer_get_time(void)
{
    return 0;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

bool tud_connect(void)
{
    connect_calls++;
    return true;
}

bool tud_disconnect(void)
{
    disconnect_calls++;
    return true;
}

esp_err_t usb_ctl_init(void)
{
    ctl_init_calls++;
    return ESP_OK;
}

uint32_t storage_holds(void)
{
    return fake_holds;
}

// Mirrors storage.c: nothing mounted locally, held or recently used, or released to the host.
esp_err_t storage_release_to_host(int64_t idle_us)
{
    (void)idle_us;
    if (!fake_mounted) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fake_holds || !fake_idle) {
        return ESP_ERR_INVALID_STATE;
    }
    return storage_unmount();
}

static void reset_fakes(void)
{
    fake_count = 5584;
    fake_size = 4096;
    fake_mount_result = ESP_OK;
    fake_io_result = ESP_OK;
    fake_install_result = ESP_OK;
    fake_mounted = false;
    fake_holds = 0;
    fake_idle = false;
    mount_calls = unmount_calls = install_calls = sense_calls = 0;
    connect_calls = disconnect_calls = ctl_init_calls = 0;
    last_lba = last_offset = last_size = 0;
}

int main(void)
{
    reset_fakes();
    uint8_t vendor[8], product[16], revision[4];
    tud_msc_inquiry_cb(0, vendor, product, revision);
    assert(memcmp(vendor, "Fosi Aud", 8) == 0);
    assert(memcmp(product, "Fosi Audio MD3  ", 16) == 0);
    assert(memcmp(revision, "1.0 ", 4) == 0);

    uint32_t blocks = 0;
    uint16_t block_size = 0;
    tud_msc_capacity_cb(0, &blocks, &block_size);
    assert(blocks == 5584);
    assert(block_size == 4096);

    uint8_t data[4096] = {0};
    assert(tud_msc_read10_cb(0, 5583, 0, data, sizeof(data)) == 4096);
    assert(last_lba == 5583 && last_size == 4096);
    assert(tud_msc_read10_cb(0, 5584, 0, data, sizeof(data)) == -1);
    assert(tud_msc_read10_cb(0, 0, 0, data, 512) == -1);
    assert(tud_msc_write10_cb(0, 5583, 0, data, sizeof(data)) == 4096);
    assert(tud_msc_write10_cb(0, 5584, 0, data, sizeof(data)) == -1);

    fake_mounted = true;
    assert(!tud_msc_test_unit_ready_cb(0));
    assert(unmount_calls == 0);
    assert(last_sense_key == SCSI_SENSE_NOT_READY);
    assert(last_sense_code == MSC_ASC_MEDIUM_NOT_PRESENT);

    // An idle local mount goes back to the host, with a UNIT ATTENTION so it re-reads the volume.
    reset_fakes();
    fake_mounted = true;
    fake_idle = true;
    assert(!tud_msc_test_unit_ready_cb(0));
    assert(unmount_calls == 1);
    assert(!fake_mounted);
    assert(last_sense_key == SCSI_SENSE_UNIT_ATTENTION);
    assert(last_sense_code == MSC_ASC_MEDIUM_CHANGED);

    // The album's hold keeps the volume local even when idle.
    reset_fakes();
    fake_mounted = true;
    fake_idle = true;
    fake_holds = STORAGE_HOLD_ALBUM;
    assert(!tud_msc_test_unit_ready_cb(0));
    assert(unmount_calls == 0);
    assert(last_sense_key == SCSI_SENSE_NOT_READY);

    reset_fakes();
    assert(tud_msc_test_unit_ready_cb(0));
    assert(unmount_calls == 1);
    fake_mount_result = ESP_FAIL;
    assert(!tud_msc_test_unit_ready_cb(0));
    assert(last_sense_key == SCSI_SENSE_NOT_READY);
    assert(last_sense_code == MSC_ASC_MEDIUM_NOT_PRESENT);

    reset_fakes();
    assert(tud_msc_start_stop_cb(0, 0, false, true));
    assert(mount_calls == 1);
    fake_mount_result = ESP_FAIL;
    assert(!tud_msc_start_stop_cb(0, 0, false, true));

    reset_fakes();
    fake_holds = STORAGE_HOLD_ALBUM;
    tud_mount_cb();
    assert(unmount_calls == 0);
    fake_holds = 0;
    tud_mount_cb();
    assert(unmount_calls == 1);
    tud_umount_cb();
    assert(mount_calls == 1);
    assert(tud_msc_is_writable_cb(0));

    usb_msc_reconnect();
    assert(disconnect_calls == 1 && connect_calls == 1);

    assert(usb_msc_init() == ESP_OK);
    assert(install_calls == 1);
    assert(ctl_init_calls == 1);
    assert(usb_msc_init() == ESP_OK);
    assert(install_calls == 1);
    s_installed = false;
    fake_size = 512;
    assert(usb_msc_init() == ESP_ERR_INVALID_STATE);

    puts("USB MSC callback tests passed");
    return 0;
}
