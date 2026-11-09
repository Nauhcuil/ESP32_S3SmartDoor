/**
 * @file    face_selftest.cpp
 * @brief   模型自检实现（调试用，正式固件通过开关整体关闭）。
 *
 * @note    排查“画面里明明有人却检不出人脸”时，最需要区分两种情况：
 *          1. 模型/推理链路本身坏了（内存布局、输入解释、模型文件）；
 *          2. 摄像头送进来的像素不是模型期望的样子（格式、字节序、数据损坏）。
 *          用 ESP-DL 示例里确定能检出的图像跑同一个检测器即可区分：
 *          示例图也检不出 → 链路问题；示例图正常 → 问题在摄像头数据。
 */

#include "face_selftest.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "human_face_detect_mnp01.hpp"
#include "human_face_detect_msr01.hpp"

static const char *TAG = "face_selftest";

/**
 * @brief 示例图像是否编入（图像在 src/ 子目录的 .hpp 里，未编入时数组不存在）
 * @note  用 __has_include 检测，避免在未放入图像时链接失败。
 */
/**
 * @brief 示例图像自检开关：1 = 在首帧到达后对编译期示例图跑一次检测并打印各缩放系数的结果
 * @note  图像以 const 数组编进 .rodata（src/selftest_image.hpp），约 610 KB Flash；
 *        只用于验证检测链路（见文件头说明）。正式固件必须为 0——0 时图像数组被
 *        编译器整体裁掉，既不占 Flash 也不进 RAM。
 */
#ifndef FACE_SELFTEST_IMAGE_DEBUG
#define FACE_SELFTEST_IMAGE_DEBUG 0
#endif

#if __has_include("src/selftest_image.hpp") && FACE_SELFTEST_IMAGE_DEBUG
#include "src/selftest_image.hpp"
#define FACE_SELFTEST_ENABLED 1
#endif

#ifndef FACE_SELFTEST_ENABLED
#define FACE_SELFTEST_ENABLED 0
#endif

#if FACE_SELFTEST_ENABLED

/** @brief 示例图像放大倍数（小图更容易漏检，放大两倍用于确认 RGB565 路径本身可用） */
#define SELFTEST_SCALE 2

/**
 * @brief 把示例图像（RGB888）缩放后画到指定尺寸的灰色 RGB565 画布上
 * @param w/h     输出画布尺寸（模拟实时帧，例如 320x240）
 * @param fit_w   示例图像在画布上的宽度（等比缩放到该宽度；0 = 按画布高度自适应）
 * @param swap    true = “高字节在前”的摄像头字节布局；false = 位域正确的低字节在前
 * @return 新分配的 PSRAM 缓冲（宽*高 个 uint16_t），失败返回 NULL
 * @note  - 摄像头输出的是“高字节在前”的 RGB565 字节流（首字节 = RRRRRGGG），
 *          而 XTensa 是小端，所以把该字节流当 uint16_t 读得到的是“字节交换后的值”。
 *        - swap=false 时按“位域正确”的值写入（低字节在前）；
 *          swap=true 时再交换一次，模拟摄像头字节流被按 uint16 直接解释的样子。
 *        - 只保留灰度（R=G=B）：人脸检测网络主要依赖灰度与结构，灰度足以区分两种解释，
 *          同时省掉 RGB 通道布局的干扰。画布其余区域填黑。
 */
static uint16_t *build_gray565(int w, int h, int fit_w, bool swap)
{
    uint16_t *dst = (uint16_t *)heap_caps_malloc((size_t)w * h * sizeof(uint16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst == NULL)
    {
        return NULL;
    }
    memset(dst, 0, (size_t)w * h * sizeof(uint16_t));

    const uint8_t *src = SELFTEST_IMAGE_ELEMENT;
    const int sw = SELFTEST_IMAGE_WIDTH;
    const int sh = SELFTEST_IMAGE_HEIGHT;

    // 等比缩放：以 fit_w 为准计算高；fit_w 为 0 时按画布高度铺满
    int dw = fit_w > 0 ? fit_w : h * sw / sh;
    int dh = dw * sh / sw;
    if (dw > w)
    {
        dw = w;
        dh = dw * sh / sw;
    }
    if (dh > h)
    {
        dh = h;
        dw = dh * sw / sh;
    }
    const int ox = (w - dw) / 2;
    const int oy = (h - dh) / 2;

    for (int y = 0; y < dh; y++)
    {
        const int sy = y * sh / dh;
        for (int x = 0; x < dw; x++)
        {
            const int sx = x * sw / dw;
            const uint8_t *p = src + ((size_t)sy * sw + sx) * 3;
            const uint8_t lum = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            const uint16_t g5 = (uint16_t)(lum >> 3);
            // R=G=B=灰度：红 5 位 | 绿 6 位 | 蓝 5 位
            const uint16_t v = (uint16_t)((g5 << 11) | ((g5 << 1) << 5) | g5);
            dst[(size_t)(oy + y) * w + (ox + x)] = swap ? (uint16_t)((v >> 8) | (v << 8)) : v;
        }
    }
    return dst;
}

long face_selftest_detect(HumanFaceDetectMSR01 &detector)
{
    // ---- 路径 1：示例原图 RGB888（与 esp-dl 例程完全一致的调用方式）----
    std::list<dl::detect::result_t> &cands =
        detector.infer((uint8_t *)SELFTEST_IMAGE_ELEMENT,
                       {SELFTEST_IMAGE_HEIGHT, SELFTEST_IMAGE_WIDTH, SELFTEST_IMAGE_CHANNEL});
    const size_t n_cands = cands.size();
    ESP_LOGI(TAG, "[模型自检] RGB888 %dx%d：MSR01 候选 %u", SELFTEST_IMAGE_WIDTH,
             SELFTEST_IMAGE_HEIGHT, (unsigned)n_cands);
    for (auto &c : cands)
    {
        ESP_LOGI(TAG, "[模型自检]   候选(%d,%d,%d,%d) 分数 %.2f", (int)c.box[0], (int)c.box[1],
                 (int)c.box[2], (int)c.box[3], c.score);
    }

    // ---- 路径 2：MNP01 对上面的候选做精修 ----
    HumanFaceDetectMNP01 stage2(0.4F, 0.3F, 10);
    std::list<dl::detect::result_t> &results = stage2.infer(
        (uint8_t *)SELFTEST_IMAGE_ELEMENT,
        {SELFTEST_IMAGE_HEIGHT, SELFTEST_IMAGE_WIDTH, SELFTEST_IMAGE_CHANNEL}, cands);
    ESP_LOGI(TAG, "[模型自检] RGB888：MNP01 结果 %u", (unsigned)results.size());
    for (auto &r : results)
    {
        ESP_LOGI(TAG, "[模型自检]   结果(%d,%d,%d,%d) 分数 %.2f", (int)r.box[0], (int)r.box[1],
                 (int)r.box[2], (int)r.box[3], r.score);
    }

    // ---- 路径 3：uint16_t 输入的两种字节布局（同一张灰度图）----
    // 若某一种布局能检出而另一种检不出，说明 ESP-DL 对 16 位输入的字节假设与
    // 摄像头输出不一致，这就是“实时画面检不出人脸”的根因。
    const int w = SELFTEST_IMAGE_WIDTH * SELFTEST_SCALE;
    const int h = SELFTEST_IMAGE_HEIGHT * SELFTEST_SCALE;
    // 这里刻意用 scale=0.3 的自检检测器，而不是实时检测器：
    // 实时检测器现在是 resize_scale=1.0（为了检出小人脸），把 780x1048 的图按 1.0
    // 送进 MSR01 会需要几十 MB 的中间张量，分配失败后模型内部指针为空 → 崩溃。
    // 本路径只验证“字节布局”，用 0.3 就够了。
    HumanFaceDetectMSR01 layout_probe(0.3F, 0.3F, 10, 0.3F);
    for (int mode = 0; mode < 2; mode++)
    {
        const bool swap = (mode == 1);
        // swap=false：位域正确的值（低字节在前）；swap=true：高字节在前（摄像头字节流布局）
        uint16_t *gray = build_gray565(w, h, 0, swap);
        if (gray == NULL)
        {
            ESP_LOGE(TAG, "[模型自检] 灰度缓冲分配失败（%dx%d）", w, h);
            return (long)results.size();
        }
        std::list<dl::detect::result_t> &c16 = layout_probe.infer(gray, {h, w, 3});
        ESP_LOGI(TAG, "[模型自检] uint16 %dx%d 灰度 RGB565（%s）：MSR01 候选 %u", w, h,
                 swap ? "高字节在前，同摄像头" : "低字节在前，位域正确", (unsigned)c16.size());
        heap_caps_free(gray);
    }

    // ---- 路径 4：按实时帧尺寸（QVGA 320x240）扫描 resize_scale ----
    // 路径 3 用的是放大两倍的图，人脸在输入里很大，即使 resize_scale 把画面缩得很小
    // 也能检出；实时帧是 320x240，站在镜头前的人脸往往只有几十像素，缩小 0.3 倍后
    // 只剩十几像素，检测器就完全看不见了。这里把同一张示例图按“站近/站远”两种尺度
    // 贴进 QVGA 画布，用不同 resize_scale 各跑一遍，直接给出“要多大的 scale 才检得出”。
    static const int qvga_w = 320;
    static const int qvga_h = 240;
    static const int fits[2] = {0, 150}; // 0 = 按画布高度铺满（人脸较大）；150 = 小脸（远处）
    static const float scales[3] = {0.3F, 0.5F, 1.0F};
    for (int k = 0; k < 2; k++)
    {
        uint16_t *scene = build_gray565(qvga_w, qvga_h, fits[k], true); // 与摄像头同字节布局
        if (scene == NULL)
        {
            ESP_LOGE(TAG, "[模型自检] QVGA 画布分配失败");
            break;
        }
        for (int s = 0; s < 3; s++)
        {
            // 逐个构造/析构，避免同时驻留 3 份模型内存
            HumanFaceDetectMSR01 probe(0.3F, 0.3F, 10, scales[s]);
            std::list<dl::detect::result_t> &cq = probe.infer(scene, {qvga_h, qvga_w, 3});
            ESP_LOGI(TAG, "[模型自检] QVGA 320x240（人脸尺度 %s，resize_scale %.1f）：候选 %u",
                     fits[k] ? "小（远处）" : "大（站近）", scales[s], (unsigned)cq.size());
            for (auto &c : cq)
            {
                ESP_LOGI(TAG, "[模型自检]   候选(%d,%d,%d,%d) 分数 %.2f", (int)c.box[0],
                         (int)c.box[1], (int)c.box[2], (int)c.box[3], c.score);
            }
        }
        heap_caps_free(scene);
    }

    return (long)results.size();
}

#else

long face_selftest_detect(HumanFaceDetectMSR01 &detector)
{
    (void)detector;
    return -1;
}

#endif
