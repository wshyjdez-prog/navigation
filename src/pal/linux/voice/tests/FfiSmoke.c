/* 纯 C 调用方：模拟核心层经 FFI 使用 LinuxVoiceAdapter，验证 ABI 不依赖 C++/Qt。
 * Piper 合成到临时 WAV（不需要声卡），等待 SPEAK_FINISHED 回调后销毁。 */
#include "contracts/VoiceFfi.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct CoreState {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int started;
    int finished;
    int errors;
    uint64_t speak_id;
} CoreState;

static void on_voice_event(const P1VoiceEvent* ev, void* user) {
    CoreState* core = (CoreState*)user;
    pthread_mutex_lock(&core->mutex);
    if (ev->struct_size < sizeof(P1VoiceEvent)) core->errors++;
    switch (ev->type) {
    case P1_VOICE_EVENT_SERVICE_READY: if (strncmp(ev->text, "tts=", 4) == 0) core->ready = 1; break;
    case P1_VOICE_EVENT_SPEAK_STARTED: if (ev->request_id == core->speak_id) core->started = 1; break;
    case P1_VOICE_EVENT_SPEAK_FINISHED: if (ev->request_id == core->speak_id) core->finished = 1; break;
    case P1_VOICE_EVENT_SERVICE_ERROR:
        if (strncmp(ev->text, "asr=", 4) == 0) break; /* 本测试有意不启用识别（asr=none） */
        /* fall through */
    case P1_VOICE_EVENT_ERROR:
        fprintf(stderr, "voice error %d: %s\n", ev->error_code, ev->text);
        core->errors++;
        break;
    default: break;
    }
    pthread_cond_broadcast(&core->cond);
    pthread_mutex_unlock(&core->mutex);
}

int main(void) {
    char wav[] = "/tmp/p1-voice-ffi-XXXXXX";
    int fd = mkstemp(wav);
    if (fd < 0) return 2;
    close(fd);

    CoreState core;
    memset(&core, 0, sizeof core);
    pthread_mutex_init(&core.mutex, NULL);
    pthread_cond_init(&core.cond, NULL);

    P1VoiceConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.tts_backend = "piper";
    cfg.asr_backend = "none"; /* 只测播报路径，不加载识别模型 */
    cfg.output_wav_path = wav;

    if (p1_voice_abi_version() != P1_VOICE_ABI_VERSION) return 3;
    printf("backends: %s\n", p1_voice_backends());
    int32_t err = 0;
    P1VoiceAdapter* adapter = p1_voice_create(&cfg, on_voice_event, &core, &err);
    if (!adapter) { fprintf(stderr, "create failed: %s\n", p1_voice_error_string(err)); return 4; }

    /* speak 在初始化完成前提交也会排队；回调绝不在 API 调用栈内发生，因此可持锁调用。 */
    uint64_t id = 0;
    pthread_mutex_lock(&core.mutex);
    err = p1_voice_speak(adapter, "前方路口左转", 0, &id);
    core.speak_id = id;
    pthread_mutex_unlock(&core.mutex);
    if (err != P1_VOICE_OK || id == 0) return 5;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 30;
    pthread_mutex_lock(&core.mutex);
    while (!core.finished && core.errors == 0) {
        if (pthread_cond_timedwait(&core.cond, &core.mutex, &deadline) != 0) break;
    }
    const int ok = core.ready && core.started && core.finished && core.errors == 0;
    pthread_mutex_unlock(&core.mutex);

    if (p1_voice_destroy(adapter) != P1_VOICE_OK) return 6;
    FILE* f = fopen(wav, "rb");
    long size = 0;
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
    remove(wav);
    printf("ready=%d started=%d finished=%d errors=%d wav_bytes=%ld\n",
           core.ready, core.started, core.finished, core.errors, size);
    return (ok && size > 44) ? 0 : 1;
}
