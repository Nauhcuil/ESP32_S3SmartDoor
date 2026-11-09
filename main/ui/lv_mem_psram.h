/**
 * @file    lv_mem_psram.h
 * @brief   LVGL 内存池的 PSRAM 分配（通过编译宏注入到 lvgl 组件的 lv_mem.c 内部）。
 *
 * @note    背景：LVGL 默认把堆开成静态数组（.bss，48 KB），这块内存永远占着内部 RAM。
 *          M4 接入 WiFi 后内部 RAM 只剩几 KB，face_task 的 16 KB 连续栈与 WiFi 驱动
 *          初始化直接报 ESP_ERR_NO_MEM。LVGL 8.3 支持 LV_MEM_POOL_INCLUDE /
 *          LV_MEM_POOL_ALLOC：TLSF 分配器不变，只是把“池”换成外部函数返回的内存，
 *          于是这 48 KB 就能落到 PSRAM。
 *          注入方式见 main/CMakeLists.txt 的 target_compile_definitions（给
 *          lvgl__lvgl 组件加 -DLV_MEM_POOL_INCLUDE），不需要修改 managed_components。
 *          代价：LVGL 对象结构体位于 PSRAM，渲染时会多走外部总线；实测预览与
 *          UI 刷新仍在可接受范围（对象结构体命中数据缓存）。
 *
 * @note    刻意写成 **header-only + static inline**：如果把分配函数放进 main 组件的
 *          .c，链接顺序是 lvgl → main → lvgl，lv_mem.c 的引用在 libmain.a 之前出现，
 *          会出现 undefined reference（静态库单向解析）。放进头文件后由 lv_mem.c
 *          自己内联展开，不产生跨库符号。
 */

#ifndef LV_MEM_PSRAM_H
#define LV_MEM_PSRAM_H

#include <stddef.h>

#include "esp_heap_caps.h"

/**
 * @brief 为 LVGL 分配内存池（PSRAM）
 * @param size 池大小（字节），由 LV_MEM_SIZE 决定（当前 48 KB）
 * @return 池首地址；PSRAM 不足时返回 NULL（随后 LVGL 会在 lv_init 中崩溃，故打印错误）
 */
static inline void *lv_mem_pool_alloc_psram(size_t size)
{
    void *pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pool == NULL)
    {
        // lv_mem.c 里没有日志标签，这里用 printf 保证错误一定能出现在串口
        extern int printf(const char *fmt, ...);
        printf("E lv_mem_psram: LVGL 内存池分配失败（%u 字节 PSRAM）\n", (unsigned)size);
        return NULL;
    }
    return pool;
}

/** @brief lv_mem.c 调用 LV_MEM_POOL_ALLOC(LV_MEM_SIZE) 取得内存池 */
#define LV_MEM_POOL_ALLOC(size) lv_mem_pool_alloc_psram(size)

#endif /* LV_MEM_PSRAM_H */
