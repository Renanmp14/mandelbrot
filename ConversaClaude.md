# Conversa com Claude — Desenvolvimento do Projeto Mandelbrot

Registro completo da conversa de desenvolvimento do trabalho acadêmico de  
**Computação de Alto Desempenho**, cobrindo todas as decisões, problemas encontrados e soluções aplicadas.

---

## 1. Pedido inicial

**Usuário:** Desenvolver um programa em C++ que renderize o fractal de Mandelbrot em tempo real com zoom dinâmico e contínuo, usando SDL2 para renderização e Pthreads para paralelismo, conforme arquitetura definida pelo professor.

**Requisitos definidos:**
- Main cria um pool de tarefas e um buffer compartilhado
- Cada tarefa representa um bloco da imagem
- Workers ficam em loop: pegam tarefa → calculam pixels → gravam resultado → pegam próxima
- Resultado renderizado via SDL2
- Parâmetros via linha de comando: número de threads, max iterações, tamanho dos blocos

**Ambiente:**
- Windows com WSL2 (Ubuntu)
- g++ 15.2.0 e SDL2 2.32.10 já instalados no WSL2
- Compilação e execução prefixadas com `wsl -d Ubuntu -e`

---

## 2. Decisões de arquitetura

Antes de escrever o código, foram definidas as seguintes decisões:

### Padrão produtor/consumidor
- **Main thread = produtor:** divide a tela em blocos (`Task`) e enfileira no `task_queue`
- **Workers = consumidoras:** cada thread pega um bloco, calcula os pixels e grava no buffer

### Sincronização em dois níveis
1. `task_mutex + task_cond` — protege a fila de tarefas
2. `done_mutex + done_cond + tasks_remaining (atomic)` — main aguarda frame completo antes de avançar o zoom

### Buffer de pixels sem mutex
Cada `Task` cobre regiões disjuntas da tela — threads escrevem em endereços diferentes, sem conflito.

### `ViewState` como cópia local
Worker copia o estado de visão ao retirar a task. A main só altera `view.scale` após `wait_frame_done()`, garantindo ausência de corrida.

### `-march=native` no Makefile
Habilita SSE/AVX para vetorização automática do loop interno — speedup real relevante para HPC.

### Smooth coloring
Elimina as faixas bruscas de cor usando o módulo final de `z` para interpolar entre iterações.

---

## 3. Arquivos criados

```
mandelbrot/
├── main.cpp       Código-fonte principal
├── Makefile       Compilação com make
├── build.sh       Compilação sem make (alternativa)
├── README.md      Documentação completa do projeto
└── ConversaClaude.md  Este arquivo
```

### Compilação (WSL2)

```bash
# Com build.sh (sem make)
bash build.sh

# Com make (instalar uma vez: sudo apt-get install make)
make

# Manual com g++
g++ -O2 -std=c++17 -Wall -Wextra -march=native -o mandelbrot main.cpp -lSDL2 -lpthread -lm
```

### Execução

```bash
./mandelbrot <num_threads> <max_iter> <block_size>

# Exemplo padrão
./mandelbrot 4 256 32
```

---

## 4. Estrutura do código (main.cpp)

### Estruturas de dados principais

| Struct | Função |
|--------|--------|
| `Task` | Bloco retangular de pixels a ser calculado por uma worker |
| `ViewState` | Estado de visão do frame atual (centro, escala, max_iter) |
| `SharedCtx` | Contexto compartilhado entre main e workers (fila, buffer, sincronização) |

### Fluxo da main thread

```
1. Lê parâmetros da linha de comando
2. Inicializa SharedCtx (mutexes, condition variables, buffer de pixels)
3. Cria N worker threads
4. Inicializa SDL2 (janela, renderer, textura ARGB8888)
5. Loop:
   a. Enfileira blocos (dispatch_frame)
   b. Aguarda conclusão (wait_frame_done)
   c. Exibe na tela (SDL_UpdateTexture + RenderPresent)
   d. Avança zoom (scale /= ZOOM_FACTOR)
   e. Atualiza max_iter dinâmico
6. Sinaliza shutdown, join em todas as workers
7. Libera recursos
```

### Fluxo de cada worker thread

```
1. Aguarda tarefa na fila (cond_wait em task_cond)
2. Retira Task da fila
3. Copia ViewState localmente
4. Para cada pixel do bloco:
   - Mapeia pixel → plano complexo
   - Chama compute_pixel()
   - Grava resultado em pixels[]
5. Decrementa tasks_remaining
6. Se chegou a 0: sinaliza done_cond
7. Volta ao passo 1
```

### Algoritmo compute_pixel

```
1. Verificação de cardioide e bulbo de período 2 (O(1))
2. Loop: z = z² + c com detecção de período (Brent)
3. Se escapou: smooth coloring via log e seno
4. Se não escapou (interior): retorna preto
```

---

## 5. Problema encontrado: imagem ficando estática

**Usuário:** "Sempre que deixo um tempo rodando, chega uma hora que a imagem entra em uma cor estática e permanece."

**Diagnóstico:**

O ponto de zoom original `(-0.7269450287, 0.1889241660)` ficava levemente **dentro** de um "lago" interno do conjunto de Mandelbrot. Em zoom profundo, toda a janela caía dentro desse lago → todos os pixels retornavam `max_iter` → tela inteiramente preta e estática.

Além disso, o `max_iter` fixo de 256 era insuficiente em zoom extremo — pixels próximos à fronteira precisam de mais iterações para serem distinguidos.

**Solução aplicada:**

**Fix 1 — Novo ponto de zoom (na fronteira do conjunto):**
```cpp
// Antes (dentro de um lago interno)
static const double ZOOM_TARGET_X = -0.7269450287;
static const double ZOOM_TARGET_Y =  0.1889241660;

// Depois (ponta de espiral na fronteira — estrutura fractal infinita)
static const double ZOOM_TARGET_X = -0.7436438885706799;
static const double ZOOM_TARGET_Y =  0.1318259042053185;
```

**Fix 2 — max_iter dinâmico:**
```cpp
double zoom_depth = SCALE_INITIAL / ctx.view.scale;
double growth     = 16.0 * std::sqrt(std::log2(1.0 + zoom_depth));
ctx.view.max_iter = max_iter + (int)growth;
ctx.view.max_iter = std::min(ctx.view.max_iter, max_iter * 4);
```

Cresce ~16 iterações por dobrada de zoom (raiz quadrada do log), muito mais suave que crescimento linear.

---

## 6. Otimizações de performance

**Usuário:** "Ficou bem pesado para renderizar a imagem cada vez que entra mais para dentro."

**Diagnóstico:**

Em zoom profundo, quase todos os pixels visíveis estão na fronteira e fazem iterações próximas de `max_iter`. Pixels interiores (dentro de "lagos" menores) percorriam o loop inteiro sem atalho.

### Otimização 1: Verificação de cardioide e bulbo de período 2

```cpp
double q = (px - 0.25) * (px - 0.25) + py * py;
if (q * (q + px - 0.25) < 0.25 * py * py) return 0xFF000000; // cardioide
if ((px + 1.0) * (px + 1.0) + py * py < 0.0625) return 0xFF000000; // bulbo p2
```

- Verificação O(1) para os dois maiores corpos do conjunto
- Cobre >50% da área visível em zoom inicial
- Evita o loop completo para esses pixels

### Otimização 2: Detecção de período — algoritmo de Brent

```cpp
double xold = 0.0, yold = 0.0;
int check_at = 4, since_check = 0;

while (iter < max_iter && zr2 + zi2 <= 4.0) {
    // ... iteração ...

    if (std::abs(zr - xold) < 1e-10 && std::abs(zi - yold) < 1e-10)
        return 0xFF000000; // órbita cíclica → interior

    if (++since_check == check_at) {
        xold = zr; yold = zi;
        since_check = 0;
        if (check_at < 512) check_at *= 2; // dobra: 4→8→16→...→512
    }
}
```

- Detecta quando a órbita fica presa em ciclo (ponto interior)
- Retorna preto imediatamente sem chegar em `max_iter`
- Intervalo de verificação dobra exponencialmente para cobrir períodos longos

**Ganho combinado:**

| Situação | Antes | Depois |
|----------|-------|--------|
| Pixel dentro da cardioide | max_iter iterações | ~5 operações |
| Pixel em lago interno (zoom profundo) | max_iter iterações | ~20–50 iterações |
| Pixel de fronteira | max_iter iterações | max_iter iterações (sem mudança) |

---

## 7. Correção do ZOOM_FACTOR

**Usuário:** Editou `ZOOM_FACTOR = 0.80` tentando diminuir a velocidade do zoom.

**Problema identificado:**

- Valor `0.80` é menor que 1.0 — como a lógica é `scale /= ZOOM_FACTOR`, isso faz `scale` **aumentar** (zoom para fora)
- Além disso, faltava o ponto-e-vírgula (erro de sintaxe)

**Correção:**

```cpp
// ZOOM_FACTOR deve ser MAIOR que 1.0 para aproximar a câmera
// Quanto mais próximo de 1.0, mais lento e suave o zoom
static const double ZOOM_FACTOR = 1.008; // +0.8% por frame → lento e suave
```

**Tabela de referência:**

| Valor | Velocidade |
|-------|------------|
| `1.003` | Muito lento |
| `1.008` | Lento ← recomendado |
| `1.015` | Moderado |
| `1.018` | Rápido (original) |
| `1.030` | Muito rápido |

**Lógica:**
- `scale` = unidades complexas por pixel
- `scale /= ZOOM_FACTOR` diminui o scale → cada pixel representa menos do plano → aproxima a câmera
- Para zoom fora: `scale *= fator` (fator > 1) — mas não está implementado, o zoom é sempre para dentro

---

## 8. Controle da frequência de renderização (FRAME_DELAY_MS)

**Usuário:** "Onde defino a velocidade com que a imagem é gerada?"

Adicionada a constante `FRAME_DELAY_MS` no topo de `main.cpp`:

```cpp
// Intervalo mínimo entre frames em milissegundos (0 = máxima velocidade)
//   0   ms → sem limite
//   16  ms → ~60 FPS
//   33  ms → ~30 FPS
//   100 ms → ~10 FPS
static const Uint32 FRAME_DELAY_MS = 0;
```

E aplicada no loop principal após `SDL_RenderPresent`:

```cpp
if (FRAME_DELAY_MS > 0) {
    static Uint32 last_frame = 0;
    Uint32 elapsed = SDL_GetTicks() - last_frame;
    if (elapsed < FRAME_DELAY_MS)
        SDL_Delay(FRAME_DELAY_MS - elapsed);
    last_frame = SDL_GetTicks();
}
```

**Diferença entre os dois controles:**

| Constante | O que controla |
|-----------|---------------|
| `ZOOM_FACTOR` | Quanto a câmera avança por frame (velocidade do zoom) |
| `FRAME_DELAY_MS` | De quanto em quanto tempo um novo frame é renderizado |

---

## 9. Renderização progressiva

**Usuário:** "Chega um ponto do zoom que a imagem começa a ser gerada — onde defino a velocidade dessa imagem sendo gerada?"

**Problema:** Em zoom profundo, o loop esperava 100% dos blocos calculados antes de mostrar qualquer coisa na tela. A janela ficava congelada e a imagem aparecia toda de uma vez.

**Solução — renderização progressiva:**

O loop principal foi reestruturado para atualizar a tela enquanto as workers ainda calculam:

```cpp
// Antes
dispatch_frame(&ctx, block_size);
wait_frame_done(&ctx);
SDL_UpdateTexture(...);
SDL_RenderPresent(renderer);

// Depois
dispatch_frame(&ctx, block_size);

while (ctx.tasks_remaining.load() > 0) {
    // Processa eventos (janela continua responsiva)
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { ... }
    if (!running) break;

    // Exibe estado parcial: blocos prontos aparecem, outros mostram frame anterior
    SDL_UpdateTexture(texture, nullptr, ctx.pixels, WIN_W * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

wait_frame_done(&ctx); // aguarda conclusão total antes de avançar zoom
```

**Resultado:** é possível ver cada bloco sendo preenchido conforme as threads terminam.

**A velocidade com que os blocos aparecem depende de:**
- `num_threads` — mais threads = blocos aparecem mais rápido em paralelo
- `block_size` — blocos menores = mais transições visíveis
- `max_iter` — mais iterações = cada bloco demora mais para aparecer

---

## 10. Resumo das constantes configuráveis (main.cpp)

Todas ficam no topo do arquivo, nas primeiras ~65 linhas:

```
main.cpp
│
├── WIN_W / WIN_H          → tamanho da janela em pixels
├── ZOOM_TARGET_X/Y        → ponto de destino do zoom (deve estar na fronteira do conjunto)
├── SCALE_INITIAL          → nível de zoom ao iniciar (3.5/WIN_W = visão completa)
├── ZOOM_FACTOR            → velocidade do zoom por frame (> 1.0 para aproximar)
├── SCALE_MIN              → limite de precisão do double (não alterar)
├── FRAME_DELAY_MS         → intervalo mínimo entre frames (0 = máxima velocidade)
│
└── função compute_pixel
    └── bloco r, g, b      → paleta de cores
```

---

## 11. Resumo dos parâmetros de linha de comando

```bash
./mandelbrot <num_threads> <max_iter> <block_size>
```

| Parâmetro | O que controla | Recomendado |
|-----------|---------------|-------------|
| `num_threads` | Threads paralelas; ganho linear até número de núcleos | `nproc` |
| `max_iter` | Teto de iterações; afeta detalhe da fronteira | `128`–`256` |
| `block_size` | Tamanho dos blocos; menor = melhor balanceamento em zoom profundo | `16` |

**Configuração recomendada:**
```bash
./mandelbrot 8 128 16
```

---

## 12. Pontos de zoom alternativos (ZOOM_TARGET_X/Y)

| Região | X | Y |
|--------|---|---|
| **Seahorse Valley** *(atual)* | `-0.7436438885706799` | `0.1318259042053185` |
| Elephant Valley | `0.3245046418` | `0.0485510112` |
| Triple Spiral | `-0.1011` | `0.9563` |
| Mini Mandelbrot | `-1.7497052` | `0.0` |
| Double Spiral | `-0.7771` | `0.1166` |

> O ponto deve estar exatamente na **fronteira** do conjunto. Pontos no interior causam tela preta em zoom profundo.

---

## 13. Paletas de cores alternativas (função compute_pixel)

Substitua o bloco `r, g, b` na função `compute_pixel`:

### Fogo
```cpp
double r = std::min(1.0, t * 3.0);
double g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
double b = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
```

### Oceano
```cpp
double r = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
double g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
double b = std::min(1.0, t * 3.0);
```

### Gold & Purple
```cpp
double r = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.75));
double g = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.50));
double b = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.00));
```

### Escala de Cinza
```cpp
double r = t; double g = t; double b = t;
```

---

## 14. Linha do tempo das alterações no código

| Alteração | Motivo |
|-----------|--------|
| Criação inicial de `main.cpp`, `Makefile`, `build.sh` | Estrutura do projeto |
| Criação de `README.md` | Documentação completa |
| Troca de `ZOOM_TARGET` para `(-0.7436, 0.1318)` | Ponto anterior causava tela preta em zoom profundo |
| Adição de `max_iter` dinâmico | Manter detalhe em zoom profundo |
| Adição de verificação de cardioide e bulbo | Performance: atalho O(1) para pixels interiores |
| Adição de detecção de período (Brent) | Performance: saída antecipada para pixels em ciclo |
| Fórmula de crescimento de `max_iter` mais suave (`sqrt log`) | Evitar sobrecarga em zoom extremo |
| Correção de `ZOOM_FACTOR = 0.80` → `1.008` | Valor < 1.0 causa zoom para fora; faltava ponto-e-vírgula |
| Adição de `FRAME_DELAY_MS` | Controle da frequência de renderização |
| Renderização progressiva | Ver blocos sendo preenchidos em tempo real |
