/* Minimal sys_log.h stub for CC1101 code — replaces ithowifi's heavy logging deps */
#pragma once

#include <esp_log.h>

/* Map ithowifi logging macros to ESP-IDF logging */
#define EM_LOG(fmt, ...) ESP_LOGE("CC1101", fmt, ##__VA_ARGS__)
#define A_LOG(fmt, ...) ESP_LOGE("CC1101", fmt, ##__VA_ARGS__)
#define C_LOG(fmt, ...) ESP_LOGE("CC1101", fmt, ##__VA_ARGS__)
#define E_LOG(fmt, ...) ESP_LOGE("CC1101", fmt, ##__VA_ARGS__)
#define W_LOG(fmt, ...) ESP_LOGW("CC1101", fmt, ##__VA_ARGS__)
#define N_LOG(fmt, ...) ESP_LOGI("CC1101", fmt, ##__VA_ARGS__)
#define I_LOG(fmt, ...) ESP_LOGI("CC1101", fmt, ##__VA_ARGS__)
#define D_LOG(fmt, ...) ESP_LOGD("CC1101", fmt, ##__VA_ARGS__)
