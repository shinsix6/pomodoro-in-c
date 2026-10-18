/*
 * Pomodoro Timer TUI
 * A simple, beautiful, and effective Pomodoro terminal timer written in C.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define DEFAULT_WORK_MIN        25.0
#define DEFAULT_BREAK_MIN        5.0
#define DEFAULT_LONG_BREAK_MIN  15.0
#define DEFAULT_CYCLES           4
#define DEFAULT_SOUND_PATH      "sounds/bell.wav"
#define FALLBACK_SOUND_PATH     "bell.wav"

/* Fixed box geometry (outer width = BOX_INNER + 2) */
#define BOX_INNER 64
#define BOX_MIN_INNER 34

/* ANSI Colors and Styling */
#define ANSI_RESET        "\033[0m"
#define ANSI_BOLD         "\033[1m"
#define ANSI_DIM          "\033[2m"

#define ANSI_FG_RED       "\033[1;31m"
#define ANSI_FG_GREEN     "\033[1;32m"
#define ANSI_FG_YELLOW    "\033[1;33m"
#define ANSI_FG_CYAN      "\033[1;36m"
#define ANSI_FG_WHITE     "\033[1;37m"
#define ANSI_FG_GRAY      "\033[90m"

#define ANSI_BG_RED       "\033[41m"
#define ANSI_BG_GREEN     "\033[42m"
#define ANSI_BG_YELLOW    "\033[43m"
#define ANSI_BG_CYAN      "\033[46m"

/* Rounded box drawing characters */
#define BOX_TL "\u256D"
#define BOX_TR "\u256E"
#define BOX_BL "\u2570"
#define BOX_BR "\u256F"
#define BOX_H  "\u2500"
#define BOX_V  "\u2502"
#define BOX_LT "\u251C"
#define BOX_RT "\u2524"

typedef enum {
    PHASE_WORK,
    PHASE_SHORT_BREAK,
    PHASE_LONG_BREAK
} Phase;

typedef struct {
    int work_sec;
    int break_sec;
    int long_break_sec;
    int cycles_before_long;
    char sound_path[512];
    char player_cmd[128];
    bool mute;
} Config;

typedef struct {
    Phase phase;
    int remaining_sec;
    int total_sec;
    int current_cycle;
    int completed_sessions;
    bool is_paused;
    bool time_up_alert;
    int alert_countdown;
} State;

/* Global state for cleanup and signal handling */
static struct termios orig_termios;
static bool raw_mode_active = false;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_win_resized = 0;

/* Big ASCII digits for 0-9 and colon (5 rows high, 4 columns wide) */
static const char *DIGITS[11][5] = {
    {"\u2588\u2588\u2588\u2588", "\u2588  \u2588", "\u2588  \u2588", "\u2588  \u2588", "\u2588\u2588\u2588\u2588"}, /* 0 */
    {"  \u2588\u2588",     "  \u2588\u2588",     "  \u2588\u2588",     "  \u2588\u2588",     "  \u2588\u2588"},     /* 1 */
    {"\u2588\u2588\u2588\u2588", "   \u2588",  "\u2588\u2588\u2588\u2588", "\u2588   ", "\u2588\u2588\u2588\u2588"}, /* 2 */
    {"\u2588\u2588\u2588\u2588", "   \u2588",  "\u2588\u2588\u2588\u2588", "   \u2588",  "\u2588\u2588\u2588\u2588"}, /* 3 */
    {"\u2588  \u2588",       "\u2588  \u2588",       "\u2588\u2588\u2588\u2588", "   \u2588",     "   \u2588"},           /* 4 */
    {"\u2588\u2588\u2588\u2588", "\u2588   ",  "\u2588\u2588\u2588\u2588", "   \u2588",  "\u2588\u2588\u2588\u2588"}, /* 5 */
    {"\u2588\u2588\u2588\u2588", "\u2588   ",  "\u2588\u2588\u2588\u2588", "\u2588  \u2588", "\u2588\u2588\u2588\u2588"}, /* 6 */
    {"\u2588\u2588\u2588\u2588", "   \u2588",     "   \u2588",     "   \u2588",     "   \u2588"},           /* 7 */
    {"\u2588\u2588\u2588\u2588", "\u2588  \u2588", "\u2588\u2588\u2588\u2588", "\u2588  \u2588", "\u2588\u2588\u2588\u2588"}, /* 8 */
    {"\u2588\u2588\u2588\u2588", "\u2588  \u2588", "\u2588\u2588\u2588\u2588", "   \u2588",  "\u2588\u2588\u2588\u2588"}, /* 9 */
    {"  ",                   "\u2588\u2588",     "  ",                   "\u2588\u2588",     "  "}                    /* : */
};

/* Progress bar glyphs (single-cell width) */
#define BAR_FILLED   "\u2588"
#define BAR_UNFILLED "\u2591"

/* Session tracker glyphs (single-cell width) */
#define DOT_DONE   "\u25CF"
#define DOT_ACTIVE "\u25BA"
#define DOT_PENDING "\u25CB"

/* ------------------------------------------------------------------ */
/* UTF-8 aware text measurement                                         */
/* ------------------------------------------------------------------ */

static int utf8_seq_len(const unsigned char *p) {
    if (p[0] < 0x80) return 1;
    if ((p[0] & 0xE0) == 0xC0) return 2;
    if ((p[0] & 0xF0) == 0xE0) return 3;
    if ((p[0] & 0xF8) == 0xF0) return 4;
    return 1;
}

static unsigned int utf8_decode(const unsigned char *p, int len) {
    switch (len) {
        case 1: return p[0];
        case 2: return (unsigned int)((p[0] & 0x1F) << 6) | (unsigned int)(p[1] & 0x3F);
        case 3: return (unsigned int)((p[0] & 0x0F) << 12) |
                       (unsigned int)((p[1] & 0x3F) << 6) | (unsigned int)(p[2] & 0x3F);
        case 4: return (unsigned int)((p[0] & 0x07) << 18) |
                       (unsigned int)((p[1] & 0x3F) << 12) |
                       (unsigned int)((p[2] & 0x3F) << 6) | (unsigned int)(p[3] & 0x3F);
    }
    return p[0];
}

/* Display columns occupied by a single code point */
static int codepoint_width(unsigned int cp) {
    if (cp == 0) return 0;
    /* Control characters take no space */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    /* Combining marks take no space */
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) return 0;
    /* Wide / fullwidth ranges */
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F5FF) ||
        (cp >= 0x1F600 && cp <= 0x1F64F) ||
        (cp >= 0x1F680 && cp <= 0x1F6FF) ||
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) return 2;
    return 1;
}

/*
 * Visible display width of a UTF-8 string, ignoring ANSI CSI escape
 * sequences (which occupy no columns).
 */
static int display_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;

    while (*p) {
        if (p[0] == 0x1B && p[1] == '[') {
            /* Skip CSI: ESC '[' params(0x30-0x3F) intermediates(0x20-0x2F) final(0x40-0x7E) */
            p += 2;
            while (*p && !(*p >= 0x40 && *p <= 0x7E)) p++;
            if (*p) p++;
            continue;
        }
        int len = utf8_seq_len(p);
        w += codepoint_width(utf8_decode(p, len));
        p += len;
    }
    return w;
}

/*
 * Copy `src` into `dst` so its visible width never exceeds `max_w`.
 * Escape sequences are always copied through; visible characters are
 * cut on UTF-8 boundaries. Guarantees the right border can never shift.
 */
static void fit_width(char *dst, size_t dst_sz, const char *src, int max_w) {
    size_t o = 0;
    int w = 0;
    const unsigned char *p = (const unsigned char *)src;
    if (!src || max_w <= 0) {
        if (dst_sz) dst[0] = '\0';
        return;
    }

    while (*p && o + 1 < dst_sz) {
        if (p[0] == 0x1B && p[1] == '[') {
            const unsigned char *start = p;
            p += 2;
            while (*p && !(*p >= 0x40 && *p <= 0x7E)) p++;
            if (*p) p++;
            size_t n = (size_t)(p - start);
            if (o + n + 1 >= dst_sz) break;
            memcpy(dst + o, start, n);
            o += n;
            continue;
        }
        int len = utf8_seq_len(p);
        int cw = codepoint_width(utf8_decode(p, len));
        if (w + cw > max_w) break;
        if (o + (size_t)len + 1 > dst_sz) break;
        memcpy(dst + o, p, (size_t)len);
        o += (size_t)len;
        w += cw;
        p += len;
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Box drawing                                                          */
/* ------------------------------------------------------------------ */

/* Horizontal rule with rounded/square ends: used for top/bottom/divider */
static void draw_hrule(int margin_x, int inner, const char *color,
                       const char *left, const char *right) {
    printf("\033[2K%*s%s%s", margin_x, "", color, left);
    for (int i = 0; i < inner; i++) fputs(BOX_H, stdout);
    printf("%s%s\n", right, ANSI_RESET);
}

/* One content row: border, padded content, border. Width is measured, not guessed. */
static void draw_box_row(int margin_x, int inner, const char *border_color,
                         const char *text, int align) {
    char line[512];

    /* LEFT rows keep a 2-column indent; CENTER rows use the full inner width */
    int lead = (align == 1) ? 2 : 0;
    if (lead > inner) lead = inner;

    /* Hard clamp: content can never consume more than inner - lead columns,
       so the right border can never shift, whatever the content is. */
    fit_width(line, sizeof(line), text ? text : "", inner - lead);
    int w = display_width(line);

    int pad_left = (align == 1) ? lead : (inner - w) / 2;
    if (pad_left < 0) pad_left = 0;
    int pad_right = inner - pad_left - w;
    if (pad_right < 0) pad_right = 0;

    printf("\033[2K%*s%s%s%s", margin_x, "", border_color, BOX_V, ANSI_RESET);
    for (int i = 0; i < pad_left; i++) putchar(' ');
    fputs(line, stdout);
    for (int i = 0; i < pad_right; i++) putchar(' ');
    printf("%s%s%s\n", border_color, BOX_V, ANSI_RESET);
}

/* Empty row inside the box */
static void draw_box_blank(int margin_x, int inner, const char *border_color) {
    printf("\033[2K%*s%s%s", margin_x, "", border_color, BOX_V);
    for (int i = 0; i < inner; i++) putchar(' ');
    printf("%s%s%s\n", border_color, BOX_V, ANSI_RESET);
}

/* ------------------------------------------------------------------ */
/* Sound playback                                                       */
/* ------------------------------------------------------------------ */

/* Check if an executable is present in PATH */
static bool command_exists(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return false;
    if (strchr(cmd, '/')) {
        return access(cmd, X_OK) == 0;
    }
    const char *path_env = getenv("PATH");
    if (!path_env) return false;

    char path_copy[2048];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        if (access(full_path, X_OK) == 0) return true;
        dir = strtok(NULL, ":");
    }
    return false;
}

/* Detect the best available audio player */
static void detect_audio_player(const Config *cfg, char *out_player, size_t out_len) {
    out_player[0] = '\0';

    if (cfg->player_cmd[0] != '\0') {
        snprintf(out_player, out_len, "%s", cfg->player_cmd);
        return;
    }

    const char *env_player = getenv("POMODORO_AUDIO_PLAYER");
    if (env_player && env_player[0] != '\0' && command_exists(env_player)) {
        snprintf(out_player, out_len, "%s", env_player);
        return;
    }

    const char *candidates[] = {
        "pw-play",           /* Modern Linux (PipeWire) */
        "paplay",            /* Linux (PulseAudio) */
        "aplay",             /* Linux (ALSA) */
        "mpv",               /* Cross-platform universal player */
        "ffplay",            /* Cross-platform ffmpeg player */
        "canberra-gtk-play", /* Linux desktop notification player */
        "afplay",            /* macOS */
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        if (command_exists(candidates[i])) {
            snprintf(out_player, out_len, "%s", candidates[i]);
            return;
        }
    }
}

/* Play the configured sound file (non-blocking) */
static void play_sound(const Config *cfg) {
    if (cfg->mute) return;

    /* Always emit the terminal bell as a guaranteed fallback */
    printf("\a");
    fflush(stdout);

    if (cfg->sound_path[0] == '\0') return;
    if (access(cfg->sound_path, R_OK) != 0) return;

    char player[128] = {0};
    detect_audio_player(cfg, player, sizeof(player));
    if (player[0] == '\0') return;

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        if (strcmp(player, "mpv") == 0) {
            execlp("mpv", "mpv", "--no-terminal", "--really-quiet", cfg->sound_path, (char *)NULL);
        } else if (strcmp(player, "ffplay") == 0) {
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
                   cfg->sound_path, (char *)NULL);
        } else if (strcmp(player, "aplay") == 0) {
            execlp("aplay", "aplay", "-q", cfg->sound_path, (char *)NULL);
        } else if (strcmp(player, "canberra-gtk-play") == 0) {
            execlp("canberra-gtk-play", "canberra-gtk-play", "-f", cfg->sound_path, (char *)NULL);
        } else {
            execlp(player, player, cfg->sound_path, (char *)NULL);
        }
        _exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* Terminal handling                                                    */
/* ------------------------------------------------------------------ */

static void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGWINCH) {
        g_win_resized = 1;
    }
}

static void disable_raw_mode(void) {
    if (!raw_mode_active) return;
    printf("\033[?25h");
    printf("\033[?1049l");
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_mode_active = false;
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return;
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = true;

    printf("\033[?1049h\033[?25l\033[2J\033[H");
    fflush(stdout);
}

static void get_terminal_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                            */
/* ------------------------------------------------------------------ */

static void render_tui(const Config *cfg, const State *state, int term_cols, int term_rows) {
    printf("\033[H");

    /* --- Fixed box geometry with a margin from the terminal edge --- */
    const int OUTER = BOX_INNER + 2;
    int margin_x = (term_cols - OUTER) / 2;
    if (margin_x < 1) margin_x = 1;

    /* Shrink only if the terminal is too narrow (never grow past fixed size) */
    int inner = BOX_INNER;
    int avail = term_cols - 2 * margin_x - 2;
    if (inner > avail) inner = avail;
    if (inner < BOX_MIN_INNER) inner = (term_cols - 4 < BOX_MIN_INNER)
                                        ? (term_cols - 4 > 8 ? term_cols - 4 : 8)
                                        : BOX_MIN_INNER;

    /* --- Phase theme --- */
    const char *accent = ANSI_FG_RED;
    const char *phase_name = "FOCUS / WORK";
    const char *badge_bg = ANSI_BG_RED;

    if (state->phase == PHASE_SHORT_BREAK) {
        accent = ANSI_FG_GREEN;
        phase_name = "SHORT BREAK";
        badge_bg = ANSI_BG_GREEN;
    } else if (state->phase == PHASE_LONG_BREAK) {
        accent = ANSI_FG_CYAN;
        phase_name = "LONG BREAK";
        badge_bg = ANSI_BG_CYAN;
    }

    /* --- Vertical fit: fixed rows + evenly distributed spare rows --- */
    const int FIXED_ROWS = 14;   /* rows that always exist (excluding box edges) */
    const int PAD_SLOTS = 4;
    int margin_y = (term_rows - (FIXED_ROWS + 2) - 2) >= 0 ? 1 : 0;
    int spare = term_rows - 2 * margin_y - 2 - FIXED_ROWS;
    if (spare < 0) spare = 0;

    int pad[PAD_SLOTS];
    int base = spare / PAD_SLOTS;
    int rem = spare % PAD_SLOTS;
    for (int i = 0; i < PAD_SLOTS; i++) pad[i] = base + (i < rem ? 1 : 0);

    /* Top margin */
    for (int i = 0; i < margin_y; i++) printf("\033[2K\n");

    /* --- Box top (rounded) --- */
    draw_hrule(margin_x, inner, accent, BOX_TL, BOX_TR);

    /* Title */
    draw_box_row(margin_x, inner, accent,
                 ANSI_BOLD ANSI_FG_WHITE "POMODORO" ANSI_RESET, 0);

    /* Divider */
    draw_hrule(margin_x, inner, accent, BOX_LT, BOX_RT);

    /* Spare row slot 1 */
    for (int i = 0; i < pad[0]; i++) draw_box_blank(margin_x, inner, accent);

    /* Phase badge */
    char badge[256];
    if (state->time_up_alert) {
        snprintf(badge, sizeof(badge), "%s%s%s%s%s%s  %s%s",
                 ANSI_BOLD, ANSI_BG_YELLOW, ANSI_FG_RED, ANSI_BOLD,
                 ANSI_BG_YELLOW, ANSI_FG_RED, "TIME'S UP", ANSI_RESET);
    } else if (state->is_paused) {
        snprintf(badge, sizeof(badge), "%s%s  %s PAUSED  %s",
                 ANSI_BOLD, badge_bg, phase_name, ANSI_RESET);
    } else {
        snprintf(badge, sizeof(badge), "%s%s  %s  %s",
                 ANSI_BOLD, badge_bg, phase_name, ANSI_RESET);
    }
    draw_box_row(margin_x, inner, accent, badge, 0);

    /* Spare row slot 2 */
    for (int i = 0; i < pad[1]; i++) draw_box_blank(margin_x, inner, accent);

    /* --- Big ASCII clock (5 rows) --- */
    int display_sec = state->remaining_sec > 0 ? state->remaining_sec : 0;
    int mins = display_sec / 60;
    int secs = display_sec % 60;
    if (mins > 99) mins = 99;

    int d[5];
    d[0] = (mins / 10) % 10;
    d[1] = mins % 10;
    d[2] = 10; /* colon */
    d[3] = (secs / 10) % 10;
    d[4] = secs % 10;

    bool blink_colon = state->is_paused && (state->alert_countdown % 2 == 0);

    for (int r = 0; r < 5; r++) {
        char clock[256];
        int o = snprintf(clock, sizeof(clock), "%s", accent);
        for (int i = 0; i < 5; i++) {
            if (i == 2 && blink_colon) {
                o += snprintf(clock + o, sizeof(clock) - (size_t)o, "    ");
            } else {
                o += snprintf(clock + o, sizeof(clock) - (size_t)o, "%s  ", DIGITS[d[i]][r]);
            }
        }
        snprintf(clock + o, sizeof(clock) - (size_t)o, "%s", ANSI_RESET);
        draw_box_row(margin_x, inner, accent, clock, 0);
    }

    /* Spare row slot 3 */
    for (int i = 0; i < pad[2]; i++) draw_box_blank(margin_x, inner, accent);

    /* --- Progress bar --- */
    int elapsed = state->total_sec - state->remaining_sec;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > state->total_sec) elapsed = state->total_sec;

    int percent = state->total_sec > 0 ? (elapsed * 100 / state->total_sec) : 100;
    if (percent > 100) percent = 100;

    /* Bar fills the available width, leaving room for [ ] and " nn%" */
    int bar_cells = inner - 10;
    if (bar_cells < 4) bar_cells = 4;

    int filled = state->total_sec > 0 ? (elapsed * bar_cells / state->total_sec) : bar_cells;
    if (filled > bar_cells) filled = bar_cells;
    if (filled < 0) filled = 0;

    char bar[512];
    int o = snprintf(bar, sizeof(bar), "%s[", accent);
    for (int i = 0; i < filled; i++)
        o += snprintf(bar + o, sizeof(bar) - (size_t)o, "%s", BAR_FILLED);
    o += snprintf(bar + o, sizeof(bar) - (size_t)o, "%s", ANSI_FG_GRAY);
    for (int i = 0; i < bar_cells - filled; i++)
        o += snprintf(bar + o, sizeof(bar) - (size_t)o, "%s", BAR_UNFILLED);
    snprintf(bar + o, sizeof(bar) - (size_t)o, "%s]%s %3d%%%s",
             accent, ANSI_BOLD ANSI_FG_WHITE, percent, ANSI_RESET);
    draw_box_row(margin_x, inner, accent, bar, 0);

    /* --- Time breakdown --- */
    char timeinfo[256];
    snprintf(timeinfo, sizeof(timeinfo),
             "%sElapsed%s %02d:%02d   %sRemaining%s %02d:%02d   %sTotal%s %02d:%02d%s",
             ANSI_DIM, ANSI_RESET, elapsed / 60, elapsed % 60,
             ANSI_DIM, ANSI_RESET, mins, secs,
             ANSI_DIM, ANSI_RESET, state->total_sec / 60, state->total_sec % 60, ANSI_RESET);
    draw_box_row(margin_x, inner, accent, timeinfo, 0);

    /* --- Session tracker --- */
    char tracker[512];
    o = snprintf(tracker, sizeof(tracker), "%sRound%s ", ANSI_DIM, ANSI_RESET);
    int shown = cfg->cycles_before_long > 12 ? 12 : cfg->cycles_before_long;
    for (int i = 1; i <= shown; i++) {
        if (i < state->current_cycle)
            o += snprintf(tracker + o, sizeof(tracker) - (size_t)o, "%s ", DOT_DONE);
        else if (i == state->current_cycle)
            o += snprintf(tracker + o, sizeof(tracker) - (size_t)o, "%s%s ",
                          i == state->current_cycle ? ANSI_BOLD : ANSI_DIM, DOT_ACTIVE);
        else
            o += snprintf(tracker + o, sizeof(tracker) - (size_t)o, "%s ", DOT_PENDING);
    }
    o += snprintf(tracker + o, sizeof(tracker) - (size_t)o, " %s%d/%d%s",
                  ANSI_DIM, state->current_cycle, cfg->cycles_before_long, ANSI_RESET);
    o += snprintf(tracker + o, sizeof(tracker) - (size_t)o,
                  "   %sSessions%s %s%d%s",
                  ANSI_DIM, ANSI_RESET, ANSI_BOLD ANSI_FG_WHITE,
                  state->completed_sessions, ANSI_RESET);
    draw_box_row(margin_x, inner, accent, tracker, 0);

    /* Spare row slot 4 */
    for (int i = 0; i < pad[3]; i++) draw_box_blank(margin_x, inner, accent);

    /* Divider */
    draw_hrule(margin_x, inner, accent, BOX_LT, BOX_RT);

    /* --- Sound status --- */
    char player[128] = {0};
    detect_audio_player(cfg, player, sizeof(player));
    const char *player_name = player[0] ? player : "terminal bell";

    char soundinfo[1024];
    if (cfg->mute) {
        snprintf(soundinfo, sizeof(soundinfo), "Sound  %sMUTED%s", ANSI_FG_YELLOW, ANSI_RESET);
    } else if (cfg->sound_path[0] == '\0') {
        snprintf(soundinfo, sizeof(soundinfo), "Sound  %s%s%s",
                 ANSI_DIM, player_name, ANSI_RESET);
    } else if (access(cfg->sound_path, R_OK) != 0) {
        snprintf(soundinfo, sizeof(soundinfo),
                 "Sound  %s%s%s %s(not found)%s",
                 ANSI_FG_YELLOW, cfg->sound_path, ANSI_RESET, ANSI_DIM, ANSI_RESET);
    } else {
        snprintf(soundinfo, sizeof(soundinfo),
                 "Sound  %s%s%s %s(%s)%s",
                 ANSI_FG_CYAN, cfg->sound_path, ANSI_RESET, ANSI_DIM, player_name, ANSI_RESET);
    }
    draw_box_row(margin_x, inner, accent, soundinfo, 1);

    /* --- Key hints --- */
    char keys[512];
    snprintf(keys, sizeof(keys),
             "%sSpace%s %s  %sn%s next  %sr%s reset  %st%s sound  %sm%s mute  %sq%s quit",
             ANSI_BOLD ANSI_FG_WHITE, ANSI_RESET, state->is_paused ? "resume" : "pause",
             ANSI_BOLD ANSI_FG_WHITE, ANSI_RESET,
             ANSI_BOLD ANSI_FG_WHITE, ANSI_RESET,
             ANSI_BOLD ANSI_FG_WHITE, ANSI_RESET,
             ANSI_BOLD ANSI_FG_WHITE, ANSI_RESET,
             ANSI_BOLD ANSI_FG_WHITE, ANSI_RESET);
    draw_box_row(margin_x, inner, accent, keys, 1);

    /* --- Box bottom (rounded) --- */
    draw_hrule(margin_x, inner, accent, BOX_BL, BOX_BR);

    printf("\033[J");
    fflush(stdout);
}

/* Switch to next phase */
static void advance_phase(const Config *cfg, State *state) {
    state->time_up_alert = false;
    if (state->phase == PHASE_WORK) {
        state->completed_sessions++;
        if (state->current_cycle >= cfg->cycles_before_long) {
            state->phase = PHASE_LONG_BREAK;
            state->total_sec = cfg->long_break_sec;
            state->remaining_sec = cfg->long_break_sec;
            state->current_cycle = 1;
        } else {
            state->phase = PHASE_SHORT_BREAK;
            state->total_sec = cfg->break_sec;
            state->remaining_sec = cfg->break_sec;
            state->current_cycle++;
        }
    } else {
        state->phase = PHASE_WORK;
        state->total_sec = cfg->work_sec;
        state->remaining_sec = cfg->work_sec;
    }
}

/* Print CLI usage / help */
static void print_help(const char *prog_name) {
    printf("Pomodoro Timer TUI\n");
    printf("A simple, beautiful terminal Pomodoro timer written in C.\n\n");
    printf("USAGE:\n  %s [OPTIONS]\n\n", prog_name);
    printf("OPTIONS:\n");
    printf("  -s, --sound <path>       Sound file to play when time is up\n");
    printf("                             (wav, mp3, ogg, flac, ...)\n");
    printf("  -w, --work <min>         Work session minutes (default: 25)\n");
    printf("  -b, --break <min>        Short break minutes (default: 5)\n");
    printf("  -l, --long-break <min>   Long break minutes (default: 15)\n");
    printf("  -c, --cycles <count>     Work sessions before a long break (default: 4)\n");
    printf("  -p, --player <cmd>       Audio player command (default: auto-detect)\n");
    printf("  -m, --mute               Start with audio muted\n");
    printf("  -t, --test-sound         Test sound playback and exit\n");
    printf("  -h, --help               Show this help and exit\n\n");
    printf("ENVIRONMENT VARIABLES:\n");
    printf("  POMODORO_SOUND           Path to sound file\n");
    printf("  POMODORO_AUDIO_PLAYER    Preferred player command\n\n");
    printf("KEYBINDINGS:\n");
    printf("  [Space]      Pause / Resume\n");
    printf("  [n] [Enter]  Skip / Advance to next phase\n");
    printf("  [r]          Reset current phase timer\n");
    printf("  [t]          Test sound playback\n");
    printf("  [m]          Toggle mute\n");
    printf("  [+] [-]      Add / subtract 1 minute\n");
    printf("  [q] [Esc]    Quit\n\n");
    printf("EXAMPLES:\n");
    printf("  %s\n", prog_name);
    printf("  %s -s sounds/bell.wav\n", prog_name);
    printf("  %s -s /usr/share/sounds/alarm.wav -w 50 -b 10\n", prog_name);
    printf("  %s --test-sound -s sounds/bell.wav\n", prog_name);
}

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN); /* reap audio player children automatically */

    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.work_sec = (int)(DEFAULT_WORK_MIN * 60.0);
    cfg.break_sec = (int)(DEFAULT_BREAK_MIN * 60.0);
    cfg.long_break_sec = (int)(DEFAULT_LONG_BREAK_MIN * 60.0);
    cfg.cycles_before_long = DEFAULT_CYCLES;

    const char *env_sound = getenv("POMODORO_SOUND");
    if (env_sound && env_sound[0] != '\0') {
        strncpy(cfg.sound_path, env_sound, sizeof(cfg.sound_path) - 1);
    } else if (access(DEFAULT_SOUND_PATH, R_OK) == 0) {
        strncpy(cfg.sound_path, DEFAULT_SOUND_PATH, sizeof(cfg.sound_path) - 1);
    } else if (access(FALLBACK_SOUND_PATH, R_OK) == 0) {
        strncpy(cfg.sound_path, FALLBACK_SOUND_PATH, sizeof(cfg.sound_path) - 1);
    }

    bool test_sound_and_exit = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_help(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--sound")) {
            if (i + 1 < argc) strncpy(cfg.sound_path, argv[++i], sizeof(cfg.sound_path) - 1);
            else { fprintf(stderr, "Error: %s requires a file path\n", argv[i]); return 1; }
        } else if (!strncmp(argv[i], "--sound=", 8)) {
            strncpy(cfg.sound_path, argv[i] + 8, sizeof(cfg.sound_path) - 1);
        } else if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--work")) {
            if (i + 1 < argc) { double v = atof(argv[++i]); if (v > 0) cfg.work_sec = (int)(v * 60.0); }
        } else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--break")) {
            if (i + 1 < argc) { double v = atof(argv[++i]); if (v > 0) cfg.break_sec = (int)(v * 60.0); }
        } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--long-break") || !strcmp(argv[i], "--long")) {
            if (i + 1 < argc) { double v = atof(argv[++i]); if (v > 0) cfg.long_break_sec = (int)(v * 60.0); }
        } else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--cycles")) {
            if (i + 1 < argc) { int v = atoi(argv[++i]); if (v > 0) cfg.cycles_before_long = v; }
        } else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--player")) {
            if (i + 1 < argc) strncpy(cfg.player_cmd, argv[++i], sizeof(cfg.player_cmd) - 1);
        } else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mute")) {
            cfg.mute = true;
        } else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--test-sound")) {
            test_sound_and_exit = true;
        } else {
            fprintf(stderr, "Unknown option: %s (use -h for help)\n", argv[i]);
            return 1;
        }
    }

    if (test_sound_and_exit) {
        char player[128] = {0};
        detect_audio_player(&cfg, player, sizeof(player));
        printf("Testing sound configuration:\n");
        printf("  Sound File: %s\n", cfg.sound_path[0] ? cfg.sound_path : "(terminal bell only)");
        printf("  Player:     %s\n", player[0] ? player : "(none found - terminal bell)");
        if (cfg.sound_path[0] && access(cfg.sound_path, R_OK) != 0)
            printf("  Warning: '%s' cannot be read!\n", cfg.sound_path);
        printf("  Playing sound now...\n");
        play_sound(&cfg);
        sleep(2);
        printf("Done.\n");
        return 0;
    }

    State state;
    memset(&state, 0, sizeof(state));
    state.phase = PHASE_WORK;
    state.remaining_sec = cfg.work_sec;
    state.total_sec = cfg.work_sec;
    state.current_cycle = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);

    enable_raw_mode();

    int cols = 80, rows = 24;
    get_terminal_size(&cols, &rows);

    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    render_tui(&cfg, &state, cols, rows);

    while (g_running) {
        if (g_win_resized) {
            g_win_resized = 0;
            get_terminal_size(&cols, &rows);
            printf("\033[2J");
            render_tui(&cfg, &state, cols, rows);
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            char ch = 0;
            if (read(STDIN_FILENO, &ch, 1) > 0) {
                if (ch == 'q' || ch == 'Q' || ch == 3 || ch == 27) {
                    break;
                } else if (ch == ' ') {
                    if (state.time_up_alert) advance_phase(&cfg, &state);
                    else state.is_paused = !state.is_paused;
                    render_tui(&cfg, &state, cols, rows);
                } else if (ch == 'n' || ch == 'N' || ch == '\n' || ch == '\r') {
                    advance_phase(&cfg, &state);
                    render_tui(&cfg, &state, cols, rows);
                } else if (ch == 'r' || ch == 'R') {
                    state.remaining_sec = state.total_sec;
                    state.time_up_alert = false;
                    render_tui(&cfg, &state, cols, rows);
                } else if (ch == 't' || ch == 'T') {
                    play_sound(&cfg);
                } else if (ch == 'm' || ch == 'M') {
                    cfg.mute = !cfg.mute;
                    render_tui(&cfg, &state, cols, rows);
                } else if (ch == '+' || ch == '=') {
                    state.remaining_sec += 60;
                    state.total_sec += 60;
                    render_tui(&cfg, &state, cols, rows);
                } else if (ch == '-' || ch == '_') {
                    if (state.remaining_sec > 60) {
                        state.remaining_sec -= 60;
                        if (state.total_sec > 60) state.total_sec -= 60;
                    }
                    render_tui(&cfg, &state, cols, rows);
                }
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_sec = (double)(now.tv_sec - last_tick.tv_sec) +
                             (double)(now.tv_nsec - last_tick.tv_nsec) / 1e9;

        if (elapsed_sec >= 1.0) {
            int seconds_passed = (int)elapsed_sec;
            last_tick.tv_sec += seconds_passed;
            state.alert_countdown++;

            if (!state.is_paused && !state.time_up_alert) {
                state.remaining_sec -= seconds_passed;
                if (state.remaining_sec <= 0) {
                    state.remaining_sec = 0;
                    state.time_up_alert = true;
                    play_sound(&cfg);
                }
            }
            render_tui(&cfg, &state, cols, rows);
        }
    }

    disable_raw_mode();
    printf("\nPomodoro session ended. Have a great day!\n");
    return 0;
}