#include <stdlib.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "soc/reset_reasons.h"
#include "cJSON.h"

#include "asic_result_task.h"
#include "create_jobs_task.h"
#include "hashrate_monitor_task.h"
#include "fan_controller_task.h"
#include "statistics_task.h"
#include "global_state.h"
#include "system.h"
#include "http_server.h"
#include "protocol_coordinator.h"
#include "i2c_bitaxe.h"
#include "adc.h"
#include "nvs_config.h"
#include "self_test.h"
#include "asic.h"
#include "bap/bap.h"
#include "device_config.h"
#include "connect.h"
#include "asic_reset.h"
#include "asic_init.h"
#include "task_monitor.h"
#include "filesystem.h"
#include "log_buffer.h"
#include "setup_ble.h"
#include "esp_ota_ops.h"

static GlobalState GLOBAL_STATE;

static const char * TAG = "bitaxe";

#define DEFAULT_GPIO_I2C_SDA CONFIG_GPIO_I2C_SDA
#define DEFAULT_GPIO_I2C_SCL CONFIG_GPIO_I2C_SCL

static void heap_alloc_failed_hook(size_t requested_size, uint32_t caps, const char *function_name)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        ESP_EARLY_LOGE(TAG, "%s failed to allocate %zu bytes from PSRAM", function_name, requested_size);
        abort();
    }
}

static void *cjson_malloc_psram(size_t size)
{
    if (esp_psram_is_initialized()) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    return malloc(size);
}

static void cjson_free_psram(void *ptr)
{
    free(ptr);
}

// ESP-Miner-Hex: optional restart fix (setting "Full Restart", NVS fullcorereset).
// Some boards (seen on a GekkoScience Gamma 601) find 0 chips after a CPU-only
// reset (restart from the web UI, OTA, panic, watchdog), while the reset button,
// USB or a power cycle reset the whole digital core and work. With the setting
// on, every CPU-only reset is turned into one full core reset. The RTC_NOINIT
// flag survives that reset and stops a loop; the reason of the first boot is
// kept so the API still shows panics and watchdogs. Off = official behaviour.
#define CORE_RESET_MAGIC 0x48455852u
static RTC_NOINIT_ATTR uint32_t core_reset_magic;
static RTC_NOINIT_ATTR uint32_t core_reset_from;
static RTC_NOINIT_ATTR uint32_t core_reset_reason;
static esp_reset_reason_t boot_reset_reason = ESP_RST_UNKNOWN;

esp_reset_reason_t SYSTEM_reset_reason(void)
{
    return boot_reset_reason == ESP_RST_UNKNOWN ? esp_reset_reason() : boot_reset_reason;
}

static void escalate_cpu_reset(void)
{
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    boot_reset_reason = esp_reset_reason();

    if (core_reset_magic == CORE_RESET_MAGIC) {
        core_reset_magic = 0;
        boot_reset_reason = (esp_reset_reason_t) core_reset_reason;
        ESP_LOGW(TAG, "Full restart done (CPU reset 0x%02x -> 0x%02x), original reset reason %d",
                 (unsigned) core_reset_from, (unsigned) reason, (int) boot_reset_reason);
        return;
    }

    if (!nvs_config_get_bool(NVS_CONFIG_FULL_CORE_RESET)) {
        return;
    }

    switch (reason) {
    case RESET_REASON_CPU0_SW:
    case RESET_REASON_CPU0_MWDT0:
    case RESET_REASON_CPU0_MWDT1:
    case RESET_REASON_CPU0_RTC_WDT:
        break;
    default:
        return;
    }

    ESP_LOGW(TAG, "CPU-only reset (0x%02x) and Full Restart is on: resetting the whole chip", (unsigned) reason);
    core_reset_from = reason;
    core_reset_reason = boot_reset_reason;
    core_reset_magic = CORE_RESET_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_rom_software_reset_system();
}

void app_main(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(heap_alloc_failed_hook));

    cJSON_Hooks hooks = {
        .malloc_fn = cjson_malloc_psram,
        .free_fn = cjson_free_psram
    };
    cJSON_InitHooks(&hooks);
    if (esp_psram_is_initialized()) {
        GLOBAL_STATE.psram_is_available = true;
        log_buffer_init();
    } else {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
    }

    ESP_LOGI(TAG, "Welcome to the bitaxe - FOSS || GTFO!");

    if (xTaskCreateWithCaps(cpu_monitor_task, "cpu_monitor", 4096, (void *)&GLOBAL_STATE, 1, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating cpu monitor task");
    }
#ifdef CONFIG_ENABLE_TASK_MONITOR
    if (xTaskCreateWithCaps(task_monitor_task, "task_monitor", 8192, NULL, 1, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating task monitor task");
    }
#endif
  
    // Initialize RST pin to low early to minimize ASIC power consumption
    ESP_ERROR_CHECK(asic_hold_reset_low());
    ESP_LOGI(TAG, "RST pin initialized to low");

    // Allow the ASIC reset line to settle before continuing startup.
    vTaskDelay(100 / portTICK_PERIOD_MS);
    // Init ADC
    ADC_init();

    // initialize the ESP32 NVS
    if (nvs_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    // ESP-Miner-Hex: before anything talks to the voltage regulator or the ASICs
    escalate_cpu_reset();

    // Check firmware version migration (resets useCustomWWW on update/downgrade)
    SYSTEM_check_firmware_migration();

    // Confirm app validity for OTA rollback
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA update, confirming app validity");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Ensure SSID is initialized before any screen/self-test uses it.
    GLOBAL_STATE.SYSTEM_MODULE.ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);
    if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
        ESP_LOGW(TAG, "No SSID configured in NVS, using empty string");
        GLOBAL_STATE.SYSTEM_MODULE.ssid = strdup("");
        if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for SSID");
            return;
        }
    }

    if (device_config_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device config");
        return;
    }

    // Init I2C
    if (GLOBAL_STATE.DEVICE_CONFIG.pins.i2c != NULL) {
        ESP_ERROR_CHECK(i2c_bitaxe_init(GLOBAL_STATE.DEVICE_CONFIG.pins.i2c->sda, GLOBAL_STATE.DEVICE_CONFIG.pins.i2c->scl));
        ESP_LOGI(TAG, "I2C initialized successfully");
    } else {
        ESP_LOGI(TAG, "I2C pins not configured for board; skipping I2C initialization");
    }

    // wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    if (self_test_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init self test");
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);
    if (scoreboard_init(&GLOBAL_STATE.SYSTEM_MODULE.scoreboard) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init scoreboard");
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        wifi_init(&GLOBAL_STATE);
    }

    esp_err_t system_init_ret = SYSTEM_init_peripherals(&GLOBAL_STATE);
    
    if (system_init_ret == ESP_OK) {
        // bitaxeHex: core stack stays off until the controlled bring-up in asic_initialize()
        asic_hex_bringup_register(&GLOBAL_STATE);
        if (xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating power management task");
        }
        if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
            if (xTaskCreate(FAN_CONTROLLER_task, "fan_controller", 8192, (void *) &GLOBAL_STATE, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating fan controller task");
            }
        }
    } else {
        ESP_LOGE(TAG, "Critical peripheral initialization failure (%s). Entering degraded mode.",
                 esp_err_to_name(system_init_ret));
    }
    
    // Read version info (from SPIFFS if custom WWW is active)
    SYSTEM_init_versions(&GLOBAL_STATE);

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        // start the API for AxeOS
        start_rest_server(&GLOBAL_STATE);
    }

    // Pre-cache partition descriptions and space usage percentage
    SYSTEM_init_partitions(&GLOBAL_STATE);

    // Initialize BAP interface
    esp_err_t bap_ret = BAP_init(&GLOBAL_STATE);
    if (bap_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BAP interface: %d", bap_ret);
        // Continue anyway, as BAP is not critical for core functionality
    }

    // While the device is still in setup mode (config AP up but no WiFi
    // connection), expose the BLE provisioning service so the miner can be
    // configured over Bluetooth. A short grace period avoids spinning up BLE on
    // a normal boot that connects within a few seconds. setup_ble_start() is
    // idempotent and only takes effect once the AP is actually enabled.
    int setup_ble_grace_ms = 0;
    while (!GLOBAL_STATE.SYSTEM_MODULE.is_connected) {
        if (GLOBAL_STATE.SYSTEM_MODULE.ap_enabled && setup_ble_grace_ms >= 5000) {
            setup_ble_start(&GLOBAL_STATE);
        }
        setup_ble_grace_ms += 100;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Connected to WiFi: tear down the setup BLE service to free the radio.
    setup_ble_stop();

    queue_init(&GLOBAL_STATE.stratum_queue);

    // The self-test feeds create_jobs_task a hardcoded stratum V1 mock job.
    // SYSTEM_init_system() picked the protocol from the configured pool, so pin
    // V1 here — before create_jobs_task latches it — or an SV2-configured device
    // would cast the mock mining_notify to sv2_job_t.
    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.stratum_protocol = STRATUM_PROTOCOL_V1;
    }

    if (system_init_ret == ESP_OK) {
        if (asic_initialize(&GLOBAL_STATE, ASIC_INIT_COLD_BOOT, 0) == 0) {
            if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
                return;
            }

            self_test_show_message(&GLOBAL_STATE, GLOBAL_STATE.SYSTEM_MODULE.asic_status);
            system_init_ret = ESP_FAIL;
        } else {
            if (xTaskCreate(create_jobs_task, "stratum miner", 8192, (void *) &GLOBAL_STATE, 20, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating stratum miner task");
            }
            if (xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating asic result task");
            }

            if (xTaskCreateWithCaps(hashrate_monitor_task, "hashrate monitor", 8192, (void *) &GLOBAL_STATE, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
                ESP_LOGE(TAG, "Error creating hashrate monitor task");
            }
            if (xTaskCreateWithCaps(statistics_task, "statistics", 8192, (void *) &GLOBAL_STATE, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
                ESP_LOGE(TAG, "Error creating statistics task");
            }
        }
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        protocol_coordinator_init(&GLOBAL_STATE);
        if (xTaskCreateWithCaps(protocol_coordinator_task, "protocol coord", 8192, (void *) &GLOBAL_STATE, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Error creating protocol coordinator task");
        }
    }

    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;
        if (xTaskCreateWithCaps(self_test_task, "self_test", 8192, (void *) &GLOBAL_STATE, 10, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Error creating self test task");
        }
    }
}
