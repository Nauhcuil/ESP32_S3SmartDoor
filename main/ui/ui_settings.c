/**
 * @file    ui_settings.c
 * @brief   设备设置页（M3）实现：识别阈值、活体与抓拍开关、管理员密码、恢复出厂。
 *
 * @note    1. 可写项都走现成接口：阈值/活体由 face_service 处理（同时写 NVS），抓拍开关
 *             与密码走 app_nvs；本页自己不做文件操作。
 *          2. 会阻塞的操作（设阈值要等 face_task 回执、清人脸库/日志/抓拍要写 Flash）
 *             一律放进一次性工作线程。**不要在 LVGL 回调里直接调用**：face_task 正忙
 *             时这些调用可能等上秒级，界面与预览会一起冻住。
 *          3. 管理员密码只存在 NVS（出厂默认值见 app_config.h），不硬编码、不在界面显示。
 *             修改密码与恢复出厂都要先输入当前密码：用 lv_msgbox + 数字键盘，二次确认。
 *          4. 网络配置（M4 的配网）与设备诊断（M6）在本页给出入口：网络当前提示“未启用”，
 *             诊断按钮跳转到诊断页（页面 M6 才填满真实数据）。
 *          5. 布局：内容区 464x236，7 行 x 30 px + 2 px 间距 = 224，正好填满。
 */

#include "ui_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access_log.h"
#include "app_config.h"
#include "app_nvs.h"
#include "audio_service.h"
#include "esp_err.h"
#include "esp_log.h"
#include "face_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreatePinnedToCoreWithCaps（PSRAM 栈） */
#include "net_service.h"
#include "freertos/task.h"
#include "snapshot.h"
#include "ui_app.h"
#include "ui_lvgl_port.h"
#include "ui_settings.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "voice_assistant.h" /* 音量 +/-：写 ES8311 并按档位播报 */

static const char *TAG = "ui_settings"; // 日志标签

/* ==================== 页面参数 ==================== */
/** @brief 阈值步进（每次点 -/+ 调整 0.05：步进太小要连点十几次才能看出区别） */
#define UI_SET_THR_STEP 0.05f
/** @brief 阈值可调范围（下限 0.40 已相当宽松，上限 0.85 避免完全无法识别） */
#define UI_SET_THR_MIN 0.40f
#define UI_SET_THR_MAX 0.85f
/**
 * @brief 一次性工作线程栈
 * @note  不能只看本函数自己的调用深度：face_service 的阻塞调用在超时时会在本任务里
 *        打一行 ESP_LOGW（"命令 %d 等待超时…"），printf 家族的 vsnprintf 对浮点/格式化
 *        的栈占用是 KB 级。实测 3 KB 栈在“网络配置”流程里触发 stack overflow 复位
 *        （日志：A stack overflow in task ui_set_job has been detected）。内部 RAM
 *        稳态约 14 KB、最大连续块 8 KB，取 4 KB 可创建（与 button_task 同量级）。
 */
#define UI_SET_TASK_STACK 4096
/**
 * @brief 行高与行间距
 * @note  内容区只有 236 px 高，现在有 7 行（阈值/音量/活体/抓拍/密码/网络/诊断）：
 *        原来 6 行用 34 + 4 = 38*6 = 228；加音量行后必须收紧到 30 + 2 = 32*7 = 224，
 *        否则最后一行会落到内容区外被裁掉。按钮高度 26，卡片上下各 2 px 内边距，
 *        30 px 行高正好放得下。
 */
#define UI_SET_ROW_H 30
#define UI_SET_ROW_GAP 2
/** @brief 行内按钮尺寸 */
#define UI_SET_BTN_W 84
#define UI_SET_BTN_H 26
/** @brief 音量步进（与语音命令 VOICE_VOLUME_STEP 一致，10%） */
#define UI_SET_VOL_STEP 10
/** @brief 刷新定时器周期（只在有“待刷新”标记时工作） */
#define UI_SET_TIMER_MS 300
/** @brief 定时器取锁超时（取不到就跳过本轮） */
#define UI_SET_LOCK_MS 20

/* ==================== 控件 ==================== */
static lv_obj_t *s_val_threshold = NULL; /**< 阈值数值 */
static lv_obj_t *s_val_volume = NULL;    /**< 音量数值（0~100） */
static lv_obj_t *s_val_liveness = NULL;  /**< 活体开关按钮（文字即状态） */
static lv_obj_t *s_val_snapshot = NULL;  /**< 抓拍开关按钮 */
static lv_obj_t *s_val_pin = NULL;       /**< 密码占位（****） */
static lv_obj_t *s_val_wifi = NULL;      /**< 网络配置状态 */
static lv_timer_t *s_timer = NULL;       /**< 刷新定时器 */

/** @brief 当前打开的模态框（确认/输入框），离开页面时关闭 */
static lv_obj_t *s_modal = NULL;
/** @brief 一次性工作线程是否在运行（防止连点重复执行） */
static volatile bool s_busy = false;
/** @brief 有数值需要刷新（工作线程改了阈值等） */
static volatile bool s_dirty = false;

static void refresh_network(void);

/**
 * @brief 把运行时状态刷到控件上（锁内调用）
 * @note  数值一律从接口重新读，不做局部缓存——避免页面显示与 face_service/NVS 不一致。
 */
static void refresh_values(void)
{
    if (s_val_threshold != NULL)
    {
        char text[16];
        snprintf(text, sizeof(text), "%.2f", face_service_get_threshold());
        lv_label_set_text(s_val_threshold, text);
    }
    if (s_val_volume != NULL)
    {
        char text[8];
        snprintf(text, sizeof(text), "%d", (int)audio_service_get_volume());
        lv_label_set_text(s_val_volume, text);
    }
    if (s_val_liveness != NULL)
    {
        lv_obj_t *label = lv_obj_get_child(s_val_liveness, 0);
        if (label != NULL)
        {
            lv_label_set_text(label, face_service_get_liveness() ? UI_TXT_ON : UI_TXT_OFF);
        }
    }
    if (s_val_snapshot != NULL)
    {
        lv_obj_t *label = lv_obj_get_child(s_val_snapshot, 0);
        if (label != NULL)
        {
            lv_label_set_text(label, app_nvs_get_snapshot_enabled() ? UI_TXT_ON : UI_TXT_OFF);
        }
    }
    if (s_val_pin != NULL)
    {
        lv_label_set_text(s_val_pin, "****"); // 只显示占位，绝不显示真实密码
    }
    refresh_network();
}

/**
 * @brief 只刷新网络状态（锁内调用）
 * @note  与 refresh_values() 分开：其它项要读 NVS / 调 face_service，没必要每 300 ms 做一次；
 *        网络状态变化（连上/掉线/进配网）是异步的，本函数用"文字变化才重绘 + 改颜色"。
 */
static void refresh_network(void)
{
    if (s_val_wifi == NULL)
    {
        return;
    }

    const char *text = net_service_status_text();
    if (strcmp(lv_label_get_text(s_val_wifi), text) != 0)
    {
        lv_label_set_text(s_val_wifi, text);
    }

    uint32_t color = UI_COLOR_TEXT_DIM;
    switch (net_service_get_status())
    {
    case NET_STATUS_CONNECTED:
        color = UI_COLOR_ACCENT; // 已连接：蓝色，和"待配置"一眼区分
        break;
    case NET_STATUS_PROVISIONING:
        color = UI_COLOR_WARN; // 配网中：橙色
        break;
    case NET_STATUS_CONNECTING:
        color = UI_COLOR_TEXT;
        break;
    default:
        color = UI_COLOR_TEXT_DIM;
        break;
    }
    lv_obj_set_style_text_color(s_val_wifi, lv_color_hex(color), 0);
}

/**
 * @brief 关闭当前模态框（锁内调用）
 */
static void close_modal(void)
{
    if (s_modal != NULL)
    {
        lv_obj_t *mbox = s_modal;
        s_modal = NULL;
        lv_msgbox_close(mbox);
    }
}

/**
 * @brief 模态框销毁时同步 s_modal（用户点“取消”也会走到这里）
 */
static void modal_gone_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (mbox == s_modal)
    {
        s_modal = NULL;
    }
}

/**
 * @brief 刷新定时器：网络状态每拍检查；其它项只在工作线程改过后补刷新
 * @note  定时器常驻但开销极小（多数拍次只在 LVGL 任务里比较一次字符串）。
 */
static void settings_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const bool dirty = s_dirty;
    const bool visible = (ui_app_current_page() == UI_PAGE_SETTINGS);

    if (!dirty && !visible)
    {
        return; // 页面不可见时既没有待刷新项，也没必要更新网络状态文字
    }
    if (!ui_lvgl_lock(UI_SET_LOCK_MS))
    {
        return; // 下轮再试（dirty 保留，不会丢）
    }
    if (dirty)
    {
        refresh_values();
        s_dirty = false;
    }
    else
    {
        refresh_network();
    }
    ui_lvgl_unlock();
}

/* =================================================================
 * 工作线程
 * ================================================================= */

/** @brief 工作类型 */
typedef enum
{
    UI_SET_JOB_THRESHOLD = 0, /**< 设识别阈值（arg.f 有效） */
    UI_SET_JOB_LIVENESS,      /**< 切换活体开关（arg.f：0 或 1） */
    UI_SET_JOB_SNAPSHOT,      /**< 切换陌生人抓拍开关（arg.f：0 或 1） */
    UI_SET_JOB_PIN,           /**< 修改管理员密码（arg.pin 有效） */
    UI_SET_JOB_FACTORY,       /**< 恢复出厂 */
    UI_SET_JOB_PROVISION,     /**< 进入配网模式（M4） */
} ui_set_job_t;

/** @brief 工作线程参数（堆上分配，线程结束时释放） */
typedef struct
{
    ui_set_job_t kind;
    float f;
    uint32_t pin;
} job_arg_t;

/**
 * @brief 一次性工作线程：执行阻塞调用并把结果写到状态栏
 * @param arg job_arg_t*
 * @note  本线程自删除；s_busy 在结束时清掉，允许下一次操作。
 */
static void job_worker(void *arg)
{
    job_arg_t *job = (job_arg_t *)arg;

    switch (job->kind)
    {
    case UI_SET_JOB_THRESHOLD:
    {
        const esp_err_t err = face_service_set_threshold(job->f);
        ui_app_set_status(err == ESP_OK ? UI_TXT_SET_SAVED : UI_TXT_SET_SAVE_FAIL,
                          err == ESP_OK ? UI_COLOR_OK : UI_COLOR_DANGER);
        ESP_LOGI(TAG, "识别阈值设为 %.2f：%s", job->f, esp_err_to_name(err));
        break;
    }
    case UI_SET_JOB_LIVENESS:
    {
        const bool enable = job->f > 0.5f;
        // set_liveness 同时更新 face_service 的缓存变量与 face_liveness 模块的开关，
        // 不经过命令队列（face_task 不持有活体开关），所以不会阻塞等回执。
        const esp_err_t err = face_service_set_liveness(enable);
        ui_app_set_status(err == ESP_OK ? UI_TXT_SET_SAVED : UI_TXT_SET_SAVE_FAIL,
                          err == ESP_OK ? UI_COLOR_OK : UI_COLOR_DANGER);
        ESP_LOGI(TAG, "活体检测%s", enable ? "已开启" : "已关闭");
        break;
    }
    case UI_SET_JOB_SNAPSHOT:
    {
        const bool enable = job->f > 0.5f;
        const esp_err_t err = app_nvs_set_snapshot_enabled(enable);
        ui_app_set_status(err == ESP_OK ? UI_TXT_SET_SAVED : UI_TXT_SET_SAVE_FAIL,
                          err == ESP_OK ? UI_COLOR_OK : UI_COLOR_DANGER);
        ESP_LOGI(TAG, "陌生人抓拍%s", enable ? "已开启" : "已关闭");
        break;
    }
    case UI_SET_JOB_PIN:
    {
        const esp_err_t err = app_nvs_set_admin_pin(job->pin);
        ui_app_set_status(err == ESP_OK ? UI_TXT_SET_SAVED : UI_TXT_SET_SAVE_FAIL,
                          err == ESP_OK ? UI_COLOR_OK : UI_COLOR_DANGER);
        ESP_LOGI(TAG, "管理员密码修改%s", err == ESP_OK ? "成功" : "失败");
        break;
    }
    case UI_SET_JOB_FACTORY:
    {
        // 恢复出厂：清空人脸库 → 参数回默认值 → 清日志与抓拍。
        // 不整区擦 NVS：M4 的 WiFi/云端参数由各自模块管理，这里只复位本页涉及的项。
        const esp_err_t e1 = face_service_clear_all();
        const esp_err_t e2 = face_service_set_threshold(APP_CFG_FACE_THRESHOLD);
        const esp_err_t e3 = face_service_set_liveness(APP_CFG_LIVENESS_ENABLE != 0);
        const esp_err_t e4 = app_nvs_set_snapshot_enabled(true);
        const esp_err_t e5 = app_nvs_set_admin_pin(APP_CFG_ADMIN_PIN_DEFAULT);
        (void)access_log_clear();
        (void)snapshot_clear();

        const bool ok = (e1 == ESP_OK) && (e2 == ESP_OK) && (e3 == ESP_OK) && (e4 == ESP_OK) &&
                        (e5 == ESP_OK);
        ui_app_set_status(ok ? UI_TXT_DONE : UI_TXT_SET_SAVE_FAIL,
                          ok ? UI_COLOR_OK : UI_COLOR_DANGER);
        ESP_LOGI(TAG, "恢复出厂%s（清库=%d 阈值=%d 活体=%d 抓拍=%d 密码=%d）",
                 ok ? "完成" : "部分失败", (int)e1, (int)e2, (int)e3, (int)e4, (int)e5);
        break;
    }
    case UI_SET_JOB_PROVISION:
    {
        // esp_wifi_set_mode(APSTA) + 启动 HTTP 服务约需百毫秒，放线程里做
        const esp_err_t err = net_service_start_provisioning();
        if (err == ESP_OK)
        {
            char ap[24];
            net_service_get_ap_ssid(ap, sizeof(ap));
            char msg[64];
            // 只写热点名与 IP：文字须落在 ui/font_symbols.txt 子集内（"浏览器访问"不在子集里）
            snprintf(msg, sizeof(msg), "%s %s  http://192.168.4.1", UI_TXT_SET_PROV_AP, ap);
            ui_app_set_status(msg, UI_COLOR_WARN);
            ESP_LOGI(TAG, "配网模式已开启：热点 %s，浏览器访问 192.168.4.1", ap);
        }
        else
        {
            ui_app_set_status(UI_TXT_SET_SAVE_FAIL, UI_COLOR_DANGER);
            ESP_LOGW(TAG, "开启配网模式失败：%s", esp_err_to_name(err));
        }
        break;
    }
    default:
        break;
    }

    s_dirty = true;
    s_busy = false;
    free(job);
    vTaskDelete(NULL);
}

/**
 * @brief 启动一次性工作线程
 * @return true 已启动
 */
static bool start_job(ui_set_job_t kind, float f, uint32_t pin)
{
    if (s_busy)
    {
        ui_app_set_status(UI_TXT_PLEASE_WAIT, UI_COLOR_WARN);
        return false;
    }
    job_arg_t *job = (job_arg_t *)malloc(sizeof(*job));
    if (job == NULL)
    {
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return false;
    }
    job->kind = kind;
    job->f = f;
    job->pin = pin;

    s_busy = true;
    if (xTaskCreate(job_worker, "ui_set_job", UI_SET_TASK_STACK, job, 2, NULL) != pdPASS)
    {
        s_busy = false;
        free(job);
        ESP_LOGE(TAG, "启动设置线程失败");
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return false;
    }
    return true;
}

/* =================================================================
 * 数字密码输入框
 * ================================================================= */

/** @brief 对话框用途 */
typedef enum
{
    UI_SET_DLG_PIN = 0, /**< 修改管理员密码：先验当前密码，再输两次新密码 */
    UI_SET_DLG_FACTORY, /**< 恢复出厂：验当前密码 */
    UI_SET_DLG_WIFI,    /**< 网络配置（M4 接入）：验当前密码 */
} ui_set_dlg_t;

/** @brief 对话框阶段：0 = 验证当前密码，1 = 输入新密码 */
#define UI_SET_PHASE_VERIFY 0
#define UI_SET_PHASE_NEW    1

/** @brief 对话框上下文（随模态框一起释放） */
typedef struct
{
    int purpose;     /**< ui_set_dlg_t */
    int phase;       /**< UI_SET_PHASE_* */
    lv_obj_t *mbox;  /**< 模态框 */
    lv_obj_t *ta;    /**< 数字输入框 */
    char title[32];  /**< 标题缓存（切换阶段时改文字用） */
} dlg_arg_t;

/**
 * @brief 从输入框取出 PIN（只接受数字）
 * @return PIN；为空或含非数字返回 UINT32_MAX
 */
static uint32_t read_pin(lv_obj_t *ta)
{
    const char *s = lv_textarea_get_text(ta);
    if (s == NULL || s[0] == '\0')
    {
        return UINT32_MAX;
    }
    uint32_t v = 0;
    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return UINT32_MAX;
        }
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > 99999999U)
        {
            return UINT32_MAX;
        }
    }
    return v;
}

/**
 * @brief 把对话框切到“输入新密码”阶段
 */
static void dlg_to_new_phase(dlg_arg_t *arg)
{
    arg->phase = UI_SET_PHASE_NEW;
    lv_label_set_text(lv_msgbox_get_text(arg->mbox), UI_TXT_SET_PIN_NEW);
    lv_textarea_set_text(arg->ta, "");
    lv_obj_add_state(arg->ta, LV_STATE_FOCUSED);
}

/**
 * @brief 对话框“确定”处理：先验密码，再按用途执行
 * @note  在 LVGL 任务里执行，只做校验与启动线程，不阻塞。
 */
static void dlg_confirm(dlg_arg_t *arg)
{
    const uint32_t pin = read_pin(arg->ta);
    if (pin == UINT32_MAX)
    {
        ui_app_set_status(UI_TXT_SET_PIN_INPUT, UI_COLOR_WARN);
        return;
    }

    if (arg->phase == UI_SET_PHASE_VERIFY)
    {
        if (pin != app_nvs_get_admin_pin())
        {
            // 密码错误时**保持对话框打开**：让用户直接重输，不用再点一次按钮
            ui_app_set_status(UI_TXT_SET_PIN_WRONG, UI_COLOR_DANGER);
            lv_textarea_set_text(arg->ta, "");
            return;
        }

        if (arg->purpose == UI_SET_DLG_PIN)
        {
            dlg_to_new_phase(arg);
            return;
        }
        if (arg->purpose == UI_SET_DLG_FACTORY)
        {
            close_modal();
            start_job(UI_SET_JOB_FACTORY, 0.0f, 0);
            return;
        }
        if (arg->purpose == UI_SET_DLG_WIFI)
        {
            // 验密码通过后开热点：设备随即可被手机重新配网（保存后重启生效）
            close_modal();
            start_job(UI_SET_JOB_PROVISION, 0.0f, 0);
            return;
        }
        close_modal();
        return;
    }

    // 新密码阶段：写入 NVS
    close_modal();
    start_job(UI_SET_JOB_PIN, 0.0f, pin);
}

/**
 * @brief 对话框按钮回调
 * @param e 事件（user_data 为 dlg_arg_t*）
 */
static void dlg_mbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    dlg_arg_t *arg = (dlg_arg_t *)lv_event_get_user_data(e);
    if (mbox == NULL || arg == NULL)
    {
        return;
    }
    const char *btn = lv_msgbox_get_active_btn_text(mbox);
    if (btn == NULL || strcmp(btn, UI_TXT_CONFIRM) != 0)
    {
        close_modal(); // 取消
        return;
    }
    dlg_confirm(arg);
}

/**
 * @brief 对话框销毁时释放上下文
 */
static void dlg_free_cb(lv_event_t *e)
{
    dlg_arg_t *arg = (dlg_arg_t *)lv_event_get_user_data(e);
    if (arg != NULL)
    {
        if (arg->mbox == s_modal)
        {
            s_modal = NULL;
        }
        free(arg);
    }
}

/**
 * @brief 弹出数字密码对话框
 * @param purpose 用途
 * @param title   标题
 * @note  - 消息框宽度 420、内容区纵向排列“输入框 + 数字键盘”，保证 320 px 高的屏幕上
 *          键盘不会被裁掉；键盘用 LV_KEYBOARD_MODE_NUMBER，只出数字键。
 *        - 模态框挂在 lv_layer_top() 上并居中：它不属于任何页面，切页时由 leave 回调关闭。
 */
static void open_pin_dialog(ui_set_dlg_t purpose, const char *title)
{
    close_modal();

    dlg_arg_t *arg = (dlg_arg_t *)calloc(1, sizeof(*arg));
    if (arg == NULL)
    {
        ui_app_set_status(UI_TXT_STATUS_NO_MEM, UI_COLOR_DANGER);
        return;
    }
    arg->purpose = (int)purpose;
    arg->phase = UI_SET_PHASE_VERIFY;
    snprintf(arg->title, sizeof(arg->title), "%s", title);

    static const char *btns[] = {UI_TXT_CONFIRM, UI_TXT_CANCEL, ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, title, UI_TXT_SET_PIN_INPUT, btns, false);
    if (mbox == NULL)
    {
        free(arg);
        return;
    }
    arg->mbox = mbox;
    s_modal = mbox;

    // 中文必须显式设置字体（默认字体没有汉字，会显示空白）
    lv_obj_set_style_text_font(mbox, UI_FONT_BODY, 0);
    lv_obj_set_size(mbox, 420, 280);
    lv_obj_center(mbox);

    lv_obj_t *content = lv_msgbox_get_content(mbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 4, 0);

    lv_obj_t *ta = lv_textarea_create(content);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 8);
    lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_textarea_set_password_mode(ta, true); // 输入时显示圆点，避免旁人看到密码
    lv_obj_set_size(ta, 320, 40);
    lv_obj_set_style_text_font(ta, UI_FONT_BODY, 0);
    arg->ta = ta;

    lv_obj_t *kb = lv_keyboard_create(content);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, 0);
    lv_obj_set_size(kb, 380, 110);

    lv_obj_add_event_cb(mbox, dlg_mbox_cb, LV_EVENT_VALUE_CHANGED, arg);
    lv_obj_add_event_cb(mbox, dlg_free_cb, LV_EVENT_DELETE, arg);
    lv_obj_add_event_cb(mbox, modal_gone_cb, LV_EVENT_DELETE, NULL);

    ESP_LOGI(TAG, "弹出密码对话框：%s", title);
}

/* =================================================================
 * 行内控件回调
 * ================================================================= */

/**
 * @brief 阈值减/加按钮回调
 * @param e 事件（user_data：+1 加，-1 减）
 */
static void threshold_step_cb(lv_event_t *e)
{
    const intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    float t = face_service_get_threshold() + (dir > 0 ? UI_SET_THR_STEP : -UI_SET_THR_STEP);
    if (t < UI_SET_THR_MIN)
    {
        t = UI_SET_THR_MIN;
    }
    if (t > UI_SET_THR_MAX)
    {
        t = UI_SET_THR_MAX;
    }
    if (t == face_service_get_threshold())
    {
        return; // 已在边界，不再发命令
    }
    start_job(UI_SET_JOB_THRESHOLD, t, 0);
}

/**
 * @brief 音量 -/+ 回调
 * @param e 事件（user_data：+1 加，-1 减）
 *
 * @note  音量不经过 face_service，也不需要 NVS：audio_service 是唯一 owner，
 *        voice_assistant_set_volume_and_report() 写 ES8311 并按档位播报。
 *        这是**唯一在 LVGL 回调里直接执行**的可写项——不涉及 Flash、不阻塞，
 *        代价只有一次 I2C 写寄存器（几十微秒）。
 */
static void volume_step_cb(lv_event_t *e)
{
    const intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    int vol = (int)audio_service_get_volume() + (dir > 0 ? UI_SET_VOL_STEP : -UI_SET_VOL_STEP);
    if (vol < 0)
    {
        vol = 0;
    }
    if (vol > 100)
    {
        vol = 100;
    }
    (void)voice_assistant_set_volume_and_report(vol);
    /* 回调本身就在 LVGL 锁内：直接刷新数值标签，用户点一下立即看到变化 */
    if (s_val_volume != NULL)
    {
        char text[8];
        snprintf(text, sizeof(text), "%d", vol);
        lv_label_set_text(s_val_volume, text);
    }
}

/**
 * @brief 活体开关回调
 */
static void liveness_toggle_cb(lv_event_t *e)
{
    (void)e;
    start_job(UI_SET_JOB_LIVENESS, face_service_get_liveness() ? 0.0f : 1.0f, 0);
}

/**
 * @brief 陌生人抓拍开关回调
 */
static void snapshot_toggle_cb(lv_event_t *e)
{
    (void)e;
    start_job(UI_SET_JOB_SNAPSHOT, app_nvs_get_snapshot_enabled() ? 0.0f : 1.0f, 0);
}

/** @brief “新密码”按钮回调 */
static void change_pin_cb(lv_event_t *e)
{
    (void)e;
    open_pin_dialog(UI_SET_DLG_PIN, UI_TXT_SET_PIN);
}

/** @brief “网络配置”按钮回调（M4 才真正可用） */
static void wifi_cb(lv_event_t *e)
{
    (void)e;
    open_pin_dialog(UI_SET_DLG_WIFI, UI_TXT_SET_WIFI);
}

/** @brief “恢复出厂”按钮回调 */
static void factory_cb(lv_event_t *e)
{
    (void)e;
    open_pin_dialog(UI_SET_DLG_FACTORY, UI_TXT_SET_FACTORY);
}

/** @brief “设备诊断”按钮回调：跳到诊断页 */
static void diag_cb(lv_event_t *e)
{
    (void)e;
    ui_app_switch_page(UI_PAGE_DIAG);
}

/* =================================================================
 * 页面
 * ================================================================= */

/**
 * @brief 创建设置行卡片（上下留白 2 px、左右 10 px）
 * @param content 页面内容区
 * @param y       纵坐标
 * @return 卡片对象
 * @note  ui_card_create() 默认四边 8 px 内边距，行高 34 px 时内容区只剩 18 px，
 *        26 px 高的按钮会被父对象裁掉上下各 4 px。这里改成上下 2 px、左右 10 px。
 */
static lv_obj_t *create_row_card(lv_obj_t *content, int y)
{
    const int w = UI_SCREEN_W - UI_PAD * 2; // 464
    lv_obj_t *card = ui_card_create(content, 0, y, w, UI_SET_ROW_H);
    lv_obj_set_style_pad_top(card, 2, 0);
    lv_obj_set_style_pad_bottom(card, 2, 0);
    lv_obj_set_style_pad_left(card, 10, 0);
    lv_obj_set_style_pad_right(card, 10, 0);
    return card;
}

/**
 * @brief 创建一行：左侧名称 + 右侧数值标签或按钮
 * @param content 页面内容区
 * @param y       纵坐标
 * @param name    名称
 * @param value   数值文字（可为 NULL）
 * @param btn     按钮文字（可为 NULL）
 * @param cb      按钮回调
 * @return 数值标签（value 为 NULL 时返回 NULL）
 */
static lv_obj_t *create_row(lv_obj_t *content, int y, const char *name, const char *value,
                            const char *btn, lv_event_cb_t cb)
{
    lv_obj_t *card = create_row_card(content, y);
    lv_obj_t *label = ui_label_create(card, name, UI_FONT_BODY, UI_COLOR_TEXT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *value_label = NULL;
    if (value != NULL)
    {
        const uint32_t color = strcmp(value, UI_TXT_NOT_ENABLED) == 0 ? UI_COLOR_TEXT_DIM
                                                                     : UI_COLOR_ACCENT;
        value_label = ui_label_create(card, value, UI_FONT_BODY, color);
        lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, btn != NULL ? -92 : 0, 0);
    }
    if (btn != NULL)
    {
        lv_obj_t *b = ui_button_create(card, btn, 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2, UI_SET_BTN_W,
                                       UI_SET_BTN_H, cb, NULL);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return value_label;
}

void ui_settings_leave(void)
{
    if (ui_lvgl_lock(UI_SET_LOCK_MS))
    {
        close_modal();
        ui_lvgl_unlock();
    }
}

lv_obj_t *ui_settings_create(void)
{
    lv_obj_t *content = ui_page_create(UI_TXT_SET_TITLE, true);
    if (content == NULL)
    {
        return NULL;
    }

    const int step = UI_SET_ROW_H + UI_SET_ROW_GAP; // 32
    int y = 0;

    // ---- 1. 识别阈值：[-] 0.55 [+] ----
    {
        lv_obj_t *card = create_row_card(content, y);
        lv_obj_t *label = ui_label_create(card, UI_TXT_SET_THRESHOLD, UI_FONT_BODY, UI_COLOR_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *minus = ui_button_create(card, "-", 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2, 40,
                                           UI_SET_BTN_H, threshold_step_cb, (void *)(intptr_t)-1);
        lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -132, 0);

        s_val_threshold = ui_label_create(card, "0.55", UI_FONT_BODY, UI_COLOR_ACCENT);
        lv_obj_align(s_val_threshold, LV_ALIGN_RIGHT_MID, -40, 0);

        lv_obj_t *plus = ui_button_create(card, "+", 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2, 40,
                                          UI_SET_BTN_H, threshold_step_cb, (void *)(intptr_t)1);
        lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    y += step;

    // ---- 2. 音量：[-] 60 [+]（与语音命令“声音大一点/小一点”等价）----
    {
        lv_obj_t *card = create_row_card(content, y);
        lv_obj_t *label = ui_label_create(card, UI_TXT_SET_VOLUME, UI_FONT_BODY, UI_COLOR_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *minus = ui_button_create(card, "-", 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2, 40,
                                           UI_SET_BTN_H, volume_step_cb, (void *)(intptr_t)-1);
        lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -132, 0);

        s_val_volume = ui_label_create(card, "60", UI_FONT_BODY, UI_COLOR_ACCENT);
        lv_obj_align(s_val_volume, LV_ALIGN_RIGHT_MID, -40, 0);

        lv_obj_t *plus = ui_button_create(card, "+", 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2, 40,
                                          UI_SET_BTN_H, volume_step_cb, (void *)(intptr_t)1);
        lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    y += step;

    // ---- 3. 活体检测：按钮文字即状态 ----
    {
        lv_obj_t *card = create_row_card(content, y);
        lv_obj_t *label = ui_label_create(card, UI_TXT_SET_LIVENESS, UI_FONT_BODY, UI_COLOR_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        s_val_liveness = ui_button_create(card, UI_TXT_ON, 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2,
                                          UI_SET_BTN_W, UI_SET_BTN_H, liveness_toggle_cb, NULL);
        lv_obj_align(s_val_liveness, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    y += step;

    // ---- 4. 陌生人抓拍 ----
    {
        lv_obj_t *card = create_row_card(content, y);
        lv_obj_t *label =
            ui_label_create(card, UI_TXT_SET_SNAPSHOT, UI_FONT_BODY, UI_COLOR_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        s_val_snapshot = ui_button_create(card, UI_TXT_ON, 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2,
                                          UI_SET_BTN_W, UI_SET_BTN_H, snapshot_toggle_cb, NULL);
        lv_obj_align(s_val_snapshot, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    y += step;

    // ---- 5. 管理员密码（只显示占位 + 修改入口）----
    s_val_pin = create_row(content, y, UI_TXT_SET_PIN, "****", UI_TXT_SET_PIN_NEW, change_pin_cb);
    y += step;

    // ---- 6. 网络配置（M4 接入配网）----
    s_val_wifi = create_row(content, y, UI_TXT_SET_WIFI, UI_TXT_NOT_ENABLED, UI_TXT_SET_PROV,
                            wifi_cb);
    y += step;

    // ---- 7. 设备诊断入口 + 恢复出厂（危险操作，需密码确认）----
    {
        lv_obj_t *card = create_row_card(content, y);
        lv_obj_t *btn_diag = ui_button_create(card, UI_TXT_HOME_DIAG, 0, (UI_SET_ROW_H - UI_SET_BTN_H) / 2,
                                              UI_SET_BTN_W + 16, UI_SET_BTN_H, diag_cb, NULL);
        lv_obj_align(btn_diag, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *btn_factory = ui_button_create(card, UI_TXT_SET_FACTORY, 0,
                                                 (UI_SET_ROW_H - UI_SET_BTN_H) / 2, UI_SET_BTN_W + 16,
                                                 UI_SET_BTN_H, factory_cb, NULL);
        lv_obj_align(btn_factory, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // 诊断页从设置页进入，返回也应回到设置页
    ui_page_set_back_target(UI_PAGE_DIAG, UI_PAGE_SETTINGS);
    ui_page_set_leave_cb(UI_PAGE_SETTINGS, ui_settings_leave);

    if (s_timer == NULL)
    {
        s_timer = lv_timer_create(settings_timer_cb, UI_SET_TIMER_MS, NULL);
    }
    refresh_values();

    return lv_obj_get_parent(content);
}
