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
#include <chrono>

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

    int overlay_alpha;
    double overlay_threshold;
    bool overlay_enabled;
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

// ── Métricas reais por bloco (escritas pelo worker, lidas pela main) ─────────
// Sem mutex: cada bloco é território exclusivo de um worker por vez.
// A main lê com relaxed — artefato visual ocasional é aceitável.
static int g_blocks_x = 0;
static int g_blocks_y = 0;
static int g_num_blocks = 0;

struct BlockMetrics
{
    std::atomic<uint64_t> iter_count; // total de iterações do último cálculo
    std::atomic<uint64_t> us_elapsed; // tempo de processamento em microssegundos
};
static BlockMetrics *g_metrics = nullptr;

// ════════════════════════════════════════════════════════════
//  KERNEL MANDELBROT
// ════════════════════════════════════════════════════════════

// Retorna a cor ARGB e acumula o número de iterações em `iter_out`.
// Nenhuma heurística de posição — custo depende exclusivamente da matemática.
static uint32_t compute_pixel(double px, double py, int max_iter, int palette,
                              uint64_t &iter_out)
{
    {
        double q = (px - 0.25) * (px - 0.25) + py * py;
        if (q * (q + px - 0.25) < 0.25 * py * py)
        {
            iter_out += max_iter;
            return 0xFF000000;
        }
        if ((px + 1.0) * (px + 1.0) + py * py < 0.0625)
        {
            iter_out += max_iter;
            return 0xFF000000;
        }
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
        {
            iter_out += (uint64_t)iter;
            return 0xFF000000;
        }
        if (++since_check == check_at)
        {
            xold = zr;
            yold = zi;
            since_check = 0;
            if (check_at < 512)
                check_at *= 2;
        }
    }

    iter_out += (uint64_t)iter;

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

        // Escala depende apenas da fase — nenhuma heurística de posição
        double log_scale = std::log(cfg.scale_initial) - (double)task.phase * std::log(cfg.zoom_factor);
        bool precision_limit = (log_scale < std::log(cfg.scale_min));
        double scale = precision_limit ? cfg.scale_min : std::exp(log_scale);

        // max_iter depende apenas da profundidade de zoom — igual para todos os blocos
        double zoom_depth = cfg.scale_initial / scale;
        double growth = 16.0 * std::sqrt(std::log2(1.0 + zoom_depth));
        int max_iter = std::min(cfg.max_iter_base + (int)growth, cfg.max_iter_cap);

        const int W = cfg.win_w, H = cfg.win_h;

        // ── Instrumentação: timestamp de início ──────────────────────
        auto t0 = std::chrono::steady_clock::now();

        uint64_t block_iters = 0;
        for (int py = task.y0; py < task.y1; py++)
            for (int px = task.x0; px < task.x1; px++)
            {
                double cx = cfg.zoom_x + (px - W * 0.5) * scale;
                double cy = cfg.zoom_y + (py - H * 0.5) * scale;
                ctx->pixels[py * W + px] = compute_pixel(cx, cy, max_iter, cfg.palette, block_iters);
            }

        // ── Instrumentação: timestamp de fim ─────────────────────────
        auto t1 = std::chrono::steady_clock::now();
        uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // ── Grava métricas reais no slot do bloco ────────────────────
        int idx = (task.y0 / cfg.block_size) * g_blocks_x + (task.x0 / cfg.block_size);
        if (idx >= 0 && idx < g_num_blocks)
        {
            g_metrics[idx].iter_count.store(block_iters, std::memory_order_relaxed);
            g_metrics[idx].us_elapsed.store(us, std::memory_order_relaxed);
        }

        // ── Telemetria de fase máxima ────────────────────────────────
        uint64_t prev = ctx->max_phase_seen.load(std::memory_order_relaxed);
        while (task.phase > prev &&
               !ctx->max_phase_seen.compare_exchange_weak(prev, task.phase,
                                                          std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }

        // ── Reinsere bloco com phase+1 ───────────────────────────────
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
    cfg.max_iter_base = std::max(16, arg_int(argc, argv, "--max-iter", 256));
    cfg.max_iter_cap = cfg.max_iter_base * 4;
    cfg.block_size = std::max(4, arg_int(argc, argv, "--block-size", 64));
    cfg.palette = arg_int(argc, argv, "--palette", 0);
    cfg.zoom_x = arg_dbl(argc, argv, "--zoom-x", -0.7436438885706799);
    cfg.zoom_y = arg_dbl(argc, argv, "--zoom-y", 0.1318259042053185);
    cfg.zoom_factor = std::max(1.0001, arg_dbl(argc, argv, "--zoom-factor", 1.008));
    cfg.scale_initial = 3.5 / cfg.win_w;
    cfg.scale_min = 1e-13;
    cfg.phase_cap = std::max(0, arg_int(argc, argv, "--phase-cap", 0));

    cfg.overlay_alpha = std::clamp(arg_int(argc, argv, "--overlay-alpha", 80), 0, 255);

    cfg.overlay_threshold = std::clamp(arg_dbl(argc, argv, "--overlay-threshold", 0.90), 0.0, 1.0);

    cfg.overlay_enabled = arg_int(argc, argv, "--overlay", 1) != 0;

    const int num_threads = std::max(1, arg_int(argc, argv, "--threads", 4));

    g_blocks_x = (cfg.win_w + cfg.block_size - 1) / cfg.block_size;
    g_blocks_y = (cfg.win_h + cfg.block_size - 1) / cfg.block_size;
    g_num_blocks = g_blocks_x * g_blocks_y;
    g_metrics = new BlockMetrics[g_num_blocks];
    for (int i = 0; i < g_num_blocks; i++)
    {
        g_metrics[i].iter_count.store(0);
        g_metrics[i].us_elapsed.store(0);
    }

    printf("╔══ Mandelbrot — Fases Paralelas ════════════════════╗\n");
    printf("║  threads      = %d\n", num_threads);
    printf("║  max_iter     = %d → %d\n", cfg.max_iter_base, cfg.max_iter_cap);
    printf("║  block_size   = %d×%d px\n", cfg.block_size, cfg.block_size);
    printf("║  janela       = %d×%d px\n", cfg.win_w, cfg.win_h);
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

        // ── Overlay de custo real ────────────────────────────────────────
        // Usa us_elapsed como métrica primária (tempo real de CPU por bloco).
        // Coleta min e max para normalizar — nenhum limite fixo, tudo relativo.
        {
            uint64_t cost_min = UINT64_MAX, cost_max = 0;
            for (int i = 0; i < g_num_blocks; i++)
            {
                uint64_t c = g_metrics[i].us_elapsed.load(std::memory_order_relaxed);
                if (c < cost_min)
                    cost_min = c;
                if (c > cost_max)
                    cost_max = c;
            }
            if (cost_min == UINT64_MAX)
                cost_min = 0;
            uint64_t cost_range = (cost_max > cost_min) ? (cost_max - cost_min) : 1;

            const int BS = cfg.block_size;
            for (int by = 0; by < g_blocks_y; by++)
            {
                for (int bx = 0; bx < g_blocks_x; bx++)
                {
                    uint64_t c = g_metrics[by * g_blocks_x + bx].us_elapsed.load(std::memory_order_relaxed);
                    double t = (double)(c - cost_min) / (double)cost_range;

                    if (!cfg.overlay_enabled)
                        continue;

                    if (t < cfg.overlay_threshold)
                        continue;

                    SDL_SetRenderDrawColor(
                        renderer,
                        255,
                        0,
                        0,
                        cfg.overlay_alpha);

                    SDL_Rect rect = {
                        bx * BS, by * BS,
                        std::min(BS, cfg.win_w - bx * BS),
                        std::min(BS, cfg.win_h - by * BS)};
                    SDL_RenderFillRect(renderer, &rect);
                }
            }
        }

        // ── Grid ─────────────────────────────────────────────────────────
        {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
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
            uint64_t cost_min = UINT64_MAX, cost_max = 0;
            for (int i = 0; i < g_num_blocks; i++)
            {
                uint64_t c = g_metrics[i].us_elapsed.load(std::memory_order_relaxed);
                if (c < cost_min)
                    cost_min = c;
                if (c > cost_max)
                    cost_max = c;
            }
            if (cost_min == UINT64_MAX)
                cost_min = 0;
            uint64_t fase_max = ctx.max_phase_seen.load(std::memory_order_relaxed);
            snprintf(title, sizeof(title),
                     "Mandelbrot | FPS:%u | fase_max:%llu | custo min:%lluµs max:%lluµs",
                     fps_frames,
                     (unsigned long long)fase_max,
                     (unsigned long long)cost_min,
                     (unsigned long long)cost_max);
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
                running = false;
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
    delete[] g_metrics;
    return 0;
}