# Mandelbrot — Sem barreira de frame

## Compilação

```bash
g++ -O2 -o mandelbrot main.cpp -lmingw32 -lSDL2main -lSDL2 -lpthread
```

> **Requisitos:** GCC com suporte a C++17, SDL2, pthreads (MSYS2/UCRT64 no Windows ou qualquer distro Linux).

---

## Execução

```bash
./mandelbrot [opções]
```

| Parâmetro | Padrão | Descrição |
|---|---|---|
| `--threads N` | `4` | Número de threads trabalhadoras |
| `--max-iter N` | `64` | Iterações base do Mandelbrot |
| `--block-size N` | `64` | Lado do bloco em pixels |
| `--width N` | `900` | Largura da janela |
| `--height N` | `900` | Altura da janela |
| `--zoom-x F` | `-0.7436…` | Coordenada X do alvo do zoom |
| `--zoom-y F` | `0.1318…` | Coordenada Y do alvo do zoom |
| `--zoom-factor F` | `1.008` | Fator de zoom por fase |
| `--palette N` | `0` | Paleta de cores (0–4) |
| `--phase-cap N` | `0` | Fase máxima por bloco (0 = infinito) |
| `--overlay N` | `1` | Ativa o overlay de custo (1=ativo, 0=desativado) |
| `--overlay-alpha N` | `80` | Opacidade do overlay (0–255) |
| `--overlay-threshold F` | `0.90` | Percentil de custo a partir do qual o overlay exibe vermelho |

**Exemplo:**
```bash
./mandelbrot --threads 4 --block-size 64 --max-iter 32
```

---

## Paletas

| Código | Nome | Gradiente |
|---|---|---|
| `0` | Padrão | Azul → verde → roxo |
| `1` | Fogo | Vermelho → laranja → amarelo |
| `2` | Oceano | Preto → azul profundo → ciano |
| `3` | Gold & Purple | Ciclo dourado/roxo |
| `4` | Cinza | Escala de cinza |

---

## Arquitetura

### O problema com a arquitetura de frames

Na abordagem clássica, cada frame exige que **todas** as threads terminem antes que a próxima iteração de zoom comece:

```
dispatch_frame() → wait_all_tasks() → advance_zoom() → repete
```

Isso cria uma barreira de sincronização: a thread mais lenta define o ritmo de todas as outras. Threads que terminam seus blocos rapidamente ficam ociosas esperando.

### Abordagem "Fases paralelas sem barreira"

Nesta implementação, a noção de "frame" é eliminada completamente. Cada bloco da tela carrega sua própria fase de zoom e se autoperpetua:

```
worker pega bloco  →  calcula pixels  →  escreve buffer  →  reinicia com phase+1
```

Não existe sincronização entre blocos. Regiões computacionalmente simples (bordas do conjunto) avançam centenas de fases à frente de regiões densas (interior, filamentos). A tela exibe simultaneamente diferentes níveis de zoom para diferentes regiões.

---

## Componentes do código

### `struct Task`

```cpp
struct Task {
    int      x0, y0, x1, y1;
    uint64_t phase;
};
```

Unidade básica de trabalho. Cada `Task` descreve um bloco retangular da tela e o nível de zoom (`phase`) em que deve ser calculado. Uma `Task` na fase `N` produz uma `Task` na fase `N+1` ao terminar — sem nenhuma coordenação central.

---

### `struct Config`

Parâmetros globais imutáveis após a inicialização. Workers leem diretamente, sem mutex, porque nenhum campo é modificado durante a execução.

Todos os campos são imutáveis após a inicialização. Workers leem diretamente, sem mutex.

---

### `struct SharedCtx`

Estado verdadeiramente compartilhado entre threads:

| Campo | Tipo | Uso |
|---|---|---|
| `pixels` | `uint32_t*` | Buffer de saída ARGB (escrito pelos workers) |
| `max_phase_seen` | `atomic<uint64_t>` | Fase máxima global, para telemetria |
| `task_queue` | `queue<Task>` | Fila de tarefas pendentes |
| `task_mutex` / `task_cond` | mutex + condvar | Sincronização produtor/consumidor |
| `shutdown` | `bool` | Sinal de encerramento |

O buffer `pixels` não precisa de mutex porque blocos distintos mapeiam para regiões distintas da memória — não há sobreposição de escrita entre workers.

---

### `compute_pixel`

Kernel do Mandelbrot. Dado um ponto `(px, py)` no plano complexo, retorna a cor ARGB.

**Otimização 1 — Cardioide e bulbo de período 2:**
Verifica analiticamente se o ponto pertence às duas maiores regiões do conjunto (que sempre divergem para infinito). Retorna preto imediatamente, evitando o loop de iteração.

**Otimização 2 — Detecção de período (algoritmo de Brent):**
Pontos no interior do conjunto nunca escapam, fazendo o loop iterar até `max_iter`. O algoritmo de Brent detecta quando a órbita entra em ciclo (o ponto com certeza está no conjunto) e retorna preto antes de esgotar `max_iter`.

**Smooth coloring:**
Sem essa técnica, a cor seria determinada pelo número inteiro de iterações, criando faixas bruscas. O smooth coloring extrapola uma contagem fracionária usando o módulo final de `z`, eliminando as faixas.

```
t = (iter + 1 - log(log|z|) / log(2)) / max_iter
```

---

### `worker_func`

Loop principal de cada thread trabalhadora:

```
1. Bloqueia no mutex aguardando tarefa na fila
2. Retira uma Task da fila
3. Calcula scale = exp(log(scale_initial) - phase × log(zoom_factor))
4. Determina max_iter (dinâmico por zoom, igual para todos os blocos na mesma fase)
5. Calcula todos os pixels do bloco
6. Registra iter_count e us_elapsed em g_metrics[idx] (para o overlay)
7. Reinsere o bloco com phase+1 na fila
```

A escala usa logaritmo para evitar underflow numérico em fases muito altas (`phase` pode ser `uint64_t` — valores enormes). Quando a escala cai abaixo de `scale_min` (limite de precisão do `double`), o bloco reinicia da fase 0.

---

### `seed_queue`

Chamada uma única vez no início. Insere todos os blocos da grade na fila com `phase = 0`. A partir daí, cada worker perpetua seus próprios blocos — a fila nunca esvazia (exceto com `--phase-cap`).

---

### Overlay e grid (loop de renderização)

A main thread não coordena workers. Ela apenas:

1. Copia `ctx.pixels` para uma textura SDL2 (`SDL_UpdateTexture`)
2. Renderiza o fractal
3. **Overlay de custo:** para cada bloco, lê `g_metrics[idx].us_elapsed` e desenha um retângulo vermelho semitransparente nos blocos acima do `overlay_threshold` (os mais caros relativamente)
4. **Grid:** linhas brancas semitransparentes delimitando cada bloco
5. Atualiza o título com FPS, fase máxima, custo mínimo e custo máximo em µs

O overlay usa `SDL_BLENDMODE_BLEND` com alpha ≈ 60%, permitindo que o fractal apareça por baixo da coloração de fase.

---

### `g_metrics`

```cpp
struct BlockMetrics {
    std::atomic<uint64_t> iter_count;
    std::atomic<uint64_t> us_elapsed;
};
static BlockMetrics* g_metrics;
```

Array global, um `BlockMetrics` por bloco da grade. Workers escrevem com `memory_order_relaxed` ao terminar cada bloco — sem garantia de ordem, pois é apenas telemetria visual. A main thread lê com `relaxed` para montar o overlay.

O índice de um bloco é `(y0 / block_size) * g_blocks_x + (x0 / block_size)`.

---

## Métricas no título da janela

```
Mandelbrot | FPS:60 | fase_max:847 | custo min:120µs max:4300µs
```

| Métrica | Significado |
|---|---|
| `FPS` | Frames por segundo da thread de renderização |
| `fase_max` | Maior fase já alcançada por qualquer bloco |
| `custo min` | Tempo de processamento do bloco mais barato no último segundo |
| `custo max` | Tempo de processamento do bloco mais caro no último segundo |

Uma diferença grande entre `custo min` e `custo max` confirma que os blocos estão evoluindo de forma genuinamente assíncrona — blocos baratos acumulam muitas mais fases que os caros.

---

## Garantias de correção

| Situação | Tratamento |
|---|---|
| Dois workers escrevem no mesmo pixel | Impossível por design — blocos são regiões exclusivas |
| Main lê pixel sendo escrito | Artefato visual passageiro, sem crash (leitura parcial de uint32) |
| Phase atinge limite de precisão | Bloco reinicia do zero automaticamente |
| Encerramento com fila ocupada | `shutdown = true` + `broadcast` acorda e encerra todos os workers |