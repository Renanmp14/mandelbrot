#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>
#include <pthread.h>
#include <queue>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <string>

// ════════════════════════════════════════════════════════════
//  ARGUMENTOS
// ════════════════════════════════════════════════════════════

static const char *get_arg(int argc, char **argv, const char *key)
{
    for (int i = 1; i < argc - 1; i++)
        if (std::strcmp(argv[i], key) == 0)
            return argv[i + 1];
    return nullptr;
}
static int arg_int(int argc, char **argv, const char *key, int def)
{
    const char *v = get_arg(argc, argv, key);
    return v ? std::atoi(v) : def;
}
static double arg_dbl(int argc, char **argv, const char *key, double def)
{
    const char *v = get_arg(argc, argv, key);
    return v ? std::atof(v) : def;
}

// ════════════════════════════════════════════════════════════
//  ESTRUTURAS
// ════════════════════════════════════════════════════════════

struct Task
{
    int x0, y0, x1, y1;
    uint64_t phase;
};

struct Config
{
    int win_w, win_h;
    int max_iter_base, max_iter_cap;
    int palette;
    double zoom_x, zoom_y;
    double scale_initial, scale_min, zoom_factor;
    int block_size, phase_cap;
    double iter_bias;
};

struct SharedCtx
{
    Config cfg;
    uint32_t *pixels;
    std::atomic<uint64_t> max_phase_seen;
    std::queue<Task> task_queue;
    pthread_mutex_t task_mutex;
    pthread_cond_t task_cond;
    bool shutdown;
};

static int g_blocks_x = 0;
static int g_blocks_y = 0;
static int g_num_blocks = 0;
static std::atomic<uint64_t> *g_block_phase = nullptr;

// ════════════════════════════════════════════════════════════
//  WORKLOAD POR REGIÃO
// ════════════════════════════════════════════════════════════

static int block_max_iter(const Config &cfg, int x0, int y0, int x1, int y1)
{
    if (cfg.iter_bias <= 0.0)
        return cfg.max_iter_base;

    double bx = (x0 + x1) * 0.5 / cfg.win_w;
    double by = (y0 + y1) * 0.5 / cfg.win_h;
    double dx = (bx - 0.5) * 2.0;
    double dy = (by - 0.5) * 2.0;
    double dist = std::min(std::sqrt(dx * dx + dy * dy) / 1.414, 1.0);
    double t = (1.0 - dist) * cfg.iter_bias;

    return std::max(cfg.max_iter_base,
                    std::min((int)(cfg.max_iter_base + t * (cfg.max_iter_cap - cfg.max_iter_base)),
                             cfg.max_iter_cap));
}

// ════════════════════════════════════════════════════════════
//  KERNEL MANDELBROT
// ════════════════════════════════════════════════════════════

static uint32_t compute_pixel(double px, double py, int max_iter, int palette)
{
    {
        double q = (px - 0.25) * (px - 0.25) + py * py;
        if (q * (q + px - 0.25) < 0.25 * py * py)
            return 0xFF000000;
        if ((px + 1.0) * (px + 1.0) + py * py < 0.0625)
            return 0xFF000000;
    }

    double zr = 0, zi = 0, zr2 = 0, zi2 = 0;
    int iter = 0;
    double xold = 0, yold = 0;
    int check_at = 4, since_check = 0;

    while (iter < max_iter && zr2 + zi2 <= 4.0)
    {
        zi = 2.0 * zr * zi + py;
        zr = zr2 - zi2 + px;
        zr2 = zr * zr;
        zi2 = zi * zi;
        iter++;
        if (std::abs(zr - xold) < 1e-10 && std::abs(zi - yold) < 1e-10)
            return 0xFF000000;
        if (++since_check == check_at)
        {
            xold = zr;
            yold = zi;
            since_check = 0;
            if (check_at < 512)
                check_at *= 2;
        }
    }
    if (iter == max_iter)
        return 0xFF000000;

    double log_zn = std::log(zr2 + zi2) * 0.5;
    double nu = std::log(log_zn / std::log(2.0)) / std::log(2.0);
    double t = ((double)(iter + 1) - nu) / (double)max_iter;

    double r, g, b;
    switch (palette)
    {
    case 1:
        r = std::min(1.0, t * 3.0);
        g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
        b = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
        break;
    case 2:
        r = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
        g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
        b = std::min(1.0, t * 3.0);
        break;
    case 3:
        r = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.75));
        g = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.50));
        b = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.00));
        break;
    case 4:
        r = t;
        g = t;
        b = t;
        break;
    default:
        r = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.00));
        g = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.33));
        b = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.67));
        break;
    }
    return 0xFF000000 | ((uint32_t)(r * 255.0) << 16) | ((uint32_t)(g * 255.0) << 8) | (uint32_t)(b * 255.0);
}

// ════════════════════════════════════════════════════════════
//  WORKER
// ════════════════════════════════════════════════════════════

static void *worker_func(void *arg)
{
    SharedCtx *ctx = static_cast<SharedCtx *>(arg);
    const Config &cfg = ctx->cfg;

    while (true)
    {
        pthread_mutex_lock(&ctx->task_mutex);
        while (ctx->task_queue.empty() && !ctx->shutdown)
            pthread_cond_wait(&ctx->task_cond, &ctx->task_mutex);

        if (ctx->shutdown && ctx->task_queue.empty())
        {
            pthread_mutex_unlock(&ctx->task_mutex);
            break;
        }

        Task task = ctx->task_queue.front();
        ctx->task_queue.pop();
        pthread_mutex_unlock(&ctx->task_mutex);

        double log_scale = std::log(cfg.scale_initial) - (double)task.phase * std::log(cfg.zoom_factor);
        bool precision_limit = (log_scale < std::log(cfg.scale_min));
        double scale = precision_limit ? cfg.scale_min : std::exp(log_scale);

        double zoom_depth = cfg.scale_initial / scale;
        double growth = 16.0 * std::sqrt(std::log2(1.0 + zoom_depth));
        int dyn_iter = cfg.max_iter_base + (int)growth;
        int reg_iter = block_max_iter(cfg, task.x0, task.y0, task.x1, task.y1);
        int max_iter = std::max(cfg.max_iter_base, std::min({dyn_iter, reg_iter, cfg.max_iter_cap}));

        const int W = cfg.win_w, H = cfg.win_h;
        for (int py = task.y0; py < task.y1; py++)
            for (int px = task.x0; px < task.x1; px++)
            {
                double cx = cfg.zoom_x + (px - W * 0.5) * scale;
                double cy = cfg.zoom_y + (py - H * 0.5) * scale;
                ctx->pixels[py * W + px] = compute_pixel(cx, cy, max_iter, cfg.palette);
            }

        uint64_t prev = ctx->max_phase_seen.load(std::memory_order_relaxed);
        while (task.phase > prev &&
               !ctx->max_phase_seen.compare_exchange_weak(prev, task.phase,
                                                          std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }

        {
            int idx = (task.y0 / cfg.block_size) * g_blocks_x + (task.x0 / cfg.block_size);
            if (idx >= 0 && idx < g_num_blocks)
                g_block_phase[idx].store(task.phase, std::memory_order_relaxed);
        }

        bool at_cap = (cfg.phase_cap > 0 && (int64_t)task.phase >= cfg.phase_cap - 1);
        if (!at_cap)
        {
            Task next = task;
            next.phase = precision_limit ? 0 : task.phase + 1;
            pthread_mutex_lock(&ctx->task_mutex);
            if (!ctx->shutdown)
            {
                ctx->task_queue.push(next);
                pthread_cond_signal(&ctx->task_cond);
            }
            pthread_mutex_unlock(&ctx->task_mutex);
        }
    }
    return nullptr;
}

// ════════════════════════════════════════════════════════════
//  SEED
// ════════════════════════════════════════════════════════════

static void seed_queue(SharedCtx *ctx)
{
    const int W = ctx->cfg.win_w, H = ctx->cfg.win_h, BS = ctx->cfg.block_size;
    pthread_mutex_lock(&ctx->task_mutex);
    for (int y = 0; y < H; y += BS)
        for (int x = 0; x < W; x += BS)
            ctx->task_queue.push({x, y, std::min(x + BS, W), std::min(y + BS, H), 0});
    pthread_cond_broadcast(&ctx->task_cond);
    pthread_mutex_unlock(&ctx->task_mutex);
}

// ════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
    Config cfg;
    cfg.win_w = std::max(200, arg_int(argc, argv, "--width", 900));
    cfg.win_h = std::max(200, arg_int(argc, argv, "--height", 900));
    cfg.max_iter_base = std::max(16, arg_int(argc, argv, "--max-iter", 64));
    cfg.max_iter_cap = cfg.max_iter_base * 8;
    cfg.block_size = std::max(4, arg_int(argc, argv, "--block-size", 64));
    cfg.palette = arg_int(argc, argv, "--palette", 0);
    cfg.zoom_x = arg_dbl(argc, argv, "--zoom-x", -0.7436438885706799);
    cfg.zoom_y = arg_dbl(argc, argv, "--zoom-y", 0.1318259042053185);
    cfg.zoom_factor = std::max(1.0001, arg_dbl(argc, argv, "--zoom-factor", 1.008));
    cfg.scale_initial = 3.5 / cfg.win_w;
    cfg.scale_min = 1e-13;
    cfg.phase_cap = std::max(0, arg_int(argc, argv, "--phase-cap", 0));
    cfg.iter_bias = std::max(0.0, std::min(1.0, arg_dbl(argc, argv, "--iter-bias", 1.0)));

    const int num_threads = std::max(1, arg_int(argc, argv, "--threads", 4));

    g_blocks_x = (cfg.win_w + cfg.block_size - 1) / cfg.block_size;
    g_blocks_y = (cfg.win_h + cfg.block_size - 1) / cfg.block_size;
    g_num_blocks = g_blocks_x * g_blocks_y;
    g_block_phase = new std::atomic<uint64_t>[g_num_blocks];
    for (int i = 0; i < g_num_blocks; i++)
        g_block_phase[i].store(0);

    printf("╔══ Mandelbrot — Fases Paralelas ════════════════════╗\n");
    printf("║  threads      = %d\n", num_threads);
    printf("║  max_iter     = %d → %d\n", cfg.max_iter_base, cfg.max_iter_cap);
    printf("║  block_size   = %d×%d px\n", cfg.block_size, cfg.block_size);
    printf("║  janela       = %d×%d px\n", cfg.win_w, cfg.win_h);
    printf("║  iter_bias    = %.2f\n", cfg.iter_bias);
    printf("║  palette      = %d\n", cfg.palette);
    printf("╚════════════════════════════════════════════════════╝\n\n");

    SharedCtx ctx;
    ctx.cfg = cfg;
    ctx.shutdown = false;
    ctx.max_phase_seen.store(0);
    ctx.pixels = new uint32_t[(size_t)cfg.win_w * cfg.win_h]();
    pthread_mutex_init(&ctx.task_mutex, nullptr);
    pthread_cond_init(&ctx.task_cond, nullptr);

    std::vector<pthread_t> workers((size_t)num_threads);
    for (int i = 0; i < num_threads; i++)
        pthread_create(&workers[(size_t)i], nullptr, worker_func, &ctx);

    seed_queue(&ctx);

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "Mandelbrot — Fases Paralelas",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.win_w, cfg.win_h, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, cfg.win_w, cfg.win_h);

    bool running = true;
    Uint32 fps_t0 = SDL_GetTicks();
    Uint32 fps_frames = 0;
    char title[320];

    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }
        if (!running)
            break;

        SDL_UpdateTexture(tex, nullptr, ctx.pixels, cfg.win_w * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, tex, nullptr, nullptr);

        // ── Overlay: pinta apenas os blocos atrasados em laranja→vermelho ──
        {
            // Calcula a fase média entre todos os blocos
            double sum = 0.0;
            for (int i = 0; i < g_num_blocks; i++)
                sum += (double)g_block_phase[i].load(std::memory_order_relaxed);
            double avg = sum / (double)g_num_blocks;

            // Fase mínima (bloco mais lento) para normalizar o atraso
            uint64_t ph_min = UINT64_MAX;
            for (int i = 0; i < g_num_blocks; i++)
            {
                uint64_t p = g_block_phase[i].load(std::memory_order_relaxed);
                if (p < ph_min)
                    ph_min = p;
            }

            // Intervalo de atraso: de avg até ph_min (quanto mais longe, mais vermelho)
            double lag_range = std::max(1.0, avg - (double)ph_min);

            const int BS = cfg.block_size;
            for (int by = 0; by < g_blocks_y; by++)
            {
                for (int bx = 0; bx < g_blocks_x; bx++)
                {
                    uint64_t ph = g_block_phase[by * g_blocks_x + bx].load(std::memory_order_relaxed);
                    double lag = avg - (double)ph; // positivo = atrasado

                    if (lag <= 0.0)
                        continue; // no ritmo ou adiantado: sem overlay

                    // t ∈ [0, 1]: 0 = levemente atrasado (laranja), 1 = muito atrasado (vermelho)
                    double t = std::min(lag / lag_range, 1.0);

                    // laranja (255,140,0) → vermelho (220,0,0)
                    uint8_t R = (uint8_t)(255 - (int)(35.0 * t));  // 255 → 220
                    uint8_t G = (uint8_t)(140 - (int)(140.0 * t)); // 140 → 0
                    uint8_t B = 0;
                    // alpha cresce com o atraso: 60 (quase invisível) → 200 (bem opaco)
                    uint8_t A = (uint8_t)(60 + (int)(140.0 * t));

                    SDL_SetRenderDrawColor(renderer, R, G, B, A);
                    SDL_Rect rect = {
                        bx * BS, by * BS,
                        std::min(BS, cfg.win_w - bx * BS),
                        std::min(BS, cfg.win_h - by * BS)};
                    SDL_RenderFillRect(renderer, &rect);
                }
            }
        }

        // ── Grid ──────────────────────────────────────────────────────────
        {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 80);
            const int BS = cfg.block_size;
            for (int x = 0; x <= cfg.win_w; x += BS)
                SDL_RenderDrawLine(renderer, x, 0, x, cfg.win_h - 1);
            for (int y = 0; y <= cfg.win_h; y += BS)
                SDL_RenderDrawLine(renderer, 0, y, cfg.win_w - 1, y);
        }

        SDL_RenderPresent(renderer);

        fps_frames++;
        Uint32 now = SDL_GetTicks();
        if (now - fps_t0 >= 1000)
        {
            uint64_t ph_min = UINT64_MAX, ph_max = 0;
            for (int i = 0; i < g_num_blocks; i++)
            {
                uint64_t p = g_block_phase[i].load(std::memory_order_relaxed);
                if (p < ph_min)
                    ph_min = p;
                if (p > ph_max)
                    ph_max = p;
            }
            if (ph_min == UINT64_MAX)
                ph_min = 0;
            snprintf(title, sizeof(title),
                     "Mandelbrot | FPS:%u | fase min:%llu max:%llu spread:%llu",
                     fps_frames,
                     (unsigned long long)ph_min,
                     (unsigned long long)ph_max,
                     (unsigned long long)(ph_max - ph_min));
            SDL_SetWindowTitle(window, title);
            fps_frames = 0;
            fps_t0 = now;
        }

        if (cfg.phase_cap > 0)
        {
            pthread_mutex_lock(&ctx.task_mutex);
            bool vazia = ctx.task_queue.empty();
            pthread_mutex_unlock(&ctx.task_mutex);
            if (vazia)
            {
                running = false;
            }
        }
    }

    pthread_mutex_lock(&ctx.task_mutex);
    ctx.shutdown = true;
    pthread_cond_broadcast(&ctx.task_cond);
    pthread_mutex_unlock(&ctx.task_mutex);
    for (pthread_t &t : workers)
        pthread_join(t, nullptr);

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    pthread_mutex_destroy(&ctx.task_mutex);
    pthread_cond_destroy(&ctx.task_cond);
    delete[] ctx.pixels;
    delete[] g_block_phase;
    return 0;
}