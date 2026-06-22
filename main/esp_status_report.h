#ifndef ESP_STATUS_REPORT_H
#define ESP_STATUS_REPORT_H

#include "esp_err.h"

esp_err_t esp_status_report_start_task(void);
void esp_status_report_request(void);
void esp_status_report_publish_once(void);

#endif
