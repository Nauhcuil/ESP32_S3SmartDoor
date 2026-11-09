/**
 * @file    ui_logs.c
 * @brief   门禁记录页（M3）实现：分页列表、翻页、清空。
 *
 * @note    1. 数据来源是 access_log（`/spiffs/log/events.jsonl`），每次拉取都会真正读
 *              Flash，因此**只在页面可见且内容可能变化时**才读：翻页、刚清空、第 0 页
 *             且总条数变化。
 *          2. 读文件与控件更新分两段：先在锁外读（Flash 读会短暂关闭 cache，不能占着
 *             LVGL 锁），拿到数据后再取锁重建列表；取不到锁就放弃本轮，下次定时器再试。
 *          3. 列表每页 4 行：内容区 464x236，行高 30 px + 表头 + 页码行 + 按钮行正好放满，
 *             不做滚动（滚动列表与翻页按钮同时存在时用户很难判断“还有没有下一页”）。
 *          4. 时间列来自记录的 ts_ms：clock_service 在启动时已把时间设为编译时刻，所以
 *             ts_ms 通常是 epoch 毫秒；极端情况下（时区解析失败等）退回只显示结果。
 */

#include "ui_logs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "access_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_app.h"
#include "ui_lvgl_port.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_logs"; // 日志标签

/* ==================== 页面参数 ==================== */
/** @brief 每页记录数（4 行 x 30 px） */
#define UI_LOGS_PAGE_SIZE 4
/** @brief 记住的页数上限（超过后不再前翻，避免无界数组） */
#define UI_LOGS_MAX_PAGES 64
/** @brief 刷新定时器周期（毫秒） */
#define UI_LOGS_TIMER_MS 500
/** @brief 定时器里取 LVGL 锁的等待时间（取不到就跳过本轮） */
#define UI_LOGS_LOCK_MS 20
/** @brief 列表行高（16 px 字体 + 上下留白，手指不误触） */
#define UI_LOGS_ITEM_H 30
/** @brief 清空工作线程栈（只做几次阻塞调用与日志） */
#define UI_LOGS_TASK_STACK 3072

/** @brief 判定 ts_ms 是 epoch 毫秒而不是开机毫秒的阈值（2023-11-14） */
#define UI_LOGS_EPOCH_MS_THRESHOLD 1700000000000LL

/* ==================== 控件与状态 ==================== */
static lv_obj_t *s_list = NULL;       /**< 记录列表 */
static lv_obj_t *s_page_label = NULL; /**< "共 N 条 第 x/y 页" */
static lv_obj_t *s_btn_prev = NULL;   /**< 上一页 */
static lv_obj_t *s_btn_next = NULL;   /**< 下一页 */
static lv_obj_t *s_btn_clear = NULL;  /**< 清空记录 */
static lv_timer_t *s_timer = NULL;    /**< 刷新定时器（创建一次，长期运行） */

/** @brief 当前页号（0 = 最新一页） */
static int s_page = 0;
/**
 * @brief 每一页的 seq 上界：翻到第 k 页时读 “seq < s_page_before[k]” 的记录
 * @note  s_page_before[0] = 0 表示不限制（从最新开始）；第 k+1 页的上界取第 k 页
 *        最旧一条的 seq，这样翻页结果与“当时”一致，不受期间新增记录影响。
 */
static uint32_t s_page_before[UI_LOGS_MAX_PAGES] = {0};

/** @brief 拉取缓冲（只在定时器/翻页处理里使用） */
static access_log_record_t s_records[UI_LOGS_PAGE_SIZE];

/** @brief 上一次重建列表时的总条数（用来判断第 0 页是否需要刷新） */
static uint32_t s_seen_total = 0xFFFFFFFFu;
/** @brief 强制下一轮刷新（翻页、清空、进入页面时置位） */
static volatile bool s_force_refresh = true;
/** @brief 定时器上次看到的页面（进入记录页时强制刷新） */
static ui_page_t s_last_page = UI_PAGE_MAX;
/** @brief 清空线程是否在运行 */
static volatile bool s_clearing = false;
/** @brief 文字缓存（内容未变不重绘） */
static char s_page_text[64] = "";

/**
 * @brief 计算总页数
 * @param total 总条数
 * @return 页数（至少 1，空库也显示“第 1/1 页”）
 */
static int total_pages(uint32_t total)
{
    const int pages = (int)((total + UI_LOGS_PAGE_SIZE - 1) / UI_LOGS_PAGE_SIZE);
    return pages < 1 ? 1 : pages;
}

/**
 * @brief 把 ts_ms 格式化成 "MM-DD HH:MM"
 * @param ts_ms 时间戳：epoch 毫秒或开机毫秒
 * @param buf   输出缓冲（至少 16 字节）
 * @param size  缓冲大小
 * @note  时间不可信时（ts 过小）输出 "--"：宁可显示未知，也不要显示 1970 年的假时间。
 */
static void format_ts(int64_t ts_ms, char *buf, size_t size)
{
    if (ts_ms < UI_LOGS_EPOCH_MS_THRESHOLD)
    {
        snprintf(buf, size, "--");
        return;
    }
    time_t sec = (time_t)(ts_ms / 1000);
    struct tm tm_rec;
    if (localtime_r(&sec, &tm_rec) == NULL)
    {
        snprintf(buf, size, "--");
        return;
    }
    // 先格式化到本地缓冲再按长度拷贝：直接 snprintf 到调用方缓冲时，编译器无法
    // 证明缓冲区足够（tm_* 是 int，最坏 11 位），-Werror=format-truncation 会报错。
    char tmp[24];
    const size_t len = (size_t)snprintf(tmp, sizeof(tmp), "%02d-%02d %02d:%02d", tm_rec.tm_mon + 1,
                                        tm_rec.tm_mday, tm_rec.tm_hour, tm_rec.tm_min);
    const size_t n = len + 1 < size ? len + 1 : size;
    memcpy(buf, tmp, n);
    buf[n - 1] = '\0';
}

/**
 * @brief 取一条记录的“结果”文字与颜色
 * @param rec   日志记录
 * @param text  输出文字
 * @param color 输出颜色（0xRRGGBB）
 * @note  陌生人事件写入的是 type=stranger / result=info，单看 result 会显示成“仅记录”，
 *        所以先按 type 判断。
 */
static void result_text(const access_log_record_t *rec, const char **text, uint32_t *color)
{
    if (strcmp(rec->type, ACCESS_LOG_TYPE_STRANGER) == 0)
    {
        *text = UI_TXT_LOG_STRANGER;
        *color = UI_COLOR_WARN;
        return;
    }
    if (strcmp(rec->result, ACCESS_LOG_RESULT_OPEN) == 0)
    {
        *text = UI_TXT_LOG_PASS;
        *color = UI_COLOR_OK;
        return;
    }
    if (strcmp(rec->result, ACCESS_LOG_RESULT_DENY_LIVE) == 0)
    {
        *text = UI_TXT_LOG_LIVENESS_FAIL;
        *color = UI_COLOR_DANGER;
        return;
    }
    if (strcmp(rec->result, ACCESS_LOG_RESULT_DENY) == 0)
    {
        *text = UI_TXT_LOG_DENY;
        *color = UI_COLOR_DANGER;
        return;
    }
    *text = UI_TXT_UNKNOWN;
    *color = UI_COLOR_TEXT_DIM;
}

/**
 * @brief 按当前页号拉取记录（锁外调用）
 * @return 实际条数
 */
static size_t fetch_page(void)
{
    uint32_t before = 0;
    if (s_page > 0 && s_page < UI_LOGS_MAX_PAGES)
    {
        before = s_page_before[s_page];
    }
    return access_log_read_page(s_records, UI_LOGS_PAGE_SIZE, UI_LOGS_PAGE_SIZE, before);
}

/**
 * @brief 重建列表与页码文字（调用方须持有 LVGL 锁）
 * @param n     本次拉取的条数
 * @param total 日志总条数
 */
static void rebuild(size_t n, uint32_t total)
{
    if (s_list != NULL)
    {
        ui_list_clear(s_list);
        if (n == 0)
        {
            ui_list_add_item(s_list, UI_TXT_HOME_NO_RECORD, UI_COLOR_TEXT_DIM, NULL, NULL);
        }
        for (size_t i = 0; i < n; i++)
        {
            char time_text[16];
            format_ts(s_records[i].ts_ms, time_text, sizeof(time_text));

            const char *result = UI_TXT_UNKNOWN;
            uint32_t color = UI_COLOR_TEXT_DIM;
            result_text(&s_records[i], &result, &color);

            // 陌生人记录的 name 为空，显示“陌生人”而不是空白列
            const char *name = s_records[i].name[0] != '\0' ? s_records[i].name
                                                            : UI_TXT_LOG_STRANGER;
            char row[96];
            snprintf(row, sizeof(row), "%s   %s   %s", time_text, name, result);
            lv_obj_t *label = ui_list_add_item(s_list, row, color, NULL, NULL);
            if (label != NULL && lv_obj_get_parent(label) != NULL)
            {
                lv_obj_set_height(lv_obj_get_parent(label), UI_LOGS_ITEM_H);
            }
        }
    }

    const int pages = total_pages(total);
    char text[64];
    snprintf(text, sizeof(text), "%s %u %s   %s %d/%d %s", UI_TXT_LOG_TOTAL, (unsigned)total,
             UI_TXT_LOG_COUNT, UI_TXT_LOG_PAGE, s_page + 1, pages, UI_TXT_LOG_PAGE_OF);
    if (s_page_label != NULL && strcmp(text, s_page_text) != 0)
    {
        snprintf(s_page_text, sizeof(s_page_text), "%s", text);
        lv_label_set_text(s_page_label, text);
    }

    // 首/末页把翻页按钮置灰（仍然可点，但处理函数里也会再判断一次）
    if (s_btn_prev != NULL)
    {
        if (s_page <= 0)
        {
            lv_obj_add_state(s_btn_prev, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_clear_state(s_btn_prev, LV_STATE_DISABLED);
        }
    }
    if (s_btn_next != NULL)
    {
        if (s_page + 1 >= pages)
        {
            lv_obj_add_state(s_btn_next, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_clear_state(s_btn_next, LV_STATE_DISABLED);
        }
    }
}

/**
 * @brief 拉取并刷新一页
 * @param total  日志总条数
 * @param force  是否强制重建（否则内容未变时跳过）
 * @note  本函数在定时器与翻页按钮里共用：先锁外读文件，再锁内更新控件。
 */
static void refresh_page(uint32_t total, bool force)
{
    const size_t n = fetch_page();

    if (!ui_lvgl_lock(UI_LOGS_LOCK_MS))
    {
        s_force_refresh = true; // 没拿到锁，下一轮再来
        return;
    }

    // 记住下一页的 seq 上界：本页最旧一条的 seq（新的在前，所以是最后一条）。
    // 只在翻页后的第一次刷新时记录，之后本页因新事件刷新不会改写它——否则上界会随
    // 新记录变化，翻回去时页边界对不上，同一批记录可能出现在两页里。
    if (force && n > 0 && s_page + 1 < UI_LOGS_MAX_PAGES)
    {
        s_page_before[s_page + 1] = s_records[n - 1].seq;
    }

    rebuild(n, total);
    s_seen_total = total;
    if (force)
    {
        s_force_refresh = false;
    }
    ui_lvgl_unlock();
}

/**
 * @brief “上一页”按钮回调
 */
static void prev_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_page <= 0)
    {
        return;
    }
    s_page--;
    s_force_refresh = true;
    ESP_LOGI(TAG, "翻到第 %d 页", s_page + 1);
}

/**
 * @brief “下一页”按钮回调
 */
static void next_page_cb(lv_event_t *e)
{
    (void)e;
    const int pages = total_pages(access_log_get_count());
    if (s_page + 1 >= pages)
    {
        return;
    }
    // 下一页的上界由本页最旧一条的 seq 决定，refresh_page() 会填好；这里只前进页号
    s_page++;
    s_force_refresh = true;
    ESP_LOGI(TAG, "翻到第 %d 页", s_page + 1);
}

/**
 * @brief 清空工作线程：调用 access_log_clear() 并复位页号
 * @param arg 未使用
 * @note  清空要删 4 个日志文件（Flash 操作），放在独立线程里做，避免卡住 LVGL。
 */
static void clear_worker(void *arg)
{
    (void)arg;

    const esp_err_t err = access_log_clear();
    if (err == ESP_OK)
    {
        s_page = 0;
        memset(s_page_before, 0, sizeof(s_page_before));
        s_force_refresh = true;
        s_seen_total = 0xFFFFFFFFu;
        ui_app_set_status(UI_TXT_LOG_CLEARED, UI_COLOR_OK);
        ESP_LOGI(TAG, "门禁记录已清空");
    }
    else
    {
        ui_app_set_status(UI_TXT_STATUS_DB_FAIL, UI_COLOR_DANGER);
        ESP_LOGW(TAG, "清空门禁记录失败：%s", esp_err_to_name(err));
    }

    s_clearing = false;
    vTaskDelete(NULL);
}

/** @brief 确认框参数（关闭时释放） */
typedef struct
{
    int unused; /**< 占位：确认框不需要额外数据，仅用于区分生命周期 */
} clear_arg_t;

/**
 * @brief 确认框按钮回调：点“确定”才清空
 * @param e 事件（user_data 为 clear_arg_t*）
 */
static void clear_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (mbox == NULL)
    {
        return;
    }
    const char *btn = lv_msgbox_get_active_btn_text(mbox);
    if (btn != NULL && strcmp(btn, UI_TXT_CONFIRM) == 0)
    {
        if (!s_clearing)
        {
            s_clearing = true;
            if (xTaskCreate(clear_worker, "ui_logs_clr", UI_LOGS_TASK_STACK, NULL, 2, NULL) !=
                pdPASS)
            {
                s_clearing = false;
                ESP_LOGE(TAG, "启动清空线程失败");
                ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
            }
        }
    }
    lv_msgbox_close(mbox);
}

/**
 * @brief 确认框关闭时释放参数
 * @param e 事件（user_data 为 clear_arg_t*）
 */
static void clear_arg_free_cb(lv_event_t *e)
{
    clear_arg_t *arg = (clear_arg_t *)lv_event_get_user_data(e);
    if (arg != NULL)
    {
        free(arg);
    }
}

/**
 * @brief “清空记录”按钮回调：先弹确认框
 * @param e 事件
 * @note  日志一旦清空无法恢复，必须二次确认；确认框居中并统一用中文字体
 *        （默认字体没有汉字，会显示空白）。
 */
static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_clearing)
    {
        return;
    }

    clear_arg_t *arg = (clear_arg_t *)malloc(sizeof(*arg));
    if (arg == NULL)
    {
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return;
    }
    arg->unused = 0;

    static const char *btns[] = {UI_TXT_CONFIRM, UI_TXT_CANCEL, ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, UI_TXT_LOG_CLEAR, UI_TXT_LOG_CLEAR_CONFIRM, btns, false);
    if (mbox == NULL)
    {
        free(arg);
        return;
    }
    lv_obj_set_style_text_font(mbox, UI_FONT_BODY, 0);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, clear_confirm_cb, LV_EVENT_VALUE_CHANGED, arg);
    lv_obj_add_event_cb(mbox, clear_arg_free_cb, LV_EVENT_DELETE, arg);
}

/**
 * @brief 刷新定时器：页面可见时按需拉取
 * @note  只有三种情况才读 Flash：刚进入页面、刚翻页、第 0 页且总条数变化（有新事件）。
 */
static void logs_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    const ui_page_t page = ui_app_current_page();
    if (page != UI_PAGE_LOGS)
    {
        s_last_page = page;
        return;
    }
    if (page != s_last_page)
    {
        s_last_page = page;
        s_force_refresh = true; // 进入页面：强制重建（列表可能停在很久以前的内容）
    }

    const uint32_t total = access_log_get_count();
    if (!s_force_refresh && !(s_page == 0 && total != s_seen_total))
    {
        return;
    }
    refresh_page(total, s_force_refresh);
}

void ui_logs_leave(void)
{
    // 离开时不改页号，只保证下次进来一定重新读一次（期间可能有新事件）
    s_force_refresh = true;
}

lv_obj_t *ui_logs_create(void)
{
    lv_obj_t *content = ui_page_create(UI_TXT_LOG_TITLE, true);
    if (content == NULL)
    {
        return NULL;
    }

    // ---- 表头 ----
    lv_obj_t *header =
        ui_label_create(content, UI_TXT_LOG_TIME "      " UI_TXT_LOG_NAME "      " UI_TXT_LOG_RESULT,
                        UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_set_pos(header, 4, 0);

    // ---- 记录列表（4 行，不滚动）----
    lv_obj_t *list = ui_list_create(content, 0, 22, 464, 132);
    s_list = list;

    // ---- 页码 ----
    s_page_label =
        ui_label_create(content, UI_TXT_LOG_TOTAL " 0 " UI_TXT_LOG_COUNT, UI_FONT_BODY,
                        UI_COLOR_TEXT_DIM);
    lv_obj_set_pos(s_page_label, 4, 160);

    // ---- 翻页与清空 ----
    s_btn_prev = ui_button_create(content, UI_TXT_PREV_PAGE, 0, 186, 96, 36, prev_page_cb, NULL);
    s_btn_next = ui_button_create(content, UI_TXT_NEXT_PAGE, 104, 186, 96, 36, next_page_cb, NULL);
    s_btn_clear = ui_button_create(content, UI_TXT_LOG_CLEAR, 328, 186, 136, 36, clear_btn_cb, NULL);

    if (s_btn_prev != NULL)
    {
        lv_obj_add_state(s_btn_prev, LV_STATE_DISABLED); // 初始在第 1 页
    }
    if (s_btn_next != NULL)
    {
        lv_obj_add_state(s_btn_next, LV_STATE_DISABLED); // 刷新后按页数恢复
    }
    (void)s_btn_clear;

    ui_page_set_leave_cb(UI_PAGE_LOGS, ui_logs_leave);

    if (s_timer == NULL)
    {
        s_timer = lv_timer_create(logs_timer_cb, UI_LOGS_TIMER_MS, NULL);
    }
    s_force_refresh = true;

    return lv_obj_get_parent(content);
}
