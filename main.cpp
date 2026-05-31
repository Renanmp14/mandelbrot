/**
 * main.cpp — Fractal de Mandelbrot com Zoom Contínuo
 * Disciplina: Computação de Alto Desempenho
 *
 * Arquitetura: Pool de Threads (Pthreads) + Renderização SDL2
 * ─────────────────────────────────────────────────────────────
 *  Main thread  →  Produz tarefas (blocos da tela) e renderiza
 *  Worker threads → Consomem tarefas e calculam os pixels
 *
 * Uso:
 *   ./mandelbrot <num_threads> <max_iter> <block_size>
 *
 * Exemplo:
 *   ./mandelbrot 4 256 32
 */

#include <SDL2/SDL.h>
#include <pthread.h>

#include <queue>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <algorithm>

// ════════════════════════════════════════════════════════════
//  CONFIGURAÇÕES GLOBAIS
// ════════════════════════════════════════════════════════════

static const int    WIN_W          = 900;
static const int    WIN_H          = 900;

// Ponto alvo do zoom: ponta de uma espiral no Seahorse Valley.
// Este ponto fica exatamente na FRONTEIRA do conjunto de Mandelbrot,
// garantindo estrutura fractal visível em qualquer nível de zoom.
// (O ponto anterior -0.7269/0.1889 ficava levemente dentro de um "lago"
//  interno do conjunto, fazendo a tela ficar totalmente preta em zoom profundo.)
static const double ZOOM_TARGET_X  = -0.7436438885706799;
static const double ZOOM_TARGET_Y  =  0.1318259042053185;

// Escala inicial: enquadra o conjunto de Mandelbrot inteiro na tela (~3.5 unidades)
static const double SCALE_INITIAL  = 3.5 / WIN_W;

// Fator de zoom por frame — deve ser MAIOR que 1.0 para aproximar a câmera.
// Quanto mais próximo de 1.0, mais lento e suave o zoom.
//   1.005 = +0.5%/frame → muito lento, cinematográfico
//   1.008 = +0.8%/frame → lento e suave       ← atual
//   1.015 = +1.5%/frame → moderado
//   1.018 = +1.8%/frame → rápido (original)
static const double ZOOM_FACTOR    = 1.008;

// Limite de precisão do double (~15 dígitos); abaixo disso reinicia o zoom
static const double SCALE_MIN      = 1e-13;

// Intervalo mínimo entre frames em milissegundos (0 = renderiza o mais rápido possível)
// Aumentar esse valor reduz a frequência de novos renders, aliviando o CPU.
//   0   ms → sem limite, máxima velocidade
//   16  ms → ~60 FPS
//   33  ms → ~30 FPS
//   100 ms → ~10 FPS
static const Uint32 FRAME_DELAY_MS = 0;

// ════════════════════════════════════════════════════════════
//  ESTRUTURAS DE DADOS
// ════════════════════════════════════════════════════════════

/**
 * Task: descreve um bloco retangular de pixels a ser calculado.
 * A tela inteira é dividida em N tarefas pelo produtor (main thread).
 */
struct Task {
    int x0, y0;   // canto superior esquerdo, inclusivo
    int x1, y1;   // canto inferior direito, exclusivo
};

/**
 * ViewState: captura os parâmetros de visão de um frame específico.
 * As workers fazem uma cópia local para evitar leitura de estado em transição.
 */
struct ViewState {
    double cx, cy;   // centro da visão no plano complexo
    double scale;    // unidades complexas por pixel (menor = mais zoom)
    int    max_iter; // máximo de iterações do algoritmo de Mandelbrot
};

/**
 * SharedCtx: contexto compartilhado entre a main thread e todas as workers.
 *
 * Sincronização:
 *  • task_mutex + task_cond   → protegem a fila de tarefas
 *  • done_mutex + done_cond   → sinalizam conclusão do frame à main thread
 *  • tasks_remaining (atomic) → contador de tarefas pendentes no frame atual
 *  • pixels (sem mutex)       → cada Task cobre pixels disjuntos, sem conflito
 */
struct SharedCtx {

    // ── Fila de tarefas (produtor/consumidor) ──────────────
    std::queue<Task>  task_queue;
    pthread_mutex_t   task_mutex;   // protege task_queue
    pthread_cond_t    task_cond;    // acorda workers quando há tarefas

    // ── Buffer de resultado ────────────────────────────────
    // Formato ARGB: 0xAARRGGBB — tamanho WIN_W × WIN_H pixels
    uint32_t* pixels;

    // ── Estado de visão atual ──────────────────────────────
    // Escrito apenas pela main thread (após wait_frame_done); lido pelas workers
    ViewState view;

    // ── Conclusão de frame ─────────────────────────────────
    std::atomic<int>  tasks_remaining;  // decrementado por cada worker ao terminar
    pthread_mutex_t   done_mutex;
    pthread_cond_t    done_cond;        // sinalizado quando tasks_remaining == 0

    // ── Encerramento ───────────────────────────────────────
    bool shutdown;                      // protegido por task_mutex
};

// ════════════════════════════════════════════════════════════
//  CÁLCULO DO MANDELBROT E COLORAÇÃO
// ════════════════════════════════════════════════════════════

/**
 * Calcula a cor ARGB do ponto (px, py) no plano complexo.
 *
 * Otimizações aplicadas (em ordem de execução):
 *
 *  1. Verificação de cardioide e bulbo de período 2
 *     O conjunto de Mandelbrot tem dois corpos principais que cobrem a maior
 *     parte da área visível em zoom inicial. Verificar se o ponto está dentro
 *     deles é O(1) e evita o loop inteiro para esses pixels.
 *
 *  2. Detecção de período — algoritmo de Brent
 *     Pontos no interior do conjunto ficam presos em órbitas periódicas
 *     (o valor de z começa a se repetir). Sem essa verificação, o loop roda
 *     até max_iter mesmo para pixels totalmente interiores.
 *     Estratégia: a cada 'check_at' iterações, compara z atual com o z
 *     salvo no último checkpoint. Se forem iguais (dentro de epsilon), a
 *     órbita é periódica → ponto interior → retorna preto imediatamente.
 *     O intervalo dobra exponencialmente (4 → 8 → 16 → ... → 512) para
 *     cobrir períodos longos sem verificar a cada iteração.
 *
 *  3. Smooth coloring (apenas para pontos externos)
 *     Usa o módulo final de z para interpolar entre iterações, eliminando
 *     as faixas bruscas de cor na fronteira do conjunto.
 */
static uint32_t compute_pixel(double px, double py, int max_iter)
{
    // ── Otimização 1: cardioide principal e bulbo de período 2 ───────────
    // Fórmula exata para o interior da cardioide: q*(q + Re(c) - 0.25) < Im(c)²/4
    // Fórmula exata para o bulbo de período 2:   |c + 1|² < 0.0625
    {
        double q = (px - 0.25) * (px - 0.25) + py * py;
        if (q * (q + px - 0.25) < 0.25 * py * py) return 0xFF000000;
        if ((px + 1.0) * (px + 1.0) + py * py < 0.0625) return 0xFF000000;
    }

    double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
    int    iter = 0;

    // ── Otimização 2: detecção de período (Brent) ────────────────────────
    double xold = 0.0, yold = 0.0;   // último ponto de referência salvo
    int    check_at    = 4;           // intervalo atual de verificação
    int    since_check = 0;           // iterações desde o último checkpoint

    while (iter < max_iter && zr2 + zi2 <= 4.0) {
        zi  = 2.0 * zr * zi + py;
        zr  = zr2 - zi2 + px;
        zr2 = zr * zr;
        zi2 = zi * zi;
        iter++;

        // Compara z atual com o ponto de referência salvo
        if (std::abs(zr - xold) < 1e-10 && std::abs(zi - yold) < 1e-10)
            return 0xFF000000;  // órbita cíclica → interior do conjunto

        // Atualiza checkpoint e dobra o intervalo (até 512)
        if (++since_check == check_at) {
            xold       = zr;
            yold       = zi;
            since_check = 0;
            if (check_at < 512) check_at *= 2;
        }
    }

    // Interior do conjunto (esgotou iterações sem escapar) → preto
    if (iter == max_iter) return 0xFF000000;

    // ── Smooth coloring ──────────────────────────────────────────────────
    // log_zn  = log(|z|)
    // nu      = log₂(log₂(|z|)) — normalização logarítmica dupla
    // smooth  = valor contínuo que elimina bandas bruscas de cor
    double log_zn = std::log(zr2 + zi2) * 0.5;
    double nu     = std::log(log_zn / std::log(2.0)) / std::log(2.0);
    double smooth = (double)(iter + 1) - nu;

    // Paleta via funções seno com 3 canais defasados em 120°
    double t = smooth / (double)max_iter;
    double r = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.00));
    double g = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.33));
    double b = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.67));

    return 0xFF000000
         | ((uint32_t)(r * 255.0) << 16)
         | ((uint32_t)(g * 255.0) <<  8)
         |  (uint32_t)(b * 255.0);
}

// ════════════════════════════════════════════════════════════
//  FUNÇÃO DA THREAD TRABALHADORA (WORKER)
// ════════════════════════════════════════════════════════════

/**
 * Loop executado por cada worker thread:
 *   1. Aguarda tarefa na fila (condition variable)
 *   2. Retira a tarefa
 *   3. Calcula todos os pixels do bloco
 *   4. Decrementa tasks_remaining; se chegou a 0, acorda a main thread
 */
static void* worker_func(void* arg)
{
    SharedCtx* ctx = static_cast<SharedCtx*>(arg);

    while (true) {

        // ── 1. Aguarda e retira uma tarefa da fila ────────────
        pthread_mutex_lock(&ctx->task_mutex);

        // Espera enquanto a fila está vazia e não foi pedido encerramento
        while (ctx->task_queue.empty() && !ctx->shutdown) {
            pthread_cond_wait(&ctx->task_cond, &ctx->task_mutex);
        }

        // Encerramento solicitado sem tarefas pendentes → sai
        if (ctx->task_queue.empty()) {
            pthread_mutex_unlock(&ctx->task_mutex);
            break;
        }

        Task task = ctx->task_queue.front();
        ctx->task_queue.pop();
        pthread_mutex_unlock(&ctx->task_mutex);

        // ── 2. Captura estado de visão (cópia local) ──────────
        // Seguro: a main thread não altera ctx->view enquanto há tasks ativas
        ViewState v = ctx->view;

        // ── 3. Calcula os pixels do bloco ──────────────────────
        for (int py = task.y0; py < task.y1; py++) {
            for (int px = task.x0; px < task.x1; px++) {

                // Converte coordenada de pixel → plano complexo
                // O centro da tela corresponde a (v.cx, v.cy)
                double cx = v.cx + (px - WIN_W * 0.5) * v.scale;
                double cy = v.cy + (py - WIN_H * 0.5) * v.scale;

                // Grava resultado diretamente no buffer — sem mutex pois
                // cada task cobre uma região exclusiva da imagem
                ctx->pixels[py * WIN_W + px] = compute_pixel(cx, cy, v.max_iter);
            }
        }

        // ── 4. Sinaliza conclusão da tarefa ───────────────────
        // Decremento atômico; se chegou a zero, este era o último bloco do frame
        if (--ctx->tasks_remaining == 0) {
            pthread_mutex_lock(&ctx->done_mutex);
            pthread_cond_signal(&ctx->done_cond);
            pthread_mutex_unlock(&ctx->done_mutex);
        }
    }

    return nullptr;
}

// ════════════════════════════════════════════════════════════
//  PRODUÇÃO E ESPERA DE FRAME (CHAMADAS DA MAIN THREAD)
// ════════════════════════════════════════════════════════════

/**
 * Divide a tela em blocos de block_size×block_size pixels e enfileira
 * as tarefas. Deve ser chamado APÓS atualizar ctx->view.
 *
 * Define tasks_remaining ANTES de enfileirar, garantindo que o
 * contador já está correto mesmo que uma worker processe muito rápido.
 */
static void dispatch_frame(SharedCtx* ctx, int block_size)
{
    std::vector<Task> tasks;
    tasks.reserve((WIN_W / block_size + 1) * (WIN_H / block_size + 1));

    for (int y = 0; y < WIN_H; y += block_size) {
        for (int x = 0; x < WIN_W; x += block_size) {
            tasks.push_back({
                x,
                y,
                std::min(x + block_size, WIN_W),
                std::min(y + block_size, WIN_H)
            });
        }
    }

    // Registra quantas tarefas devem ser concluídas ANTES de enfileirar
    ctx->tasks_remaining.store((int)tasks.size());

    pthread_mutex_lock(&ctx->task_mutex);
    for (const Task& t : tasks) ctx->task_queue.push(t);
    pthread_cond_broadcast(&ctx->task_cond);   // acorda todas as workers em espera
    pthread_mutex_unlock(&ctx->task_mutex);
}

/**
 * Bloqueia a main thread até que todas as tarefas do frame sejam concluídas.
 *
 * Uso de done_mutex garante ausência de "lost wakeup":
 *   Se a última task terminar ANTES de entrarmos no cond_wait,
 *   a checagem tasks_remaining > 0 já será falsa e não esperamos.
 */
static void wait_frame_done(SharedCtx* ctx)
{
    pthread_mutex_lock(&ctx->done_mutex);
    while (ctx->tasks_remaining.load() > 0) {
        pthread_cond_wait(&ctx->done_cond, &ctx->done_mutex);
    }
    pthread_mutex_unlock(&ctx->done_mutex);
}

// ════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    // ── Parâmetros via linha de comando ───────────────────────
    int num_threads = (argc >= 2) ? std::atoi(argv[1]) : 4;
    int max_iter    = (argc >= 3) ? std::atoi(argv[2]) : 256;
    int block_size  = (argc >= 4) ? std::atoi(argv[3]) : 32;

    if (num_threads < 1) num_threads = 1;
    if (max_iter    < 16) max_iter   = 16;
    if (block_size  < 4)  block_size = 4;

    printf("╔══ Mandelbrot ══════════════════════════════╗\n");
    printf("║  threads   = %d\n", num_threads);
    printf("║  max_iter  = %d\n", max_iter);
    printf("║  block_size= %d×%d px\n", block_size, block_size);
    printf("║  janela    = %d×%d px\n", WIN_W, WIN_H);
    printf("╚════════════════════════════════════════════╝\n");
    printf("Pressione ESC ou feche a janela para sair.\n\n");

    // ── Inicializa o contexto compartilhado ───────────────────
    SharedCtx ctx;
    ctx.shutdown = false;
    ctx.tasks_remaining.store(0);
    ctx.pixels = new uint32_t[WIN_W * WIN_H];

    pthread_mutex_init(&ctx.task_mutex, nullptr);
    pthread_cond_init (&ctx.task_cond,  nullptr);
    pthread_mutex_init(&ctx.done_mutex, nullptr);
    pthread_cond_init (&ctx.done_cond,  nullptr);

    // Estado de visão inicial: centro apontado para o alvo, campo aberto
    ctx.view = { ZOOM_TARGET_X, ZOOM_TARGET_Y, SCALE_INITIAL, max_iter };

    // ── Cria o pool de threads trabalhadoras ──────────────────
    std::vector<pthread_t> workers(num_threads);
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&workers[i], nullptr, worker_func, &ctx);
        printf("  Worker thread %d criada.\n", i);
    }

    // ── Inicializa SDL2 ───────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Mandelbrot — Computação de Alto Desempenho",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, 0
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow falhou: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer falhou: %s\n", SDL_GetError());
        return 1;
    }

    // Textura de streaming: atualizamos a cada frame com nosso buffer de pixels
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,       // formato: 0xAARRGGBB (compatível com nosso buffer)
        SDL_TEXTUREACCESS_STREAMING,
        WIN_W, WIN_H
    );

    // ── Loop principal de renderização ────────────────────────
    bool     running    = true;
    uint64_t frame_no   = 0;
    uint64_t fps_frames = 0;
    Uint32   fps_t0     = SDL_GetTicks();

    while (running) {

        // ─── Fase de computação com renderização progressiva ─────
        // Enfileira os blocos e atualiza a tela enquanto os workers calculam.
        // Assim é possível VER cada bloco sendo preenchido conforme as threads
        // terminam — em vez de esperar tudo pronto para mostrar de uma vez.
        dispatch_frame(&ctx, block_size);

        while (ctx.tasks_remaining.load() > 0) {
            // Processa eventos mesmo durante o cálculo (janela continua responsiva)
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                if (ev.type == SDL_KEYDOWN &&
                    ev.key.keysym.sym == SDLK_ESCAPE) running = false;
            }
            if (!running) break;

            // Exibe o estado parcial: blocos prontos aparecem, os outros mostram
            // o frame anterior enquanto aguardam ser calculados
            SDL_UpdateTexture(texture, nullptr, ctx.pixels, WIN_W * sizeof(uint32_t));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        // Aguarda conclusão total antes de avançar o zoom
        wait_frame_done(&ctx);
        if (!running) break;

        // Exibe o frame completo final
        SDL_UpdateTexture(texture, nullptr, ctx.pixels, WIN_W * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        // ─── Throttle de frame ────────────────────────────────
        // Garante que o loop não rode mais rápido que FRAME_DELAY_MS por frame.
        // Se o cálculo já levou mais que o delay, não espera nada.
        if (FRAME_DELAY_MS > 0) {
            static Uint32 last_frame = 0;
            Uint32 elapsed = SDL_GetTicks() - last_frame;
            if (elapsed < FRAME_DELAY_MS)
                SDL_Delay(FRAME_DELAY_MS - elapsed);
            last_frame = SDL_GetTicks();
        }

        // ─── Avança o zoom ────────────────────────────────────
        // Divide a escala (menos unidades/pixel = mais zoom)
        ctx.view.scale /= ZOOM_FACTOR;
        frame_no++;

        // max_iter dinâmico: cresce com o zoom para manter detalhe na fronteira.
        // Usa raiz quadrada do log para crescer bem mais devagar em zoom extremo:
        //   zoom 10×    → base + ~11   iters
        //   zoom 1000×  → base + ~50   iters
        //   zoom 10⁶×   → base + ~100  iters   (vs ~299 com a fórmula anterior)
        // O cap em 4× o base garante que nunca explode independente do zoom.
        double zoom_depth = SCALE_INITIAL / ctx.view.scale;
        double growth     = 16.0 * std::sqrt(std::log2(1.0 + zoom_depth));
        ctx.view.max_iter = max_iter + (int)growth;
        ctx.view.max_iter = std::min(ctx.view.max_iter, max_iter * 4); // cap em 4×

        // Reinicia quando o double não tem mais precisão suficiente
        if (ctx.view.scale < SCALE_MIN) {
            printf("Limite de precisão atingido. Reiniciando zoom (frame %llu).\n",
                   (unsigned long long)frame_no);
            ctx.view.scale    = SCALE_INITIAL;
            ctx.view.max_iter = max_iter; // volta ao max_iter base
            frame_no = 0;
        }

        // ─── Atualiza título com FPS e zoom ───────────────────
        fps_frames++;
        Uint32 now = SDL_GetTicks();
        if (now - fps_t0 >= 1000) {
            double zoom = SCALE_INITIAL / ctx.view.scale;
            char title[128];
            snprintf(title, sizeof(title),
                     "Mandelbrot | FPS: %llu | Zoom: %.2e×  [threads=%d]",
                     (unsigned long long)fps_frames, zoom, num_threads);
            SDL_SetWindowTitle(window, title);
            fps_frames = 0;
            fps_t0 = now;
        }
    }

    // ── Encerramento limpo ────────────────────────────────────
    // Ativa flag de shutdown e acorda todas as workers para que saiam do loop
    pthread_mutex_lock(&ctx.task_mutex);
    ctx.shutdown = true;
    pthread_cond_broadcast(&ctx.task_cond);
    pthread_mutex_unlock(&ctx.task_mutex);

    for (pthread_t& t : workers) pthread_join(t, nullptr);
    printf("Todas as threads encerradas. Total de frames: %llu\n",
           (unsigned long long)frame_no);

    // Libera recursos SDL
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // Libera primitivos e memória
    pthread_mutex_destroy(&ctx.task_mutex);
    pthread_cond_destroy (&ctx.task_cond);
    pthread_mutex_destroy(&ctx.done_mutex);
    pthread_cond_destroy (&ctx.done_cond);
    delete[] ctx.pixels;

    return 0;
}
