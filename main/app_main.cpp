/*
 * Itho HRU300 Matter Fan Control
 *
 * Integrates esp-matter FanControl cluster with CC1101 RF communication
 * to control an Itho Daalderop HRU300 WTW ventilation unit.
 *
 * Commands: sent via CC1101 RF as a virtual remote (configurable ID, type
 *           RFTCVE). Reception is interrupt-driven via the CC1101 GDO pin (GPIO 4).
 * Status:   polled over I2C with the 2400/2401 status query (the per-field
 *           datatype table is learned once via 2400; the absolute fanspeed
 *           field drives the mode). RF 31DA broadcasts from the HRU300 are
 *           received as a secondary feedback source.
 *
 * The HRU300 cannot be turned off; speeds are low/medium/high:
 *   PercentSetting detents: 33 = low, 66 = medium, 100 = high (0% → low)
 *   FanMode: 0 Off → low, 1 Low, 2 Medium, 3 High, 5 Auto
 * Mode from status (absolute fanspeed %): <1 off, <32 low, <60 medium, else high
 */

// Arduino and CC1101 — MUST be included before Matter headers to avoid
// INADDR_NONE macro conflict between lwIP (inet.h) and Arduino (IPAddress.h)
#include <Arduino.h>
#include "IthoCC1101.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/i2c.h>
#include <freertos/semphr.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>

static const char *TAG = "itho_fan";
static uint16_t fan_endpoint_id = 0;
static IthoCC1101 itho;

// --- User configuration ---
// RF remote ID the HRU300 is paired with (sniffed from a physical remote).
// Set your own in main/local_config.h (gitignored, see local_config.example.h).
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef ITHO_REMOTE_ID
#define ITHO_REMOTE_ID 0xDE, 0xAD, 0x01 // placeholder — set your own in local_config.h
#endif

// Override Arduino's weak btInUse() so initArduino() does NOT release
// Bluetooth controller memory — Matter needs it for CHIPoBLE commissioning.
// Without this, Arduino calls esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)
// before Matter initializes NimBLE, causing BLE init to fail.
extern "C" bool btInUse() { return true; }

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace esp_matter::cluster;
using namespace chip::app::Clusters;

// FanControl cluster and attribute IDs
static constexpr uint32_t FAN_CONTROL_CLUSTER_ID = 0x0202;
static constexpr uint32_t FAN_MODE_ATTR_ID = 0x0000;
static constexpr uint32_t PERCENT_SETTING_ATTR_ID = 0x0002;
static constexpr uint32_t PERCENT_CURRENT_ATTR_ID = 0x0003;
static constexpr uint8_t FAN_MODE_SEQUENCE = 2; // 2 = Off/Low/Medium/High

// TemperatureMeasurement cluster (I2C 2401 temperature fields)
static constexpr uint32_t TEMP_MEAS_CLUSTER_ID = 0x0402;
static constexpr uint32_t TEMP_MEAS_VALUE_ATTR_ID = 0x0000;

// On/Off cluster (Summer Night Boost switch)
static constexpr uint32_t ON_OFF_CLUSTER_ID = 0x0006;
static constexpr uint32_t ON_OFF_ATTR_ID = 0x0000;

static uint16_t temp_endpoint_ids[4] = {}; // outside, supply (inlet), extract, exhaust
static uint16_t snb_endpoint_id = 0;       // Summer Night Boost switch

// Dragging a speed slider produces a burst of PercentSetting writes, so the
// resulting RF command is only transmitted once the writes stop arriving.
static constexpr uint32_t RF_SEND_DEBOUNCE_MS = 600;

// I2C configuration (non-CVE board: SDA=GPIO27, SCL=GPIO26)
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_27;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_26;
static constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
static constexpr uint8_t I2C_MASTER_ADDR = 0x41;  // Itho unit address (command byte 0x82 = 0x41<<1|W)
static constexpr uint8_t I2C_SLAVE_ADDR = 0x40;   // ESP32 slave address for receiving responses
static constexpr uint32_t I2C_MASTER_FREQ = 100000;
static constexpr size_t I2C_BUF_SIZE = 512;

// I2C 31DA query command
// The HRU250/300 answer the 0x2401 status query, not 31DA (31DA is what the
// HMI panel exchanges as 0xCF3A on these units). 0x2400 returns the per-field
// datatype table needed to parse the 0x2401 response.
static const uint8_t I2C_CMD_STATUS_FORMAT[] = {0x82, 0x80, 0x24, 0x00, 0x04, 0x00, 0xD6};
static const uint8_t I2C_CMD_STATUS[]        = {0x82, 0x80, 0x24, 0x01, 0x04, 0x00, 0xD5};

// Polling interval for I2C status queries
static constexpr uint32_t I2C_POLL_INTERVAL_MS = 10000;

// Fail safe button (GPIO 32 on non-CVE board) — used for HRU300 join/pairing
static constexpr gpio_num_t FAIL_SAFE_PIN = GPIO_NUM_32;
static constexpr gpio_num_t STATUS_LED_PIN = GPIO_NUM_16;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
static constexpr uint32_t BUTTON_PRESS_THRESHOLD_MS = 100;  // min press duration to trigger

// Map Matter FanMode to IthoCommand
static IthoCommand fan_mode_to_itho(uint8_t fan_mode)
{
    switch (fan_mode)
    {
    case 0: return IthoLow;    // Off — the HRU300 cannot be turned off, map to Low
    case 1: return IthoLow;    // Low
    case 2: return IthoMedium; // Medium
    case 3: return IthoHigh;   // High
    case 5: return IthoAuto;   // Auto
    default:
        if (fan_mode == 4) return IthoHigh;  // On → High
        if (fan_mode == 6) return IthoAuto;  // Smart → Auto
        return IthoUnknown;
    }
}

// Map Matter PercentSetting to an IthoCommand. The HRU300 has three speeds,
// so the 0-100 range is split into bands.
static IthoCommand fan_percent_to_itho(uint8_t percent)
{
    if (percent <= 33) return IthoLow;    // includes 0 — the HRU300 cannot be turned off
    if (percent <= 66) return IthoMedium;
    return IthoHigh;
}

// Map Matter PercentSetting to the equivalent FanMode
static uint8_t fan_percent_to_mode(uint8_t percent)
{
    if (percent == 0)  return 0; // Off
    if (percent <= 33) return 1; // Low
    if (percent <= 66) return 2; // Medium
    return 3;                    // High
}

// Map the unit's actual fanspeed (field 10, %) to a Matter FanMode.
// Bands from live calibration: low ≈ 20% (raw 51), medium ≈ 43% (raw 110),
// high ≈ 78% (raw ~200). Field 9 (demand) is unusable for this: it reads ~0
// at low speed, so it cannot distinguish off from low.
static uint8_t fan_status_pct_to_mode(int32_t pct)
{
    if (pct < 1)  return 0; // off
    if (pct < 32) return 1; // low
    if (pct < 60) return 2; // medium
    return 3;               // high
}

// Snap a requested percentage to the nearest supported detent. The HRU300
// only has three speeds and cannot be turned off, so the controller slider
// settles on 33/66/100 (a 0% request becomes low).
static uint8_t fan_percent_snap(uint8_t percent)
{
    if (percent <= 33) return 33;
    if (percent <= 66) return 66;
    return 100;
}

// Map Matter FanMode to a percent value (mirrors fan_info_to_percent)
static uint8_t fan_mode_to_percent(uint8_t fan_mode)
{
    switch (fan_mode)
    {
    case 0: return 0;   // Off
    case 1: return 33;  // Low
    case 2: return 66;  // Medium
    case 3: return 100; // High
    case 4: return 100; // On → High
    case 5: return 50;  // Auto
    case 6: return 50;  // Smart → Auto
    default: return 0;
    }
}

// Set while attributes are written from unit feedback. esp_matter's update()
// runs the PRE_UPDATE callback synchronously, so without this guard every
// status report would be echoed back to the unit as an RF command.
static volatile bool feedback_update_in_progress = false;

// Command awaiting transmission by rf_send_task. -1 means nothing pending.
static volatile int32_t pending_rf_cmd = -1;
static volatile uint32_t pending_rf_cmd_ms = 0;

// Serializes CC1101 access between the receive, transmit and join-button tasks;
// the radio driver shares one SPI bus and its own packet buffers.
static SemaphoreHandle_t rf_mutex = nullptr;

// Hand a command to rf_send_task instead of transmitting inline: the attribute
// callback holds the CHIP stack lock, and sendRFCommand() retries three times.
static void queue_rf_command(IthoCommand cmd)
{
    pending_rf_cmd_ms = millis();
    pending_rf_cmd = (int32_t)cmd;
}

// Map FanInfo (from 31DA response) to Matter FanMode
static uint8_t fan_info_to_mode(uint8_t fan_info)
{
    switch (fan_info)
    {
    case 0x00: return 0; // off
    case 0x01: return 1; // low
    case 0x02: return 2; // medium
    case 0x03: return 3; // high
    case 0x15: return 0; // away → off
    case 0x18: return 5; // auto
    case 0x19: return 5; // autonight → auto
    default:   return 0; // unknown → off
    }
}

// Map FanInfo to PercentCurrent
static uint8_t fan_info_to_percent(uint8_t fan_info)
{
    switch (fan_info)
    {
    case 0x00: return 0;   // off
    case 0x01: return 33;  // low
    case 0x02: return 66;  // medium
    case 0x03: return 100; // high
    case 0x15: return 0;   // away → off
    case 0x18: return 50;  // auto
    case 0x19: return 50;  // autonight → auto
    default:   return 0;   // unknown → off
    }
}

// Update Matter FanMode and PercentCurrent attributes
static void update_fan_attributes(uint8_t fan_info)
{
    uint8_t new_mode = fan_info_to_mode(fan_info);
    uint8_t new_percent = fan_info_to_percent(fan_info);

    ESP_LOGI(TAG, "FanInfo=0x%02X → FanMode=%d, Percent=%d%%", fan_info, new_mode, new_percent);

    feedback_update_in_progress = true;

    esp_matter_attr_val_t mode_val = esp_matter_uint8(new_mode);
    esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, FAN_MODE_ATTR_ID, &mode_val);

    esp_matter_attr_val_t percent_val = esp_matter_uint8(new_percent);
    esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_CURRENT_ATTR_ID, &percent_val);

    // PercentSetting is nullable; keep it aligned with the reported speed so a
    // controller's slider follows changes made at the unit or a physical remote.
    esp_matter_attr_val_t setting_val = esp_matter_nullable_uint8(new_percent);
    esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_SETTING_ATTR_ID, &setting_val);

    feedback_update_in_progress = false;
}

// Update Matter attributes from an HRU300 2401 status report. The mode comes
// from the actual fanspeed (field 10). Both percent attributes are snapped to
// the matching detent: Apple Home displays PercentCurrent, and showing the
// real (ramping) speed there made the slider land on non-detent values.
// The real measured speed stays available in the serial log.
static void update_fan_attributes_speed(int32_t demand_pct, int32_t abs_pct)
{
    uint8_t new_mode = fan_status_pct_to_mode(abs_pct);
    uint8_t detent = fan_mode_to_percent(new_mode);

    ESP_LOGI(TAG, "Status speed: demand=%ld%% actual=%ld%% → FanMode=%d, PercentSetting=%d%%",
             (long)demand_pct, (long)abs_pct, new_mode, detent);

    feedback_update_in_progress = true;

    esp_matter_attr_val_t mode_val = esp_matter_uint8(new_mode);
    esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, FAN_MODE_ATTR_ID, &mode_val);

    esp_matter_attr_val_t percent_val = esp_matter_uint8(detent);
    esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_CURRENT_ATTR_ID, &percent_val);

    esp_matter_attr_val_t setting_val = esp_matter_nullable_uint8(detent);
    esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_SETTING_ATTR_ID, &setting_val);

    feedback_update_in_progress = false;
}

// --- I2C communication ---

// Compute I2C checksum: sum of all bytes, negated
static uint8_t i2c_checksum(const uint8_t *buf, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += buf[i];
    return (uint8_t)(-sum);
}

// Overall time to listen for a matching response before giving up
static constexpr uint32_t I2C_RESPONSE_TIMEOUT_MS = 2000;
// A gap longer than this between received chunks marks the end of a frame
// (the HRU300 sends its answer in multiple chunks, like ithowifi handles)
static constexpr uint32_t I2C_FRAME_GAP_MS = 50;

// Send I2C command as master, then switch to slave mode to receive the response.
// The bus is shared with the unit's HMI panel, so frames with other opcodes
// (e.g. 0xCF3A) can arrive addressed to us — those are skipped and we keep
// listening until a frame passes checksum+opcode validation or the deadline
// expires. Returns frame length (including address byte at [0]), or 0 on failure.
static size_t i2c_query(const uint8_t *cmd, size_t cmd_len, uint16_t expected_opcode,
                        uint8_t *rxbuf, size_t rxbuf_size)
{
    // --- Master send phase ---
    i2c_config_t master_conf = {};
    master_conf.mode = I2C_MODE_MASTER;
    master_conf.sda_io_num = I2C_SDA_PIN;
    master_conf.scl_io_num = I2C_SCL_PIN;
    master_conf.sda_pullup_en = GPIO_PULLUP_DISABLE;
    master_conf.scl_pullup_en = GPIO_PULLUP_DISABLE;
    master_conf.master.clk_speed = I2C_MASTER_FREQ;
    master_conf.clk_flags = I2C_SCLK_SRC_FLAG_FOR_NOMAL;

    if (i2c_param_config(I2C_PORT, &master_conf) != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C master param_config failed");
        return 0;
    }
    if (i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C master driver_install failed");
        return 0;
    }
    i2c_set_timeout(I2C_PORT, 0xFFFFF);

    i2c_cmd_handle_t link = i2c_cmd_link_create();
    i2c_master_start(link);
    i2c_master_write(link, (uint8_t *)cmd, cmd_len, true);
    i2c_master_stop(link);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, link, 200 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(link);
    i2c_driver_delete(I2C_PORT);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "I2C master send failed: %s", esp_err_to_name(ret));
        return 0;
    }

    // --- Slave receive phase ---
    // Switch to slave mode immediately (like ithowifi does): the HRU300
    // answers within a few ms, so any delay here risks missing the response.
    i2c_config_t slave_conf = {};
    slave_conf.mode = I2C_MODE_SLAVE;
    slave_conf.sda_io_num = I2C_SDA_PIN;
    slave_conf.scl_io_num = I2C_SCL_PIN;
    slave_conf.sda_pullup_en = GPIO_PULLUP_DISABLE;
    slave_conf.scl_pullup_en = GPIO_PULLUP_DISABLE;
    slave_conf.slave.addr_10bit_en = 0;
    slave_conf.slave.slave_addr = I2C_SLAVE_ADDR;
    slave_conf.slave.maximum_speed = 400000;
    slave_conf.clk_flags = I2C_SCLK_SRC_FLAG_FOR_NOMAL;

    if (i2c_param_config(I2C_PORT, &slave_conf) != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C slave param_config failed");
        return 0;
    }
    if (i2c_driver_install(I2C_PORT, I2C_MODE_SLAVE, I2C_BUF_SIZE, I2C_BUF_SIZE, 0) != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C slave driver_install failed");
        return 0;
    }
    i2c_set_timeout(I2C_PORT, 0xFFFFF);

    // Listen for frames until the overall deadline. A gap between chunks
    // longer than I2C_FRAME_GAP_MS marks the end of a frame.
    uint32_t deadline = millis() + I2C_RESPONSE_TIMEOUT_MS;
    unsigned frames_seen = 0; // diagnostic: frames seen but rejected

    while ((int32_t)(millis() - deadline) < 0)
    {
        // Collect one frame; [0] is the slave address byte (as in ithowifi)
        rxbuf[0] = I2C_SLAVE_ADDR << 1;
        size_t frame_len = 1;
        while (frame_len < rxbuf_size)
        {
            int n = i2c_slave_read_buffer(I2C_PORT, &rxbuf[frame_len],
                                          rxbuf_size - frame_len,
                                          I2C_FRAME_GAP_MS / portTICK_PERIOD_MS);
            if (n <= 0)
                break;
            frame_len += n;
        }

        if (frame_len <= 1)
            continue; // silence on the bus, keep listening until the deadline

        // (blob hex dump removed after bring-up; sub-frame skips are still
        // logged individually below)

        // A blob can contain several back-to-back sub-frames (HMI traffic can
        // be glued to our own response when chunks arrive without a gap).
        // Each sub-frame is self-contained:
        //   0x82, opcode_hi, opcode_lo, zone, data_len, data..., checksum
        // where the checksum also covers the receiver address byte (0x80),
        // which we fabricate at rxbuf[0] like ithowifi does. Walk the
        // sub-frames and return the first one matching the expected opcode.
        size_t o = 1;
        while (o + 5 < frame_len)
        {
            if (rxbuf[o] != 0x82)
            {
                o++; // resync: scan for the next sub-frame start
                continue;
            }
            size_t sub_total = 5 + (size_t)rxbuf[o + 4] + 1; // hdr + data + checksum
            if (o + sub_total > frame_len)
                break; // truncated sub-frame

            uint8_t sum = I2C_SLAVE_ADDR << 1;
            for (size_t i = o; i < o + sub_total; i++)
                sum += rxbuf[i];
            if (sum != 0)
            {
                o++; // not a valid sub-frame boundary, keep scanning
                continue;
            }

            frames_seen++;
            uint16_t opcode = (rxbuf[o + 1] << 8) | rxbuf[o + 2];
            // The top bits carry status flags, so compare with the same
            // 0x3FFF mask ithowifi uses in checkI2cReply.
            if ((opcode & 0x3FFF) != (expected_opcode & 0x3FFF))
            {
                ESP_LOGI(TAG, "I2C: skipping sub-frame with opcode 0x%04X", opcode);
                o += sub_total;
                continue;
            }

            // Normalize to the caller's layout: [0]=addr, [1..]=sub-frame
            memmove(&rxbuf[1], &rxbuf[o], sub_total);
            rxbuf[0] = I2C_SLAVE_ADDR << 1;

            i2c_driver_delete(I2C_PORT);
            return sub_total + 1;
        }
    }

    i2c_driver_delete(I2C_PORT);
    if (frames_seen > 0)
        ESP_LOGW(TAG, "I2C query 0x%04X: no matching response (%u other frame(s) seen)",
                 expected_opcode, frames_seen);
    else
        ESP_LOGW(TAG, "I2C query 0x%04X: no matching response (bus silent)", expected_opcode);
    return 0;
}

// --- 2401 status parsing (HRU250/300) ---

// Itho datatype byte: bit 7 = signed, bits 6-4 = length code, bits 3-0 = divider
// index (same encoding as ithowifi's getLengthFromDatatype/getDividerFromDatatype)
static uint8_t itho_dt_length(int8_t dt)
{
    switch (dt & 0x70)
    {
    case 0x10: return 2;
    case 0x20:
    case 0x70: return 4;
    default:   return 1;
    }
}

static uint32_t itho_dt_divider(int8_t dt)
{
    static const uint32_t dividers[] = {
        1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000,
        1, 1, 1, 1, 256, 2,
        256}; // 0x0F is used by this HRU300's fanspeed fields: 1/256 fixed point
    return dividers[dt & 0x0F];
}

struct itho_field_format
{
    uint8_t length;
    bool is_signed;
    uint32_t divider;
};

static itho_field_format status_fields[64];
static size_t status_field_count = 0;

// Field indices in the HRU250/300 status message (ithoHRU250_300StatusLabels
// in ithowifi; the mappings are identity for the first fields)
static constexpr size_t STATUS_IDX_ERROR = 0;
static constexpr size_t STATUS_IDX_STATUS = 1;
static constexpr size_t STATUS_IDX_REL_SPEED = 9;  // Relative fanspeed (%)
static constexpr size_t STATUS_IDX_ABS_SPEED = 10; // Absolute speed of the fan (%)

// Temperature fields (ithowifi hru250_300.h labels)
static constexpr size_t STATUS_IDX_TEMP_OUTSIDE = 2; // Measured outside temperature
static constexpr size_t STATUS_IDX_TEMP_SUPPLY = 5;  // Inlet temperature
static constexpr size_t STATUS_IDX_TEMP_EXTRACT = 6; // Temperature of the extracted air
static constexpr size_t STATUS_IDX_TEMP_EXHAUST = 7; // Temperature of the blown out air

struct itho_hru_status
{
    int32_t error;
    int32_t status;
    int32_t rel_speed_pct;
    int32_t abs_speed_pct;
    int32_t temp_centi[4]; // centi-°C: outside, supply, extract, exhaust
};

// Convert a raw field value to a percentage. Fanspeed-type fields use
// divider 256 (raw = fraction of full scale in 1/256 steps); plain percent
// fields carry divider 1.
static int32_t itho_field_to_pct(size_t idx, int32_t raw)
{
    uint32_t div = status_fields[idx].divider;
    if (div == 256)
        return (raw * 100 + 128) / 256;
    if (div == 0)
        return raw;
    return raw / (int32_t)div;
}

// Query 2400 to learn the per-field datatype table of this unit's 2401 status
static bool i2c_query_status_format(void)
{
    uint8_t rxbuf[I2C_BUF_SIZE] = {};

    size_t len = i2c_query(I2C_CMD_STATUS_FORMAT, sizeof(I2C_CMD_STATUS_FORMAT), 0x2400,
                           rxbuf, sizeof(rxbuf));
    if (len < 7)
        return false;

    // Response layout: [0]=addr, [1]=0x82, [2-3]=opcode, [4]=zone,
    // [5]=data length, [6+]=one datatype byte per field, [len-1]=checksum
    uint8_t data_length = rxbuf[5];
    if (data_length == 0 || data_length > 64 || len < 6 + data_length + 1)
    {
        ESP_LOGW(TAG, "I2C 2400: bad format response (dataLength=%d, len=%zu)", data_length, len);
        return false;
    }

    status_field_count = data_length;
    for (size_t i = 0; i < data_length; i++)
    {
        int8_t dt = (int8_t)rxbuf[6 + i];
        status_fields[i].length = itho_dt_length(dt);
        status_fields[i].is_signed = (dt & 0x80) != 0;
        status_fields[i].divider = itho_dt_divider(dt);
    }
    ESP_LOGI(TAG, "I2C 2400: status format learned, %d fields", (int)status_field_count);
    return true;
}

// Query 2401 status and extract the fields of interest
static bool i2c_query_status(itho_hru_status *out)
{
    if (status_field_count == 0 && !i2c_query_status_format())
        return false;

    uint8_t rxbuf[I2C_BUF_SIZE] = {};

    size_t len = i2c_query(I2C_CMD_STATUS, sizeof(I2C_CMD_STATUS), 0x2401, rxbuf, sizeof(rxbuf));
    if (len < 7)
        return false;

    // Walk the fields per the learned format; data starts at [6], big-endian
    int32_t values[64];
    size_t pos = 6;
    for (size_t f = 0; f < status_field_count; f++)
    {
        uint8_t flen = status_fields[f].length;
        if (pos + flen > len - 1)
        {
            ESP_LOGW(TAG, "I2C 2401: response too short for format (field %zu, len=%zu)", f, len);
            status_field_count = 0; // relearn the format on the next poll
            return false;
        }
        int32_t v = 0;
        for (uint8_t i = 0; i < flen; i++)
            v = (v << 8) | rxbuf[pos + i];
        if (status_fields[f].is_signed)
        {
            if (flen == 1)      v = (int8_t)v;
            else if (flen == 2) v = (int16_t)v;
        }
        values[f] = v;
        pos += flen;
    }

    if (status_field_count <= STATUS_IDX_ABS_SPEED)
    {
        ESP_LOGW(TAG, "I2C 2401: only %d fields, expected at least %d",
                 (int)status_field_count, (int)STATUS_IDX_ABS_SPEED + 1);
        return false;
    }

    out->error = values[STATUS_IDX_ERROR];
    out->status = values[STATUS_IDX_STATUS];
    // Fanspeed fields on the HRU300 use divider 256: the raw byte is the
    // fraction of full scale in 1/256 steps, so percent = raw * 100 / 256.
    out->rel_speed_pct = itho_field_to_pct(STATUS_IDX_REL_SPEED, values[STATUS_IDX_REL_SPEED]);
    out->abs_speed_pct = itho_field_to_pct(STATUS_IDX_ABS_SPEED, values[STATUS_IDX_ABS_SPEED]);

    // Temperature fields in centi-°C (datatype 0x92 = signed, 2 bytes, /100)
    static const size_t temp_fields[4] = {STATUS_IDX_TEMP_OUTSIDE, STATUS_IDX_TEMP_SUPPLY,
                                          STATUS_IDX_TEMP_EXTRACT, STATUS_IDX_TEMP_EXHAUST};
    for (size_t i = 0; i < 4; i++)
    {
        uint32_t div = status_fields[temp_fields[i]].divider;
        out->temp_centi[i] = (div == 0) ? values[temp_fields[i]] * 100
                                        : values[temp_fields[i]] * 100 / (int32_t)div;
    }
    return true;
}

// --- I2C 0x2410 settings (Summer Night Boost) ---
// Protocol per ithowifi sendQuery2410/setSetting2410: a 26-byte command with the
// setting index at [23]; query byte [4]=0x04, set byte [4]=0x06. The value sits
// big-endian at [6..9] (set) and in the reply at [6..9]=current, [10..13]=min,
// [14..17]=max, [22]=datatype. Checksum = negated sum (i2c_checksum above).

// Extract a value from a 4-byte big-endian field; only the last `length` bytes
// belong to the value (matching ithowifi's decodeQuery2410).
static int32_t i2c_be_value(const uint8_t *p, uint8_t length, bool is_signed)
{
    if (length == 0 || length > 4)
        length = 4;
    int32_t v = 0;
    for (uint8_t i = 4 - length; i < 4; i++)
        v = (v << 8) | p[i];
    if (is_signed)
    {
        if (length == 1)
            v = (int8_t)v;
        else if (length == 2)
            v = (int16_t)v;
    }
    return v;
}

static bool i2c_query_setting(uint8_t index, int32_t *value, int32_t *min, int32_t *max, uint8_t *datatype)
{
    uint8_t cmd[26] = {0x82, 0x80, 0x24, 0x10, 0x04, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF};
    cmd[23] = index;
    cmd[25] = i2c_checksum(cmd, 25);

    uint8_t rxbuf[I2C_BUF_SIZE] = {};
    size_t len = i2c_query(cmd, sizeof(cmd), 0x2410, rxbuf, sizeof(rxbuf));
    if (len < 23)
        return false;

    uint8_t flen = itho_dt_length((int8_t)rxbuf[22]);
    bool sgn = (rxbuf[22] & 0x80) != 0;
    *value = i2c_be_value(&rxbuf[6], flen, sgn);
    *min = i2c_be_value(&rxbuf[10], flen, sgn);
    *max = i2c_be_value(&rxbuf[14], flen, sgn);
    *datatype = rxbuf[22];
    return true;
}

static bool i2c_set_setting(uint8_t index, int32_t value, uint8_t length)
{
    uint8_t cmd[26] = {0x82, 0x80, 0x24, 0x10, 0x06, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF};
    cmd[23] = index;
    for (uint8_t i = 0; i < length && i < 4; i++)
        cmd[9 - i] = (value >> (8 * i)) & 0xFF;
    cmd[25] = i2c_checksum(cmd, 25);

    uint8_t rxbuf[I2C_BUF_SIZE] = {};
    size_t len = i2c_query(cmd, sizeof(cmd), 0x2410, rxbuf, sizeof(rxbuf));
    return len > 1; // a checksummed 2410 reply means the unit accepted the write
}

// Summer Night Boost switch state. The setting index depends on the unit's
// settings version: 95 for v6/8/9, 96 for v10 (ithowifi hru250_300.h maps).
static volatile int snb_pending = -1; // -1 = none, 0/1 = write requested
static int snb_setting_index = -1;    // -1 = not located yet
static uint8_t snb_value_length = 1;

static void snb_update_attribute(bool on)
{
    feedback_update_in_progress = true;
    esp_matter_attr_val_t v = esp_matter_bool(on);
    esp_matter::attribute::update(snb_endpoint_id, ON_OFF_CLUSTER_ID, ON_OFF_ATTR_ID, &v);
    feedback_update_in_progress = false;
}

// Handle the Summer Night Boost switch: locate the setting once, apply pending
// writes, refresh the OnOff attribute from the unit roughly once a minute.
static void snb_poll(void)
{
    if (snb_setting_index < 0)
    {
        int32_t val, mn, mx;
        uint8_t dt;
        for (uint8_t idx = 95; idx <= 96; idx++)
        {
            if (i2c_query_setting(idx, &val, &mn, &mx, &dt) && mn == 0 && mx == 1)
            {
                snb_setting_index = idx;
                snb_value_length = itho_dt_length((int8_t)dt);
                ESP_LOGI(TAG, "Summer Night Boost found at 2410 index %d (currently %s)",
                         idx, val ? "on" : "off");
                snb_update_attribute(val != 0);
                break;
            }
        }
        return;
    }

    if (snb_pending >= 0)
    {
        int v = snb_pending;
        snb_pending = -1;
        if (i2c_set_setting((uint8_t)snb_setting_index, v, snb_value_length))
        {
            ESP_LOGI(TAG, "Summer Night Boost set to %s", v ? "on" : "off");
            snb_update_attribute(v != 0);
        }
        else
        {
            ESP_LOGW(TAG, "Summer Night Boost set failed");
        }
    }

    static uint8_t refresh = 0;
    if (++refresh >= 6)
    {
        refresh = 0;
        int32_t val, mn, mx;
        uint8_t dt;
        if (i2c_query_setting((uint8_t)snb_setting_index, &val, &mn, &mx, &dt))
            snb_update_attribute(val != 0);
    }
}

// --- Tasks ---

// 31DA RF callback — secondary feedback source (broadcast from HRU300 or physical remote)
static void rf31da_callback(const uint8_t *payload, uint8_t len, uint8_t srcId0, uint8_t srcId1, uint8_t srcId2)
{
    if (len < 1 || payload == nullptr)
        return;

    uint8_t fan_info = payload[0];
    ESP_LOGI(TAG, "RF 31DA feedback: FanInfo=0x%02X", fan_info);
    update_fan_attributes(fan_info);
}

// GDO interrupt flag — the CC1101 signals a received packet on GPIO 4
// (ithowifi non-CVE board: hardwareManager.itho_irq_pin)
static volatile bool rf_rx_pending = false;

static void IRAM_ATTR itho_gdo_isr()
{
    rf_rx_pending = true;
}

// CC1101 receive task — interrupt-driven like ithowifi: the GDO rising edge sets a
// flag; only then does the task drain the RX FIFO (receivePacket), decode and parse.
// Polling checkForNewPacket() unconditionally (as before) only reads a stale buffer
// and never drains the FIFO, leaving the radio in RX overflow.
static void cc1101_task(void *arg)
{
    pinMode(GPIO_NUM_4, INPUT);
    attachInterrupt(GPIO_NUM_4, itho_gdo_isr, RISING);
    // If GDO is already HIGH, a packet arrived before the ISR was attached — catch up
    if (digitalRead(GPIO_NUM_4) == HIGH)
        rf_rx_pending = true;

    while (true)
    {
        if (rf_rx_pending)
        {
            rf_rx_pending = false;

            xSemaphoreTake(rf_mutex, portMAX_DELAY);
            itho.receivePacket();
            itho.decodeBufferedPacket();
            IthoPacket *packet = itho.checkForNewPacket();
            if (itho.parseMessage(packet))
            {
                IthoCommand cmd = itho.getLastCommand(packet);
                ESP_LOGD(TAG, "RF packet: id=%06lX/%06lX/%06lX opcode=0x%04X command=%d",
                         (unsigned long)packet->deviceId0, (unsigned long)packet->deviceId1,
                         (unsigned long)packet->deviceId2, packet->opcode, (int)cmd);
            }
            xSemaphoreGive(rf_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// I2C status polling task — queries HRU300 every I2C_POLL_INTERVAL_MS
static void i2c_poll_task(void *arg)
{
    // Wait a bit for system to stabilize before first query
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (true)
    {
        itho_hru_status st;
        if (i2c_query_status(&st))
        {
            ESP_LOGI(TAG, "I2C 2401: error=%ld status=%ld rel=%ld%% abs=%ld%%",
                     (long)st.error, (long)st.status, (long)st.rel_speed_pct, (long)st.abs_speed_pct);

            // 0xEF-style "sensor not available" markers land far above 100
            if (st.rel_speed_pct >= 0 && st.rel_speed_pct <= 100)
                update_fan_attributes_speed(st.rel_speed_pct, st.abs_speed_pct);

            // Temperature sensors (centi-°C; skip implausible sensor-absent markers)
            for (size_t i = 0; i < 4; i++)
            {
                if (temp_endpoint_ids[i] == 0 || st.temp_centi[i] < -5000 || st.temp_centi[i] > 10000)
                    continue;
                esp_matter_attr_val_t tv = esp_matter_nullable_int16(nullable<int16_t>((int16_t)st.temp_centi[i]));
                esp_matter::attribute::update(temp_endpoint_ids[i], TEMP_MEAS_CLUSTER_ID,
                                              TEMP_MEAS_VALUE_ATTR_ID, &tv);
            }
        }

        // Summer Night Boost switch (runs its own 2410 queries/writes)
        snb_poll();

        vTaskDelay(pdMS_TO_TICKS(I2C_POLL_INTERVAL_MS));
    }
}

// RF transmit task — sends commands queued by the Matter attribute callback once
// the writes have settled, keeping transmissions off the CHIP stack lock.
static void rf_send_task(void *arg)
{
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        int32_t cmd = pending_rf_cmd;
        if (cmd < 0)
            continue;

        if (millis() - pending_rf_cmd_ms < RF_SEND_DEBOUNCE_MS)
            continue;

        pending_rf_cmd = -1;

        xSemaphoreTake(rf_mutex, portMAX_DELAY);
        itho.sendRFCommand(0, (IthoCommand)cmd);
        xSemaphoreGive(rf_mutex);

        ESP_LOGI(TAG, "RF command sent: remote=0, command=%d", (int)cmd);
    }
}

// Fail safe button task — monitors GPIO 32 for press, sends IthoJoin to pair with HRU300
// How to use:
//   1. Put HRU300 into join mode (press and hold button on ventilation unit)
//   2. Press fail safe button on ithowifi add-on board
//   3. LED blinks rapidly during join, then stays on briefly when done
static void button_task(void *arg)
{
    // Configure fail safe button pin (active HIGH, internal pulldown)
    pinMode(FAIL_SAFE_PIN, INPUT_PULLDOWN);
    // Configure status LED
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    bool button_was_pressed = false;
    uint32_t press_start_ms = 0;

    while (true)
    {
        bool button_pressed = (digitalRead(FAIL_SAFE_PIN) == HIGH);

        if (button_pressed && !button_was_pressed)
        {
            // Button just pressed — start timing
            press_start_ms = millis();
        }
        else if (!button_pressed && button_was_pressed)
        {
            // Button just released — check if it was a valid press
            uint32_t press_duration = millis() - press_start_ms;
            if (press_duration >= BUTTON_PRESS_THRESHOLD_MS)
            {
                ESP_LOGI(TAG, "Fail safe button pressed (%lu ms) — sending IthoJoin", press_duration);

                // Blink LED rapidly to indicate join in progress
                for (int i = 0; i < 6; i++)
                {
                    digitalWrite(STATUS_LED_PIN, HIGH);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    digitalWrite(STATUS_LED_PIN, LOW);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                // Send join command via CC1101 RF
                xSemaphoreTake(rf_mutex, portMAX_DELAY);
                itho.sendRFCommand(0, IthoJoin);
                xSemaphoreGive(rf_mutex);
                ESP_LOGI(TAG, "IthoJoin RF command sent (remote 0)");

                // Turn LED on briefly to indicate join sequence complete
                digitalWrite(STATUS_LED_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(2000));
                digitalWrite(STATUS_LED_PIN, LOW);
            }
        }

        button_was_pressed = button_pressed;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    }
}

// --- Matter callbacks ---

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    // Summer Night Boost switch — queue the 2410 write for the I2C poll task
    if (cluster_id == ON_OFF_CLUSTER_ID)
    {
        if (type == POST_UPDATE && attribute_id == ON_OFF_ATTR_ID && !feedback_update_in_progress)
        {
            snb_pending = val->val.b ? 1 : 0;
            ESP_LOGI(TAG, "Summer Night Boost write: %s", snb_pending ? "on" : "off");
        }
        return ESP_OK;
    }

    if (cluster_id != FAN_CONTROL_CLUSTER_ID || feedback_update_in_progress)
    {
        return ESP_OK;
    }

    if (type == PRE_UPDATE && attribute_id == FAN_MODE_ATTR_ID)
    {
        uint8_t fan_mode = val->val.u8;
        IthoCommand cmd = fan_mode_to_itho(fan_mode);

        ESP_LOGI(TAG, "FanMode write: %d → IthoCommand %d", fan_mode, (int)cmd);

        if (cmd != IthoUnknown)
        {
            queue_rf_command(cmd);

            // Mirror the percent attributes (writing different attributes than
            // the in-flight FanMode write, so no overwrite conflict).
            uint8_t percent = fan_mode_to_percent(fan_mode);
            feedback_update_in_progress = true;
            esp_matter_attr_val_t setting_val = esp_matter_nullable_uint8(percent);
            esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_SETTING_ATTR_ID, &setting_val);
            esp_matter_attr_val_t percent_val = esp_matter_uint8(percent);
            esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_CURRENT_ATTR_ID, &percent_val);
            feedback_update_in_progress = false;
        }
        else
        {
            ESP_LOGW(TAG, "Unknown fan mode: %d", fan_mode);
        }
    }
    else if (type == POST_UPDATE && attribute_id == PERCENT_SETTING_ATTR_ID)
    {
        // Apple Home drives the fan exclusively through PercentSetting, never FanMode.
        // The attribute is nullable and spec-limited to 0-100, so anything above
        // 100 is either the 0xFF null marker or invalid — nothing to act on.
        if (val->val.u8 > 100)
        {
            return ESP_OK;
        }

        uint8_t requested = val->val.u8;
        uint8_t snapped = fan_percent_snap(requested);
        IthoCommand cmd = fan_percent_to_itho(snapped);
        uint8_t mode = fan_percent_to_mode(snapped);

        ESP_LOGI(TAG, "PercentSetting write: %d%% → snapped %d%%, FanMode %d, IthoCommand %d",
                 requested, snapped, mode, (int)cmd);

        queue_rf_command(cmd);

        // Snap PercentSetting to the nearest detent and mirror FanMode and
        // PercentCurrent, so every controller shows the same speed. This runs
        // in POST_UPDATE because the controller's raw value is only committed
        // after PRE_UPDATE returns — snapping earlier would be overwritten.
        feedback_update_in_progress = true;
        esp_matter_attr_val_t setting_val = esp_matter_nullable_uint8(snapped);
        esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_SETTING_ATTR_ID, &setting_val);
        esp_matter_attr_val_t mode_val = esp_matter_uint8(mode);
        esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, FAN_MODE_ATTR_ID, &mode_val);
        esp_matter_attr_val_t percent_val = esp_matter_uint8(snapped);
        esp_matter::attribute::update(fan_endpoint_id, FAN_CONTROL_CLUSTER_ID, PERCENT_CURRENT_ATTR_ID, &percent_val);
        feedback_update_in_progress = false;
    }

    return ESP_OK;
}

extern "C" void app_main()
{
    // Initialize NVS
    nvs_flash_init();

    // Initialize Arduino framework (needed for SPI, delay, etc.)
    initArduino();

    // Guard CC1101 access before any task can reach the radio
    rf_mutex = xSemaphoreCreateMutex();
    if (rf_mutex == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create RF mutex");
        return;
    }

    // Initialize CC1101 RF module
    itho.init();
    itho.setSendTries(3);
    itho.setRF31DACallback(rf31da_callback);

    // Virtual remote ID (ITHO_REMOTE_ID from local_config.h).
    // addRFDevice() refuses to register while bindAllowed == false, and it stores
    // the ID as destinationID only — sendRFCommand() needs sourceID set explicitly,
    // otherwise it falls back to defaultID {33,66,99} and the HRU300 ignores us.
    itho.setBindAllowed(true);
    itho.addRFDevice(ITHO_REMOTE_ID, RemoteTypes::RFTCVE);
    itho.updateSourceID(ITHO_REMOTE_ID, 0);
    itho.setBindAllowed(false);

    ESP_LOGI(TAG, "CC1101 initialized, chip version: %d", itho.getChipVersion());

    // Create Matter node
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);

    // Create fan endpoint
    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    add_device_type(endpoint, ESP_MATTER_FAN_DEVICE_TYPE_ID, ESP_MATTER_FAN_DEVICE_TYPE_VERSION);

    // Create required clusters
    cluster::identify::config_t identify_config;
    cluster::groups::config_t groups_config;
    descriptor::create(endpoint, CLUSTER_FLAG_SERVER);
    identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);
    groups::create(endpoint, &groups_config, CLUSTER_FLAG_SERVER);

    // Create FanControl cluster manually (v1.0's fan_control::create() is not implemented)
    cluster_t *cluster = cluster::create(endpoint, FAN_CONTROL_CLUSTER_ID, CLUSTER_FLAG_SERVER);
    cluster::global::attribute::create_feature_map(cluster, 0);
    cluster::global::attribute::create_cluster_revision(cluster, 2);
    fan_control::attribute::create_fan_mode(cluster, 0);
    fan_control::attribute::create_fan_mode_sequence(cluster, FAN_MODE_SEQUENCE);
    fan_control::attribute::create_percent_setting(cluster, nullable<uint8_t>(0));
    fan_control::attribute::create_percent_current(cluster, 0);

    fan_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Itho Fan Matter endpoint_id: %d", fan_endpoint_id);

    // Aggregator: the sensors and the switch are created as bridged endpoints
    // behind it, each with a Bridged Device Basic Information cluster whose
    // NodeLabel Apple Home displays as the accessory name.
    endpoint_t *agg = aggregator::create(node, ENDPOINT_FLAG_NONE, NULL);

    // Temperature sensors from I2C 2401 fields 2/5/6/7 (labels per ithowifi
    // hru250_300.h): outside, supply (inlet), extract, exhaust
    static char temp_names[4][24] = {"Outside Temperature", "Supply Temperature",
                                     "Extract Temperature", "Exhaust Temperature"};
    for (size_t i = 0; i < 4; i++)
    {
        temperature_sensor::config_t ts_config;
        ts_config.temperature_measurement.min_measured_value = nullable<int16_t>(-5000);
        ts_config.temperature_measurement.max_measured_value = nullable<int16_t>(10000);
        endpoint_t *t_ep = temperature_sensor::create(node, &ts_config, ENDPOINT_FLAG_BRIDGE, NULL);
        if (t_ep == nullptr)
            continue;
        bridged_device_basic::config_t bdb_config;
        cluster_t *bdb = bridged_device_basic::create(t_ep, &bdb_config, CLUSTER_FLAG_SERVER);
        basic_information::attribute::create_node_label(bdb, temp_names[i], strlen(temp_names[i]));
        endpoint::set_parent_endpoint(t_ep, agg);
        temp_endpoint_ids[i] = endpoint::get_id(t_ep);
    }
    ESP_LOGI(TAG, "Temperature endpoints (outside/supply/extract/exhaust): %d %d %d %d",
             temp_endpoint_ids[0], temp_endpoint_ids[1], temp_endpoint_ids[2], temp_endpoint_ids[3]);

    // Summer Night Boost switch (I2C 2410 setting)
    static char snb_name[] = "Summer Night Boost";
    on_off_plugin_unit::config_t snb_config;
    endpoint_t *s_ep = on_off_plugin_unit::create(node, &snb_config, ENDPOINT_FLAG_BRIDGE, NULL);
    if (s_ep != nullptr)
    {
        bridged_device_basic::config_t bdb_config;
        cluster_t *bdb = bridged_device_basic::create(s_ep, &bdb_config, CLUSTER_FLAG_SERVER);
        basic_information::attribute::create_node_label(bdb, snb_name, strlen(snb_name));
        endpoint::set_parent_endpoint(s_ep, agg);
        snb_endpoint_id = endpoint::get_id(s_ep);
    }
    ESP_LOGI(TAG, "Summer Night Boost endpoint_id: %d", snb_endpoint_id);

    // Start Matter
    esp_matter::start(NULL);

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::init();
#endif

    // Create CC1101 RF receive task
    xTaskCreate(cc1101_task, "cc1101_rx", 4096, nullptr, 5, nullptr);

    // Create I2C status polling task
    xTaskCreate(i2c_poll_task, "i2c_poll", 6144, nullptr, 4, nullptr);

    // Create RF transmit task (debounced commands from Matter writes)
    xTaskCreate(rf_send_task, "rf_send", 4096, nullptr, 5, nullptr);

    // Create fail safe button task (HRU300 join/pairing)
    xTaskCreate(button_task, "btn_join", 3072, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "Itho HRU300 Matter Fan started (RF commands + I2C status polling + join button)");
}
