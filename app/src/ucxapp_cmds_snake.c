/* Break-time games, host only. app.snake: the snake eats data packets, interactive in a terminal (console and TUI),
   demo plays itself. app.hunt: walk the module towards an access point, guided by its Wi-Fi scans. */

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"

#include <stdio.h>
#include <string.h>

#define SNAKE_W 30
#define SNAKE_H 15
#define SNAKE_MAX (SNAKE_W * SNAKE_H)

typedef struct {
    uint8_t x[SNAKE_MAX], y[SNAKE_MAX];   /* [0] is the head */
    int len, dx, dy, ndx, ndy, foodX, foodY, score;
    uint32_t rng;
    bool dead;
} snake_t;

static int gBest;

static uint32_t rnd(snake_t *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

static bool onSnake(const snake_t *s, int x, int y, int from)
{
    for (int i = from; i < s->len; i++) {
        if (s->x[i] == x && s->y[i] == y) {
            return true;
        }
    }
    return false;
}

static void placeFood(snake_t *s)
{
    if (s->len >= SNAKE_MAX) {
        s->foodX = s->foodY = -1;
        return;
    }
    do {
        s->foodX = (int)(rnd(s) % SNAKE_W);
        s->foodY = (int)(rnd(s) % SNAKE_H);
    } while (onSnake(s, s->foodX, s->foodY, 0));
}

static void snakeInit(snake_t *s, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->rng = seed != 0 ? seed : 1;
    s->len = 3;
    for (int i = 0; i < s->len; i++) {
        s->x[i] = (uint8_t)(SNAKE_W / 2 - i);
        s->y[i] = SNAKE_H / 2;
    }
    s->dx = s->ndx = 1;
    placeFood(s);
}

/* A reverse into the own neck is ignored. */
static void snakeTurn(snake_t *s, int dx, int dy)
{
    if (dx != -s->dx || dy != -s->dy) {
        s->ndx = dx;
        s->ndy = dy;
    }
}

static bool hits(const snake_t *s, int nx, int ny, bool eat)
{
    if (nx < 0 || ny < 0 || nx >= SNAKE_W || ny >= SNAKE_H) {
        return true;
    }
    /* the tail moves away this step unless the snake grows */
    return onSnake(s, nx, ny, 0) && (eat || s->x[s->len - 1] != nx || s->y[s->len - 1] != ny);
}

static void snakeStep(snake_t *s)
{
    int nx, ny;
    bool eat;

    if (s->dead) {
        return;
    }
    s->dx = s->ndx;
    s->dy = s->ndy;
    nx = s->x[0] + s->dx;
    ny = s->y[0] + s->dy;
    eat = nx == s->foodX && ny == s->foodY;
    if (hits(s, nx, ny, eat)) {
        s->dead = true;
        return;
    }
    if (eat) {
        s->len++;
        s->score += 10;
    }
    memmove(s->x + 1, s->x, (size_t)(s->len - 1));
    memmove(s->y + 1, s->y, (size_t)(s->len - 1));
    s->x[0] = (uint8_t)nx;
    s->y[0] = (uint8_t)ny;
    if (eat) {
        placeFood(s);
    }
    if (s->score > gBest) {
        gBest = s->score;
    }
}

static bool safe(const snake_t *s, int dx, int dy)
{
    int nx = s->x[0] + dx, ny = s->y[0] + dy;

    return !hits(s, nx, ny, nx == s->foodX && ny == s->foodY);
}

/* demo pilot: head for the packet, else keep going, else any safe way */
static void pilot(snake_t *s)
{
    static const int kDirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int want[2][2] = { { s->foodX > s->x[0] ? 1 : -1, 0 }, { 0, s->foodY > s->y[0] ? 1 : -1 } };

    if (s->foodX == s->x[0]) {
        want[0][0] = 0;
    }
    if (s->foodY == s->y[0]) {
        want[1][1] = 0;
    }
    for (int i = 0; i < 2; i++) {
        if ((want[i][0] != 0 || want[i][1] != 0) && (want[i][0] != -s->dx || want[i][1] != -s->dy) &&
            safe(s, want[i][0], want[i][1])) {
            snakeTurn(s, want[i][0], want[i][1]);
            return;
        }
    }
    if (safe(s, s->dx, s->dy)) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        if ((kDirs[i][0] != -s->dx || kDirs[i][1] != -s->dy) && safe(s, kDirs[i][0], kDirs[i][1])) {
            snakeTurn(s, kDirs[i][0], kDirs[i][1]);
            return;
        }
    }
}

typedef struct {
    char *out;
    size_t size, n;
} buf_t;

static void put(buf_t *b, const char *str)
{
    size_t len = strlen(str);

    if (b->n + len < b->size) {
        memcpy(b->out + b->n, str, len + 1);
        b->n += len;
    }
}

/* One frame as text: plain cells for the demo, colored blocks for the terminal. */
static void render(const snake_t *s, char *out, size_t size, bool color)
{
    const char *nl = color ? "\033[0m\r\n" : "\n";
    buf_t b = { out, size, 0 };

    out[0] = '\0';
    for (int y = -1; y <= SNAKE_H; y++) {
        bool edge = y < 0 || y == SNAKE_H;

        put(&b, color ? "\033[36m" : "");
        put(&b, edge ? "+" : "|");
        put(&b, color ? "\033[0m" : "");
        for (int x = 0; x < SNAKE_W; x++) {
            if (edge) {
                put(&b, color ? "\033[36m--\033[0m" : "--");
            } else if (s->x[0] == x && s->y[0] == y) {
                put(&b, !color ? "@@" : s->dead ? "\033[91m\xE2\x96\x88\xE2\x96\x88\033[0m" :
                                                  "\033[92m\xE2\x96\x88\xE2\x96\x88\033[0m");
            } else if (onSnake(s, x, y, 1)) {
                put(&b, color ? "\033[32m\xE2\x96\x93\xE2\x96\x93\033[0m" : "oo");
            } else if (s->foodX == x && s->foodY == y) {
                put(&b, color ? "\033[93m<>\033[0m" : "<>");
            } else {
                put(&b, "  ");
            }
        }
        put(&b, color ? "\033[36m" : "");
        put(&b, edge ? "+" : "|");
        put(&b, nl);
    }
}

int ucxapp_snake_play(void)
{
    static char frame[16384];
    snake_t s;
    bool paused = false, quit = false;
    uint32_t next;

    snakeInit(&s, ucxapp_platform_tick_ms());
    fputs("\033[?25l\033[2J", stdout);
    next = ucxapp_platform_tick_ms();
    while (!quit) {
        int key;

        while ((key = ucxapp_platform_read_key()) >= 0) {
            switch (key) {
            case UCXAPP_KEY_UP: case 'w': case 'W': snakeTurn(&s, 0, -1); break;
            case UCXAPP_KEY_DOWN: case 's': case 'S': snakeTurn(&s, 0, 1); break;
            case UCXAPP_KEY_LEFT: case 'a': case 'A': snakeTurn(&s, -1, 0); break;
            case UCXAPP_KEY_RIGHT: case 'd': case 'D': snakeTurn(&s, 1, 0); break;
            case 'p': case 'P': case ' ': paused = !paused; break;
            case 'r': case 'R': case 13:
                if (s.dead) {
                    snakeInit(&s, ucxapp_platform_tick_ms());
                }
                break;
            case 'q': case 'Q': case 27: case 3: quit = true; break;
            default: break;
            }
        }
        if ((int32_t)(ucxapp_platform_tick_ms() - next) >= 0) {
            int ms = 140 - s.score / 2;

            next = ucxapp_platform_tick_ms() + (uint32_t)(ms < 60 ? 60 : ms);
            if (!paused) {
                snakeStep(&s);
            }
            render(&s, frame, sizeof(frame), true);
            fputs("\033[H\033[1;95m  u-blox snake\033[0m  eat the \033[93m<>\033[0m packets\033[K\r\n", stdout);
            fputs(frame, stdout);
            fprintf(stdout, "  Score \033[1m%d\033[0m   length %d   best %d   %s\033[K\r\n", s.score, s.len, gBest,
                    s.dead ? "\033[91mlink lost!\033[0m Enter or r: again, q: quit" :
                    paused ? "paused (p)" : "arrows/WASD steer, p pause, q quit");
            fflush(stdout);
        }
        ucxapp_platform_wait_key(10);
    }
    fputs("\033[0m\033[2J\033[H\033[?25h", stdout);
    fflush(stdout);
    return s.score;
}

/* app.snake [demo [steps=300] [seed=1]] */
static int cmdSnake(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc >= 2 && strcmp(argv[1], "demo") == 0) {
        static char frame[8192];
        int32_t steps = 300, seed = 1, n = 0;
        snake_t s;

        if (argc > 4 || (argc >= 3 && (!ucxapp_parse_int(argv[2], &steps) || steps < 1 || steps > 100000)) ||
            (argc == 4 && (!ucxapp_parse_int(argv[3], &seed) || seed < 1))) {
            return UCXAPP_ERR_USAGE;
        }
        snakeInit(&s, (uint32_t)seed);
        while (n < steps && !s.dead) {
            pilot(&s);
            snakeStep(&s);
            n++;
        }
        render(&s, frame, sizeof(frame), false);
        for (char *line = frame, *end; *line != '\0'; line = end + 1) {
            end = strchr(line, '\n');
            if (end == NULL) {
                break;
            }
            *end = '\0';
            ucxapp_print("%s\n", line);
        }
        ucxapp_print("Score %d, length %d after %d steps%s\n", s.score, s.len, (int)n, s.dead ? ": link lost" : "");
        return UCXAPP_OK;
    }
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_platform_terminal_enter()) {
        ucxapp_print("app.snake needs an interactive terminal (the console or --tui); try 'app.snake demo', "
                     "or right-click FX on the web page\n");
        return UCXAPP_ERR_UNSUPPORTED;
    }
    fputs("\033[?1049h", stdout);
    int score = ucxapp_snake_play();
    fputs("\033[?1049l", stdout);
    fflush(stdout);
    ucxapp_platform_terminal_leave();
    ucxapp_print("Snake score %d (best %d)\n", score, gBest);
    return UCXAPP_OK;
}

#define HUNT_WIN_DBM (-40)
#define HUNT_MAX_AP  32

static int findAp(const ucxapp_wifi_ap_t *aps, int n, const uint8_t bssid[6], const char *ssid)
{
    int best = -1;

    for (int i = 0; i < n; i++) {
        bool match = bssid != NULL ? memcmp(aps[i].bssid, bssid, 6) == 0 : strcmp(aps[i].ssid, ssid) == 0;

        if (match && (best < 0 || aps[i].rssi > aps[best].rssi)) {
            best = i;
        }
    }
    return best;
}

static bool huntStop(void)
{
    for (int t = 0; t < 10; t++) {
        if (ucxapp_platform_key_pressed()) {
            (void)ucxapp_platform_get_key();
            return true;
        }
        ucxapp_platform_sleep_ms(50);
    }
    return false;
}

/* app.hunt [ssid] [rounds=20] */
static int cmdHunt(ucxapp_t *app, int argc, char *argv[])
{
    static ucxapp_wifi_ap_t aps[HUNT_MAX_AP];
    const char *ssid = NULL;
    int32_t rounds = 20;
    uint8_t bssid[6];
    char name[33];
    int n, i, last, best, start;

    if (argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    for (int a = 1; a < argc; a++) {
        int32_t v;

        if (a == argc - 1 && ucxapp_parse_int(argv[a], &v)) {
            if (v < 1 || v > 1000) {
                return UCXAPP_ERR_USAGE;
            }
            rounds = v;
        } else if (a == 1) {
            ssid = argv[a];
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (app->ucx == NULL) {
        ucxapp_print("ERROR: not connected to a module\n");
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    n = ucxapp_wifi_scan(app, aps, HUNT_MAX_AP);
    if (n < 0) {
        return UCXAPP_ERR_FAILED;
    }
    i = ssid != NULL ? findAp(aps, n, NULL, ssid) : (n > 0 ? 0 : -1);
    for (int k = 0; ssid == NULL && k < n; k++) {
        if (aps[k].rssi > aps[i].rssi) {
            i = k;
        }
    }
    if (i < 0) {
        ucxapp_print(ssid != NULL ? "No access point named \"%s\" in range: try another (wifi.scan lists them)\n" :
                                    "No access point in range%s\n", ssid != NULL ? ssid : "");
        return UCXAPP_ERR_FAILED;
    }
    memcpy(bssid, aps[i].bssid, sizeof(bssid));
    snprintf(name, sizeof(name), "%s", aps[i].ssid[0] != '\0' ? aps[i].ssid : "(hidden)");
    last = best = start = aps[i].rssi;
    ucxapp_print("Signal hunt: walk the module to \"%s\" (%02X:%02X:%02X:%02X:%02X:%02X, channel %d); "
                 "%d dBm wins, %d rounds, any key stops\n", name, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
                 bssid[5], aps[i].channel, HUNT_WIN_DBM, (int)rounds);
    for (int32_t r = 1; r <= rounds; r++) {
        char bar[64] = "";
        int rssi, fill;
        const char *hint;

        if (r > 1) {
            if (huntStop()) {
                ucxapp_print("Stopped\n");
                break;
            }
            n = ucxapp_wifi_scan(app, aps, HUNT_MAX_AP);
            i = n > 0 ? findAp(aps, n, bssid, NULL) : -1;
        }
        if (i < 0) {
            ucxapp_print("%3d  %-20.20s  out of range: go back!\n", (int)r, name);
            continue;
        }
        rssi = aps[i].rssi;
        fill = (rssi + 95) / 5;   /* -95 dBm empty, -40 dBm full: 11 blocks */
        fill = fill < 0 ? 0 : fill > 11 ? 11 : fill;
        for (int b = 0; b < 11; b++) {
            strcat(bar, b < fill ? "\xE2\x96\x88" : "\xC2\xB7");
        }
        hint = r == 1 ? "start" : rssi >= last + 3 ? "hotter!" : rssi <= last - 3 ? "colder" : "same";
        if (rssi > best) {
            best = rssi;
        }
        ucxapp_print("%3d  %-20.20s  %4d dBm  %s  %s\n", (int)r, name, rssi, bar, hint);
        last = rssi;
        if (rssi >= HUNT_WIN_DBM) {
            ucxapp_print("Found it in %d rounds: %d dBm, %d dB better than the start\n", (int)r, rssi, rssi - start);
            return UCXAPP_OK;
        }
    }
    ucxapp_print("Best %d dBm (start %d dBm, %+d dB); %d dBm wins\n", best, start, best - start, HUNT_WIN_DBM);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t kCmds[] = {
    { "app.snake", "[demo [steps=300] [seed=1]]",
      "Snake eats data packets: arrows/WASD steer, p pause, q quit; demo lets the app play", 0, cmdSnake },
    { "app.hunt", "[ssid] [rounds=20]",
      "Signal hunt: walk the module to an access point, hotter or colder after every scan", 0, cmdHunt },
};

void ucxapp_register_snake_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
