#include "ssh_targets.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ssh_tgt";
static const char *NVS_NS = "ssh_tgt";

int ssh_targets_load(ssh_target_t *out, int max)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return 0;

    int count = 0;
    for (int i = 0; i < max && i < SSH_TARGET_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "t%d", i);
        size_t len = sizeof(ssh_target_t);
        if (nvs_get_blob(h, key, &out[count], &len) == ESP_OK) {
            count++;
        } else {
            break;  // targets are stored contiguously
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d targets", count);
    return count;
}

bool ssh_target_save(int idx, const ssh_target_t *target)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    char key[16];
    snprintf(key, sizeof(key), "t%d", idx);
    esp_err_t err = nvs_set_blob(h, key, target, sizeof(ssh_target_t));
    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Save t%d failed: %s", idx, esp_err_to_name(err));
    return err == ESP_OK;
}

bool ssh_target_delete(int idx, int count)
{
    if (idx < 0 || idx >= count)
        return false;

    // Load all targets
    ssh_target_t targets[SSH_TARGET_MAX];
    int n = ssh_targets_load(targets, SSH_TARGET_MAX);
    if (idx >= n)
        return false;

    // Shift targets down to fill the gap
    for (int i = idx; i < n - 1; i++)
        targets[i] = targets[i + 1];

    // Rewrite all slots
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    for (int i = 0; i < n - 1; i++) {
        char key[16];
        snprintf(key, sizeof(key), "t%d", i);
        nvs_set_blob(h, key, &targets[i], sizeof(ssh_target_t));
    }

    // Erase the last slot
    char key[16];
    snprintf(key, sizeof(key), "t%d", n - 1);
    nvs_erase_key(h, key);

    nvs_commit(h);
    nvs_close(h);
    return true;
}
