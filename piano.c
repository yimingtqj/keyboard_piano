#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdbool.h>

#pragma comment(lib, "winmm.lib")

#define SOUND_DIR "sound"          // sound\a.mp3 ~ sound\z.mp3
#define SCORE_DIR "scores"         // 乐谱 txt 文件夹
#define SCORE_GLOB "*.txt"

#define CHANNELS_PER_SOUND 16

#define DEFAULT_BPM 100
#define MIN_BPM 40
#define MAX_BPM 220

/*
    钢琴式听感优化：
    1. 按键立即触发音头，保证实时响应。
    2. 不再按理论时值硬切 MP3，避免八分/十六分没声音。
    3. 所有时值都走“最小可听门限 + 轻微淡出释放”，尽量保持音量一致。
    4. 短音短促但音头清楚，长音保留更久，切换音符保留自然余音。
*/
#define PIANO_MAX_GATE_MS 9000
#define SCORE_FINISH_TAIL_MS 1800

/*
    自动播放速度控制：
    1. AUTO_BPM_SCORE_STEP_RATIO = 0.82，让相邻音符播放稍快。
    2. 不再把速度压得过快，乐谱 BPM 仍然基本有效。
    3. 需要更细的 rhythm 时，在乐谱里使用单独的 . 作为 3ms 极短静默。
*/
#define AUTO_BPM_SCORE_STEP_RATIO 0.82

#define DOT_REST_MS 1               // BPM 乐谱中，单独的 . 表示极短静默
#define AUTO_MIN_STEP_MS DOT_REST_MS // 自动播放最小间隔，与 . 保持一致
#define MAX_EVENTS 32768
#define RAW_SCORE_SIZE 262144
#define MAX_SCORE_FILES 128
#define PATH_SIZE 512

typedef enum {
    MODE_NORMAL,
    MODE_BPM_INSERT,
    MODE_SCORE
} Mode;

typedef struct {
    char note;       // a-z 表示音源；' ' 表示休止
    int denom;       // 1=全音符，2=二分，4=四分，8=八分，16=十六分
    int dotted;      // 1 表示附点
    int fixed_ms;    // 特殊静默时使用；>0 则直接按固定毫秒播放
} NoteEvent;

typedef struct {
    char name[PATH_SIZE];
    char path[PATH_SIZE];
} ScoreFile;

typedef struct {
    Mode mode;
    bool paused;

    int bpm;
    int manual_denom;
    int manual_dotted;

    NoteEvent score[MAX_EVENTS];
    int score_len;
    int score_pos;
    char score_file[PATH_SIZE];
    bool score_finishing;
    ULONGLONG score_finish_until;
} App;

static int sound_ready[26][CHANNELS_PER_SOUND];
static int next_channel[26];

/*
    钢琴式释放控制：
    note_fade_tick：开始淡出的时间；
    note_off_tick ：真正停止该通道的时间；
    note_volume   ：记录当前音量，避免每 1ms 反复发送 setaudio。
*/
static ULONGLONG note_fade_tick[26][CHANNELS_PER_SOUND];
static ULONGLONG note_off_tick[26][CHANNELS_PER_SOUND];
static int note_volume[26][CHANNELS_PER_SOUND];

/* ========================= 基础工具 ========================= */

static void copy_text(char* dst, size_t size, const char* src) {
    if (size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static const char* mode_name(Mode mode) {
    switch (mode) {
    case MODE_NORMAL: return "NORMAL";
    case MODE_BPM_INSERT: return "BPM_INSERT";
    case MODE_SCORE:  return "SCORE";
    default: return "UNKNOWN";
    }
}

static void print_prompt(const App* app) {
    if (app->mode == MODE_NORMAL) printf(">>> ");
    else if (app->mode == MODE_BPM_INSERT) printf("BPM >>> ");
    else if (app->mode == MODE_SCORE) printf("SCORE >>> ");
}

static bool is_comment_line(const char* line) {
    const unsigned char* p = (const unsigned char*)line;

    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
    }

    while (*p == ' ' || *p == '\t') p++;
    return *p == '#' || *p == ';';
}

static bool is_rest_char(int ch) {
    return ch == '0' || ch == '-' || ch == '_';
}

static bool is_note_char(int ch) {
    return isalpha((unsigned char)ch);
}

/* ========================= 音源控制 ========================= */

static void print_mci_error(const char* cmd, MCIERROR err) {
    if (!err) return;

    char msg[256];
    if (mciGetErrorStringA(err, msg, sizeof(msg))) {
        printf("[MCI] %s -> %s\n", cmd, msg);
    } else {
        printf("[MCI] %s -> error %lu\n", cmd, (unsigned long)err);
    }
}

static void load_sounds(void) {
    printf("正在预加载音源...\n");

    for (char c = 'a'; c <= 'z'; ++c) {
        int idx = c - 'a';

        for (int i = 0; i < CHANNELS_PER_SOUND; ++i) {
            char cmd[PATH_SIZE + 64];

            snprintf(cmd, sizeof(cmd),
                     "open \"%s\\%c.mp3\" type mpegvideo alias s%c%d",
                     SOUND_DIR, c, c, i);

            MCIERROR err = mciSendStringA(cmd, NULL, 0, NULL);
            sound_ready[idx][i] = (err == 0);
            note_volume[idx][i] = 1000;

            if (err) print_mci_error(cmd, err);
        }
    }

    printf("音源预加载完成。\n");
}

static void close_sounds(void) {
    for (char c = 'a'; c <= 'z'; ++c) {
        int idx = c - 'a';

        for (int i = 0; i < CHANNELS_PER_SOUND; ++i) {
            if (!sound_ready[idx][i]) continue;

            char cmd[64];
            snprintf(cmd, sizeof(cmd), "close s%c%d", c, i);
            mciSendStringA(cmd, NULL, 0, NULL);
        }
    }
}

static void set_note_volume(char c, int ch, int volume) {
    c = (char)tolower((unsigned char)c);
    if (c < 'a' || c > 'z') return;
    if (ch < 0 || ch >= CHANNELS_PER_SOUND) return;

    volume = clamp_int(volume, 0, 1000);

    int idx = c - 'a';
    if (!sound_ready[idx][ch]) return;
    if (note_volume[idx][ch] == volume) return;

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "setaudio s%c%d volume to %d", c, ch, volume);
    mciSendStringA(cmd, NULL, 0, NULL);

    note_volume[idx][ch] = volume;
}

static int play_note(char c) {
    c = (char)tolower((unsigned char)c);
    if (c < 'a' || c > 'z') return -1;

    int idx = c - 'a';
    int ch = -1;

    /*
        优先找当前没有安排释放的空闲通道。
        这样同一个音快速重复时，不会把前一个音硬切掉。
    */
    for (int i = 0; i < CHANNELS_PER_SOUND; ++i) {
        int test = (next_channel[idx] + i) % CHANNELS_PER_SOUND;

        if (sound_ready[idx][test] && note_off_tick[idx][test] == 0) {
            ch = test;
            break;
        }
    }

    /*
        如果所有通道都在响，只能轮换抢一个。
        CHANNELS_PER_SOUND 已提升到 16，正常弹奏基本不会走到这里。
    */
    if (ch < 0) {
        for (int i = 0; i < CHANNELS_PER_SOUND; ++i) {
            int test = (next_channel[idx] + i) % CHANNELS_PER_SOUND;

            if (sound_ready[idx][test]) {
                ch = test;
                break;
            }
        }
    }

    if (ch < 0 || !sound_ready[idx][ch]) return -1;

    next_channel[idx] = (ch + 1) % CHANNELS_PER_SOUND;

    note_fade_tick[idx][ch] = 0;
    note_off_tick[idx][ch] = 0;
    set_note_volume(c, ch, 1000);

    char cmd[64];

    /*
        每次新触发都从音头开始，这是钢琴音色的“击弦感”来源。
        但不会再硬切其他正在响的通道，所以切换音符会更自然。
    */
    snprintf(cmd, sizeof(cmd), "stop s%c%d", c, ch);
    mciSendStringA(cmd, NULL, 0, NULL);

    snprintf(cmd, sizeof(cmd), "seek s%c%d to start", c, ch);
    mciSendStringA(cmd, NULL, 0, NULL);

    snprintf(cmd, sizeof(cmd), "play s%c%d", c, ch);
    mciSendStringA(cmd, NULL, 0, NULL);

    return ch;
}

static void stop_note_channel(char c, int ch) {
    c = (char)tolower((unsigned char)c);
    if (c < 'a' || c > 'z') return;
    if (ch < 0 || ch >= CHANNELS_PER_SOUND) return;

    int idx = c - 'a';
    if (!sound_ready[idx][ch]) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "stop s%c%d", c, ch);
    mciSendStringA(cmd, NULL, 0, NULL);

    note_fade_tick[idx][ch] = 0;
    note_off_tick[idx][ch] = 0;
    set_note_volume(c, ch, 1000);
}

static void schedule_piano_release(char c, int ch, int gate_ms, int release_ms) {
    c = (char)tolower((unsigned char)c);
    if (c < 'a' || c > 'z') return;
    if (ch < 0 || ch >= CHANNELS_PER_SOUND) return;

    int idx = c - 'a';

    if (gate_ms < 300) {
        gate_ms = 300;
    }

    if (gate_ms > PIANO_MAX_GATE_MS) {
        gate_ms = PIANO_MAX_GATE_MS;
    }

    if (release_ms < 120) {
        release_ms = 120;
    }

    if (release_ms > gate_ms / 2) {
        release_ms = gate_ms / 2;
    }

    ULONGLONG now = GetTickCount64();

    note_fade_tick[idx][ch] = now + (ULONGLONG)(gate_ms - release_ms);
    note_off_tick[idx][ch] = now + (ULONGLONG)gate_ms;
}

static void process_note_offs(void) {
    ULONGLONG now = GetTickCount64();

    for (char c = 'a'; c <= 'z'; ++c) {
        int idx = c - 'a';

        for (int ch = 0; ch < CHANNELS_PER_SOUND; ++ch) {
            ULONGLONG off = note_off_tick[idx][ch];
            if (off == 0) continue;

            if (now >= off) {
                stop_note_channel(c, ch);
                continue;
            }

            ULONGLONG fade = note_fade_tick[idx][ch];
            if (fade != 0 && now >= fade) {
                ULONGLONG fade_len = off > fade ? off - fade : 1;
                ULONGLONG remain = off > now ? off - now : 0;

                int volume = (int)((1000ULL * remain) / fade_len);
                volume = clamp_int(volume, 0, 1000);

                /*
                    音量变化太密会让 MCI 卡顿，所以只在差异明显时发送。
                */
                if (abs(note_volume[idx][ch] - volume) >= 80) {
                    set_note_volume(c, ch, volume);
                }
            }
        }
    }
}

static void trigger_event(NoteEvent e) {
    if (e.note != ' ') {
        play_note(e.note);
    }
}

/* ========================= 节奏计算 ========================= */

static NoteEvent make_note(char note, int denom, int dotted) {
    NoteEvent e;
    e.note = note;
    e.denom = denom;
    e.dotted = dotted;
    e.fixed_ms = 0;
    return e;
}

static NoteEvent make_rest(int denom, int dotted) {
    return make_note(' ', denom, dotted);
}

static NoteEvent make_fixed(char note, int ms) {
    NoteEvent e;
    e.note = note;
    e.denom = 0;
    e.dotted = 0;
    e.fixed_ms = ms;
    return e;
}

static int event_duration_ms(const App* app, NoteEvent e) {
    if (e.fixed_ms > 0) {
        return e.fixed_ms;
    }

    int denom = e.denom > 0 ? e.denom : 4;
    int bpm = clamp_int(app->bpm, MIN_BPM, MAX_BPM);

    double quarter = 60000.0 / (double)bpm;
    double whole = quarter * 4.0;
    double duration = whole / (double)denom;

    if (e.dotted) {
        duration *= 1.5;
    }

    int ms = (int)(duration + 0.5);
    if (ms < 20) ms = 20;
    return ms;
}

static int auto_step_duration_ms(const App* app, NoteEvent e) {
    int ms = event_duration_ms(app, e);

    /*
        BPM 自动播放：
        - 普通音符严格按 BPM 和时值计算；
        - 单独的 . 使用 DOT_REST_MS；
        - 最小间隔不再强制 20ms，而是 AUTO_MIN_STEP_MS。
    */
    if (app->mode == MODE_SCORE && e.fixed_ms <= 0) {
        ms = (int)(ms * AUTO_BPM_SCORE_STEP_RATIO + 0.5);
    }

    if (ms < AUTO_MIN_STEP_MS) {
        ms = AUTO_MIN_STEP_MS;
    }

    return ms;
}

static int piano_release_ms_for_event(NoteEvent e) {
    if (e.fixed_ms > 0) return 120;

    /*
        为了让 𝅝/𝅗𝅥/♩/♪/♬ 听起来音量更一致，
        不再给长音特别长的淡出时间。
        长短主要由 gate_ms 决定，淡出只做轻微收尾。
    */
    int denom = e.denom > 0 ? e.denom : 4;

    switch (denom) {
    case 1:  return e.dotted ? 220 : 200;  // 全音符
    case 2:  return e.dotted ? 200 : 180;  // 二分音符
    case 4:  return e.dotted ? 180 : 160;  // 四分音符
    case 8:  return e.dotted ? 160 : 140;  // 八分音符
    case 16: return e.dotted ? 140 : 120;  // 十六分音符
    case 32: return 110;
    default: return 160;
    }
}

static int piano_min_gate_ms_for_event(NoteEvent e) {
    if (e.fixed_ms > 0) return e.fixed_ms;

    int denom = e.denom > 0 ? e.denom : 4;

    /*
        最小可听音长：
        短音不能太短，否则音头不完整；
        但也不让长音和短音的响度差过大。
    */
    switch (denom) {
    case 1:  return e.dotted ? 3600 : 3000;
    case 2:  return e.dotted ? 2500 : 2000;
    case 4:  return e.dotted ? 1700 : 1300;
    case 8:  return e.dotted ? 1250 : 1000;
    case 16: return e.dotted ? 1100 : 900;
    case 32: return 760;
    default: return 1300;
    }
}

static double piano_gate_ratio_for_event(NoteEvent e) {
    if (e.fixed_ms > 0) return 1.0;

    int denom = e.denom > 0 ? e.denom : 4;

    /*
        比例整体收敛，避免某些时值被拉得过长或过早淡出，
        让不同符号的音量听起来更接近。
    */
    switch (denom) {
    case 1:  return 1.05;
    case 2:  return 1.10;
    case 4:  return 1.20;
    case 8:  return 1.45;
    case 16: return 1.80;
    case 32: return 2.00;
    default: return 1.20;
    }
}

static int piano_gate_ms_for_event(const App* app, NoteEvent e) {
    int nominal = event_duration_ms(app, e);

    if (e.fixed_ms > 0) {
        return nominal;
    }

    int min_gate = piano_min_gate_ms_for_event(e);
    double ratio = piano_gate_ratio_for_event(e);

    int gate = (int)(nominal * ratio + 0.5);

    if (gate < min_gate) {
        gate = min_gate;
    }

    if (gate > PIANO_MAX_GATE_MS) {
        gate = PIANO_MAX_GATE_MS;
    }

    return gate;
}

static void trigger_timed_event(const App* app, NoteEvent e) {
    if (e.note == ' ') {
        return;
    }

    int ch = play_note(e.note);
    if (ch < 0) {
        return;
    }

    /*
        所有新版时值都经过钢琴式门限优化：
        - 16分/8分：短促，但不硬切到没声音；
        - 4分：标准按键释放感；
        - 2分/全音符：更长余音；
        - 附点：自动延长。
         fixed_ms 谱仍只触发，不按 50ms 硬切。
    */
    if (e.fixed_ms <= 0) {
        int gate = piano_gate_ms_for_event(app, e);
        int release = piano_release_ms_for_event(e);
        schedule_piano_release(e.note, ch, gate, release);
    }
}

/* ========================= 乐谱文件扫描 ========================= */

static int score_file_cmp(const void* a, const void* b) {
    const ScoreFile* x = (const ScoreFile*)a;
    const ScoreFile* y = (const ScoreFile*)b;
    return _stricmp(x->name, y->name);
}

static int scan_scores(ScoreFile files[], int max_count) {
    char pattern[PATH_SIZE];

    snprintf(pattern, sizeof(pattern), "%s\\%s", SCORE_DIR, SCORE_GLOB);

    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int count = 0;

    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count >= max_count) break;

        copy_text(files[count].name, sizeof(files[count].name), data.cFileName);
        snprintf(files[count].path, sizeof(files[count].path),
                 "%s\\%s", SCORE_DIR, data.cFileName);
        count++;
    } while (FindNextFileA(h, &data));

    FindClose(h);

    qsort(files, (size_t)count, sizeof(files[0]), score_file_cmp);
    return count;
}

static int read_number_by_getch(int max_number) {
    char buf[16] = {0};
    int len = 0;

    while (1) {
        int key = _getch();

        if (key == 0 || key == 224) {
            if (_kbhit()) _getch();
            continue;
        }

        if (key == 27) {
            printf("\n已取消。\n");
            return 0;
        }

        if (key == '\r' || key == '\n') {
            if (len == 0) {
                printf("\n>>> (请输入序号，ESC取消)：");
                continue;
            }

            int n = atoi(buf);
            if (n >= 1 && n <= max_number) {
                printf("\n");
                return n;
            }

            printf("\n>>> (序号无效，请输入 1-%d，ESC取消)：", max_number);
            len = 0;
            buf[0] = '\0';
            continue;
        }

        if (key == 8) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                printf("\b \b");
            }
            continue;
        }

        if (isdigit((unsigned char)key) && len < (int)sizeof(buf) - 1) {
            buf[len++] = (char)key;
            buf[len] = '\0';
            putchar(key);
        }
    }
}

static bool choose_score(char* out_path, size_t out_size, char* out_name, size_t name_size) {
    ScoreFile files[MAX_SCORE_FILES];
    int count = scan_scores(files, MAX_SCORE_FILES);

    if (count <= 0) {
        printf("\n没有找到乐谱文件。\n");
        printf("请把 .txt 乐谱放到 %s 文件夹。\n", SCORE_DIR);
        return false;
    }

    printf("\n================ 乐谱列表 ================\n");
    for (int i = 0; i < count; ++i) {
        printf("[%02d] %s\n", i + 1, files[i].name);
    }

    printf("\n>>> 输入序号后回车，ESC取消：");

    int choice = read_number_by_getch(count);
    if (choice <= 0) return false;

    copy_text(out_path, out_size, files[choice - 1].path);
    copy_text(out_name, name_size, files[choice - 1].name);
    printf("\n=========================================\n");
    return true;
}

/* ========================= 乐谱解析 ========================= */

static bool file_has_begin_score(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    bool found = false;
    char line[2048];

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "BEGIN_SCORE")) {
            found = true;
            break;
        }
    }

    fclose(fp);
    return found;
}

static bool parse_bpm_line(const char* line, int* bpm_out) {
    const char* p = strstr(line, "BPM");
    if (!p) p = strstr(line, "bpm");
    if (!p) return false;

    p = strchr(p, '=');
    if (!p) return false;

    int bpm = atoi(p + 1);
    if (bpm < MIN_BPM || bpm > MAX_BPM) {
        return false;
    }

    *bpm_out = bpm;
    return true;
}

static bool append_event(App* app, NoteEvent e) {
    if (app->score_len >= MAX_EVENTS) {
        printf("乐谱太长，最多支持 %d 个事件。\n", MAX_EVENTS);
        return false;
    }

    app->score[app->score_len++] = e;
    return true;
}

static bool read_score_body(App* app, const char* path, char* raw, int raw_size) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        printf("无法打开乐谱：%s\n", path);
        return false;
    }

    bool use_marker = file_has_begin_score(path);
    bool reading = !use_marker;

    raw[0] = '\0';
    int raw_len = 0;

    char line[2048];

    while (fgets(line, sizeof(line), fp)) {
        int bpm;
        if (parse_bpm_line(line, &bpm)) {
            app->bpm = bpm;
            continue;
        }

        if (strstr(line, "BEGIN_SCORE")) {
            reading = true;
            continue;
        }

        if (strstr(line, "END_SCORE")) {
            break;
        }

        if (!reading || is_comment_line(line)) {
            continue;
        }

        int len = (int)strlen(line);
        if (raw_len + len + 2 >= raw_size) {
            fclose(fp);
            printf("乐谱文本过长。\n");
            return false;
        }

        memcpy(raw + raw_len, line, (size_t)len);
        raw_len += len;
        raw[raw_len++] = '\n';
        raw[raw_len] = '\0';
    }

    fclose(fp);
    return true;
}

static bool parse_token_event(App* app, const char* token) {
    char note = '\0';
    int denom = 4;
    int dotted = 0;

    const char* p = token;

    while (*p && !isalpha((unsigned char)*p) && !isdigit((unsigned char)*p) &&
           *p != '-' && *p != '_') {
        p++;
    }

    if (*p == '\0') return true;

    if (is_rest_char((unsigned char)*p)) {
        note = ' ';
    } else if (is_note_char((unsigned char)*p)) {
        note = (char)tolower((unsigned char)*p);
        if (note < 'a' || note > 'z') return true;
    } else {
        return true;
    }

    const char* slash = strchr(p, '/');
    if (slash) {
        denom = atoi(slash + 1);
        if (denom <= 0) denom = 4;

        if (strchr(slash + 1, '.')) {
            dotted = 1;
        }
    }

    if (denom != 1 && denom != 2 && denom != 4 &&
        denom != 8 && denom != 16 && denom != 32) {
        denom = 4;
    }

    return append_event(app, make_note(note, denom, dotted));
}

static bool parse_structured_score(App* app, const char* raw) {
    char* copy = _strdup(raw);
    if (!copy) return false;

    const char* seps = " \t\r\n,;|";
    char* ctx = NULL;
    char* token = strtok_s(copy, seps, &ctx);

    while (token) {
        /*
            单独的 . 表示极短静默。
            用它可以让自动播放的节奏更细腻，例如：
            h/16 . j/16 . l/16
            注意：h/4. 仍然表示附点四分音符，不受影响。
        */
        if (strcmp(token, ".") == 0) {
            if (!append_event(app, make_fixed(' ', DOT_REST_MS))) {
                free(copy);
                return false;
            }

            token = strtok_s(NULL, seps, &ctx);
            continue;
        }

        char* slash = strchr(token, '/');
        if (!slash) {
            /*
                为了提高可读性，BPM 乐谱也允许单独写字母。
                单独字母默认按十六分音符处理，适合快速连续按键效果。
            */
            if (strlen(token) == 1 && isalpha((unsigned char)token[0])) {
                char note = (char)tolower((unsigned char)token[0]);
                if (!append_event(app, make_note(note, 16, 0))) {
                    free(copy);
                    return false;
                }

                token = strtok_s(NULL, seps, &ctx);
                continue;
            }

            token = strtok_s(NULL, seps, &ctx);
            continue;
        }

        *slash = '\0';

        if (strlen(token) != 1) {
            token = strtok_s(NULL, seps, &ctx);
            continue;
        }

        char note_char = token[0];
        char note;

        if (is_rest_char(note_char)) {
            note = ' ';
        } else if (isalpha((unsigned char)note_char)) {
            note = (char)tolower((unsigned char)note_char);
        } else {
            token = strtok_s(NULL, seps, &ctx);
            continue;
        }

        char* dur = slash + 1;
        int dotted = 0;
        char* dot = strchr(dur, '.');

        if (dot) {
            dotted = 1;
            *dot = '\0';
        }

        int denom = atoi(dur);
        if (denom <= 0) denom = 4;

        if (!append_event(app, make_note(note, denom, dotted))) {
            free(copy);
            return false;
        }

        token = strtok_s(NULL, seps, &ctx);
    }

    free(copy);
    return true;
}

static bool load_score(App* app, const char* path) {
    static char raw[RAW_SCORE_SIZE];

    app->score_len = 0;
    app->score_pos = 0;
    app->score_finishing = false;
    app->score_finish_until = 0;
    copy_text(app->score_file, sizeof(app->score_file), path);

    if (!read_score_body(app, path, raw, sizeof(raw))) {
        return false;
    }

    /*
        只支持 BPM 乐谱：
        - 推荐写法：h/4、h/8、0/4、.
        - 单独字母也允许，默认按十六分音符处理。
        - 换行只排版，不影响音乐。
    */
    if (!parse_structured_score(app, raw)) {
        return false;
    }

    if (app->score_len <= 0) {
        printf("乐谱为空，或没有可播放内容。\n");
        return false;
    }

    return true;
}

/* ========================= 显示 ========================= */

static const char* duration_symbol(NoteEvent e) {
    if (e.fixed_ms > 0) {
        return "";   // 乐谱只显示字母和静默点，不显示 50ms
    }

    switch (e.denom) {
    case 1:  return e.dotted ? "𝅝." : "𝅝";   // 全音符
    case 2:  return e.dotted ? "𝅗𝅥." : "𝅗𝅥";   // 二分音符
    case 4:  return e.dotted ? "♩."  : "♩";   // 四分音符
    case 8:  return e.dotted ? "♪."  : "♪";   // 八分音符
    case 16: return e.dotted ? "♬."  : "♬";   // 十六分音符
    case 32: return e.dotted ? "♬."  : "♬";
    default: return e.dotted ? "♩."  : "♩";
    }
}

static void print_structured_score_view(const App* app) {
    /*
        BPM乐谱可视化：
        H♩ = H 音四分音符
        H♪ = H 音八分音符
        H𝅗𝅥 = H 音二分音符
        0♩ = 四分休止
        .  = 极短静默

        展示时不在音符之间额外插入空格。
    */
    const int per_line = 24;

    for (int i = 0; i < app->score_len; ++i) {
        NoteEvent e = app->score[i];

        if (e.note == ' ' && e.fixed_ms == DOT_REST_MS) {
            printf(".");
        } else {
            char note = e.note == ' ' ? '0' : (char)toupper((unsigned char)e.note);
            printf("%c%s", note, duration_symbol(e));
        }

        if ((i + 1) % per_line == 0) {
            putchar('\n');
        }
    }

    if (app->score_len % per_line != 0) {
        putchar('\n');
    }
}

static void print_score(const App* app) {
    printf("\n================ 当前乐谱 ================\n");
    printf("文件：%s\n", app->score_file);
    printf("BPM：%d\n", app->bpm);
    printf("事件数：%d\n", app->score_len);
    printf("格式：BPM 音符/时值\n");
    printf("显示：字母=音符，0=休止，.=极短静默，♩=四分，♪=八分，𝅗𝅥=二分，𝅝=全音符\n\n");

    print_structured_score_view(app);

    printf("==========================================\n");
}


static void print_bpm_manual_event(NoteEvent e) {
    char note = e.note == ' ' ? '0' : (char)toupper((unsigned char)e.note);
    printf("%c%s", note, duration_symbol(e));
}

static void print_current_bpm_manual_setting(const App* app) {
    NoteEvent preview = make_note('h', app->manual_denom, app->manual_dotted);
    printf("\n当前时值：%s  BPM=%d\n", duration_symbol(preview), app->bpm);
    print_prompt(app);
}

static void print_bpm_manual_intro(const App* app) {
    printf("\n-- BPM INPUT --\n");
    printf("字母=切换音符并播放；0/空格=休止；1/2/4/8/6=切换时值；ESC=返回\n");
    printf("符号：0=休止，𝅝=全音符，𝅗𝅥=二分，♩=四分，♪=八分，♬=十六分\n");
    print_current_bpm_manual_setting(app);
}

static void cycle_manual_denom(App* app) {
    switch (app->manual_denom) {
    case 1:  app->manual_denom = 2; break;
    case 2:  app->manual_denom = 4; break;
    case 4:  app->manual_denom = 8; break;
    case 8:  app->manual_denom = 16; break;
    case 16: app->manual_denom = 1; break;
    default: app->manual_denom = 4; break;
    }
}

static void show_help(void) {
    printf("\n================ 帮助 ================\n");
    printf("NORMAL 普通模式：\n");
    printf("  i       进入BPM手动输入模式，按键立即响，并按BPM乐谱时值显示\n");
    printf("  s       选择 scores 文件夹里的乐谱并自动播放\n");
    printf("  p       暂停 / 继续\n");
    printf("  x       停止播放\n");
    printf("  + / -   加快 / 放慢 BPM\n");
    printf("  h       显示帮助\n");
    printf("  q       退出\n\n");

    printf("SCORE 自动播放模式：\n");
    printf("  s       继续选择乐谱\n");
    printf("  p       暂停 / 继续\n");
    printf("  + / -   调整 BPM\n");
    printf("  ESC     返回普通模式\n\n");

    printf("BPM 手动输入模式：\n");
    printf("  字母      切换音符并播放\n");
    printf("  0/空格    休止\n");
    printf("  1/2/4/8/6 切换时值：𝅝/𝅗𝅥/♩/♪/♬\n");
        printf("  Tab       循环切换时值\n");
    printf("  + / -     调整 BPM\n");
    printf("  ESC       返回普通模式\n\n");

    printf("BPM 乐谱格式：\n");
    printf("  休止符使用 0/4、-/4 或 _/4；乐谱中单独的 . 表示 3ms 极短静默，可细分音符间隔；手动输入不识别 .。\n");
    printf("  BPM=100\n");
    printf("  BEGIN_SCORE\n");
    printf("  h/16 . j/16 . l/16 . m/16\n");
    printf("  0/4 k/8 j/8 i/4 h/2\n");
    printf("  END_SCORE\n\n");

    printf("新钢琴式映射：\n");
    printf("  低音1-7 = a b c d e f g\n");
    printf("  中音1-7 = h i j k l m n\n");
    printf("  高音1-7 = o p q r s t u\n");
    printf("  倍高音1-5 = v w x y z\n");
    printf("======================================\n");
}

static void print_banner(void) {
    printf("\n");
    printf("  .   ____        _          ____   _                         \n");
    printf(" /\\\\ / ___'_ __  (_)  ___   |  _ \\ (_)  __ _  _ __    ___     \n");
    printf("( ( )\\___ | '_ \\ | | / _ \\  | |_) || | / _` || '_ \\  / _ \\    \n");
    printf(" \\\\/  ___)| | | || || (_) | |  __/ | || (_| || | | || (_) |   \n");
    printf("  '  |____|_| |_||_| \\___/  |_|    |_| \\__,_||_| |_| \\___/    \n");
    printf("==============================================================\n");
    printf(" :: Vito Character Audio Player                               \n");
    printf(" :: score rhythm: BPM + note duration                         \n");
    printf(" :: channels=16                                               \n");
    printf(" :: score: scores\\*.txt                                     \n");
    printf(" :: author: yiming                                           \n");
    printf("==============================================================\n");

    printf("Vito字符音源播放器已启动\n");
    printf("-- NORMAL 普通模式 --\n");
    printf("  [1] i    弹奏模式\n");
    printf("  [2] s    乐谱播放\n");
    printf("  [3] h    查看帮助\n");
    printf("  [4] q    退出程序\n\n");
}

/* ========================= 模式控制 ========================= */

static void enter_normal(App* app) {
    app->mode = MODE_NORMAL;
    app->paused = false;
    app->score_finishing = false;
    app->score_finish_until = 0;
    printf("\n-- NORMAL --\n");
    print_prompt(app);
}


static void enter_score_select(App* app) {
    app->mode = MODE_SCORE;
    app->paused = true;
    app->score_pos = 0;
    app->score_len = 0;
    app->score_finishing = false;
    app->score_finish_until = 0;

    printf("\n-- SCORE --\n");
    printf("按 s 继续选择乐谱，ESC 返回普通模式。\n");
    print_prompt(app);
}

static void stop_playback(App* app) {
    app->score_pos = 0;
    app->score_len = 0;
    app->paused = false;
    app->mode = MODE_NORMAL;
    app->score_finishing = false;
    app->score_finish_until = 0;

    printf("\n-- NORMAL --\n");
    printf("已停止播放。\n");
    print_prompt(app);
}

static void change_bpm(App* app, int delta) {
    app->bpm = clamp_int(app->bpm + delta, MIN_BPM, MAX_BPM);
    printf("\nBPM：%d\n", app->bpm);
    print_prompt(app);
}

/* 返回 true 表示继续运行，false 表示退出 */
static bool handle_key(App* app, int key, ULONGLONG* next_tick) {
    if (key == 0 || key == 224) {
        if (_kbhit()) _getch();
        return true;
    }

    if (app->mode == MODE_BPM_INSERT) {
        /*
            BPM 手动模式原则：
            a-z 全部作为音符使用，不能再占用 h/p/q/x 等字母做命令。
            需要帮助、退出、停止时，先按 ESC 回 NORMAL，再按 h/q/x。
        */
        if (key == 27) {
            enter_normal(app);
            return true;
        }

        if (key == '+') {
            change_bpm(app, 5);
            return true;
        }

        if (key == '-') {
            change_bpm(app, -5);
            return true;
        }

        if (key == '\t') {
            cycle_manual_denom(app);
            print_current_bpm_manual_setting(app);
            return true;
        }

        if (key == '1' || key == '2' || key == '4' || key == '8' || key == '6') {
            if (key == '6') app->manual_denom = 16;
            else app->manual_denom = key - '0';

            print_current_bpm_manual_setting(app);
            return true;
        }

        if (key == ' ' || key == '0') {
            NoteEvent e = make_rest(app->manual_denom, app->manual_dotted);
            print_bpm_manual_event(e);
            return true;
        }

        if (is_note_char(key)) {
            char note = (char)tolower((unsigned char)key);

            if (note >= 'a' && note <= 'z') {
                NoteEvent e = make_note(note, app->manual_denom, app->manual_dotted);
                trigger_timed_event(app, e);   // 立即响，同时按当前时值自动停止
                print_bpm_manual_event(e);
            }

            return true;
        }

        return true;
    }

    switch (key) {
    case 'q':
    case 'Q':
        return false;

    case 'h':
    case 'H':
        show_help();
        print_prompt(app);
        return true;

    case 27:
        enter_normal(app);
        return true;

    case 'p':
    case 'P':
        if (app->mode == MODE_SCORE && app->score_len > 0 && !app->score_finishing) {
            app->paused = !app->paused;

            /*
                重复按 p：
                第一次暂停；再次按 p 继续。
                继续时从当前时刻重新启动下一个节拍，避免卡在旧 tick。
            */
            *next_tick = GetTickCount64();

            printf(app->paused ? "\n已暂停。\n" : "\n继续播放。\n");
            print_prompt(app);
        } else {
            printf("\n当前没有正在播放的乐谱，按 s 选择乐谱。\n");
            print_prompt(app);
        }
        return true;

    case 'x':
    case 'X':
        stop_playback(app);
        return true;

    case '+':
        change_bpm(app, 5);
        return true;

    case '-':
        change_bpm(app, -5);
        return true;
    }

    if (app->mode == MODE_NORMAL) {
        if (key == 'i' || key == 'I') {
            app->mode = MODE_BPM_INSERT;
            app->paused = false;
            app->manual_denom = 1;
            app->manual_dotted = 0;
            print_bpm_manual_intro(app);
            return true;
        }
    }

    if (app->mode == MODE_NORMAL || app->mode == MODE_SCORE) {
        if (key == 's' || key == 'S') {
            char path[PATH_SIZE];
            char name[PATH_SIZE];

            if (choose_score(path, sizeof(path), name, sizeof(name)) &&
                load_score(app, path)) {
                print_score(app);
                app->mode = MODE_SCORE;
                app->paused = false;
                app->score_pos = 0;
                app->score_finishing = false;
                app->score_finish_until = 0;
                *next_tick = GetTickCount64();
                printf("-- SCORE -- 正在播放：%s\n", name);
                print_prompt(app);
            } else {
                enter_score_select(app);
            }

            return true;
        }
    }

    return true;
}

/* ========================= 播放循环 ========================= */

static bool next_event(App* app, NoteEvent* out) {
    if (app->mode != MODE_SCORE) {
        return false;
    }

    if (app->score_pos < app->score_len) {
        *out = app->score[app->score_pos++];
        return true;
    }

    /*
        乐谱事件已经播放完，但钢琴音源还有余音。
        不要立刻回普通模式，先等一段尾音。
    */
    if (!app->score_finishing) {
        app->score_finishing = true;
        app->score_finish_until = GetTickCount64() + SCORE_FINISH_TAIL_MS;
        return false;
    }

    if (GetTickCount64() >= app->score_finish_until) {
        printf("\n乐谱播放完成。\n");
        enter_score_select(app);
        return false;
    }

    return false;
}

static void process_playback(App* app, ULONGLONG* next_tick) {
    if (app->paused) {
        *next_tick = GetTickCount64();
        return;
    }

    ULONGLONG now = GetTickCount64();
    if (now < *next_tick) return;

    NoteEvent e;
    if (next_event(app, &e)) {
        trigger_timed_event(app, e);  // 自动和手动共用同一个“触发 + 按时值停止”逻辑
        *next_tick = now + (ULONGLONG)auto_step_duration_ms(app, e);
    } else {
        *next_tick = now + 20;
    }
}

/* ========================= 主函数 ========================= */

int main(void) {
    static App app;
    memset(&app, 0, sizeof(app));
    app.mode = MODE_NORMAL;
    app.bpm = DEFAULT_BPM;
    app.manual_denom = 1;
    app.manual_dotted = 0;

    ULONGLONG next_tick = GetTickCount64();

    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleA("piano demo");

    system("color f4");

    print_banner();
    load_sounds();
    print_prompt(&app);

    while (1) {
        while (_kbhit()) {
            if (!handle_key(&app, _getch(), &next_tick)) {
                close_sounds();
                printf("\n程序已退出。\n");
                return 0;
            }
        }

        process_playback(&app, &next_tick);
        process_note_offs();
        Sleep(1);
    }
}
