#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SSH_TARGET_MAX       8
#define SSH_TARGET_NAME_LEN  16
#define SSH_TARGET_HOST_LEN  48
#define SSH_TARGET_USER_LEN  32
#define SSH_TARGET_PASS_LEN  32

typedef struct {
    char     name[SSH_TARGET_NAME_LEN];
    char     host[SSH_TARGET_HOST_LEN];
    uint16_t port;
    char     user[SSH_TARGET_USER_LEN];
    char     pass[SSH_TARGET_PASS_LEN];
} ssh_target_t;

// Load all targets from NVS. Returns count (0..SSH_TARGET_MAX).
int ssh_targets_load(ssh_target_t *out, int max);

// Save a target at the given slot index (0..SSH_TARGET_MAX-1).
// Returns true on success.
bool ssh_target_save(int idx, const ssh_target_t *target);

// Delete a target at the given slot index.
bool ssh_target_delete(int idx, int count);
