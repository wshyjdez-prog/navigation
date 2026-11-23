/*
 * P1 LinuxVoiceAdapter —— 语音 PAL 的 C ABI（FFI）边界。1 号维护。
 *
 * 任务书要求 speak() 调用本地 TTS、listen() 调用本地 ASR，设置中文语言环境，
 * 语音回调通过 FFI 传回核心层，并管理语音服务生命周期。本头文件只使用 C 类型，
 * 核心层（C/C++/其他语言）通过它调用 libp1_voice.so，不依赖 Qt 与 C++ ABI。
 *
 * ── 线程与回调 ───────────────────────────────────────────────────────────
 *  1. 所有 p1_voice_* 函数线程安全，可在任意线程调用，均不阻塞在语音合成/识别上
 *     （p1_voice_destroy 除外：它等待内部线程退出）。
 *  2. 回调只在适配器内部的“回调分发线程”上逐个调用（同一实例串行、按发生顺序），
 *     绝不会在 p1_voice_* 调用栈内同步回调，调用方可在持有自身锁时调用本 API。
 *  3. 回调中可以调用 speak/listen/cancel/stop_listening；**不得** 调用
 *     p1_voice_destroy（返回 P1_VOICE_ERR_IN_CALLBACK，不做任何事）。
 *  4. 回调应尽快返回；耗时处理请转交核心层自己的线程（Qt 侧用 QueuedConnection）。
 *
 * ── 缓冲区所有权 ────────────────────────────────────────────────────────
 *  - P1VoiceConfig 中的字符串仅在 p1_voice_create 调用期间被读取，适配器内部复制。
 *  - speak 的 text 在调用返回前被复制。
 *  - P1VoiceEvent 及其 text 指针由适配器拥有，仅在回调执行期间有效；需要保留时复制。
 *
 * ── 生命周期 ────────────────────────────────────────────────────────────
 *  create → (TTS、ASR 各自后台初始化) → 每个子系统一次 SERVICE_READY 或 SERVICE_ERROR
 *  （text 以 "tts=" / "asr=" 开头）→ speak/listen ...；某子系统失败不影响另一个（降级）。
 *  → destroy：取消全部在途请求，先送达对应 *_CANCELLED 事件，再停止并回收线程、
 *  关闭音频与 TTS/ASR 服务；destroy 返回后保证不再有任何回调，user_data 可释放。
 *  初始化完成前提交的 speak/listen 会排队，初始化失败时以 ERROR 事件结束。
 */
#ifndef P1_CONTRACTS_VOICE_FFI_H
#define P1_CONTRACTS_VOICE_FFI_H

#include <stdint.h>

#if defined(_WIN32)
#define P1_VOICE_API __declspec(dllexport)
#else
#define P1_VOICE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define P1_VOICE_ABI_VERSION 1u

/* 错误码：0 成功，负数失败。 */
typedef enum P1VoiceError {
    P1_VOICE_OK = 0,
    P1_VOICE_ERR_INVALID_ARGUMENT = -1,
    P1_VOICE_ERR_NOT_READY = -2,          /* 服务初始化失败或正在关闭 */
    P1_VOICE_ERR_BACKEND_UNAVAILABLE = -3,/* 后端未编译、库/守护进程不可用或已被占用 */
    P1_VOICE_ERR_MODEL_MISSING = -4,      /* 模型/音色资源缺失或损坏 */
    P1_VOICE_ERR_PERMISSION_DENIED = -5,  /* 无法访问音频服务（沙箱未授予麦克风/音频） */
    P1_VOICE_ERR_DEVICE = -6,             /* 无可用麦克风/播放设备或音频流失败 */
    P1_VOICE_ERR_BUSY = -7,               /* 已在识别中，一次只允许一个 listen */
    P1_VOICE_ERR_TIMEOUT = -8,            /* 识别超时 */
    P1_VOICE_ERR_IN_CALLBACK = -9,        /* 在回调线程内调用了 destroy */
    P1_VOICE_ERR_NO_SPEECH = -10,         /* 识别结束但没有可用文本 */
    P1_VOICE_ERR_INTERNAL = -11
} P1VoiceError;

typedef enum P1VoiceEventType {
    P1_VOICE_EVENT_SERVICE_READY = 1,     /* text = "tts=<后端>" 或 "asr=<后端>"，每个子系统一次 */
    P1_VOICE_EVENT_SERVICE_ERROR = 2,     /* text = "tts=<后端>: 原因" 等；error_code 有效 */
    P1_VOICE_EVENT_SPEAK_STARTED = 3,     /* 首个音频块已交给音频输出 */
    P1_VOICE_EVENT_SPEAK_FINISHED = 4,
    P1_VOICE_EVENT_SPEAK_CANCELLED = 5,
    P1_VOICE_EVENT_LISTEN_STARTED = 6,    /* 已开始采集 */
    P1_VOICE_EVENT_LISTEN_PARTIAL = 7,    /* 中间结果（后端支持时） */
    P1_VOICE_EVENT_LISTEN_RESULT = 8,     /* 最终识别文本（UTF-8，简体中文） */
    P1_VOICE_EVENT_LISTEN_CANCELLED = 9,
    P1_VOICE_EVENT_ERROR = 10             /* 针对 request_id 的失败；error_code 有效 */
} P1VoiceEventType;

typedef struct P1VoiceEvent {
    uint32_t struct_size;      /* = sizeof(P1VoiceEvent)，用于 ABI 扩展 */
    int32_t type;              /* P1VoiceEventType */
    uint64_t request_id;       /* 0 表示服务级事件 */
    int32_t error_code;        /* P1VoiceError；成功事件为 0 */
    float confidence;          /* 识别置信度 0–1；后端不提供时为 -1 */
    const char* text;          /* UTF-8，回调期间有效，可能为 "" 但不为 NULL */
    int64_t timestamp_ms;      /* Unix 毫秒 */
    int64_t monotonic_us;      /* CLOCK_MONOTONIC 微秒，用于计算时延 */
} P1VoiceEvent;

typedef void (*P1VoiceCallback)(const P1VoiceEvent* event, void* user_data);

typedef struct P1VoiceConfig {
    uint32_t struct_size;            /* = sizeof(P1VoiceConfig) */
    const char* tts_backend;         /* NULL = 默认后端 */
    const char* asr_backend;         /* NULL = 默认后端 */
    const char* language;            /* NULL = "zh-CN"；当前只支持普通话 */
    const char* resource_dir;        /* 语音离线资源根目录；NULL = 按安装布局查找 */
    const char* output_wav_path;     /* 非 NULL：TTS 写入该 WAV 而不播放（测试/基准） */
    const char* input_wav_path;      /* 非 NULL：listen 读取该 WAV 而不采集麦克风 */
    int32_t input_realtime;          /* input_wav 是否按实时速度送入（1）或尽快送入（0） */
    int32_t speech_rate;             /* -100..100，0 = 后端默认语速 */
} P1VoiceConfig;

#define P1_VOICE_SPEAK_INTERRUPT 0x1u  /* 打断正在播报的内容并清空队列 */

typedef struct P1VoiceAdapter P1VoiceAdapter;

/* 返回本库编译时启用的后端，形如 "tts=a,b;asr=c"。静态字符串，勿释放。 */
P1_VOICE_API const char* p1_voice_backends(void);
P1_VOICE_API uint32_t p1_voice_abi_version(void);
P1_VOICE_API const char* p1_voice_error_string(int32_t code);

/* 立即返回；引擎在后台加载，结果以 SERVICE_READY/SERVICE_ERROR 回调。
 * 失败（参数错误、后端未编译、同进程独占后端已被占用）时返回 NULL 并写 out_error。 */
P1_VOICE_API P1VoiceAdapter* p1_voice_create(const P1VoiceConfig* config,
                                             P1VoiceCallback callback,
                                             void* user_data,
                                             int32_t* out_error);
/* 取消全部请求并释放资源；返回后不再回调。回调线程内调用返回 ERR_IN_CALLBACK。 */
P1_VOICE_API int32_t p1_voice_destroy(P1VoiceAdapter* adapter);

/* 排队播报 UTF-8 文本；flags 可含 P1_VOICE_SPEAK_INTERRUPT。 */
P1_VOICE_API int32_t p1_voice_speak(P1VoiceAdapter* adapter, const char* text_utf8,
                                    uint32_t flags, uint64_t* out_request_id);
/* 开始识别一句话：检测到句末、调用 stop_listening、超时或输入结束时给出结果。 */
P1_VOICE_API int32_t p1_voice_listen(P1VoiceAdapter* adapter, uint32_t timeout_ms,
                                     uint64_t* out_request_id);
/* 结束采集并尽快给出当前识别结果（按住说话松开时调用）。 */
P1_VOICE_API int32_t p1_voice_stop_listening(P1VoiceAdapter* adapter);
/* 取消指定请求；request_id = 0 取消全部播报与识别。 */
P1_VOICE_API int32_t p1_voice_cancel(P1VoiceAdapter* adapter, uint64_t request_id);

/* 麦克风可用性/授权检查（同步、短超时）：OK、PERMISSION_DENIED 或 DEVICE。
 * text_out（可为 NULL）写入默认输入源名称，最多 text_capacity 字节。 */
P1_VOICE_API int32_t p1_voice_check_microphone(char* text_out, uint32_t text_capacity);

#ifdef __cplusplus
}
#endif

#endif /* P1_CONTRACTS_VOICE_FFI_H */
