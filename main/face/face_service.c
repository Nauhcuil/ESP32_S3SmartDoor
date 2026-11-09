/**
 * @file    face_service.c
 * @brief   人脸业务门面实现：命令队列、响应回传、结果快照与运行状态。
 *
 * @note    - 同一时刻只允许一个命令在途（s_api_busy 标志保护）。这个约束让响应只需一组
 *            全局槽位，避免为每个请求动态分配内存，也不会出现“超时后请求对象被释放”
 *            的悬空指针问题。
 *            - 超时后 face_task 仍可能完成命令并写入槽位；下一次请求发送前会
 *            清空响应信号量，因此不会读到上一次的旧结果。
 *            - 最近一次识别结果与当前人脸状态用独立互斥量保护，供 UI 以任意频率读取。
 */

#include "face_service.h"

#include <string.h>

#include "app_config.h"
#include "app_nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face_internal.h"
#include "face_liveness.h"
#include "freertos/semphr.h"
#include "ui_app.h"       // 开门覆盖层（M2 由 face_task 触发，这里只透传状态）
#include "ui_home_api.h"  // 主页状态卡
#include "ui_strings.h"
#include "ui_theme.h"

static const char *TAG = "face_service";

#define FACE_CMD_QUEUE_DEPTH 4
#define FACE_API_TIMEOUT_MS  15000

/** @brief 命令队列 */
static QueueHandle_t s_cmd_queue = NULL;
/**
 * @brief 请求在途标志（代替互斥量）
 * @note  **不要用互斥量做“单请求在飞”的门**：异步请求在 A 任务取锁、由 face_task
 *        在另一任务 give，而 FreeRTOS 的优先级继承只允许**加锁任务自己**解锁——
 *        face_task 解锁时断言 `xTaskPriorityDisinherit: pxTCB == pxCurrentTCBs` 失败，
 *        直接 panic 重启（现场表现：点“开始识别”后看门狗复位）。
 *        标志用临界区保护即可：take → 置位，回执 → 清位。
 */
static portMUX_TYPE s_api_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_api_busy = false;
static SemaphoreHandle_t s_reply_done = NULL;

/** @brief 响应槽位（只在 s_api_busy 为真、且只有一个请求时写入/读取） */
static esp_err_t s_reply_err = ESP_OK;
/** @brief 异步识别在途标志：异步接口置位，face_task 回执时由 face_reply_done 清除 */
static volatile bool s_async_pending = false;
static int s_reply_out_id = -1;
static face_result_t s_reply_result;
static face_record_t s_reply_list[FACE_REPLY_LIST_MAX];
static size_t s_reply_list_n = 0;

/** @brief 服务配置缓存（识别阈值 / 活体 / 策略） */
static float s_threshold = APP_CFG_FACE_THRESHOLD;
static bool s_liveness_enabled = APP_CFG_LIVENESS_ENABLE != 0;
/**
 * @brief 默认识别策略：手动
 * @note  主页平时只显示摄像头画面，用户点击“开始识别”（或按键单击）后才识别一次。
 *        自动识别会让状态卡在人走近时不停跳“识别中/结果”，也把推理负载一直压在
 *        流水线上；常驻门禁场景可改为 FACE_STRATEGY_AUTO（M4 远程/语音接入时再评估）。
 */
static face_strategy_t s_strategy = FACE_STRATEGY_MANUAL;
/** @brief 启动时需清库（模型版本变化时由 face_service_init 置位，见那里说明） */
static volatile bool s_clear_db_on_start = false;

/** @brief 最近一次识别结果 */
static SemaphoreHandle_t s_result_mutex = NULL;
static face_result_t s_last_result;
static bool s_has_result = false;

/** @brief 当前人脸状态 */
static SemaphoreHandle_t s_state_mutex = NULL;
static face_face_state_t s_face_state;

/* =================================================================
 * 内部响应回填（供 face_task 调用）
 * ================================================================= */

void face_reply_result(esp_err_t err, const face_result_t *result, int out_id)
{
    s_reply_err = err;
    s_reply_out_id = out_id;
    if (result != NULL)
    {
        s_reply_result = *result;
    }
    else
    {
        memset(&s_reply_result, 0, sizeof(s_reply_result));
        s_reply_result.id = -1;
    }
}

void face_reply_list(esp_err_t err, const face_record_t *list, size_t n)
{
    s_reply_err = err;
    if (n > FACE_REPLY_LIST_MAX)
    {
        n = FACE_REPLY_LIST_MAX;
    }
    s_reply_list_n = n;
    if (list != NULL && n > 0)
    {
        memcpy(s_reply_list, list, n * sizeof(face_record_t));
    }
}

void face_reply_err(esp_err_t err)
{
    s_reply_err = err;
}

/**
 * @brief 占用“单请求在飞”标志
 * @return true 已占用；false 已有请求在途
 * @note  只做标志读写，不跨任务转移所有权，可在任意任务调用。
 */
static bool api_acquire(void)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_api_lock);
    if (!s_api_busy)
    {
        s_api_busy = true;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_api_lock);
    return ok;
}

/**
 * @brief 占用标志，最多等待 timeout_ms（同步接口用，内部 10 ms 轮询）
 */
static bool api_acquire_timeout(uint32_t timeout_ms)
{
    const uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;)
    {
        if (api_acquire())
        {
            return true;
        }
        if (timeout_ms == 0 ||
            (uint32_t)(esp_timer_get_time() / 1000) - start >= timeout_ms)
        {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/** @brief 释放“单请求在飞”标志 */
static void api_release(void)
{
    taskENTER_CRITICAL(&s_api_lock);
    s_api_busy = false;
    taskEXIT_CRITICAL(&s_api_lock);
}

void face_reply_done(void)
{
    if (s_reply_done != NULL)
    {
        xSemaphoreGive(s_reply_done);
    }

    // 异步识别没有等待者：由这里补写失败原因并释放在途标志。
    // 成功路径的结果文字与开门覆盖层由 face_task 在识别帧上写，这里不重复。
    if (s_async_pending)
    {
        const esp_err_t err = s_reply_err;
        s_async_pending = false;
        if (err == ESP_ERR_NOT_FOUND)
        {
            ui_home_set_state(UI_TXT_FACE_NO_FACE, UI_COLOR_WARN);
        }
        else if (err == ESP_ERR_INVALID_ARG)
        {
            ui_home_set_state(UI_TXT_FACE_LOW_QUALITY, UI_COLOR_WARN);
        }
        else if (err != ESP_OK)
        {
            ui_home_set_state(UI_TXT_FACE_UNKNOWN, UI_COLOR_DANGER);
        }
        api_release();
    }
}

face_strategy_t face_task_strategy(void)
{
    return s_strategy;
}

float face_task_threshold(void)
{
    return s_threshold;
}

bool face_task_liveness_enabled(void)
{
    return s_liveness_enabled;
}

bool face_task_clear_db_on_start(void)
{
    return s_clear_db_on_start;
}

/* =================================================================
 * 内部：把 face_task 的状态写回快照
 * ================================================================= */

/**
 * @brief 更新当前人脸状态（由 face_task 调用）
 * @note  声明放在 face_task.cpp 内部；这里只提供实现，避免对外头文件膨胀。
 */
void face_service_update_face_state(const face_face_state_t *state)
{
    if (state == NULL || s_state_mutex == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return;
    }
    s_face_state = *state;
    xSemaphoreGive(s_state_mutex);
}

void face_service_publish_result(const face_result_t *result)
{
    if (result == NULL || s_result_mutex == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return;
    }
    s_last_result = *result;
    s_has_result = true;
    xSemaphoreGive(s_result_mutex);
}

/* =================================================================
 * 命令发送
 * ================================================================= */

/**
 * @brief 发送命令并等待 face_task 完成
 * @return 响应错误码；超时返回 ESP_ERR_TIMEOUT
 * @note  调用前必须已 api_acquire()。
 */
static esp_err_t send_cmd(const face_cmd_t *cmd, uint32_t timeout_ms)
{
    if (s_cmd_queue == NULL || s_reply_done == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 清掉可能残留的信号量（上一次请求超时后 face_task 才完成）
    while (xSemaphoreTake(s_reply_done, 0) == pdTRUE)
    {
    }

    if (xQueueSend(s_cmd_queue, cmd, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_reply_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        ESP_LOGW(TAG, "命令 %d 等待超时（%u ms）", (int)cmd->type, (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return s_reply_err;
}

/**
 * @brief 发送不需要相机帧的命令（清空/删除/列表/阈值）
 */
static esp_err_t send_simple_cmd(face_cmd_type_t type, int id, float value, uint32_t timeout_ms)
{
    face_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.id = id;
    cmd.value = value;
    return send_cmd(&cmd, timeout_ms);
}

/* =================================================================
 * 对外接口
 * ================================================================= */

esp_err_t face_service_init(void *camera_queue, void *display_queue)
{
    if (s_cmd_queue != NULL)
    {
        return ESP_OK; // 幂等
    }
    if (camera_queue == NULL)
    {
        ESP_LOGE(TAG, "摄像头帧队列为空");
        return ESP_ERR_INVALID_ARG;
    }

    s_reply_done = xSemaphoreCreateBinary();
    s_result_mutex = xSemaphoreCreateMutex();
    s_state_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(FACE_CMD_QUEUE_DEPTH, sizeof(face_cmd_t));
    if (s_reply_done == NULL || s_result_mutex == NULL || s_state_mutex == NULL ||
        s_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "创建同步原语失败");
        return ESP_ERR_NO_MEM;
    }

    // ---- 读取 NVS 参数（失败自动回退默认值） ----
    s_threshold = app_nvs_get_threshold();
    s_liveness_enabled = app_nvs_get_liveness();
    face_liveness_set_enabled(s_liveness_enabled);

    // ---- 模型版本迁移（M6：S16 → S8）----
    // 换模型后 storage 分区里的特征向量与新模型校验码不同，set_ids_from_flash() 会
    // 一条也读不出来；同时 S8 的量化误差比 S16 大，相似度整体偏低，阈值要跟着提高
    // （否则同一个人经常判成“陌生人”）。两个动作必须一起做：
    //   1) 把阈值重置为新模型默认值并写回 NVS（旧 NVS 里的 0.55 是 S16 时代的）；
    //   2) 置位“启动时清库”，由 face_task 在模型初始化后用 clear_id(true) 清掉旧库。
    // ver==0 表示从未记录过版本（全新固件/从没注册过人），不需要清库，只记版本。
    // 版本号就是模型的量化位宽（FACE_MODEL_VERSION，见 face_internal.h）。
    {
        const uint8_t ver = app_nvs_get_model_ver();
        if (ver != FACE_MODEL_VERSION)
        {
            if (ver != 0)
            {
                ESP_LOGW(TAG, "人脸识别模型已更换（旧版本标记 %u → %d），旧人脸库数据作废，"
                              "启动后将清空，请重新录入",
                         (unsigned)ver, (int)FACE_MODEL_VERSION);
            }
            (void)app_nvs_set_model_ver((uint8_t)FACE_MODEL_VERSION);
        }

        // 阈值默认值修订（见 APP_CFG_THRESHOLD_REV）：与模型版本分开判断，因为“只改默认
        // 阈值、不换模型”时 model_ver 不变。旧值（如 S16 时代的 0.55）对 S8 偏低，
        // 会让陌生人更容易被认成本人；这里统一重置为新默认值，用户之后可在设置页微调。
        if (app_nvs_get_threshold_rev() != APP_CFG_THRESHOLD_REV)
        {
            if (s_threshold != APP_CFG_FACE_THRESHOLD)
            {
                ESP_LOGW(TAG, "识别阈值默认值已更新（当前 %.2f → %.2f）", s_threshold,
                         (double)APP_CFG_FACE_THRESHOLD);
            }
            (void)app_nvs_set_threshold(APP_CFG_FACE_THRESHOLD);
            s_threshold = APP_CFG_FACE_THRESHOLD;
            (void)app_nvs_set_threshold_rev(APP_CFG_THRESHOLD_REV);
        }
        s_clear_db_on_start = (ver != 0 && ver != FACE_MODEL_VERSION);
    }

    memset(&s_last_result, 0, sizeof(s_last_result));
    s_last_result.id = -1;
    memset(&s_face_state, 0, sizeof(s_face_state));
    s_face_state.track_id = -1;
    s_face_state.action = FACE_ACTION_NONE;
    s_face_state.similarity = -1.0f;

    // ---- 启动模型任务 ----
    const esp_err_t err = face_task_start((QueueHandle_t)camera_queue,
                                          (QueueHandle_t)display_queue, s_cmd_queue);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "face_task 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "人脸服务已启动（阈值 %.2f，活体 %s，策略 %s）", s_threshold,
             s_liveness_enabled ? "开" : "关",
             s_strategy == FACE_STRATEGY_AUTO ? "自动识别" : "手动识别");
    return ESP_OK;
}

esp_err_t face_service_get_result(face_result_t *out, uint32_t fresh_ms)
{
    if (out == NULL || s_result_mutex == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    const bool fresh =
        s_has_result && (fresh_ms == 0 || (esp_timer_get_time() / 1000ULL) - s_last_result.ts_ms <=
                                                (uint64_t)fresh_ms);
    if (fresh)
    {
        *out = s_last_result;
    }
    xSemaphoreGive(s_result_mutex);

    if (!fresh)
    {
        memset(out, 0, sizeof(*out));
        out->id = -1;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

void face_service_get_face_state(face_face_state_t *out)
{
    if (out == NULL)
    {
        return;
    }
    if (s_state_mutex == NULL || xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        memset(out, 0, sizeof(*out));
        out->track_id = -1;
        return;
    }
    *out = s_face_state;
    xSemaphoreGive(s_state_mutex);
}

face_err_t face_service_recognize(face_result_t *out, uint32_t timeout_ms)
{
    if (s_reply_done == NULL)
    {
        return FACE_ERR_STATE;
    }
    if (timeout_ms == 0)
    {
        timeout_ms = 3000;
    }

    if (!api_acquire_timeout(timeout_ms))
    {
        return FACE_ERR_BUSY;
    }

    face_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FACE_CMD_RECOGNIZE;

    const esp_err_t err = send_cmd(&cmd, timeout_ms);
    face_err_t ret;
    if (err == ESP_OK)
    {
        ret = FACE_OK;
        if (out != NULL)
        {
            *out = s_reply_result;
        }
    }
    else if (err == ESP_ERR_NOT_FOUND)
    {
        ret = FACE_ERR_NO_FACE;
    }
    else if (err == ESP_ERR_INVALID_ARG)
    {
        ret = FACE_ERR_QUALITY;
    }
    else if (err == ESP_ERR_TIMEOUT)
    {
        ret = FACE_ERR_TIMEOUT;
    }
    else
    {
        ret = FACE_ERR_STATE;
    }

    api_release();
    return ret;
}

esp_err_t face_service_recognize_async(void)
{
    if (s_reply_done == NULL || s_cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // 已有请求在途（同步或异步）：直接拒绝，不做排队
    if (!api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 清掉可能残留的信号量（上一次请求超时后 face_task 才完成）
    while (xSemaphoreTake(s_reply_done, 0) == pdTRUE)
    {
    }

    face_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FACE_CMD_RECOGNIZE;

    // 先置标志再投递：face_task 可能在投递后立刻回执，标志晚置会漏掉那次释放
    s_async_pending = true;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE)
    {
        s_async_pending = false;
        api_release();
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

bool face_service_recognize_in_flight(void)
{
    return s_async_pending;
}

esp_err_t face_service_enroll(const char *name, int *out_id, uint32_t timeout_ms)
{
    if (s_reply_done == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0)
    {
        timeout_ms = APP_CFG_ENROLL_TIMEOUT_MS;
    }

    face_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FACE_CMD_ENROLL;
    if (name != NULL)
    {
        strncpy(cmd.name, name, sizeof(cmd.name) - 1);
    }

    if (!api_acquire_timeout(timeout_ms))
    {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = send_cmd(&cmd, timeout_ms);
    if (out_id != NULL)
    {
        *out_id = (err == ESP_OK) ? s_reply_out_id : -1;
    }
    api_release();
    return err;
}

esp_err_t face_service_delete(int id)
{
    if (s_reply_done == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!api_acquire_timeout(3000))
    {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = send_simple_cmd(FACE_CMD_DELETE, id, 0.0f, FACE_API_TIMEOUT_MS);
    api_release();
    return err;
}

esp_err_t face_service_clear_all(void)
{
    if (s_reply_done == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!api_acquire_timeout(3000))
    {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = send_simple_cmd(FACE_CMD_CLEAR, -1, 0.0f, FACE_API_TIMEOUT_MS);
    api_release();
    return err;
}

esp_err_t face_service_list(face_record_t *out, size_t cap, size_t *n)
{
    if (out == NULL || n == NULL || s_reply_done == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *n = 0;

    if (!api_acquire_timeout(3000))
    {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = send_simple_cmd(FACE_CMD_LIST, -1, 0.0f, FACE_API_TIMEOUT_MS);
    if (err == ESP_OK)
    {
        size_t cnt = s_reply_list_n < cap ? s_reply_list_n : cap;
        memcpy(out, s_reply_list, cnt * sizeof(face_record_t));
        *n = cnt;
    }
    api_release();
    return err;
}

esp_err_t face_service_set_threshold(float t)
{
    if (t < -1.0f || t > 1.0f || s_reply_done == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!api_acquire_timeout(3000))
    {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = send_simple_cmd(FACE_CMD_SET_THRESHOLD, -1, t, FACE_API_TIMEOUT_MS);
    if (err == ESP_OK)
    {
        s_threshold = t;
        (void)app_nvs_set_threshold(t); // NVS 写失败不影响本次生效
        ESP_LOGI(TAG, "识别阈值已设为 %.2f", t);
    }
    api_release();
    return err;
}

float face_service_get_threshold(void)
{
    return s_threshold;
}

esp_err_t face_service_set_liveness(bool enable)
{
    s_liveness_enabled = enable;
    face_liveness_set_enabled(enable);
    const esp_err_t err = app_nvs_set_liveness(enable);
    return err;
}

bool face_service_get_liveness(void)
{
    return s_liveness_enabled;
}

void face_service_set_strategy(face_strategy_t strategy)
{
    s_strategy = strategy;
}

void face_service_get_status(face_service_status_t *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    face_face_state_t st;
    face_service_get_face_state(&st);
    out->face_present = st.face_present;
    out->enrolled_num = st.enrolled_num;
    out->track_id = st.track_id;
    out->threshold = s_threshold;
    out->liveness_enabled = s_liveness_enabled;
    out->strategy = s_strategy;
}

const char *face_action_text(face_action_t a)
{
    switch (a)
    {
    case FACE_ACTION_OPEN:
        return UI_TXT_FACE_RECOGNIZED;
    case FACE_ACTION_DENY_LIVENESS:
        return UI_TXT_FACE_LIVENESS_DENY;
    case FACE_ACTION_UNKNOWN:
        return UI_TXT_FACE_UNKNOWN;
    default:
        // NONE：已匹配到人但活体样本还在累积
        return s_liveness_enabled ? UI_TXT_FACE_LIVENESS_RUN : UI_TXT_HOME_WAITING;
    }
}
