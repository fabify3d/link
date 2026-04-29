#ifndef AUTH_H
#define AUTH_H

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string>
#include <vector>
#include <atomic>

#include "mbedtls/pk.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

static const char *TAG_AUTH = "Auth";

class Auth
{
public:
    Auth() : inited(false), init_in_progress(false)
    {
        mbedtls_pk_init(&pk);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);
        // Seed will be done in the init task to avoid heavy work on constructor
    }

    ~Auth()
    {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    /**
     * Non-blocking init: schedules a FreeRTOS task to do the heavy initialization.
     * Returns immediately with ESP_OK if the init task was started, or error if one is already running.
     */
    esp_err_t init()
    {
        if (init_in_progress.load())
        {
            ESP_LOGW(TAG_AUTH, "Init already in progress");
            return ESP_ERR_INVALID_STATE;
        }
        init_in_progress.store(true);

        // create a dedicated task with larger stack to perform key generation/loading
        BaseType_t rc = xTaskCreatePinnedToCore(
            Auth::init_task_entry, // entry
            "auth_init",           // name
            8192,                  // stack (bytes), 8192 is safe for crypto
            this,                  // param
            tskIDLE_PRIORITY + 5,  // priority
            NULL,                  // handle
            tskNO_AFFINITY         // core (no affinity)
        );
        if (rc != pdPASS)
        {
            init_in_progress.store(false);
            ESP_LOGE(TAG_AUTH, "Failed to create init task");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    /**
     * Blocking init: runs initialization in caller task. Use only if caller has big stack.
     */
    esp_err_t initBlocking()
    {
        if (init_in_progress.load())
            return ESP_ERR_INVALID_STATE;
        init_in_progress.store(true);
        esp_err_t r = init_impl();
        init_in_progress.store(false);
        return r;
    }

    std::string getDeviceId() const { return deviceId; }

    std::string getPublicKeyPEM() const
    {
        // return copy of computed pubPem (string stored on heap)
        return pubPem;
    }

    /**
     * Sign a message (returns base64 signature). Requires initialization finished.
     */
    std::string sign(const std::string &message)
    {
        if (!inited.load())
        {
            ESP_LOGE(TAG_AUTH, "sign() called before init finished");
            return {};
        }

        // hash message
        unsigned char hash[32];
        if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(message.data()),
                           message.size(), hash, 0) != 0)
        {
            ESP_LOGE(TAG_AUTH, "sha256 failed");
            return {};
        }

        // signature buffer on heap
        size_t sig_buf_size = 512;
        std::vector<unsigned char> sigbuf(sig_buf_size);
        size_t sig_len = 0;

        int ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256,
                                  hash, sizeof(hash),
                                  sigbuf.data(), sigbuf.size(), &sig_len,
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret != 0)
        {
            ESP_LOGE(TAG_AUTH, "mbedtls_pk_sign failed: -0x%04X", -ret);
            return {};
        }

        // base64 encode
        size_t b64_max = ((sig_len + 2) / 3) * 4 + 8;
        std::vector<unsigned char> b64buf(b64_max);
        size_t b64len = 0;
        if (mbedtls_base64_encode(b64buf.data(), b64buf.size(), &b64len, sigbuf.data(), sig_len) != 0)
        {
            ESP_LOGE(TAG_AUTH, "base64 encode failed");
            return {};
        }

        return std::string(reinterpret_cast<char *>(b64buf.data()), b64len);
    }

    /**
     * Verify signature using stored public key (for debug). Accepts base64 signature.
     */
    bool verify(const std::string &message, const std::string &b64sig) const
    {
        if (!inited.load())
        {
            ESP_LOGE(TAG_AUTH, "verify() called before init finished");
            return false;
        }

        unsigned char hash[32];
        if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(message.data()),
                           message.size(), hash, 0) != 0)
        {
            ESP_LOGE(TAG_AUTH, "sha256 failed");
            return false;
        }

        // decode b64
        size_t sig_max = 512;
        std::vector<unsigned char> sigbuf(sig_max);
        size_t sig_len = 0;
        if (mbedtls_base64_decode(sigbuf.data(), sigbuf.size(), &sig_len,
                                  reinterpret_cast<const unsigned char *>(b64sig.data()), b64sig.size()) != 0)
        {
            ESP_LOGE(TAG_AUTH, "base64 decode failed");
            return false;
        }

        // mbedtls_pk_verify expects non-const context
        int rc = mbedtls_pk_verify(const_cast<mbedtls_pk_context *>(&pk), MBEDTLS_MD_SHA256,
                                   hash, sizeof(hash), sigbuf.data(), sig_len);
        return rc == 0;
    }

private:
    // atomic flags
    std::atomic<bool> inited;
    std::atomic<bool> init_in_progress;

    // mbedTLS contexts
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    // stored values (on heap)
    std::string deviceId;
    std::string pubPem;

    // FreeRTOS task entry wrapper
    static void init_task_entry(void *arg)
    {
        Auth *self = static_cast<Auth *>(arg);
        esp_err_t r = self->init_impl();
        if (r == ESP_OK)
        {
            self->inited.store(true);
            ESP_LOGI(TAG_AUTH, "Auth init completed successfully");
        }
        else
        {
            ESP_LOGE(TAG_AUTH, "Auth init failed: %s", esp_err_to_name(r));
        }
        self->init_in_progress.store(false);
        vTaskDelete(NULL); // delete this task when done
    }

    // The heavy work: run in a dedicated task with large stack
    esp_err_t init_impl()
    {
        ESP_LOGI(TAG_AUTH, "Auth init_impl starting (heavy work running in separate task)");

        // seed DRBG (do it here to keep work off main stack)
        const char *pers = "auth_init";
        int rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<const unsigned char *>(pers), strlen(pers));
        if (rc != 0)
        {
            ESP_LOGE(TAG_AUTH, "ctr_drbg_seed failed: -0x%04X", -rc);
            return ESP_FAIL;
        }

        // open NVS
        nvs_handle_t handle;
        esp_err_t err = nvs_open("auth", NVS_READWRITE, &handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_AUTH, "nvs_open failed: %s", esp_err_to_name(err));
            return err;
        }

        // check existing privkey
        size_t priv_len = 0;
        err = nvs_get_blob(handle, "privkey", NULL, &priv_len);
        if (err == ESP_OK && priv_len > 0)
        {
            std::vector<unsigned char> privbuf(priv_len);
            err = nvs_get_blob(handle, "privkey", privbuf.data(), &priv_len);
            if (err != ESP_OK)
            {
                nvs_close(handle);
                ESP_LOGE(TAG_AUTH, "nvs_get_blob(privkey) failed: %s", esp_err_to_name(err));
                return err;
            }

            // parse key
            rc = mbedtls_pk_parse_key(&pk, privbuf.data(), privbuf.size(),
                                      nullptr, 0, mbedtls_ctr_drbg_random, &ctr_drbg);
            if (rc != 0)
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_pk_parse_key failed: -0x%04X", -rc);
                nvs_close(handle);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG_AUTH, "Loaded private key from NVS");
        }
        else
        {
            // generate new keypair
            rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
            if (rc != 0)
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_pk_setup failed: -0x%04X", -rc);
                nvs_close(handle);
                return ESP_FAIL;
            }
            rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
            if (rc != 0)
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_ecp_gen_key failed: -0x%04X", -rc);
                nvs_close(handle);
                return ESP_FAIL;
            }

            // write private key PEM to heap buffer
            std::vector<unsigned char> privpem(1600);
            rc = mbedtls_pk_write_key_pem(&pk, privpem.data(), privpem.size());
            if (rc != 0)
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_pk_write_key_pem failed: -0x%04X", -rc);
                nvs_close(handle);
                return ESP_FAIL;
            }
            size_t actual_len = strlen(reinterpret_cast<char *>(privpem.data())) + 1;
            err = nvs_set_blob(handle, "privkey", privpem.data(), actual_len);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG_AUTH, "nvs_set_blob failed: %s", esp_err_to_name(err));
                nvs_close(handle);
                return err;
            }
            nvs_commit(handle);

            ESP_LOGW(TAG_AUTH, "Generated new keypair - will print public key after 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            // get public key pem
            std::vector<unsigned char> pubpem(1024);
            rc = mbedtls_pk_write_pubkey_pem(&pk, pubpem.data(), pubpem.size());
            if (rc == 0)
            {
                // safe to printf as small chunk
                printf("\n==== PUBLIC KEY BEGIN ====\n%s\n==== PUBLIC KEY END ====\n",
                       reinterpret_cast<char *>(pubpem.data()));
            }
            else
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_pk_write_pubkey_pem failed: -0x%04X", -rc);
            }
        }

        nvs_close(handle);

        // compute pubPem and deviceId (heap allocations)
        {
            std::vector<unsigned char> pubpem(1024);
            rc = mbedtls_pk_write_pubkey_pem(&pk, pubpem.data(), pubpem.size());
            if (rc != 0)
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_pk_write_pubkey_pem failed: -0x%04X", -rc);
                return ESP_FAIL;
            }
            pubPem = std::string(reinterpret_cast<char *>(pubpem.data()));
            // compute SHA256(pubPem)
            unsigned char hash[32];
            if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(pubPem.data()), pubPem.size(), hash, 0) != 0)
            {
                ESP_LOGE(TAG_AUTH, "mbedtls_sha256 failed");
                return ESP_FAIL;
            }
            // hex encode
            deviceId.clear();
            static const char hexmap[] = "0123456789abcdef";
            for (int i = 0; i < 32; ++i)
            {
                deviceId.push_back(hexmap[(hash[i] >> 4) & 0xF]);
                deviceId.push_back(hexmap[hash[i] & 0xF]);
            }
            ESP_LOGI(TAG_AUTH, "Computed deviceId=%s", deviceId.c_str());
        }

        return ESP_OK;
    }
};

#endif // AUTH_H
