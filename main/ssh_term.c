#include "ssh_term.h"
#include "term.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>

#include <wolfssh/ssh.h>

static const char *TAG = "ssh_term";

// -- Embedded SSH private key (from keys/id_ed25519) --------------------------

extern const uint8_t ssh_key_start[]     asm("_binary_id_ed25519_start");
extern const uint8_t ssh_key_end[]       asm("_binary_id_ed25519_end");
extern const uint8_t ssh_pubkey_start[]  asm("_binary_id_ed25519_pub_start");
extern const uint8_t ssh_pubkey_end[]    asm("_binary_id_ed25519_pub_end");

// -- Per-connection auth context ----------------------------------------------

typedef struct {
    const char *pass;
    int         pass_len;
    bool        pubkey_tried;
    const byte *priv_key;       // OpenSSH binary blob for pubkey auth
    word32      priv_key_sz;
    const byte *pub_key;        // SSH public key blob
    word32      pub_key_sz;
} auth_ctx_t;

static int userauth_cb(byte authType, WS_UserAuthData *authData, void *ctx)
{
    auth_ctx_t *ac = (auth_ctx_t *)ctx;
    if (authType == WOLFSSH_USERAUTH_PUBLICKEY) {
        if (ac->pubkey_tried || ac->priv_key == NULL) {
            return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
        }
        ac->pubkey_tried = true;
        authData->sf.publicKey.publicKeyType   = (const byte *)"ssh-ed25519";
        authData->sf.publicKey.publicKeyTypeSz = 11;
        authData->sf.publicKey.privateKey      = ac->priv_key;
        authData->sf.publicKey.privateKeySz    = ac->priv_key_sz;
        authData->sf.publicKey.publicKey       = ac->pub_key;
        authData->sf.publicKey.publicKeySz     = ac->pub_key_sz;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (authType == WOLFSSH_USERAUTH_PASSWORD) {
        if (ac->pass_len > 0) {
            authData->sf.password.password   = (const byte *)ac->pass;
            authData->sf.password.passwordSz = (word32)ac->pass_len;
            return WOLFSSH_USERAUTH_SUCCESS;
        }
    }
    return WOLFSSH_USERAUTH_FAILURE;
}

// -- Keyboard input task ------------------------------------------------------

typedef struct {
    WOLFSSH       *ssh;
    QueueHandle_t  keys;
    volatile bool  running;
    TaskHandle_t   task;
} kb_ctx_t;

static void keyboard_task(void *arg)
{
    kb_ctx_t *ctx = (kb_ctx_t *)arg;
    char ch;
    while (ctx->running) {
        if (xQueueReceive(ctx->keys, &ch, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (ctx->running && ch != '\0')  // '\0' is reconnect wakeup sentinel
                wolfSSH_stream_send(ctx->ssh, (byte *)&ch, 1);
        }
    }
    ctx->task = NULL;
    vTaskDelete(NULL);
}

// -- Connect helper -----------------------------------------------------------

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect() failed");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

// -- Public API ---------------------------------------------------------------

void ssh_term_init(void)
{
    wolfSSH_Init();
}

void ssh_term_connect(display_t *display, QueueHandle_t keys,
                      const ssh_target_t *target)
{
    static term_t term;
    term_init(&term);

    auth_ctx_t auth = {
        .pass         = target->pass,
        .pass_len     = (int)strlen(target->pass),
        .pubkey_tried = false,
        .priv_key     = NULL,
        .priv_key_sz  = 0,
        .pub_key      = NULL,
        .pub_key_sz   = 0,
    };

    WOLFSSH_CTX *ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "wolfSSH_CTX_new failed");
        return;
    }
    wolfSSH_SetUserAuth(ctx, userauth_cb);

    // Parse the embedded OpenSSH private key
    word32 keySz = (word32)(ssh_key_end - ssh_key_start);
    byte *parsedKey = NULL;
    word32 parsedKeySz = 0;
    const byte *keyType = NULL;
    word32 keyTypeSz = 0;
    int keyRet = wolfSSH_ReadKey_buffer(ssh_key_start, keySz,
                                        WOLFSSH_FORMAT_OPENSSH,
                                        &parsedKey, &parsedKeySz,
                                        &keyType, &keyTypeSz, NULL);
    if (keyRet == WS_SUCCESS) {
        // Load into context for KEX host key algo negotiation
        wolfSSH_CTX_UsePrivateKey_buffer(ctx, ssh_key_start, keySz,
                                         WOLFSSH_FORMAT_OPENSSH);
        // Pass raw OpenSSH blob for signing in auth callback
        auth.priv_key    = parsedKey;
        auth.priv_key_sz = parsedKeySz;
        ESP_LOGI(TAG, "Ed25519 private key parsed (%lu bytes)",
                 (unsigned long)parsedKeySz);
    } else {
        ESP_LOGW(TAG, "Failed to parse private key: %d", keyRet);
    }

    // Parse the embedded SSH public key (for auth request wire format)
    word32 pubSz = (word32)(ssh_pubkey_end - ssh_pubkey_start);
    byte *parsedPub = NULL;
    word32 parsedPubSz = 0;
    const byte *pubType = NULL;
    word32 pubTypeSz = 0;
    keyRet = wolfSSH_ReadKey_buffer(ssh_pubkey_start, pubSz,
                                    WOLFSSH_FORMAT_SSH,
                                    &parsedPub, &parsedPubSz,
                                    &pubType, &pubTypeSz, NULL);
    if (keyRet == WS_SUCCESS) {
        auth.pub_key    = parsedPub;
        auth.pub_key_sz = parsedPubSz;
        ESP_LOGI(TAG, "Ed25519 public key parsed (%lu bytes)",
                 (unsigned long)parsedPubSz);
    } else {
        ESP_LOGW(TAG, "Failed to parse public key: %d", keyRet);
    }

    // Retry loop: TCP connect + SSH handshake (Esc to cancel)
    WOLFSSH *ssh = NULL;
    int fd = -1;
    bool cancelled = false;
    for (int attempt = 1; ; attempt++) {
        // Check for Esc to cancel
        char ch;
        while (xQueueReceive(keys, &ch, 0) == pdTRUE) {
            if (ch == 0x1B) {
                cancelled = true;
                break;
            }
        }
        if (cancelled) {
            ESP_LOGI(TAG, "Connection cancelled by user");
            goto cleanup;
        }

        char msg[24];
        snprintf(msg, sizeof(msg), "ssh try %d...", attempt);
        display_clear(display);
        display_puts(display, 0, 0,                          msg);
        display_puts(display, 0, display->geom.char_h,       target->host);
        display_puts(display, 0, 3 * display->geom.char_h,   "esc to cancel");
        display_flush(display);

        fd = tcp_connect(target->host, target->port);
        if (fd < 0) {
            ESP_LOGW(TAG, "TCP connect failed (attempt %d)", attempt);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (ssh) wolfSSH_free(ssh);
        ssh = wolfSSH_new(ctx);
        if (!ssh) {
            ESP_LOGE(TAG, "wolfSSH_new failed");
            close(fd);
            goto cleanup;
        }
        wolfSSH_SetUserAuthCtx(ssh, &auth);
        wolfSSH_SetUsername(ssh, target->user);
        wolfSSH_SetChannelType(ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0);
        wolfSSH_set_fd(ssh, fd);

        if (wolfSSH_connect(ssh) == WS_SUCCESS) {
            ESP_LOGI(TAG, "SSH connected: %s@%s", target->user, target->host);
            break;
        }

        ESP_LOGW(TAG, "SSH handshake failed: %d (attempt %d)",
                 wolfSSH_get_error(ssh), attempt);
        close(fd);
        fd = -1;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // Connected -- drain any queued keys from menu navigation
    {
        char discard;
        while (xQueueReceive(keys, &discard, 0) == pdTRUE)
            ;
    }

    // Show connected status briefly
    display_clear(display);
    display_puts(display, 0, 0,                    target->user);
    display_puts(display, 0, display->geom.char_h, target->host);
    display_flush(display);

    // Start keyboard input task
    static kb_ctx_t kb_ctx;
    kb_ctx.ssh = ssh;
    kb_ctx.keys = keys;
    kb_ctx.running = true;
    xTaskCreate(keyboard_task, "kb", 4096, &kb_ctx, 5, &kb_ctx.task);

    // Read loop
    byte buf[512];
    while (1) {
        int n = wolfSSH_stream_read(ssh, buf, sizeof(buf));
        if (n > 0) {
            term_feed(&term, buf, n);
            term_render(&term, display);
        } else if (n == WS_CHANNEL_CLOSED || n == WS_EOF) {
            display_clear(display);
            display_puts(display, 0, 0, "conn closed");
            display_flush(display);
            break;
        } else if (n < 0 && n != WS_WANT_READ) {
            ESP_LOGE(TAG, "stream_read error: %d", n);
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Stop keyboard task
    kb_ctx.running = false;
    // Wait for it to exit
    for (int i = 0; i < 20 && kb_ctx.task != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(50));

    close(fd);
    wolfSSH_free(ssh);

cleanup:
    wolfSSH_CTX_free(ctx);
    if (parsedKey) free(parsedKey);
    if (parsedPub) free(parsedPub);

    display_clear(display);
    display_puts(display, 0, 0, "disconnected");
    display_flush(display);
    vTaskDelay(pdMS_TO_TICKS(2000));
}
