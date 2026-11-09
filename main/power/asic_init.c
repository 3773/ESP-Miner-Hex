#include "asic_init.h"
#include "global_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "asic.h"
#include "asic_common.h"
#include "serial.h"
#include "asic_reset.h"
#include "bm1366.h"
#include "vcore.h"
#include "adc.h"
#include "nvs_config.h"
#include "device_config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static const char *TAG = "asic_init";

// ---------------------------------------------------------------------------
// bitaxeHex bring-up
//
// The six BM1366 sit in three stacked voltage domains. CI, NRSTI and BI of
// domains 2 and 3 are shifted up by op-amps that add VDDn - GNDn, which is only
// right while the domains carry the same voltage. Measured on a Hex 304 at
// 1450 mV: chips running -> D1 1.49 V, D2 1.42 V, D3 1.42 V; chips held in
// reset -> D1 1.63 V, D2 0.84 V, D3 1.86 V; chips 5/6 left unconfigured ->
// D3 1.86 V while chips 1-4 hash at only 1.23 V.
//
// The core stack therefore stays off until the bring-up. For every detection
// attempt: stack off, stack on, release the reset at once so all chips run and
// the domains balance, then a short reset pulse. The BM1366 driver then
// addresses all chips and asks each one on its own, because the top domain's
// answers to the broadcast chip id read get lost.
// ---------------------------------------------------------------------------
#define HEX_OFF_MIN_MS 300          // stack off at least this long ...
#define HEX_OFF_RETRY_MIN_MS 1000   // ... on retries
#define HEX_OFF_MAX_MS 3000         // and at most this long (+500 ms per retry)
#define HEX_OFF_DONE_MV 450         // whole stack below this counts as discharged
#define HEX_ON_RELEASE_MS 20        // stack on -> release the reset
#define HEX_RUN_SETTLE_MS 500       // chips running before the reset pulse
#define HEX_RESET_SETTLE_MS 500     // after the reset pulse
static const uint16_t HEX_RESET_PULSE_MS[] = {10, 10, 100}; // per attempt

static GlobalState *hex_state = NULL;

static void hex_log_domains(const char *where)
{
    int stack = ADC_get_stack_mv();
    if (hex_state->hex_domain_sampling == 1) {
        int d[3];
        VCORE_get_hex_domains(hex_state, d);
        ESP_LOGI(TAG, "Hex %s: stack %d mV, D1 %d mV, D2 %d mV, D3 %d mV", where, stack, d[0], d[1], d[2]);
    } else {
        ESP_LOGI(TAG, "Hex %s: stack %d mV", where, stack);
    }
}

// Rev 304 has dividers for VDD1 (GPIO14) and VDD2 (GPIO12); on Rev 302/303
// these pins are open test points. Decide once, with the chips running: the
// readings must sit near 1/3 and 2/3 of the whole stack.
static void hex_detect_domain_sampling(void)
{
    if (hex_state->hex_domain_sampling >= 0) {
        return;
    }
    int stack = ADC_get_stack_mv();
    int vdd1 = ADC_get_hex_vdd_mv(1);
    int vdd2 = ADC_get_hex_vdd_mv(2);
    if (stack < 1500) {
        return; // stack not up, try again on the next attempt
    }
    bool fitted = vdd1 > stack * 20 / 100 && vdd1 < stack * 47 / 100 &&
                  vdd2 > stack * 53 / 100 && vdd2 < stack * 80 / 100;
    hex_state->hex_domain_sampling = fitted ? 1 : 0;
    ESP_LOGI(TAG, "Hex domain sampling %s (stack %d mV, GPIO14 %d mV, GPIO12 %d mV)",
             fitted ? "found (Rev 304)" : "not fitted (Rev 302/303), whole stack only", stack, vdd1, vdd2);
}

static void hex_set_reset(int level)
{
    gpio_set_level(CONFIG_GPIO_ASIC_RESET, level);
}

static void hex_bringup(bm1366_bringup_stage_t stage, int attempt, int found)
{
    GlobalState *g = hex_state;

    if (stage == BM1366_BRINGUP_COUNTED) {
        g->asic_init_attempts = attempt + 1;
        hex_log_domains("after detection");
        return;
    }

    uint16_t mv = g->SELF_TEST_MODULE.is_active ? g->DEVICE_CONFIG.family.asic.default_voltage_mv
                                                 : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    if (attempt == 0) {
        ESP_LOGI(TAG, "Hex bring-up: starting the core stack (%u mV per domain)", mv);
    } else {
        ESP_LOGW(TAG, "Hex bring-up: %d of %d chips answered, power cycling again (attempt %d)",
                 found, g->DEVICE_CONFIG.family.asic_count, attempt + 1);
        hex_log_domains("before off");
    }

    // stack off, chips in reset, wait until the stack is discharged
    asic_hold_reset_low();
    VCORE_set_voltage(g, 0.0f);
    int64_t t0 = esp_timer_get_time();
    int off_min = attempt == 0 ? HEX_OFF_MIN_MS : HEX_OFF_RETRY_MIN_MS;
    int off_max = HEX_OFF_MAX_MS + attempt * 500;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int ms = (int) ((esp_timer_get_time() - t0) / 1000);
        int stack = ADC_get_stack_mv();
        if ((ms >= off_min && stack >= 0 && stack < HEX_OFF_DONE_MV) || ms >= off_max) {
            ESP_LOGI(TAG, "Hex bring-up: stack off for %d ms (%d mV)", ms, stack);
            break;
        }
    }

    // stack on, let all chips run at once so the domains balance
    VCORE_set_voltage(g, (float) mv / 1000.0f);
    vTaskDelay(pdMS_TO_TICKS(HEX_ON_RELEASE_MS));
    hex_set_reset(1);
    vTaskDelay(pdMS_TO_TICKS(HEX_RUN_SETTLE_MS));
    hex_detect_domain_sampling();
    hex_log_domains("chips running");

    // short reset pulse while the domains are balanced
    int pulse = HEX_RESET_PULSE_MS[attempt % (sizeof(HEX_RESET_PULSE_MS) / sizeof(HEX_RESET_PULSE_MS[0]))];
    ESP_LOGI(TAG, "Hex bring-up: reset pulse %d ms", pulse);
    hex_set_reset(0);
    if (pulse <= 20) {
        esp_rom_delay_us(pulse * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(pulse));
    }
    hex_set_reset(1);
    vTaskDelay(pdMS_TO_TICKS(HEX_RESET_SETTLE_MS));
}

bool asic_hex_bringup_register(GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE->DEVICE_CONFIG.family.id != HEX || GLOBAL_STATE->DEVICE_CONFIG.family.asic.id != BM1366 ||
        GLOBAL_STATE->DEVICE_CONFIG.family.voltage_domains < 2) {
        return false;
    }

    hex_state = GLOBAL_STATE;
    ADC_init_hex_domains();
    GLOBAL_STATE->domain_mv[0] = GLOBAL_STATE->domain_mv[1] = GLOBAL_STATE->domain_mv[2] = -1;
    GLOBAL_STATE->hex_domain_sampling = -1;
    GLOBAL_STATE->stack_mv = -1;
    GLOBAL_STATE->hex_bringup = true;
    // the core stack stays off until asic_initialize() (see above)
    GLOBAL_STATE->ASIC_bringup_active = true;
    BM1366_set_bringup_hook(hex_bringup);
    ESP_LOGI(TAG, "Hex bring-up registered (%d domains)", GLOBAL_STATE->DEVICE_CONFIG.family.voltage_domains);
    return true;
}

#define INIT_RESET_LOW_MS 100
#define INIT_RESET_RELEASE_MS 100

#define RETRY_RESET_LOW_MS 10
#define RETRY_RESET_RELEASE_MS 100

#define ASIC_UART_SETTLE_MS 20

static uint8_t asic_initialize_chain(GlobalState *GLOBAL_STATE, asic_init_mode_t mode, uint32_t stabilization_delay_ms)
{
    const char *mode_str = (mode == ASIC_INIT_COLD_BOOT) ? "cold boot" : "recovery";
    const uint8_t max_attempts = GLOBAL_STATE->DEVICE_CONFIG.family.asic.init_retry_attempts > 0
                                     ? GLOBAL_STATE->DEVICE_CONFIG.family.asic.init_retry_attempts
                                     : 1;
    ESP_LOGI(TAG, "Starting ASIC initialization (%s mode)", mode_str);

    if (asic_reset(INIT_RESET_LOW_MS, INIT_RESET_RELEASE_MS) != ESP_OK) {
        GLOBAL_STATE->SYSTEM_MODULE.asic_status = "ASIC reset failed";
        ESP_LOGE(TAG, "ASIC reset failed!");
        return 0;
    }

    // Check actual UART state for safety
    bool uart_initialized = SERIAL_is_initialized();
    
    // Verify mode matches actual state
    if (mode == ASIC_INIT_COLD_BOOT && uart_initialized) {
        ESP_LOGW(TAG, "Cold boot mode but UART already initialized - will reset baud only");
    } else if (mode == ASIC_INIT_RECOVERY && !uart_initialized) {
        ESP_LOGW(TAG, "Recovery mode but UART not initialized - will do full init");
    }
    
    // Use actual state for decision, not just mode
    if (!uart_initialized) {
        // Fresh boot - full UART initialization
        ESP_LOGI(TAG, "Performing full UART initialization");
        SERIAL_init();
    } else {
        // Live recovery - ASIC was reset, UART needs baud reset to 115200
        // This preserves the running system and avoids reboot
        ESP_LOGI(TAG, "UART already initialized, resetting baud to %d", UART_FREQ);
        SERIAL_set_baud(UART_FREQ);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint8_t chip_count = 0;
    for (uint8_t attempt = 1; attempt <= max_attempts; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Resetting and re-probing %s chain (%u/%u)",
                     GLOBAL_STATE->DEVICE_CONFIG.family.asic.name, attempt, max_attempts);
            if (SERIAL_set_baud(UART_FREQ) != ESP_OK ||
                asic_reset(RETRY_RESET_LOW_MS, RETRY_RESET_RELEASE_MS) != ESP_OK) {
                GLOBAL_STATE->SYSTEM_MODULE.asic_status = "ASIC retry reset failed";
                ESP_LOGE(TAG, "%s retry reset failed", GLOBAL_STATE->DEVICE_CONFIG.family.asic.name);
                return 0;
            }
        }

        SERIAL_clear_buffer();
        vTaskDelay(pdMS_TO_TICKS(ASIC_UART_SETTLE_MS));

        ESP_LOGI(TAG, "Detecting ASIC chips... attempt %u/%u", attempt, max_attempts);
        clear_asic_chain_error();
        chip_count = ASIC_init(GLOBAL_STATE);

        if (chip_count > 0) {
            break;
        }
    }
    
    if (chip_count == 0) {
        const char *chain_error = get_asic_chain_error();
        ESP_LOGE(TAG, "ASIC initialization failed - chip chain detection failed");
        GLOBAL_STATE->SYSTEM_MODULE.asic_status = chain_error != NULL ? chain_error : "ASIC chain detection failed";
        return 0;
    }

    int max_baud = ASIC_set_max_baud(GLOBAL_STATE);
    if (max_baud == 0 || SERIAL_set_baud(max_baud) != ESP_OK) {
        GLOBAL_STATE->SYSTEM_MODULE.asic_status = "ASIC UART configuration failed";
        ESP_LOGE(TAG, "Failed to configure ASIC UART");
        return 0;
    }
    SERIAL_clear_buffer();

    GLOBAL_STATE->ASIC_initalized = true;
    
    if (stabilization_delay_ms > 0) {
        ESP_LOGI(TAG, "Waiting %u ms for tasks to stabilize...", stabilization_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(stabilization_delay_ms));
    }

    ESP_LOGI(TAG, "ASIC initialized successfully with %d chip(s) (%s mode)", chip_count, mode_str);
    return chip_count;
}

uint8_t asic_initialize(GlobalState *GLOBAL_STATE, asic_init_mode_t mode, uint32_t stabilization_delay_ms)
{
    if (!GLOBAL_STATE->hex_bringup) {
        return asic_initialize_chain(GLOBAL_STATE, mode, stabilization_delay_ms);
    }

    // bitaxeHex: the power management task must not touch the stack meanwhile,
    // on every way out of the init
    GLOBAL_STATE->ASIC_bringup_active = true;
    uint8_t chip_count = asic_initialize_chain(GLOBAL_STATE, mode, stabilization_delay_ms);
    GLOBAL_STATE->asic_detected = chip_count;
    GLOBAL_STATE->ASIC_bringup_active = false;
    return chip_count;
}
