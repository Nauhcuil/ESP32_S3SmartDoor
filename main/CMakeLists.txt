# 递归搜索所有源文件（.c 与 .cpp，人脸识别模块为 C++ 实现，依赖本地组件 esp-dl）
# 注意：GLOB 结果在 CMake 配置期缓存，**新增源文件后需要执行 idf.py reconfigure**
#（IDF 的组件依赖解析脚本不支持 CONFIGURE_DEPENDS，不能加该参数）
file(GLOB_RECURSE ALL_SRCS "who_human_face_recognition.cpp" "./*.c" "./*.cpp")
# 注册组件
# SRCS: 源文件列表
# INCLUDE_DIRS: 头文件目录列表（"." 为本目录，"font" 为点阵字库目录，子目录按模块划分）
# EMBED_FILES: 嵌入的文件列表
# EMBED_FILES: 嵌入的语音播报素材（M5 第二期，离线 TTS 生成的 16 kHz/16bit/单声道裸 PCM）
# 每个文件生成链接符号 _binary_<文件名>_start/_end（文件名中的 . 替换为 _），
# 由 audio/voice_prompt.c 引用。替换素材保持格式与文件名不变即可，无需改代码；
# 新增素材要同时更新 voice_prompt.c 的 extern 声明与 s_assets[] 表。
set(PROMPT_ASSETS
    "audio/prompts/p_hello.pcm"
    "audio/prompts/p_recognizing.pcm"
    "audio/prompts/p_open.pcm"
    "audio/prompts/p_liveness.pcm"
    "audio/prompts/p_unknown.pcm"
    "audio/prompts/p_no_face.pcm"
    "audio/prompts/p_too_far.pcm"
    "audio/prompts/p_quality.pcm"
    "audio/prompts/p_enroll_ok.pcm"
    "audio/prompts/p_enroll_err.pcm"
    "audio/prompts/p_denied.pcm"
    "audio/prompts/p_query_log.pcm"
    "audio/prompts/p_menu.pcm"
    "audio/prompts/p_vol_up.pcm"
    "audio/prompts/p_vol_down.pcm"
    "audio/prompts/p_cancel.pcm"
    "audio/prompts/p_remote.pcm")
# 链接符号名与源文件名一致：_binary_<basename>_start，所以这里保持文件名即符号名
set(PROMPT_EMBED)
foreach(f ${PROMPT_ASSETS})
    list(APPEND PROMPT_EMBED "${CMAKE_CURRENT_LIST_DIR}/${f}")
endforeach()

idf_component_register(SRCS ${ALL_SRCS}
                       INCLUDE_DIRS "." "font" "sys" "ui" "face" "net" "audio"
                       EMBED_FILES ${PROMPT_EMBED})

# ==================== LVGL 堆放到 PSRAM（M4 内部 RAM 预算） ====================
# LVGL 默认把堆开成 48 KB 静态数组（内部 RAM .bss），加上 38 KB 绘制缓冲、WiFi 驱动的
# 静态缓冲和 face_task 的 16 KB 连续栈，内部 RAM 必然不够（实测剩余 2.4 KB，
# face_task 与 esp_wifi_init 互相抢，谁后启动谁失败）。
# LVGL 8.3 支持 LV_MEM_POOL_INCLUDE / LV_MEM_POOL_ALLOC：TLSF 分配器不变，只是把内存池
# 换成外部函数提供的内存。这里给 lvgl__lvgl 组件注入宏，把池改为 PSRAM 分配
# （实现在 main/ui/lv_mem_psram.c）。**不改 managed_components 里的任何源码**，
# 组件升级后本配置照常生效。
# 注意：LV_MEM_POOL_* 只有在 CONFIG_LV_MEM_ADR=0（默认）时才被 lv_conf_internal.h 采用。
idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
if(LVGL_LIB)
    # 只注入头文件：LV_MEM_POOL_ALLOC 与分配函数都定义在 lv_mem_psram.h 里（header-only
    # + static inline），由 lv_mem.c 自行内联，避免跨库符号与链接顺序问题。
    target_compile_definitions(${LVGL_LIB} PRIVATE LV_MEM_POOL_INCLUDE="lv_mem_psram.h")
    target_include_directories(${LVGL_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ui")
endif()
