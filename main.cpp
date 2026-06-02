/**
 * main.cpp — Fractal de Mandelbrot com Zoom Contínuo
 * Disciplina: Computação de Alto Desempenho
 *
 * Arquitetura: Pool de Threads (Pthreads) + Renderização SDL2
 * ─────────────────────────────────────────────────────────────
 *  Main thread  →  Produz tarefas (blocos da tela) e renderiza
 *  Worker threads → Consomem tarefas e calculam os pixels
 *
 * Uso (todos os parâmetros têm valores padrão):
 *   ./mandelbrot [--threads N] [--max-iter N] [--block-size N]
 *               [--width N] [--height N]
 *               [--zoom-x F] [--zoom-y F]
 *               [--zoom-factor F] [--frame-delay N]
 *               [--palette N]
 *
 * Paletas disponíveis:
 *   0 = Padrão (azul/verde/roxo)
 *   1 = Fogo   (vermelho/laranja/amarelo)
 *   2 = Oceano (azul profundo)
 *   3 = Gold & Purple (arco-íris)
 *   4 = Escala de Cinza
 *
 * Exemplo:
 *   ./mandelbrot --threads 8 --max-iter 256 --block-size 16 --palette 1
 */

#include <SDL2/SDL.h>
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
//  PARSING DE ARGUMENTOS
// ════════════════════════════════════════════════════════════

static const char* get_arg(int argc, char** argv, const char* key)
{
    for (int i = 1; i < argc - 1; i++) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return nullptr;
}

static int    arg_int(int argc, char** argv, const char* key, int    def)
{
    const char* v = get_arg(argc, argv, key);
    return v ? std::atoi(v) : def;
}

static double arg_dbl(int argc, char** argv, const char* key, double def)
{
    const char* v = get_arg(argc, argv, key);
    return v ? std::atof(v) : def;
}

// ════════════════════════════════════════════════════════════
//  ESTRUTURAS DE DADOS
// ════════════════════════════════════════════════════════════

/**
 * Task: descreve um bloco retangular de pixels a ser calculado.
 */
struct Task {
    int x0, y0;   // canto superior esquerdo, inclusivo
    int x1, y1;   // canto inferior direito, exclusivo
};

/**
 * ViewState: captura os parâmetros de visão de um frame específico.
 * Workers fazem cópia local para evitar leitura de estado em transição.
 */
struct ViewState {
    double cx, cy;    // centro no plano complexo
    double scale;     // unidades complexas por pixel (menor = mais zoom)
    int    max_iter;  // máximo de iterações
    int    palette;   // paleta de cores (0–4)
};

/**
 * SharedCtx: contexto compartilhado entre main e workers.
 */
struct SharedCtx {

    // ── Dimensões da janela ────────────────────────────────
    int win_w, win_h;

    // ── Fila de tarefas (produtor/consumidor) ──────────────
    std::queue<Task> task_queue;
    pthread_mutex_t  task_mutex;
    pthread_cond_t   task_cond;

    // ── Buffer de resultado ────────────────────────────────
    uint32_t* pixels;   // formato ARGB: 0xAARRGGBB, tamanho win_w × win_h

    // ── Estado de visão atual ──────────────────────────────
    ViewState view;

    // ── Conclusão de frame ─────────────────────────────────
    std::atomic<int> tasks_remaining;
    pthread_mutex_t  done_mutex;
    pthread_cond_t   done_cond;

    // ── Encerramento ───────────────────────────────────────
    bool shutdown;
};

// ════════════════════════════════════════════════════════════
//  CÁLCULO DO MANDELBROT E COLORAÇÃO
// ════════════════════════════════════════════════════════════

/**
 * Calcula a cor ARGB do ponto (px, py) no plano complexo.
 *
 * Otimizações:
 *  1. Verificação de cardioide e bulbo de período 2 — O(1)
 *  2. Detecção de período (algoritmo de Brent) — saída antecipada para interiores
 *  3. Smooth coloring — elimina faixas bruscas de cor
 */
static uint32_t compute_pixel(double px, double py, int max_iter, int palette)
{
    // ── Otimização 1: cardioide principal e bulbo de período 2 ───────────
    {
        double q = (px - 0.25) * (px - 0.25) + py * py;
        if (q * (q + px - 0.25) < 0.25 * py * py) return 0xFF000000;
        if ((px + 1.0) * (px + 1.0) + py * py < 0.0625) return 0xFF000000;
    }

    double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
    int    iter = 0;

    // ── Otimização 2: detecção de período (Brent) ────────────────────────
    double xold = 0.0, yold = 0.0;
    int    check_at = 4, since_check = 0;

    while (iter < max_iter && zr2 + zi2 <= 4.0) {
        zi  = 2.0 * zr * zi + py;
        zr  = zr2 - zi2 + px;
        zr2 = zr * zr;
        zi2 = zi * zi;
        iter++;

        if (std::abs(zr - xold) < 1e-10 && std::abs(zi - yold) < 1e-10)
            return 0xFF000000;

        if (++since_check == check_at) {
            xold = zr; yold = zi;
            since_check = 0;
            if (check_at < 512) check_at *= 2;
        }
    }

    if (iter == max_iter) return 0xFF000000;

    // ── Smooth coloring ──────────────────────────────────────────────────
    double log_zn = std::log(zr2 + zi2) * 0.5;
    double nu     = std::log(log_zn / std::log(2.0)) / std::log(2.0);
    double smooth = (double)(iter + 1) - nu;
    double t      = smooth / (double)max_iter;

    double r, g, b;

    switch (palette) {

        case 1: // Fogo — vermelho → laranja → amarelo
            r = std::min(1.0, t * 3.0);
            g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
            b = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
            break;

        case 2: // Oceano — azul profundo → ciano
            r = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
            g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
            b = std::min(1.0, t * 3.0);
            break;

        case 3: // Gold & Purple — ciclo de arco-íris dourado/roxo
            r = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.75));
            g = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.50));
            b = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.00));
            break;

        case 4: // Escala de cinza
            r = t; g = t; b = t;
            break;

        default: // Padrão — azul/verde/roxo (canais defasados 120°)
            r = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.00));
            g = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.33));
            b = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.67));
            break;
    }

    return 0xFF000000
         | ((uint32_t)(r * 255.0) << 16)
         | ((uint32_t)(g * 255.0) <<  8)
         |  (uint32_t)(b * 255.0);
}

// ════════════════════════════════════════════════════════════
//  THREAD TRABALHADORA (WORKER)
// ════════════════════════════════════════════════════════════

static void* worker_func(void* arg)
{
    SharedCtx* ctx = static_cast<SharedCtx*>(arg);

    while (true) {

        pthread_mutex_lock(&ctx->task_mutex);
        while (ctx->task_queue.empty() && !ctx->shutdown)
            pthread_cond_wait(&ctx->task_cond, &ctx->task_mutex);

        if (ctx->task_queue.empty()) {
            pthread_mutex_unlock(&ctx->task_mutex);
            break;
        }

        Task task = ctx->task_queue.front();
        ctx->task_queue.pop();
        pthread_mutex_unlock(&ctx->task_mutex);

        ViewState v = ctx->view;   // cópia local — seguro contra data race

        const int W = ctx->win_w;
        const int H = ctx->win_h;

        for (int py = task.y0; py < task.y1; py++) {
            for (int px = task.x0; px < task.x1; px++) {
                double cx = v.cx + (px - W * 0.5) * v.scale;
                double cy = v.cy + (py - H * 0.5) * v.scale;
                ctx->pixels[py * W + px] = compute_pixel(cx, cy, v.max_iter, v.palette);
            }
        }

        if (--ctx->tasks_remaining == 0) {
            pthread_mutex_lock(&ctx->done_mutex);
            pthread_cond_signal(&ctx->done_cond);
            pthread_mutex_unlock(&ctx->done_mutex);
        }
    }

    return nullptr;
}

// ════════════════════════════════════════════════════════════
//  PRODUÇÃO E ESPERA DE FRAME
// ════════════════════════════════════════════════════════════

static void dispatch_frame(SharedCtx* ctx, int block_size)
{
    const int W = ctx->win_w;
    const int H = ctx->win_h;

    std::vector<Task> tasks;
    tasks.reserve((W / block_size + 1) * (H / block_size + 1));

    for (int y = 0; y < H; y += block_size)
        for (int x = 0; x < W; x += block_size)
            tasks.push_back({ x, y,
                              std::min(x + block_size, W),
                              std::min(y + block_size, H) });

    ctx->tasks_remaining.store((int)tasks.size());

    pthread_mutex_lock(&ctx->task_mutex);
    for (const Task& t : tasks) ctx->task_queue.push(t);
    pthread_cond_broadcast(&ctx->task_cond);
    pthread_mutex_unlock(&ctx->task_mutex);
}

static void wait_frame_done(SharedCtx* ctx)
{
    pthread_mutex_lock(&ctx->done_mutex);
    while (ctx->tasks_remaining.load() > 0)
        pthread_cond_wait(&ctx->done_cond, &ctx->done_mutex);
    pthread_mutex_unlock(&ctx->done_mutex);
}

// ════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    // ── Lê todos os parâmetros (com valores padrão) ───────────
    const int    num_threads  = std::max(1,  arg_int(argc, argv, "--threads",    4));
    const int    max_iter     = std::max(16, arg_int(argc, argv, "--max-iter",   256));
    const int    block_size   = std::max(4,  arg_int(argc, argv, "--block-size", 32));
    const int    win_w        = std::max(200,arg_int(argc, argv, "--width",      900));
    const int    win_h        = std::max(200,arg_int(argc, argv, "--height",     900));
    const double zoom_x       = arg_dbl(argc, argv, "--zoom-x",      -0.7436438885706799);
    const double zoom_y       = arg_dbl(argc, argv, "--zoom-y",       0.1318259042053185);
    const double zoom_factor  = std::max(1.0001, arg_dbl(argc, argv, "--zoom-factor",  1.008));
    const int    frame_delay  = std::max(0,  arg_int(argc, argv, "--frame-delay", 0));
    const int    palette      = arg_int(argc, argv, "--palette", 0);

    const double scale_initial = 3.5 / win_w;
    const double scale_min     = 1e-13;

    // ── Banner de inicialização ───────────────────────────────
    printf("╔══ Mandelbrot ═════════════════════════════════════╗\n");
    printf("║  threads     = %d\n", num_threads);
    printf("║  max_iter    = %d\n", max_iter);
    printf("║  block_size  = %d×%d px\n", block_size, block_size);
    printf("║  janela      = %d×%d px\n", win_w, win_h);
    printf("║  zoom_target = (%.6f, %.6f)\n", zoom_x, zoom_y);
    printf("║  zoom_factor = %.4f\n", zoom_factor);
    printf("║  frame_delay = %d ms\n", frame_delay);
    printf("║  palette     = %d\n", palette);
    printf("╚═══════════════════════════════════════════════════╝\n");
    printf("Pressione ESC ou feche a janela para sair.\n\n");

    // ── Contexto compartilhado ────────────────────────────────
    SharedCtx ctx;
    ctx.shutdown = false;
    ctx.tasks_remaining.store(0);
    ctx.win_w  = win_w;
    ctx.win_h  = win_h;
    ctx.pixels = new uint32_t[win_w * win_h];

    pthread_mutex_init(&ctx.task_mutex, nullptr);
    pthread_cond_init (&ctx.task_cond,  nullptr);
    pthread_mutex_init(&ctx.done_mutex, nullptr);
    pthread_cond_init (&ctx.done_cond,  nullptr);

    ctx.view = { zoom_x, zoom_y, scale_initial, max_iter, palette };

    // ── Pool de workers ───────────────────────────────────────
    std::vector<pthread_t> workers(num_threads);
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&workers[i], nullptr, worker_func, &ctx);
        printf("  Worker thread %d criada.\n", i);
    }

    // ── SDL2 ──────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Mandelbrot — Computação de Alto Desempenho",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, 0
    );
    if (!window) { fprintf(stderr, "SDL_CreateWindow falhou: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) { fprintf(stderr, "SDL_CreateRenderer falhou: %s\n", SDL_GetError()); return 1; }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, win_w, win_h
    );

    // ── Loop de renderização ──────────────────────────────────
    bool     running    = true;
    uint64_t frame_no   = 0;
    uint64_t fps_frames = 0;
    Uint32   fps_t0     = SDL_GetTicks();
    Uint32   last_frame_ts = 0;

    while (running) {

        // Renderização progressiva: atualiza tela enquanto workers calculam
        dispatch_frame(&ctx, block_size);

        while (ctx.tasks_remaining.load() > 0) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
            }
            if (!running) break;

            SDL_UpdateTexture(texture, nullptr, ctx.pixels, win_w * sizeof(uint32_t));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        wait_frame_done(&ctx);
        if (!running) break;

        SDL_UpdateTexture(texture, nullptr, ctx.pixels, win_w * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        // Throttle de frame
        if (frame_delay > 0) {
            Uint32 elapsed = SDL_GetTicks() - last_frame_ts;
            if (elapsed < (Uint32)frame_delay)
                SDL_Delay((Uint32)frame_delay - elapsed);
            last_frame_ts = SDL_GetTicks();
        }

        // Avança zoom
        ctx.view.scale /= zoom_factor;
        frame_no++;

        // max_iter dinâmico (cresce com zoom, sem explodir)
        double zoom_depth     = scale_initial / ctx.view.scale;
        double growth         = 16.0 * std::sqrt(std::log2(1.0 + zoom_depth));
        ctx.view.max_iter     = max_iter + (int)growth;
        ctx.view.max_iter     = std::min(ctx.view.max_iter, max_iter * 4);

        // Reinicia ao atingir limite de precisão do double
        if (ctx.view.scale < scale_min) {
            printf("Limite de precisão atingido. Reiniciando (frame %llu).\n",
                   (unsigned long long)frame_no);
            ctx.view.scale    = scale_initial;
            ctx.view.max_iter = max_iter;
            frame_no = 0;
        }

        // Atualiza título com FPS e nível de zoom
        fps_frames++;
        Uint32 now = SDL_GetTicks();
        if (now - fps_t0 >= 1000) {
            double zoom = scale_initial / ctx.view.scale;
            char title[160];
            snprintf(title, sizeof(title),
                     "Mandelbrot | FPS: %llu | Zoom: %.2e×  [threads=%d  iter=%d  paleta=%d]",
                     (unsigned long long)fps_frames, zoom, num_threads,
                     ctx.view.max_iter, palette);
            SDL_SetWindowTitle(window, title);
            fps_frames = 0;
            fps_t0 = now;
        }
    }

    // ── Encerramento limpo ────────────────────────────────────
    pthread_mutex_lock(&ctx.task_mutex);
    ctx.shutdown = true;
    pthread_cond_broadcast(&ctx.task_cond);
    pthread_mutex_unlock(&ctx.task_mutex);

    for (pthread_t& t : workers) pthread_join(t, nullptr);
    printf("Threads encerradas. Total de frames: %llu\n", (unsigned long long)frame_no);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    pthread_mutex_destroy(&ctx.task_mutex);
    pthread_cond_destroy (&ctx.task_cond);
    pthread_mutex_destroy(&ctx.done_mutex);
    pthread_cond_destroy (&ctx.done_cond);
    delete[] ctx.pixels;

    return 0;
}
