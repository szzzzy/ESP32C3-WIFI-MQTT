enum {
    LINE_COPY_SIZE = 4096,
    SCHEMA_VERSION = 2,
};

static bool json_writer_init(json_writer_t *writer);
static bool json_writer_reserve(json_writer_t *writer, size_t extra);
static void json_writer_append(json_writer_t *writer, const char *text);
static void json_writer_append_char(json_writer_t *writer, char ch);
static void json_writer_appendf(json_writer_t *writer, const char *fmt, ...);
static void json_writer_append_escaped(json_writer_t *writer, const char *text);
static char *json_writer_take(json_writer_t *writer);
static void json_writer_discard(json_writer_t *writer);
static bool field_present(const char **fields, int field_count, int index);
static int split_csv_fields(char *text, const char **fields, int max_fields);
static bool parse_bool_field(const char **fields, int field_count, int index, const char *name,
                             bool_field_t *out, parse_warnings_t *warnings);
static bool parse_u32_field(const char **fields, int field_count, int index, const char *name,
                            u32_field_t *out, parse_warnings_t *warnings);
static bool parse_i32_field(const char **fields, int field_count, int index, const char *name,
                            i32_field_t *out, parse_warnings_t *warnings);
static bool parse_fixed_digits(const char **fields, int field_count, int index, size_t digits,
                               char *out, size_t out_size, const char *name,
                               bool *available, parse_warnings_t *warnings);
static void add_warning(parse_warnings_t *warnings, int index, const char *name);
static void append_json_bool_or_null(json_writer_t *writer, const bool_field_t *field);
static void append_json_u32_or_null(json_writer_t *writer, const u32_field_t *field);
static void append_json_i32_or_null(json_writer_t *writer, const i32_field_t *field);
static void append_json_string_or_null(json_writer_t *writer, bool available, const char *value);
static void append_parse_warnings(json_writer_t *writer, const parse_warnings_t *warnings);
static void append_extra_fields(json_writer_t *writer, const char **fields, int field_count);
static char *build_parse_error_json(const char *line, uint32_t rx_ms, const char *error);
static char *build_time_ack_json(const char *line, const char **fields, uint32_t rx_ms);
static char *build_measurement_json(const char *line, uint32_t rx_ms,
                                    const char **fields, int field_count);

char *stm32_protocol_build_publish_json(const char *line, uint32_t rx_ms,
                                         stm32_protocol_result_t *result)
{
    char *line_copy = NULL;
    const char **fields = NULL;
    int field_count;
    size_t line_len;
    char *json;

    if (result != NULL) {
        *result = STM32_PROTOCOL_RESULT_NONE;
    }

    if (line == NULL) {
        return NULL;
    }

    if (line[0] == '\0') {
        json = build_parse_error_json(line, rx_ms, "empty_frame");
        if (result != NULL) {
            *result = json != NULL ? STM32_PROTOCOL_RESULT_PARSE_ERROR : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    line_len = strlen(line);
    if (line_len >= LINE_COPY_SIZE) {
        json = build_parse_error_json(line, rx_ms, "line_too_long");
        if (result != NULL) {
            *result = json != NULL ? STM32_PROTOCOL_RESULT_PARSE_ERROR : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return json;
    }

    line_copy = malloc(line_len + 1U);
    fields = calloc(UART_FIELD_MAX, sizeof(*fields));
    if (line_copy == NULL || fields == NULL) {
        free(line_copy);
        free(fields);
        if (result != NULL) {
            *result = STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        return NULL;
    }

    memcpy(line_copy, line, line_len + 1U);
    field_count = split_csv_fields(line_copy, fields, UART_FIELD_MAX);
    if (field_count <= 0 || fields[0][0] == '\0') {
        json = build_parse_error_json(line, rx_ms, "empty_frame");
        if (result != NULL) {
            *result = json != NULL ? STM32_PROTOCOL_RESULT_PARSE_ERROR : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        free(line_copy);
        free(fields);
        return json;
    }

    if (strcmp(fields[0], "M") == 0) {
        json = build_measurement_json(line, rx_ms, fields, field_count);
        if (result != NULL) {
            *result = json != NULL ? STM32_PROTOCOL_RESULT_MEASUREMENT : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        free(line_copy);
        free(fields);
        return json;
    }

    if (strcmp(fields[0], "T") == 0) {
        if (field_count != UART_T_FIELD_COUNT) {
            json = build_parse_error_json(line, rx_ms, "bad_time_ack_frame");
            if (result != NULL) {
                *result = json != NULL ? STM32_PROTOCOL_RESULT_PARSE_ERROR : STM32_PROTOCOL_RESULT_NO_MEMORY;
            }
            free(line_copy);
            free(fields);
            return json;
        }
        json = build_time_ack_json(line, fields, rx_ms);
        if (result != NULL) {
            *result = json != NULL ? STM32_PROTOCOL_RESULT_TIME_ACK : STM32_PROTOCOL_RESULT_NO_MEMORY;
        }
        free(line_copy);
        free(fields);
        return json;
    }

    json = build_parse_error_json(line, rx_ms, "unknown_frame_type");
    if (result != NULL) {
        *result = json != NULL ? STM32_PROTOCOL_RESULT_PARSE_ERROR : STM32_PROTOCOL_RESULT_NO_MEMORY;
    }
    free(line_copy);
    free(fields);
    return json;
}

static char *build_measurement_json(const char *line, uint32_t rx_ms,
                                    const char **fields, int field_count)
{
    parse_warnings_t warnings = {0};
    json_writer_t w;
    bool_field_t rtc_valid = {0}, finger = {0}, bpm_valid = {0}, spo2_valid = {0};
    bool_field_t rr_valid = {0}, ibi_valid = {0}, hrv_valid = {0}, motion_artifact = {0};
    bool_field_t rhythm_irregular = {0}, hrv_freq_valid = {0}, raw_signal_present = {0};
    bool_field_t spo2_ratio_valid = {0}, rtc_read_ok = {0}, uart_rx_valid = {0};
    bool_field_t uart_tx_valid = {0}, sd_log_active = {0}, ecg_valid = {0};
    bool_field_t ptt_valid = {0}, crash_flag = {0};
    u32_field_t red = {0}, ir = {0}, baseline_ir = {0}, bpm = {0}, spo2 = {0};
    u32_field_t rr = {0}, ibi = {0}, mean_ibi = {0}, sdnn = {0}, rmssd = {0};
    u32_field_t motion_score = {0}, sd1 = {0}, sd2 = {0}, sd1_sd2 = {0};
    u32_field_t lf_power = {0}, hf_power = {0}, lf_hf = {0}, signal_quality = {0};
    u32_field_t signal_ir_pi = {0}, signal_red_pi = {0}, signal_ir_ac = {0}, signal_red_ac = {0};
    u32_field_t spo2_ratio = {0}, spo2_balance = {0};
    u32_field_t baseline_range_ir = {0}, finger_on_delta = {0}, finger_off_delta = {0};
    u32_field_t ir_delta = {0}, ir_span = {0}, red_span = {0}, finger_on_confirm = {0};
    u32_field_t finger_off_confirm = {0}, sensor_last_read_status = {0}, sensor_error_streak = {0};
    u32_field_t sensor_fifo_write_ptr = {0}, sensor_fifo_read_ptr = {0};
    u32_field_t sensor_fifo_overflow = {0}, sensor_fifo_available = {0}, sensor_read_ok = {0};
    u32_field_t sensor_read_busy = {0}, sensor_read_error = {0}, sensor_recover = {0};
    u32_field_t sensor_last_sample_tick = {0}, sensor_sample_change = {0};
    u32_field_t sensor_sample_same = {0}, sensor_last_i2c_error = {0};
    u32_field_t sd_state = {0}, sd_error = {0}, sd_total_written = {0};
    u32_field_t display_refresh = {0}, display_last_refresh_tick = {0}, debug_mode = {0};
    u32_field_t current_page = {0}, ecg_hr = {0}, ecg_rr_ms = {0}, ecg_lead_off = {0};
    u32_field_t ecg_r_peak_ms = {0}, ptt_ms = {0}, ecg_sample_count = {0};
    u32_field_t ecg_adc_sat_count = {0}, ecg_dma_overflow_count = {0};
    u32_field_t ecg_lead_off_count = {0}, ecg_no_r_peak_timeout_count = {0};
    u32_field_t crash_source = {0}, crash_task = {0}, crash_phase = {0}, crash_tick = {0};
    u32_field_t reboot_count = {0}, reset_flags = {0}, max_task_phase = {0};
    u32_field_t ui_task_phase = {0}, sd_task_phase = {0}, wdt_task_phase = {0};
    u32_field_t max_task_stack = {0}, ui_task_stack = {0}, sd_task_stack = {0}, wdt_task_stack = {0};
    u32_field_t max_task_heartbeat = {0}, ui_task_heartbeat = {0};
    i32_field_t ecg_filtered = {0};
    bool date_available = false, time_available = false;
    char date[9] = {0}, time_text[7] = {0};
    bool timestamp_available, raw_ppg_available, hrv_time_available, hrv_freq_available;
    bool signal_quality_available, finger_detect_available, sensor_diag_available;
    bool system_diag_available, ecg_available, crash_available;
    bool task_phase_available, task_stack_available, task_heartbeat_available;

    if (!json_writer_init(&w)) {
        return NULL;
    }

    parse_bool_field(fields, field_count, 1, "rtc_valid", &rtc_valid, &warnings);
    parse_fixed_digits(fields, field_count, 2, 8, date, sizeof(date), "date", &date_available, &warnings);
    parse_fixed_digits(fields, field_count, 3, 6, time_text, sizeof(time_text), "time", &time_available, &warnings);
    parse_u32_field(fields, field_count, 4, "red", &red, &warnings);
    parse_u32_field(fields, field_count, 5, "ir", &ir, &warnings);
    parse_u32_field(fields, field_count, 6, "baseline_ir", &baseline_ir, &warnings);
    parse_bool_field(fields, field_count, 7, "finger", &finger, &warnings);
    parse_bool_field(fields, field_count, 8, "bpm_valid", &bpm_valid, &warnings);
    parse_u32_field(fields, field_count, 9, "bpm", &bpm, &warnings);
    parse_bool_field(fields, field_count, 10, "spo2_valid", &spo2_valid, &warnings);
    parse_u32_field(fields, field_count, 11, "spo2", &spo2, &warnings);
    parse_bool_field(fields, field_count, 12, "rr_valid", &rr_valid, &warnings);
    parse_u32_field(fields, field_count, 13, "rr", &rr, &warnings);
    parse_bool_field(fields, field_count, 14, "ibi_valid", &ibi_valid, &warnings);
    parse_u32_field(fields, field_count, 15, "ibi", &ibi, &warnings);
    parse_bool_field(fields, field_count, 16, "hrv_valid", &hrv_valid, &warnings);
    parse_u32_field(fields, field_count, 17, "mean_ibi", &mean_ibi, &warnings);
    parse_u32_field(fields, field_count, 18, "sdnn", &sdnn, &warnings);
    parse_u32_field(fields, field_count, 19, "rmssd", &rmssd, &warnings);
    parse_bool_field(fields, field_count, 20, "motion_artifact", &motion_artifact, &warnings);
    parse_u32_field(fields, field_count, 21, "motion_score", &motion_score, &warnings);
    parse_u32_field(fields, field_count, 22, "sd1", &sd1, &warnings);
    parse_u32_field(fields, field_count, 23, "sd2", &sd2, &warnings);
    parse_u32_field(fields, field_count, 24, "sd1_sd2_x100", &sd1_sd2, &warnings);
    parse_bool_field(fields, field_count, 25, "rhythm_irregular", &rhythm_irregular, &warnings);
    parse_bool_field(fields, field_count, 26, "hrv_freq_valid", &hrv_freq_valid, &warnings);
    parse_u32_field(fields, field_count, 27, "lf_power_x100", &lf_power, &warnings);
    parse_u32_field(fields, field_count, 28, "hf_power_x100", &hf_power, &warnings);
    parse_u32_field(fields, field_count, 29, "lf_hf_x100", &lf_hf, &warnings);
    parse_u32_field(fields, field_count, 30, "signal_quality", &signal_quality, &warnings);
    parse_bool_field(fields, field_count, 31, "raw_signal_present", &raw_signal_present, &warnings);
    parse_u32_field(fields, field_count, 32, "signal_ir_pi_x1000", &signal_ir_pi, &warnings);
    parse_u32_field(fields, field_count, 33, "signal_red_pi_x1000", &signal_red_pi, &warnings);
    parse_u32_field(fields, field_count, 34, "signal_ir_ac_rms", &signal_ir_ac, &warnings);
    parse_u32_field(fields, field_count, 35, "signal_red_ac_rms", &signal_red_ac, &warnings);
    parse_bool_field(fields, field_count, 36, "spo2_ratio_valid", &spo2_ratio_valid, &warnings);
    parse_u32_field(fields, field_count, 37, "spo2_ratio_x1000", &spo2_ratio, &warnings);
    parse_u32_field(fields, field_count, 38, "spo2_balance_status", &spo2_balance, &warnings);
    parse_u32_field(fields, field_count, 39, "baseline_range_ir", &baseline_range_ir, &warnings);
    parse_u32_field(fields, field_count, 40, "adaptive_finger_on_delta", &finger_on_delta, &warnings);
    parse_u32_field(fields, field_count, 41, "adaptive_finger_off_delta", &finger_off_delta, &warnings);
    parse_u32_field(fields, field_count, 42, "ir_signal_delta", &ir_delta, &warnings);
    parse_u32_field(fields, field_count, 43, "ir_signal_span", &ir_span, &warnings);
    parse_u32_field(fields, field_count, 44, "red_signal_span", &red_span, &warnings);
    parse_u32_field(fields, field_count, 45, "finger_on_confirm_count", &finger_on_confirm, &warnings);
    parse_u32_field(fields, field_count, 46, "finger_off_confirm_count", &finger_off_confirm, &warnings);
    parse_u32_field(fields, field_count, 47, "sensor_last_read_status", &sensor_last_read_status, &warnings);
    parse_u32_field(fields, field_count, 48, "sensor_error_streak", &sensor_error_streak, &warnings);
    parse_u32_field(fields, field_count, 49, "sensor_fifo_write_ptr", &sensor_fifo_write_ptr, &warnings);
    parse_u32_field(fields, field_count, 50, "sensor_fifo_read_ptr", &sensor_fifo_read_ptr, &warnings);
    parse_u32_field(fields, field_count, 51, "sensor_fifo_overflow_count", &sensor_fifo_overflow, &warnings);
    parse_u32_field(fields, field_count, 52, "sensor_fifo_available_samples", &sensor_fifo_available, &warnings);
    parse_u32_field(fields, field_count, 53, "sensor_read_ok_count", &sensor_read_ok, &warnings);
    parse_u32_field(fields, field_count, 54, "sensor_read_busy_count", &sensor_read_busy, &warnings);
    parse_u32_field(fields, field_count, 55, "sensor_read_error_count", &sensor_read_error, &warnings);
    parse_u32_field(fields, field_count, 56, "sensor_recover_count", &sensor_recover, &warnings);
    parse_u32_field(fields, field_count, 57, "sensor_last_sample_tick", &sensor_last_sample_tick, &warnings);
    parse_u32_field(fields, field_count, 58, "sensor_sample_change_count", &sensor_sample_change, &warnings);
    parse_u32_field(fields, field_count, 59, "sensor_sample_same_count", &sensor_sample_same, &warnings);
    parse_u32_field(fields, field_count, 60, "sensor_last_i2c_error", &sensor_last_i2c_error, &warnings);
    parse_bool_field(fields, field_count, 61, "rtc_read_ok", &rtc_read_ok, &warnings);
    parse_bool_field(fields, field_count, 62, "uart_rx_message_valid", &uart_rx_valid, &warnings);
    parse_bool_field(fields, field_count, 63, "uart_tx_message_valid", &uart_tx_valid, &warnings);
    parse_bool_field(fields, field_count, 64, "sd_log_active", &sd_log_active, &warnings);
    parse_u32_field(fields, field_count, 65, "sd_state", &sd_state, &warnings);
    parse_u32_field(fields, field_count, 66, "sd_error", &sd_error, &warnings);
    parse_u32_field(fields, field_count, 67, "sd_total_written", &sd_total_written, &warnings);
    parse_u32_field(fields, field_count, 68, "display_refresh_count", &display_refresh, &warnings);
    parse_u32_field(fields, field_count, 69, "display_last_refresh_tick", &display_last_refresh_tick, &warnings);
    parse_u32_field(fields, field_count, 70, "debug_mode", &debug_mode, &warnings);
    parse_u32_field(fields, field_count, 71, "current_page", &current_page, &warnings);
    parse_bool_field(fields, field_count, 72, "ecg_valid", &ecg_valid, &warnings);
    parse_u32_field(fields, field_count, 73, "ecg_hr", &ecg_hr, &warnings);
    parse_u32_field(fields, field_count, 74, "ecg_rr_ms", &ecg_rr_ms, &warnings);
    parse_u32_field(fields, field_count, 75, "ecg_lead_off", &ecg_lead_off, &warnings);
    parse_u32_field(fields, field_count, 76, "ecg_r_peak_ms", &ecg_r_peak_ms, &warnings);
    parse_i32_field(fields, field_count, 77, "ecg_filtered", &ecg_filtered, &warnings);
    parse_bool_field(fields, field_count, 78, "ptt_valid", &ptt_valid, &warnings);
    parse_u32_field(fields, field_count, 79, "ptt_ms", &ptt_ms, &warnings);
    parse_u32_field(fields, field_count, 80, "ecg_sample_count", &ecg_sample_count, &warnings);
    parse_u32_field(fields, field_count, 81, "ecg_adc_sat_count", &ecg_adc_sat_count, &warnings);
    parse_u32_field(fields, field_count, 82, "ecg_dma_overflow_count", &ecg_dma_overflow_count, &warnings);
    parse_u32_field(fields, field_count, 83, "ecg_lead_off_count", &ecg_lead_off_count, &warnings);
    parse_u32_field(fields, field_count, 84, "ecg_no_r_peak_timeout_count", &ecg_no_r_peak_timeout_count, &warnings);
    parse_bool_field(fields, field_count, 85, "crash_flag", &crash_flag, &warnings);
    parse_u32_field(fields, field_count, 86, "crash_source", &crash_source, &warnings);
    parse_u32_field(fields, field_count, 87, "crash_task", &crash_task, &warnings);
    parse_u32_field(fields, field_count, 88, "crash_phase", &crash_phase, &warnings);
    parse_u32_field(fields, field_count, 89, "crash_tick", &crash_tick, &warnings);
    parse_u32_field(fields, field_count, 90, "reboot_count", &reboot_count, &warnings);
    parse_u32_field(fields, field_count, 91, "reset_flags", &reset_flags, &warnings);
    parse_u32_field(fields, field_count, 92, "max_task_phase", &max_task_phase, &warnings);
    parse_u32_field(fields, field_count, 93, "ui_task_phase", &ui_task_phase, &warnings);
    parse_u32_field(fields, field_count, 94, "sd_task_phase", &sd_task_phase, &warnings);
    parse_u32_field(fields, field_count, 95, "wdt_task_phase", &wdt_task_phase, &warnings);
    parse_u32_field(fields, field_count, 96, "max_task_stack_hwm", &max_task_stack, &warnings);
    parse_u32_field(fields, field_count, 97, "ui_task_stack_hwm", &ui_task_stack, &warnings);
    parse_u32_field(fields, field_count, 98, "sd_task_stack_hwm", &sd_task_stack, &warnings);
    parse_u32_field(fields, field_count, 99, "wdt_task_stack_hwm", &wdt_task_stack, &warnings);
    parse_u32_field(fields, field_count, 100, "max_task_heartbeat", &max_task_heartbeat, &warnings);
    parse_u32_field(fields, field_count, 101, "ui_task_heartbeat", &ui_task_heartbeat, &warnings);

    timestamp_available = rtc_valid.available || date_available || time_available;
    raw_ppg_available = red.available || ir.available || baseline_ir.available || finger.available;
    hrv_time_available = mean_ibi.available || sdnn.available || rmssd.available;
    hrv_freq_available = lf_power.available || hf_power.available || lf_hf.available;
    signal_quality_available = signal_quality.available || raw_signal_present.available ||
                               signal_ir_pi.available || signal_red_pi.available ||
                               signal_ir_ac.available || signal_red_ac.available;
    finger_detect_available = baseline_range_ir.available || finger_on_delta.available ||
                              finger_off_delta.available || ir_delta.available ||
                              ir_span.available || red_span.available ||
                              finger_on_confirm.available || finger_off_confirm.available;
    sensor_diag_available = sensor_last_read_status.available || sensor_error_streak.available ||
                            sensor_fifo_write_ptr.available || sensor_fifo_read_ptr.available ||
                            sensor_fifo_overflow.available || sensor_fifo_available.available ||
                            sensor_read_ok.available || sensor_read_busy.available ||
                            sensor_read_error.available || sensor_recover.available ||
                            sensor_last_sample_tick.available || sensor_sample_change.available ||
                            sensor_sample_same.available || sensor_last_i2c_error.available;
    system_diag_available = rtc_read_ok.available || uart_rx_valid.available || uart_tx_valid.available ||
                            sd_log_active.available || sd_state.available || sd_error.available ||
                            sd_total_written.available || display_refresh.available ||
                            display_last_refresh_tick.available || debug_mode.available ||
                            current_page.available;
    ecg_available = ecg_hr.available || ecg_rr_ms.available || ecg_lead_off.available ||
                    ecg_r_peak_ms.available || ecg_filtered.available ||
                    ecg_sample_count.available || ecg_adc_sat_count.available ||
                    ecg_dma_overflow_count.available || ecg_lead_off_count.available ||
                    ecg_no_r_peak_timeout_count.available;
    crash_available = crash_flag.available || crash_source.available || crash_task.available ||
                      crash_phase.available || crash_tick.available || reboot_count.available ||
                      reset_flags.available;
    task_phase_available = max_task_phase.available || ui_task_phase.available ||
                           sd_task_phase.available || wdt_task_phase.available;
    task_stack_available = max_task_stack.available || ui_task_stack.available ||
                           sd_task_stack.available || wdt_task_stack.available;
    task_heartbeat_available = max_task_heartbeat.available || ui_task_heartbeat.available;

    json_writer_append(&w, "{\"bridge\":\"");
    json_writer_append_escaped(&w, MQTT_BRIDGE_NAME);
    json_writer_append(&w, "\",\"source\":\"");
    json_writer_append_escaped(&w, MQTT_SOURCE_NAME);
    json_writer_append(&w, "\",\"channel\":\"");
    json_writer_append_escaped(&w, MQTT_CHANNEL_NAME);
    json_writer_append(&w, "\",\"protocol\":\"");
    json_writer_append_escaped(&w, MQTT_PROTOCOL_NAME);
    json_writer_appendf(&w,
                        "\",\"schema_version\":%d,\"message\":\"measurement\",\"frame\":\"M\","
                        "\"rx_ms\":%lu,\"parse_ok\":true,\"raw_line\":\"",
                        SCHEMA_VERSION,
                        (unsigned long)rx_ms);
    json_writer_append_escaped(&w, line);
    json_writer_appendf(&w, "\",\"field_count\":%d,\"extra_field_count\":%d,\"parse_warnings\":",
                        field_count,
                        field_count > UART_MEASUREMENT_SCHEMA_FIELD_COUNT ?
                        field_count - UART_MEASUREMENT_SCHEMA_FIELD_COUNT : 0);
    append_parse_warnings(&w, &warnings);
    json_writer_append(&w, ",\"extra_fields\":");
    append_extra_fields(&w, fields, field_count);

#define ADD_BOOL_FIELD(name, field) do { json_writer_append(&w, ",\"" name "\":"); append_json_bool_or_null(&w, &(field)); } while (0)
#define ADD_FLAG_FIELD(name, field) do { json_writer_append(&w, ",\"" name "\":"); json_writer_append(&w, ((field).available && (field).value) ? "true" : "false"); } while (0)
#define ADD_U32_FIELD(name, field) do { json_writer_append(&w, ",\"" name "\":"); append_json_u32_or_null(&w, &(field)); } while (0)
#define ADD_I32_FIELD(name, field) do { json_writer_append(&w, ",\"" name "\":"); append_json_i32_or_null(&w, &(field)); } while (0)

    ADD_BOOL_FIELD("rtc_valid", rtc_valid);
    json_writer_append(&w, ",\"date\":");
    append_json_string_or_null(&w, date_available, date);
    json_writer_append(&w, ",\"time\":");
    append_json_string_or_null(&w, time_available, time_text);
    ADD_U32_FIELD("red", red);
    ADD_U32_FIELD("ir", ir);
    ADD_U32_FIELD("baseline_ir", baseline_ir);
    ADD_BOOL_FIELD("finger", finger);
    ADD_FLAG_FIELD("bpm_valid", bpm_valid);
    ADD_U32_FIELD("bpm", bpm);
    ADD_FLAG_FIELD("spo2_valid", spo2_valid);
    ADD_U32_FIELD("spo2", spo2);
    ADD_FLAG_FIELD("rr_valid", rr_valid);
    ADD_U32_FIELD("rr", rr);
    ADD_FLAG_FIELD("ibi_valid", ibi_valid);
    ADD_U32_FIELD("ibi", ibi);
    ADD_FLAG_FIELD("hrv_valid", hrv_valid);
    ADD_U32_FIELD("mean_ibi", mean_ibi);
    ADD_U32_FIELD("sdnn", sdnn);
    ADD_U32_FIELD("rmssd", rmssd);
    ADD_BOOL_FIELD("motion_artifact", motion_artifact);
    ADD_U32_FIELD("motion_score", motion_score);
    ADD_U32_FIELD("signal_quality", signal_quality);
    ADD_BOOL_FIELD("raw_signal_present", raw_signal_present);
    ADD_U32_FIELD("signal_ir_pi_x1000", signal_ir_pi);
    ADD_U32_FIELD("signal_red_pi_x1000", signal_red_pi);
    ADD_U32_FIELD("signal_ir_ac_rms", signal_ir_ac);
    ADD_U32_FIELD("signal_red_ac_rms", signal_red_ac);
    ADD_FLAG_FIELD("spo2_ratio_valid", spo2_ratio_valid);
    ADD_U32_FIELD("spo2_ratio_x1000", spo2_ratio);
    ADD_U32_FIELD("spo2_balance_status", spo2_balance);
    ADD_FLAG_FIELD("ecg_valid", ecg_valid);
    ADD_U32_FIELD("ecg_hr", ecg_hr);
    ADD_U32_FIELD("ecg_rr_ms", ecg_rr_ms);
    ADD_U32_FIELD("ecg_lead_off", ecg_lead_off);
    ADD_U32_FIELD("ecg_r_peak_ms", ecg_r_peak_ms);
    ADD_I32_FIELD("ecg_filtered", ecg_filtered);
    ADD_FLAG_FIELD("ptt_valid", ptt_valid);
    ADD_U32_FIELD("ptt_ms", ptt_ms);
    ADD_U32_FIELD("sensor_read_error_count", sensor_read_error);
    ADD_U32_FIELD("sensor_recover_count", sensor_recover);
    ADD_U32_FIELD("sensor_last_i2c_error", sensor_last_i2c_error);

    json_writer_append(&w, ",\"modules\":{");
    json_writer_appendf(&w, "\"timestamp\":{\"available\":%s},", timestamp_available ? "true" : "false");
    json_writer_appendf(&w,
                        "\"raw_ppg\":{\"available\":%s,\"red_available\":%s,\"ir_available\":%s},",
                        raw_ppg_available ? "true" : "false",
                        red.available ? "true" : "false",
                        ir.available ? "true" : "false");
    json_writer_appendf(&w,
                        "\"finger_status\":{\"available\":%s},"
                        "\"bpm\":{\"available\":%s,\"valid\":%s},"
                        "\"spo2\":{\"available\":%s,\"valid\":%s},"
                        "\"rr\":{\"available\":%s,\"valid\":%s},"
                        "\"ibi\":{\"available\":%s,\"valid\":%s},"
                        "\"hrv_time\":{\"available\":%s,\"valid\":%s},"
                        "\"hrv_freq\":{\"available\":%s,\"valid\":%s},"
                        "\"signal_quality\":{\"available\":%s},"
                        "\"spo2_ratio\":{\"available\":%s,\"valid\":%s},",
                        finger.available ? "true" : "false",
                        bpm.available ? "true" : "false",
                        (bpm.available && bpm_valid.available && bpm_valid.value) ? "true" : "false",
                        spo2.available ? "true" : "false",
                        (spo2.available && spo2_valid.available && spo2_valid.value) ? "true" : "false",
                        rr.available ? "true" : "false",
                        (rr.available && rr_valid.available && rr_valid.value) ? "true" : "false",
                        ibi.available ? "true" : "false",
                        (ibi.available && ibi_valid.available && ibi_valid.value) ? "true" : "false",
                        hrv_time_available ? "true" : "false",
                        (hrv_time_available && hrv_valid.available && hrv_valid.value) ? "true" : "false",
                        hrv_freq_available ? "true" : "false",
                        (hrv_freq_available && hrv_freq_valid.available && hrv_freq_valid.value) ? "true" : "false",
                        signal_quality_available ? "true" : "false",
                        spo2_ratio.available ? "true" : "false",
                        (spo2_ratio.available && spo2_ratio_valid.available && spo2_ratio_valid.value) ? "true" : "false");
    json_writer_appendf(&w, "\"finger_detect\":{\"available\":%s,\"baseline_range_ir\":", finger_detect_available ? "true" : "false");
    append_json_u32_or_null(&w, &baseline_range_ir);
    json_writer_append(&w, ",\"finger_on_confirm\":");
    append_json_u32_or_null(&w, &finger_on_confirm);
    json_writer_append(&w, ",\"finger_off_confirm\":");
    append_json_u32_or_null(&w, &finger_off_confirm);
    json_writer_appendf(&w, "},\"sensor_diag\":{\"available\":%s,\"read_ok\":", sensor_diag_available ? "true" : "false");
    append_json_u32_or_null(&w, &sensor_read_ok);
    json_writer_appendf(&w, "},\"system_diag\":{\"available\":%s,\"sd_log_active\":", system_diag_available ? "true" : "false");
    append_json_bool_or_null(&w, &sd_log_active);
    json_writer_append(&w, ",\"sd_state\":");
    append_json_u32_or_null(&w, &sd_state);
    json_writer_append(&w, ",\"sd_error\":");
    append_json_u32_or_null(&w, &sd_error);
    json_writer_append(&w, ",\"sd_total_written\":");
    append_json_u32_or_null(&w, &sd_total_written);
    json_writer_append(&w, ",\"display_refresh\":");
    append_json_u32_or_null(&w, &display_refresh);
    json_writer_append(&w, ",\"debug_mode\":");
    append_json_u32_or_null(&w, &debug_mode);
    json_writer_append(&w, ",\"current_page\":");
    append_json_u32_or_null(&w, &current_page);
    json_writer_appendf(&w, "},\"ecg\":{\"available\":%s,\"valid\":%s,\"lead_off\":",
                        ecg_available ? "true" : "false",
                        (ecg_available && ecg_valid.available && ecg_valid.value) ? "true" : "false");
    append_json_u32_or_null(&w, &ecg_lead_off);
    json_writer_append(&w, ",\"filtered\":");
    append_json_i32_or_null(&w, &ecg_filtered);
    json_writer_append(&w, ",\"sample_count\":");
    append_json_u32_or_null(&w, &ecg_sample_count);
    json_writer_append(&w, ",\"adc_sat_count\":");
    append_json_u32_or_null(&w, &ecg_adc_sat_count);
    json_writer_append(&w, ",\"dma_overflow_count\":");
    append_json_u32_or_null(&w, &ecg_dma_overflow_count);
    json_writer_append(&w, ",\"lead_off_count\":");
    append_json_u32_or_null(&w, &ecg_lead_off_count);
    json_writer_append(&w, ",\"no_r_peak_timeout_count\":");
    append_json_u32_or_null(&w, &ecg_no_r_peak_timeout_count);
    json_writer_appendf(&w, "},\"ptt\":{\"available\":%s,\"valid\":%s},",
                        ptt_ms.available ? "true" : "false",
                        (ptt_ms.available && ptt_valid.available && ptt_valid.value) ? "true" : "false");
    json_writer_appendf(&w, "\"crash\":{\"available\":%s,\"crash_flag\":", crash_available ? "true" : "false");
    append_json_bool_or_null(&w, &crash_flag);
    json_writer_append(&w, ",\"crash_source\":");
    append_json_u32_or_null(&w, &crash_source);
    json_writer_append(&w, ",\"crash_task\":");
    append_json_u32_or_null(&w, &crash_task);
    json_writer_append(&w, ",\"crash_phase\":");
    append_json_u32_or_null(&w, &crash_phase);
    json_writer_append(&w, ",\"crash_tick\":");
    append_json_u32_or_null(&w, &crash_tick);
    json_writer_append(&w, ",\"reboot_count\":");
    append_json_u32_or_null(&w, &reboot_count);
    json_writer_append(&w, ",\"reset_flags\":");
    append_json_u32_or_null(&w, &reset_flags);
    json_writer_appendf(&w, "},\"task_phase\":{\"available\":%s,\"max_task_phase\":", task_phase_available ? "true" : "false");
    append_json_u32_or_null(&w, &max_task_phase);
    json_writer_append(&w, ",\"ui_task_phase\":");
    append_json_u32_or_null(&w, &ui_task_phase);
    json_writer_append(&w, ",\"sd_task_phase\":");
    append_json_u32_or_null(&w, &sd_task_phase);
    json_writer_append(&w, ",\"wdt_task_phase\":");
    append_json_u32_or_null(&w, &wdt_task_phase);
    json_writer_appendf(&w, "},\"task_stack\":{\"available\":%s,\"max_task_stack_hwm\":", task_stack_available ? "true" : "false");
    append_json_u32_or_null(&w, &max_task_stack);
    json_writer_append(&w, ",\"ui_task_stack_hwm\":");
    append_json_u32_or_null(&w, &ui_task_stack);
    json_writer_append(&w, ",\"sd_task_stack_hwm\":");
    append_json_u32_or_null(&w, &sd_task_stack);
    json_writer_append(&w, ",\"wdt_task_stack_hwm\":");
    append_json_u32_or_null(&w, &wdt_task_stack);
    json_writer_appendf(&w, "},\"task_heartbeat\":{\"available\":%s,\"max_task_heartbeat\":", task_heartbeat_available ? "true" : "false");
    append_json_u32_or_null(&w, &max_task_heartbeat);
    json_writer_append(&w, ",\"ui_task_heartbeat\":");
    append_json_u32_or_null(&w, &ui_task_heartbeat);
    json_writer_append(&w, "}}}");

#undef ADD_BOOL_FIELD
#undef ADD_FLAG_FIELD
#undef ADD_U32_FIELD
#undef ADD_I32_FIELD

    return json_writer_take(&w);
}

static char *build_time_ack_json(const char *line, const char **fields, uint32_t rx_ms)
{
    json_writer_t w;
    bool_field_t set_ok = {0}, rtc_valid = {0};
    bool date_available = false, time_available = false;
    char date[9] = {0}, time_text[7] = {0};
    parse_warnings_t warnings = {0};

    parse_bool_field(fields, UART_T_FIELD_COUNT, 1, "set_ok", &set_ok, &warnings);
    parse_bool_field(fields, UART_T_FIELD_COUNT, 2, "rtc_valid", &rtc_valid, &warnings);
    parse_fixed_digits(fields, UART_T_FIELD_COUNT, 3, 8, date, sizeof(date), "date", &date_available, &warnings);
    parse_fixed_digits(fields, UART_T_FIELD_COUNT, 4, 6, time_text, sizeof(time_text), "time", &time_available, &warnings);
    if (!set_ok.available || !rtc_valid.available || !date_available || !time_available ||
        !field_present(fields, UART_T_FIELD_COUNT, 5)) {
        return build_parse_error_json(line, rx_ms, "bad_time_ack_frame");
    }

    if (!json_writer_init(&w)) {
        return NULL;
    }

    json_writer_append(&w, "{\"bridge\":\"");
    json_writer_append_escaped(&w, MQTT_BRIDGE_NAME);
    json_writer_append(&w, "\",\"source\":\"");
    json_writer_append_escaped(&w, MQTT_SOURCE_NAME);
    json_writer_append(&w, "\",\"channel\":\"");
    json_writer_append_escaped(&w, MQTT_CHANNEL_NAME);
    json_writer_append(&w, "\",\"protocol\":\"");
    json_writer_append_escaped(&w, MQTT_PROTOCOL_NAME);
    json_writer_appendf(&w,
                        "\",\"schema_version\":%d,\"message\":\"rtc_set_ack\",\"frame\":\"T\","
                        "\"rx_ms\":%lu,\"parse_ok\":true,\"set_ok\":%s,\"rtc_valid\":%s,"
                        "\"date\":\"%s\",\"time\":\"%s\",\"reason\":\"",
                        SCHEMA_VERSION,
                        (unsigned long)rx_ms,
                        set_ok.value ? "true" : "false",
                        rtc_valid.value ? "true" : "false",
                        date,
                        time_text);
    json_writer_append_escaped(&w, fields[5]);
    json_writer_append(&w, "\"}");
    return json_writer_take(&w);
}

static char *build_parse_error_json(const char *line, uint32_t rx_ms, const char *error)
{
    json_writer_t w;

    if (!json_writer_init(&w)) {
        return NULL;
    }

    json_writer_append(&w, "{\"bridge\":\"");
    json_writer_append_escaped(&w, MQTT_BRIDGE_NAME);
    json_writer_append(&w, "\",\"source\":\"");
    json_writer_append_escaped(&w, MQTT_SOURCE_NAME);
    json_writer_append(&w, "\",\"channel\":\"");
    json_writer_append_escaped(&w, MQTT_CHANNEL_NAME);
    json_writer_append(&w, "\",\"protocol\":\"");
    json_writer_append_escaped(&w, MQTT_PROTOCOL_NAME);
    json_writer_appendf(&w,
                        "\",\"schema_version\":%d,\"message\":\"parse_error\",\"frame\":\"?\","
                        "\"rx_ms\":%lu,\"parse_ok\":false,\"error\":\"",
                        SCHEMA_VERSION,
                        (unsigned long)rx_ms);
    json_writer_append_escaped(&w, error);
    json_writer_append(&w, "\",\"raw_line\":\"");
    json_writer_append_escaped(&w, line != NULL ? line : "");
    json_writer_append(&w, "\"}");
    return json_writer_take(&w);
}

static bool field_present(const char **fields, int field_count, int index)
{
    return fields != NULL && index >= 0 && index < field_count &&
           fields[index] != NULL && fields[index][0] != '\0';
}

static int split_csv_fields(char *text, const char **fields, int max_fields)
{
    int count = 0;
    char *cursor = text;

    if (text == NULL || fields == NULL || max_fields <= 0) {
        return 0;
    }

    fields[count++] = cursor;
    while (*cursor != '\0' && count < max_fields) {
        if (*cursor == ',') {
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
        cursor++;
    }

    return count;
}

static bool parse_bool_field(const char **fields, int field_count, int index, const char *name,
                             bool_field_t *out, parse_warnings_t *warnings)
{
    const char *text;

    if (out == NULL) {
        return false;
    }
    out->available = false;
    out->value = false;
    if (!field_present(fields, field_count, index)) {
        return false;
    }
    text = fields[index];
    if (text[1] != '\0' || (text[0] != '0' && text[0] != '1')) {
        add_warning(warnings, index, name);
        return false;
    }
    out->available = true;
    out->value = text[0] == '1';
    return true;
}

static bool parse_u32_field(const char **fields, int field_count, int index, const char *name,
                            u32_field_t *out, parse_warnings_t *warnings)
{
    const char *text;
    uint32_t value = 0;

    if (out == NULL) {
        return false;
    }
    out->available = false;
    out->value = 0;
    if (!field_present(fields, field_count, index)) {
        return false;
    }
    text = fields[index];
    while (*text != '\0') {
        uint32_t digit;
        if (*text < '0' || *text > '9') {
            add_warning(warnings, index, name);
            return false;
        }
        digit = (uint32_t)(*text - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            add_warning(warnings, index, name);
            return false;
        }
        value = value * 10U + digit;
        text++;
    }
    out->available = true;
    out->value = value;
    return true;
}

static bool parse_i32_field(const char **fields, int field_count, int index, const char *name,
                            i32_field_t *out, parse_warnings_t *warnings)
{
    const char *text;
    bool negative = false;
    int64_t value = 0;

    if (out == NULL) {
        return false;
    }
    out->available = false;
    out->value = 0;
    if (!field_present(fields, field_count, index)) {
        return false;
    }
    text = fields[index];
    if (*text == '-') {
        negative = true;
        text++;
    }
    if (*text == '\0') {
        add_warning(warnings, index, name);
        return false;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            add_warning(warnings, index, name);
            return false;
        }
        value = value * 10 + (*text - '0');
        if ((!negative && value > INT32_MAX) || (negative && value > (int64_t)INT32_MAX + 1)) {
            add_warning(warnings, index, name);
            return false;
        }
        text++;
    }
    out->available = true;
    out->value = negative ? (int32_t)-value : (int32_t)value;
    return true;
}

static bool parse_fixed_digits(const char **fields, int field_count, int index, size_t digits,
                               char *out, size_t out_size, const char *name,
                               bool *available, parse_warnings_t *warnings)
{
    const char *text;

    if (available != NULL) {
        *available = false;
    }
    if (out == NULL || out_size < digits + 1U || !field_present(fields, field_count, index)) {
        return false;
    }
    text = fields[index];
    for (size_t i = 0; i < digits; i++) {
        if (text[i] < '0' || text[i] > '9') {
            add_warning(warnings, index, name);
            return false;
        }
        out[i] = text[i];
    }
    if (text[digits] != '\0') {
        add_warning(warnings, index, name);
        return false;
    }
    out[digits] = '\0';
    if (available != NULL) {
        *available = true;
    }
    return true;
}

static void add_warning(parse_warnings_t *warnings, int index, const char *name)
{
    if (warnings == NULL) {
        return;
    }
    if (warnings->count >= PARSE_WARNING_MAX) {
        warnings->truncated = true;
        return;
    }
    snprintf(warnings->items[warnings->count],
             PARSE_WARNING_LEN,
             "field_%d_%s_invalid",
             index,
             name != NULL ? name : "value");
    warnings->count++;
}

static void append_json_bool_or_null(json_writer_t *writer, const bool_field_t *field)
{
    json_writer_append(writer, (field != NULL && field->available) ?
                       (field->value ? "true" : "false") : "null");
}

static void append_json_u32_or_null(json_writer_t *writer, const u32_field_t *field)
{
    if (field != NULL && field->available) {
        json_writer_appendf(writer, "%lu", (unsigned long)field->value);
    } else {
        json_writer_append(writer, "null");
    }
}

static void append_json_i32_or_null(json_writer_t *writer, const i32_field_t *field)
{
    if (field != NULL && field->available) {
        json_writer_appendf(writer, "%ld", (long)field->value);
    } else {
        json_writer_append(writer, "null");
    }
}

static void append_json_string_or_null(json_writer_t *writer, bool available, const char *value)
{
    if (!available) {
        json_writer_append(writer, "null");
        return;
    }
    json_writer_append_char(writer, '"');
    json_writer_append_escaped(writer, value);
    json_writer_append_char(writer, '"');
}

static void append_parse_warnings(json_writer_t *writer, const parse_warnings_t *warnings)
{
    json_writer_append_char(writer, '[');
    if (warnings != NULL) {
        for (size_t i = 0; i < warnings->count; i++) {
            if (i > 0) {
                json_writer_append_char(writer, ',');
            }
            json_writer_append_char(writer, '"');
            json_writer_append_escaped(writer, warnings->items[i]);
            json_writer_append_char(writer, '"');
        }
    }
    json_writer_append_char(writer, ']');
}

static void append_extra_fields(json_writer_t *writer, const char **fields, int field_count)
{
    json_writer_append_char(writer, '[');
    for (int i = UART_MEASUREMENT_SCHEMA_FIELD_COUNT; i < field_count; i++) {
        if (i > UART_MEASUREMENT_SCHEMA_FIELD_COUNT) {
            json_writer_append_char(writer, ',');
        }
        json_writer_append_char(writer, '"');
        json_writer_append_escaped(writer, fields[i]);
        json_writer_append_char(writer, '"');
    }
    json_writer_append_char(writer, ']');
}

static bool json_writer_init(json_writer_t *writer)
{
    if (writer == NULL) {
        return false;
    }
    writer->data = malloc(1024);
    if (writer->data == NULL) {
        writer->len = 0;
        writer->cap = 0;
        writer->ok = false;
        return false;
    }
    writer->data[0] = '\0';
    writer->len = 0;
    writer->cap = 1024;
    writer->ok = true;
    return true;
}

static bool json_writer_reserve(json_writer_t *writer, size_t extra)
{
    size_t needed;
    size_t new_cap;
    char *new_data;

    if (writer == NULL || !writer->ok) {
        return false;
    }
    needed = writer->len + extra + 1U;
    if (needed <= writer->cap) {
        return true;
    }
    new_cap = writer->cap;
    while (new_cap < needed) {
        new_cap *= 2U;
    }
    new_data = realloc(writer->data, new_cap);
    if (new_data == NULL) {
        json_writer_discard(writer);
        return false;
    }
    writer->data = new_data;
    writer->cap = new_cap;
    return true;
}

static void json_writer_append(json_writer_t *writer, const char *text)
{
    size_t len;

    if (text == NULL) {
        text = "";
    }
    len = strlen(text);
    if (!json_writer_reserve(writer, len)) {
        return;
    }
    memcpy(&writer->data[writer->len], text, len + 1U);
    writer->len += len;
}

static void json_writer_append_char(json_writer_t *writer, char ch)
{
    if (!json_writer_reserve(writer, 1)) {
        return;
    }
    writer->data[writer->len++] = ch;
    writer->data[writer->len] = '\0';
}

static void json_writer_appendf(json_writer_t *writer, const char *fmt, ...)
{
    va_list args;
    va_list args_copy;
    int needed;

    if (writer == NULL || fmt == NULL || !writer->ok) {
        return;
    }
    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0 || !json_writer_reserve(writer, (size_t)needed)) {
        va_end(args_copy);
        return;
    }
    vsnprintf(&writer->data[writer->len], writer->cap - writer->len, fmt, args_copy);
    writer->len += (size_t)needed;
    va_end(args_copy);
}

static void json_writer_append_escaped(json_writer_t *writer, const char *text)
{
    static const char hex[] = "0123456789abcdef";

    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text++;
        switch (ch) {
            case '"':
                json_writer_append(writer, "\\\"");
                break;
            case '\\':
                json_writer_append(writer, "\\\\");
                break;
            case '\b':
                json_writer_append(writer, "\\b");
                break;
            case '\f':
                json_writer_append(writer, "\\f");
                break;
            case '\n':
                json_writer_append(writer, "\\n");
                break;
            case '\r':
                json_writer_append(writer, "\\r");
                break;
            case '\t':
                json_writer_append(writer, "\\t");
                break;
            default:
                if (ch < 0x20) {
                    json_writer_append(writer, "\\u00");
                    json_writer_append_char(writer, hex[(ch >> 4) & 0x0F]);
                    json_writer_append_char(writer, hex[ch & 0x0F]);
                } else {
                    json_writer_append_char(writer, (char)ch);
                }
                break;
        }
    }
}

static char *json_writer_take(json_writer_t *writer)
{
    char *data;

    if (writer == NULL || !writer->ok) {
        if (writer != NULL) {
            json_writer_discard(writer);
        }
        return NULL;
    }
    data = writer->data;
    writer->data = NULL;
    writer->len = 0;
    writer->cap = 0;
    writer->ok = false;
    return data;
}

static void json_writer_discard(json_writer_t *writer)
{
    if (writer == NULL) {
        return;
    }
    free(writer->data);
    writer->data = NULL;
    writer->len = 0;
    writer->cap = 0;
    writer->ok = false;
}
