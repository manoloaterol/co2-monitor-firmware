#include <string.h>

#include "web_private.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

// Firmware update over the network.
//
// The image is streamed straight into the spare app partition as it arrives:
// it is around a megabyte and there is nowhere to buffer it whole. Nothing is
// made bootable until the whole image has been written and validated, so an
// upload cut halfway leaves the running firmware untouched.
//
// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image boots on probation:
// if it does not confirm itself healthy, the bootloader restores the previous
// one on the next reset. See ota_mark_healthy().

#define CHUNK 1024

// Consecutive receive timeouts tolerated, each one the server's receive
// timeout (5 s by default). A slow link gets half a minute of silence; a
// client that vanished without closing the connection does not get to hold
// the server's only task forever.
#define MAX_RECV_TIMEOUTS 6

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *why)
{
    if (h != 0) {
        esp_ota_abort(h);
    }
    ESP_LOGE(TAG, "%s", why);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, why);
    return ESP_FAIL;
}

esp_err_t ota_post(httpd_req_t *req)
{
    char buf[CHUNK];

    if (!web_request_allowed(req, true)) {
        return ESP_FAIL;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return fail(req, 0, "no OTA partition available");
    }

    if (req->content_len <= 0) {
        return fail(req, 0, "empty image");
    }
    if (req->content_len > (int)target->size) {
        return fail(req, 0, "image larger than the partition");
    }

    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return fail(req, 0, "cannot start the update");
    }

    int left     = req->content_len;
    int timeouts = 0;
    while (left > 0) {
        int want = left < CHUNK ? left : CHUNK;
        int got  = httpd_req_recv(req, buf, want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;   // a slow link is not a failed one
        }
        if (got <= 0) {
            return fail(req, handle, "upload interrupted");
        }
        timeouts = 0;

        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return fail(req, handle, "cannot write to flash");
        }

        left -= got;
    }

    // Checks the magic byte and the image header, which is what catches the
    // wrong file being uploaded before it can be made bootable.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return fail(req, 0, err == ESP_ERR_OTA_VALIDATE_FAILED
                            ? "not a valid firmware image"
                            : "cannot finish the update");
    }

    // esp_ota_end() proves the image is sound and built for this chip, not
    // that it is this project: any ESP32-S2 firmware would pass. Another
    // project's image would boot, never confirm itself and roll back, but
    // until then the device would be running someone else's code.
    esp_app_desc_t desc = { 0 };
    const esp_app_desc_t *running = esp_app_get_description();
    if (esp_ota_get_partition_description(target, &desc) != ESP_OK
        || strncmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        ESP_LOGE(TAG, "image is for project \"%.32s\", not \"%s\"",
                 desc.project_name, running->project_name);
        return fail(req, 0, "not firmware for this device");
    }
    ESP_LOGI(TAG, "image version %.32s", desc.version);

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return fail(req, 0, "cannot switch partition");
    }

    ESP_LOGW(TAG, "update written to %s, rebooting", target->label);

    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"ok\":true}");

    web_restart_soon();
    return sent;
}

void ota_mark_healthy(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   // not on probation: either already confirmed or never updated
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot confirm the update: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "update confirmed, rollback cancelled");
}
