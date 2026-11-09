/**
 * @file    ui_enroll.c
 * @brief   人脸注册页的交互实现：开始注册、实时状态提示、人脸库列表与删除。
 *
 * @note    - 分工：注册页（ui_pages.c）只负责布局，并把需要刷新的控件交给
 *              ui_enroll_attach() 登记；本模块负责按钮回调、刷新定时器和后台线程。
 *          - **不能**在 LVGL 回调里直接调用 face_service_enroll() / delete / list：
 *              这些接口会阻塞到 face_task 完成命令（最长 10 s），期间 LVGL 任务停摆、
 *              触摸无响应。这里点击时启动一个 3 KB 的工作线程（一次只跑一条命令）
 *              由它阻塞调用，结果写入 s_result_*，再由 LVGL 定时器读出来显示。
 *          - 删除列表：定时器每 500 ms 通过 face_service_list() 拉一次人脸库，
 *              比对内容后只在变化时重建列表，避免每帧重建控件造成闪烁。
 *          - 预览：本页的摄像头画面由 ui_preview 模块提供（见 ui_pages.c 的布局）。
 *          - 本文件所有 LVGL 访问都在 ui_lvgl_lock() 内进行；ui_lvgl_lock 是递归锁，
 *            因此也可以安全地由已持锁的创建函数调用。
 */

#include "ui_enroll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "face_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreatePinnedToCoreWithCaps（PSRAM 栈） */
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ui_app.h"
#include "ui_lvgl_port.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_enroll"; // 日志标签

/** @brief 录入超时：与按键注册一致，给用户 10 s 调整姿势 */
#define UI_ENROLL_TIMEOUT_MS 10000
/** @brief 工作线程栈（只做格式化与一次阻塞调用） */
#define UI_ENROLL_TASK_STACK 3072
/** @brief 状态刷新周期（毫秒） */
#define UI_ENROLL_TIMER_MS 250
/** @brief 定时器锁等待时间（LVGL 正忙时本轮跳过） */
#define UI_ENROLL_LOCK_MS 20
/** @brief 人脸库最多展示/可删除的条目数（与人脸库容量一致） */
#define UI_ENROLL_MAX_RECORDS 128
/**
 * @brief 人脸库列表行高（像素）
 * @note  比 ui_list 默认的 28 px 高，触摸目标更大；配合点击后的二次确认，
 *        避免手指落到相邻行导致误删。
 */
#define UI_ENROLL_ITEM_H 36

/**
 * @brief 联调自检开关：置 1 后固件启动 8 s 自动切到注册页并"点击"一次"开始注册"
 * @note  仅用于无人值守的链路验证（看串口日志），正式固件必须保持 0。
 */
#ifndef UI_ENROLL_SELFTEST
#define UI_ENROLL_SELFTEST 0
#endif

// =================================================================
// 状态
// =================================================================

/** @brief 注册页控件（绑定前为 NULL，定时器自动跳过） */
static lv_obj_t *s_lbl_hint = NULL;  /**< 实时状态（人脸/质量/结果） */
static lv_obj_t *s_lbl_name = NULL;  /**< 当前将要使用的姓名 */
static lv_obj_t *s_btn_start = NULL; /**< "开始注册"按钮 */
static lv_obj_t *s_list = NULL;      /**< 已录入人脸列表 */

/**
 * @enum enroll_phase_t
 * @brief 注册页当前所处的阶段
 */
typedef enum
{
    ENROLL_PHASE_IDLE = 0, /**< 空闲：等待点击"开始注册" */
    ENROLL_PHASE_RUNNING,  /**< 录入/删除中：工作线程正在调用 face_service */
    ENROLL_PHASE_DONE,     /**< 已完成：显示结果，再点按钮可重来 */
} enroll_phase_t;

/** @brief 当前阶段（工作线程写，定时器读） */
static volatile enroll_phase_t s_phase = ENROLL_PHASE_IDLE;

/** @brief 工作线程是否仍在运行 */
static volatile bool s_worker_busy = false;

/** @brief 工作线程刚完成了一次删除（定时器据此立刻刷新列表） */
static volatile bool s_list_dirty = false;

/** @brief 最近一次操作结果（s_result_mutex 保护） */
static SemaphoreHandle_t s_result_mutex = NULL;
static esp_err_t s_result_err = ESP_OK;
static int s_result_id = -1;
static char s_result_name[32] = "";
static bool s_result_is_delete = false;

/** @brief 文字缓存（内容未变不重绘） */
static char s_show_hint[96] = "";
static char s_show_name[48] = "";

/** @brief 刷新定时器（创建一次，长期运行；页面不可见时空转） */
static lv_timer_t *s_timer = NULL;

/** @brief 最近一次拉取列表的时刻（LVGL tick，用于限制失败重试频率） */
static uint32_t s_last_fetch_tick = 0;

/** @brief 定时器上次看到的页面（用于检测"刚进入删除页"，进入时强制重建列表） */
static ui_page_t s_last_page = UI_PAGE_MAX;

/** @brief 列表内容的指纹（用于判断人脸库是否变化，变化才重建列表） */
static int s_list_fp_count = -1;
static int s_list_fp_id_sum = 0;
static int64_t s_list_fp_name_hash = 0;

/** @brief 列表拉取缓冲（只在定时器里使用） */
static face_record_t s_records[UI_ENROLL_MAX_RECORDS];

// =================================================================
// 工具
// =================================================================

/**
 * @brief LOCK 内更新标签：颜色总是同步，文字只在变化时重设
 * @param label 目标标签
 * @param cache 文字缓存
 * @param cache_size 缓存大小
 * @param text  新文字
 * @param color 文字颜色（0xRRGGBB）
 */
static void set_label_if_changed(lv_obj_t *label, char *cache, size_t cache_size, const char *text,
                                 uint32_t color)
{
    if (label == NULL)
    {
        return;
    }
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (text == NULL || strcmp(cache, text) == 0)
    {
        return;
    }
    strncpy(cache, text, cache_size - 1);
    cache[cache_size - 1] = '\0';
    lv_label_set_text(label, text);
}

/** @brief 读取操作结果的安全副本 */
static void read_result(esp_err_t *err, int *id, char *name, size_t name_len, bool *is_delete)
{
    *err = ESP_ERR_TIMEOUT;
    *id = -1;
    *is_delete = false;
    name[0] = '\0';
    if (s_result_mutex == NULL || xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        return;
    }
    *err = s_result_err;
    *id = s_result_id;
    *is_delete = s_result_is_delete;
    strncpy(name, s_result_name, name_len - 1);
    name[name_len - 1] = '\0';
    xSemaphoreGive(s_result_mutex);
}

/**
 * @brief 把 face_service_enroll() 的错误码翻译成给用户看的说明
 */
static const char *enroll_err_text(esp_err_t err)
{
    switch (err)
    {
    case ESP_OK:
        return UI_TXT_ENROLL_OK;
    case ESP_ERR_NOT_FOUND:
        return UI_TXT_ENROLL_NO_FACE;
    case ESP_ERR_INVALID_ARG:
        return UI_TXT_ENROLL_BAD_QUALITY;
    case ESP_ERR_INVALID_STATE:
        // 该接口的 INVALID_STATE 涵盖"该人脸已注册"与"写入人脸库失败"，
        // 前者最常见，统一提示已注册。
        return UI_TXT_ENROLL_EXIST;
    default:
        return UI_TXT_ENROLL_FAIL;
    }
}

/**
 * @brief 写入操作结果
 * @param is_delete true 表示删除操作
 * @param err 结果码
 * @param id  人脸 ID
 * @param name 人脸名称（录入成功时才有意义）
 */
static void store_result(bool is_delete, esp_err_t err, int id, const char *name)
{
    if (s_result_mutex != NULL && xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_result_err = err;
        s_result_id = id;
        s_result_is_delete = is_delete;
        if (name != NULL && name[0] != '\0')
        {
            strncpy(s_result_name, name, sizeof(s_result_name) - 1);
            s_result_name[sizeof(s_result_name) - 1] = '\0';
        }
        else
        {
            s_result_name[0] = '\0';
        }
        xSemaphoreGive(s_result_mutex);
    }
}

// =================================================================
// 工作线程（一次只跑一条命令）
// =================================================================

/**
 * @brief 列表项/删除线程共用的一小段数据
 * @note  列表项自己持有一份（随控件销毁释放），点击时再复制一份交给删除线程，
 *        两边各自释放，避免“线程还在用、列表已经重建”造成的悬空指针。
 */
typedef struct
{
    int id;        /**< 人脸 ID */
    char name[32]; /**< 人脸名称（日志与结果展示用） */
} delete_arg_t;

/**
 * @brief 录入工作线程：阻塞调用 face_service_enroll()，结果写入共享变量
 * @param arg 未使用（姓名固定为空串，由 face_task 自动命名 Person_N）
 * @note  线程自删除。
 */
static void enroll_worker(void *arg)
{
    (void)arg;

    int id = -1;
    const esp_err_t err = face_service_enroll("", &id, UI_ENROLL_TIMEOUT_MS);

    // 成功提示里的姓名取**人脸库里实际存的记录名**：face_task 用“已录入人数”命名
    // （Person_0、Person_1…），而删除后 id 会回填，用 id 拼出来的名字会与记录不一致
    // （曾出现提示 Person_1、列表里却是 Person_0）。读不到时退回用 id 拼。
    char name[32] = "";
    if (err == ESP_OK && id >= 0)
    {
        snprintf(name, sizeof(name), UI_FMT_PERSON, id);
        face_record_t recs[8];
        size_t n = 0;
        if (face_service_list(recs, sizeof(recs) / sizeof(recs[0]), &n) == ESP_OK)
        {
            for (size_t i = 0; i < n; i++)
            {
                if (recs[i].id == id && recs[i].name[0] != '\0')
                {
                    snprintf(name, sizeof(name), "%s", recs[i].name);
                    break;
                }
            }
        }
    }
    store_result(false, err, id, name);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "录入成功：id=%d name=%s", id, name);
        // 录入后人脸库一定变了，指纹必须失效：定时器只在指纹变化时才重建列表，
        // 否则进入删除页时会因为“指纹和上次一致”而跳过重建，界面停在旧内容上
        // （曾出现“注册 1 人后删除页没有记录，注册第 2 人才出现”）。
        s_list_fp_count = -1;
        s_list_dirty = true;
    }
    else
    {
        ESP_LOGW(TAG, "录入失败：%s（%d）", esp_err_to_name(err), (int)err);
    }

    s_phase = ENROLL_PHASE_DONE;
    s_worker_busy = false;
    vTaskDelete(NULL);
}

/**
 * @brief 删除工作线程：阻塞调用 face_service_delete()，完成后让定时器刷新列表
 * @param arg delete_arg_t*（堆上分配，线程结束时释放）
 */
static void delete_worker(void *arg)
{
    delete_arg_t *d = (delete_arg_t *)arg;
    const esp_err_t err = face_service_delete(d->id);
    store_result(true, err, d->id, d->name);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "删除成功：id=%d name=%s", d->id, d->name);
        s_list_fp_count = -1; // 强制重建列表
    }
    else
    {
        ESP_LOGW(TAG, "删除失败：id=%d %s", d->id, esp_err_to_name(err));
    }

    s_list_dirty = true;
    s_phase = ENROLL_PHASE_DONE;
    s_worker_busy = false;
    free(d);
    vTaskDelete(NULL);
}

// =================================================================
// 人脸库列表
// =================================================================

/**
 * @brief 列表项被删除（列表重建/页面销毁）时释放回调参数
 * @param e 事件（user_data 为堆上的 delete_arg_t*）
 * @note  列表项持有的那份数据只在控件销毁时释放；删除线程用的是点击时复制的
 *        另一份，互不影响。
 */
static void delete_arg_free_cb(lv_event_t *e)
{
    delete_arg_t *d = (delete_arg_t *)lv_event_get_user_data(e);
    if (d != NULL)
    {
        free(d);
    }
}

/**
 * @brief 启动删除线程（已确认删除该人脸）
 * @param d 列表项数据（本函数会复制一份，调用方仍持有原指针）
 * @note  在 LVGL 任务里执行，不能阻塞：只做状态切换 + 启动线程。
 */
static void start_delete_worker(const delete_arg_t *d)
{
    if (d == NULL)
    {
        return;
    }
    if (s_worker_busy)
    {
        ESP_LOGW(TAG, "有操作正在进行，忽略本次删除");
        return;
    }

    // 复制一份给删除线程：列表随时可能重建，不能把列表项持有的指针交出去
    delete_arg_t *arg = (delete_arg_t *)malloc(sizeof(*arg));
    if (arg == NULL)
    {
        ESP_LOGE(TAG, "删除参数分配失败");
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return;
    }
    *arg = *d;

    s_phase = ENROLL_PHASE_RUNNING;
    s_worker_busy = true;

    // 栈放 PSRAM：线程只做一次阻塞门面调用（真正的 Flash 写在 face_task 里），
    // 内部 RAM 已不足以稳定切出 3 KB 连续块（MQTT 后最大块只有 1.8 KB）。
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(delete_worker, "ui_del",
                                                    UI_ENROLL_TASK_STACK, arg, 3, NULL, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
    {
        ok = xTaskCreate(delete_worker, "ui_del", UI_ENROLL_TASK_STACK, arg, 3, NULL);
    }
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "启动删除线程失败");
        free(arg);
        s_worker_busy = false;
        s_phase = ENROLL_PHASE_IDLE;
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
    }
}

/** @brief 确认框里的一份列表项数据（确认框关闭时释放） */
typedef struct
{
    delete_arg_t rec; /**< 待删除记录 */
} confirm_arg_t;

/**
 * @brief 确认框按钮回调：点“确定”才真正删除
 * @param e 事件（user_data 为 confirm_arg_t*）
 * @note  关闭确认框时由 confirm_arg_free_cb 释放参数，本函数不再 free。
 */
static void confirm_mbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    confirm_arg_t *arg = (confirm_arg_t *)lv_event_get_user_data(e);
    if (mbox == NULL || arg == NULL)
    {
        return;
    }

    const char *btn = lv_msgbox_get_active_btn_text(mbox);
    const bool ok = btn != NULL && strcmp(btn, UI_TXT_CONFIRM) == 0;
    if (ok)
    {
        start_delete_worker(&arg->rec);
    }
    lv_msgbox_close(mbox);
}

/**
 * @brief 确认框关闭时释放参数
 * @param e 事件（user_data 为 confirm_arg_t*）
 */
static void confirm_arg_free_cb(lv_event_t *e)
{
    confirm_arg_t *arg = (confirm_arg_t *)lv_event_get_user_data(e);
    if (arg != NULL)
    {
        free(arg);
    }
}

/**
 * @brief 点击列表项：弹出确认框，确认后才删除
 * @param e 事件（user_data 为堆上的 delete_arg_t*）
 * @note  **不能点一下就直接删**：手指落点稍偏就会点到相邻那个人，连续误点会一次
 *        删掉好几个已注册人脸。改成二次确认（“删除 Person_N ?” + 确定/取消），
 *        确认框居中显示、只能点一个按钮；误删几乎不可能。确认框里用的是复制出来的
 *        数据，列表随时重建也不影响。
 */
static void delete_item_cb(lv_event_t *e)
{
    const delete_arg_t *d = (const delete_arg_t *)lv_event_get_user_data(e);
    if (d == NULL)
    {
        return;
    }
    if (s_worker_busy)
    {
        ESP_LOGW(TAG, "有操作正在进行，忽略本次删除");
        return;
    }

    confirm_arg_t *arg = (confirm_arg_t *)malloc(sizeof(*arg));
    if (arg == NULL)
    {
        ESP_LOGE(TAG, "确认框参数分配失败");
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return;
    }
    arg->rec = *d;

    char text[64];
    snprintf(text, sizeof(text), "%s %s ?", UI_TXT_ACTION_DELETE, d->name);
    static const char *btns[] = {UI_TXT_CONFIRM, UI_TXT_CANCEL, ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, UI_TXT_ENROLL_DELETE, text, btns, false);
    if (mbox == NULL)
    {
        free(arg);
        return;
    }

    // 确认框默认用 LVGL 内置字体（只有拉丁字母），中文会显示成空白；
    // 把 16 px 中文字库设在容器上（字体是继承属性，子控件会跟着用）。
    lv_obj_set_style_text_font(mbox, UI_FONT_BODY, 0);
    // 居中：父对象为 NULL 时消息框挂在 lv_layer_top() 上，默认是顶部居中的小框或
    // 随内容撑开，这里显式居中，避免出现在屏幕边角。
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, confirm_mbox_cb, LV_EVENT_VALUE_CHANGED, arg);
    lv_obj_add_event_cb(mbox, confirm_arg_free_cb, LV_EVENT_DELETE, arg);
    ESP_LOGI(TAG, "弹出删除确认：id=%d name=%s", d->id, d->name);
}

/**
 * @brief 重建人脸库列表
 * @param records 人脸记录
 * @param n       条数
 * @note  调用方须持有 LVGL 锁。
 */
static void rebuild_list(const face_record_t *records, size_t n)
{
    if (s_list == NULL)
    {
        return;
    }
    // 旧列表项的 delete_arg_t 由列表项的 LV_EVENT_DELETE 回调释放（见 delete_arg_free_cb）。
    ui_list_clear(s_list);

    if (n == 0)
    {
        ui_list_add_item(s_list, UI_TXT_STATUS_DB_EMPTY, UI_COLOR_TEXT_DIM, NULL, NULL);
        return;
    }

    for (size_t i = 0; i < n; i++)
    {
        delete_arg_t *d = (delete_arg_t *)malloc(sizeof(delete_arg_t));
        if (d == NULL)
        {
            continue;
        }
        d->id = records[i].id;
        snprintf(d->name, sizeof(d->name), "%s", records[i].name);

        char text[64];
        snprintf(text, sizeof(text), "%s  %s", records[i].name, UI_TXT_ENROLL_DELETE);
        lv_obj_t *label = ui_list_add_item(s_list, text, UI_COLOR_TEXT, delete_item_cb, d);
        if (label != NULL && lv_obj_get_parent(label) != NULL)
        {
            // 行高放大到 36 px：默认 28 px 太密，手指很容易落到相邻行
            // （误删上一个人）。
            lv_obj_set_height(lv_obj_get_parent(label), UI_ENROLL_ITEM_H);
            lv_obj_add_event_cb(lv_obj_get_parent(label), delete_arg_free_cb, LV_EVENT_DELETE, d);
        }
    }
}

/**
 * @brief 用已拉取的人脸库数据刷新列表（内容未变化时不做任何操作）
 * @param n 条数（记录已在 s_records 中）
 * @note  调用方须持有 LVGL 锁；face_service_list() 由调用方在锁外调用，
 *        避免阻塞 LVGL 任务。
 */
static void refresh_list(size_t n)
{
    if (s_list == NULL)
    {
        return;
    }

    int id_sum = 0;
    int64_t name_hash = 0;
    for (size_t i = 0; i < n; i++)
    {
        id_sum += s_records[i].id;
        for (const char *p = s_records[i].name; *p != '\0'; p++)
        {
            name_hash = name_hash * 31 + (unsigned char)*p;
        }
    }

    if ((int)n == s_list_fp_count && id_sum == s_list_fp_id_sum && name_hash == s_list_fp_name_hash)
    {
        return; // 未变化
    }
    s_list_fp_count = (int)n;
    s_list_fp_id_sum = id_sum;
    s_list_fp_name_hash = name_hash;

    rebuild_list(s_records, n);
    ESP_LOGI(TAG, "人脸库列表已刷新：%u 项", (unsigned)n);
}

// =================================================================
// 定时器：把状态刷到页面
// =================================================================

/**
 * @brief 生成实时提示（基于最新人脸状态）
 * @param hint 输出缓冲区
 * @param size 缓冲区大小
 * @param color 输出文字颜色
 */
static void build_hint(char *hint, size_t size, uint32_t *color)
{
    face_face_state_t st;
    face_service_get_face_state(&st);

    if (!st.face_present)
    {
        *color = s_phase == ENROLL_PHASE_RUNNING ? UI_COLOR_WARN : UI_COLOR_TEXT_DIM;
        snprintf(hint, size, "%s", UI_TXT_ENROLL_NO_FACE);
        return;
    }

    if (!st.quality_pass)
    {
        *color = UI_COLOR_WARN;
        snprintf(hint, size, "%s", UI_TXT_ENROLL_BAD_QUALITY);
        return;
    }

    if (s_phase == ENROLL_PHASE_RUNNING)
    {
        *color = UI_COLOR_ACCENT;
        snprintf(hint, size, "%s", UI_TXT_FACE_ENROLLING);
    }
    else
    {
        *color = UI_COLOR_OK;
        snprintf(hint, size, "%s", UI_TXT_ENROLL_DETECTING);
    }
}

/**
 * @brief 刷新姓名与状态文字
 */
static void refresh_texts(void)
{
    face_face_state_t st;
    face_service_get_face_state(&st);

    char name[48];
    snprintf(name, sizeof(name), UI_TXT_ENROLL_NAME ": " UI_FMT_PERSON, st.enrolled_num);
    set_label_if_changed(s_lbl_name, s_show_name, sizeof(s_show_name), name, UI_COLOR_TEXT_DIM);

    char hint[96];
    uint32_t color = UI_COLOR_TEXT_DIM;

    if (s_phase == ENROLL_PHASE_DONE)
    {
        esp_err_t err;
        int id;
        bool is_delete;
        char res_name[32];
        read_result(&err, &id, res_name, sizeof(res_name), &is_delete);
        if (is_delete)
        {
            if (err == ESP_OK)
            {
                snprintf(hint, sizeof(hint), "%s %s", UI_TXT_ENROLL_DELETED, res_name);
                color = UI_COLOR_OK;
            }
            else
            {
                snprintf(hint, sizeof(hint), "%s %s", UI_TXT_ENROLL_DELETE_FAIL,
                         esp_err_to_name(err));
                color = UI_COLOR_DANGER;
            }
        }
        else if (err == ESP_OK)
        {
            snprintf(hint, sizeof(hint), "%s %s", UI_TXT_ENROLL_OK, res_name);
            color = UI_COLOR_OK;
        }
        else
        {
            snprintf(hint, sizeof(hint), "%s", enroll_err_text(err));
            color = UI_COLOR_DANGER;
        }
        set_label_if_changed(s_lbl_hint, s_show_hint, sizeof(s_show_hint), hint, color);
        return;
    }

    build_hint(hint, sizeof(hint), &color);
    set_label_if_changed(s_lbl_hint, s_show_hint, sizeof(s_show_hint), hint, color);
}

/**
 * @brief 定时器回调：按阶段刷新页面
 */
static void enroll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    const ui_page_t page = ui_app_current_page();
    const bool on_enroll = (page == UI_PAGE_ENROLL);
    const bool on_list = (page == UI_PAGE_FACE_LIST);
    if (!on_enroll && !on_list)
    {
        return; // 两个页面都不可见时空转
    }
    if (on_enroll && s_lbl_hint == NULL)
    {
        return; // 注册页控件尚未绑定
    }
    if (on_list && s_list == NULL)
    {
        return; // 人脸库列表尚未绑定
    }

    // ---- 0. 页面切换检测：进入删除页时强制重建列表 ----
    // 只在“后台”重建列表不够可靠：第一轮进入删除页时曾出现列表空且连“人脸库为空”
    // 提示都没有（必须点返回再进来才正常）。进入页面时把指纹作废，保证这一次一定
    // 重新拉取并重建，界面内容不依赖上一个页面里的后台刷新时机。
    if (page != s_last_page)
    {
        if (page == UI_PAGE_FACE_LIST)
        {
            s_list_fp_count = -1;
            ESP_LOGI(TAG, "进入删除页：强制刷新人脸库列表");
        }
        s_last_page = page;
    }

    // ---- 1. 锁外拉取人脸库（会阻塞到 face_task 处理完命令，不能占着 LVGL 锁）----
    // 两种时候拉：录入/删除刚完成（s_list_dirty），或者还没有成功建过列表
    // （s_list_fp_count < 0——否则直接进删除页会是空的）。后者失败后最多每秒重试
    // 一次：face_service_list() 会阻塞到命令超时，不能让定时器每 250 ms 卡一次。
    bool fetched = false;
    size_t n = 0;
    const bool want_fetch =
        s_list_dirty || (s_list_fp_count < 0 && (lv_tick_get() - s_last_fetch_tick) >= 1000);
    if (want_fetch && !s_worker_busy && s_list != NULL)
    {
        s_last_fetch_tick = lv_tick_get();
        s_list_dirty = false; // 先清标记：失败的话等下次录入/删除再拉，避免每秒重试
        if (face_service_list(s_records, UI_ENROLL_MAX_RECORDS, &n) == ESP_OK)
        {
            fetched = true;
        }
    }

    // ---- 2. 锁内刷新文字与列表 ----
    if (!ui_lvgl_lock(UI_ENROLL_LOCK_MS))
    {
        if (fetched)
        {
            s_list_dirty = true; // 没拿到锁，下次再刷
        }
        return; // 本轮跳过
    }

    if (on_enroll)
    {
        refresh_texts();
    }
    if (fetched)
    {
        refresh_list(n); // 内容未变时不重建
    }

    ui_lvgl_unlock();
}

// =================================================================
// 交互
// =================================================================

/**
 * @brief 创建刷新定时器（注册页与人脸库页共用；谁先绑定谁创建）
 * @note  两个页面在 ui_app_start() 里先后创建，这里只创建一次，之后长期运行，
 *        页面不可见时定时器空转。
 */
static void ensure_timer(void)
{
    if (s_timer != NULL)
    {
        return;
    }
    s_timer = lv_timer_create(enroll_timer_cb, UI_ENROLL_TIMER_MS, NULL);
    if (s_timer == NULL)
    {
        ESP_LOGE(TAG, "创建刷新定时器失败");
    }
}

/**
 * @brief "开始注册"按钮回调
 * @note  在 LVGL 任务里执行，不能阻塞：只做状态切换 + 启动工作线程。
 */
static void start_btn_cb(lv_event_t *e)
{
    (void)e;

    if (s_worker_busy)
    {
        ESP_LOGW(TAG, "上一次操作尚未结束，忽略本次点击");
        return;
    }

    store_result(false, ESP_OK, -1, NULL);
    s_phase = ENROLL_PHASE_RUNNING;
    s_worker_busy = true;

    // 同上：栈放 PSRAM，拿不到再退回内部 RAM
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(enroll_worker, "ui_enroll",
                                                    UI_ENROLL_TASK_STACK, NULL, 3, NULL, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
    {
        ok = xTaskCreate(enroll_worker, "ui_enroll", UI_ENROLL_TASK_STACK, NULL, 3, NULL);
    }
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "启动注册线程失败");
        s_worker_busy = false;
        s_phase = ENROLL_PHASE_IDLE;
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return;
    }

    // 立即反馈；真正结果由定时器读取
    set_label_if_changed(s_lbl_hint, s_show_hint, sizeof(s_show_hint), UI_TXT_FACE_ENROLLING,
                         UI_COLOR_ACCENT);
    ui_app_set_status(UI_TXT_FACE_ENROLLING, UI_COLOR_ACCENT);
    ESP_LOGI(TAG, "开始注册：等待质量合格的人脸");
}

#if UI_ENROLL_SELFTEST
/**
 * @brief 联调自检：切到注册页并模拟一次按钮点击（单次定时器）
 * @note  等价于用户点"开始注册"；若画面里没有人脸，应立即回 ESP_ERR_NOT_FOUND。
 */
static void selftest_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer); // 单次触发

    if (s_btn_start == NULL)
    {
        ESP_LOGW(TAG, "自检：注册页尚未创建，跳过");
        return;
    }
    ui_app_switch_page(UI_PAGE_ENROLL);
    lv_event_send(s_btn_start, LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "自检：已切到注册页并触发一次注册请求");
}
#endif

void ui_enroll_attach(lv_obj_t *lbl_hint, lv_obj_t *lbl_name, lv_obj_t *btn_start, lv_obj_t *list)
{
    if (lbl_hint == NULL || btn_start == NULL)
    {
        ESP_LOGW(TAG, "注册页控件不完整，交互不可用");
        return;
    }

    s_lbl_hint = lbl_hint;
    s_lbl_name = lbl_name;
    s_btn_start = btn_start;
    if (list != NULL)
    {
        s_list = list; // NULL 表示列表在独立的删除页（UI_PAGE_FACE_LIST）
    }
    s_show_hint[0] = '\0';
    s_show_name[0] = '\0';

    if (s_result_mutex == NULL)
    {
        s_result_mutex = xSemaphoreCreateMutex();
        if (s_result_mutex == NULL)
        {
            ESP_LOGE(TAG, "创建结果互斥量失败");
            return;
        }
    }

    ensure_timer();
    if (s_timer == NULL)
    {
        return;
    }

    lv_obj_add_event_cb(s_btn_start, start_btn_cb, LV_EVENT_CLICKED, NULL);
    s_list_fp_count = -1; // 首次进入页面时强制拉取列表
    ESP_LOGI(TAG, "注册页交互已绑定");

#if UI_ENROLL_SELFTEST
    // 联调自检：启动 8 s 后自动切到注册页并"点击"一次按钮，
    // 用来在没有人在场的情况下验证按钮→工作线程→定时器→状态显示整条链路。
    lv_timer_create(selftest_timer_cb, 8000, NULL);
#endif
}

void ui_enroll_bind_face_list(lv_obj_t *list)
{
    if (list == NULL)
    {
        ESP_LOGW(TAG, "人脸库列表为空，删除页不可用");
        return;
    }
    s_list = list;
    s_list_fp_count = -1; // 强制下一次刷新重建列表
    ensure_timer();
    ESP_LOGI(TAG, "人脸库列表已绑定（删除人脸页）");
}

void ui_enroll_leave(void)
{
    // 离开页面时把"已完成"复位为空闲，下次进入先显示实时提示；
    // 正在录入/删除时不打断（工作线程跑完会自行置 DONE）。
    if (s_phase == ENROLL_PHASE_DONE)
    {
        s_phase = ENROLL_PHASE_IDLE;
    }
    s_show_hint[0] = '\0';
    s_show_name[0] = '\0';
    s_list_fp_count = -1; // 下次进入强制刷新列表
}
